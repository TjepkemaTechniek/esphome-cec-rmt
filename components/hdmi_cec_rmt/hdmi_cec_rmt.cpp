#ifdef USE_ESP32

#include "hdmi_cec_rmt.h"
#include "cec_entities.h"

#include <cinttypes>

#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec_rmt {

static const char *const TAG = "hdmi_cec_rmt";

static constexpr uint32_t BIT_PERIOD_US = 2400;
static constexpr size_t TX_QUEUE_MAX = 48;
static constexpr uint32_t NEVER = 4294967295UL;  // cv.update_interval "never"

// Scheduler ids (numeric, so no string allocations)
static constexpr uint32_t ID_POWER_CHECK_1 = 0x1000;  // + logical address
static constexpr uint32_t ID_POWER_CHECK_2 = 0x1100;  // + logical address
static constexpr uint32_t ID_SET_SOURCE_RETRY = 0x1200;
static constexpr uint32_t ID_REFRESH_1 = 0x1300;
static constexpr uint32_t ID_REFRESH_2 = 0x1301;

// Opcodes used here
static constexpr uint8_t OP_FEATURE_ABORT = 0x00;
static constexpr uint8_t OP_IMAGE_VIEW_ON = 0x04;
static constexpr uint8_t OP_TEXT_VIEW_ON = 0x0D;
static constexpr uint8_t OP_STANDBY = 0x36;
static constexpr uint8_t OP_USER_CONTROL_PRESSED = 0x44;
static constexpr uint8_t OP_USER_CONTROL_RELEASED = 0x45;
static constexpr uint8_t OP_GIVE_OSD_NAME = 0x46;
static constexpr uint8_t OP_SET_OSD_NAME = 0x47;
static constexpr uint8_t OP_ROUTING_CHANGE = 0x80;
static constexpr uint8_t OP_ROUTING_INFORMATION = 0x81;
static constexpr uint8_t OP_ACTIVE_SOURCE = 0x82;
static constexpr uint8_t OP_GIVE_PHYSICAL_ADDRESS = 0x83;
static constexpr uint8_t OP_REQUEST_ACTIVE_SOURCE = 0x85;
static constexpr uint8_t OP_REPORT_PHYSICAL_ADDRESS = 0x84;
static constexpr uint8_t OP_SET_STREAM_PATH = 0x86;
static constexpr uint8_t OP_SET_SYSTEM_AUDIO_MODE = 0x72;
static constexpr uint8_t OP_DEVICE_VENDOR_ID = 0x87;
static constexpr uint8_t OP_GIVE_DEVICE_VENDOR_ID = 0x8C;
static constexpr uint8_t OP_GIVE_DEVICE_POWER_STATUS = 0x8F;
static constexpr uint8_t OP_REPORT_POWER_STATUS = 0x90;
static constexpr uint8_t OP_INACTIVE_SOURCE = 0x9D;
static constexpr uint8_t OP_CEC_VERSION = 0x9E;
static constexpr uint8_t OP_GET_CEC_VERSION = 0x9F;

static constexpr uint8_t UI_POWER_ON_FUNCTION = 0x6D;

// Requests a follower must answer; if we don't support them we send <Feature Abort>.
static bool needs_feature_abort(uint8_t opcode) {
  switch (opcode) {
    case 0x08:  // Give Tuner Device Status
    case 0x1A:  // Give Deck Status
    case 0x41:  // Play
    case 0x42:  // Deck Control
    case 0x44:  // User Control Pressed
    case 0x70:  // System Audio Mode Request
    case 0x71:  // Give Audio Status
    case 0x7D:  // Give System Audio Mode Status
    case 0x8D:  // Menu Request
    case 0x91:  // Get Menu Language
    case 0x9A:  // Set Audio Rate
    case 0xA4:  // Request Short Audio Descriptor
    case 0xC3:  // Request ARC Initiation
    case 0xC4:  // Request ARC Termination
      return true;
    default:
      return false;
  }
}

static uint8_t default_device_type(uint8_t la) {
  switch (la) {
    case 0:
      return 0;  // TV
    case 1:
    case 2:
    case 9:
      return 1;  // Recording device
    case 3:
    case 6:
    case 7:
    case 10:
      return 3;  // Tuner
    case 4:
    case 8:
    case 11:
      return 4;  // Playback device
    case 5:
      return 5;  // Audio system
    default:
      return 1;  // Specific use / reserved: present as a recording device (passive, not an input)
  }
}

static const char *default_la_name(uint8_t la) {
  static const char *const NAMES[16] = {"TV",         "Recorder 1",  "Recorder 2",  "Tuner 1",
                                        "Playback 1", "Audio system", "Tuner 2",     "Tuner 3",
                                        "Playback 2", "Recorder 3",  "Tuner 4",     "Playback 3",
                                        "Reserved 12", "Reserved 13", "Specific use", "Broadcast"};
  return NAMES[la & 0x0F];
}

static const char *device_type_name(uint8_t type) {
  switch (type) {
    case 0:
      return "TV";
    case 1:
      return "Recorder";
    case 3:
      return "Tuner";
    case 4:
      return "Playback";
    case 5:
      return "Audio system";
    case 6:
      return "CEC switch";
    case 7:
      return "Processor";
    case 0xFF:
      return "?";
    default:
      return "Reserved";
  }
}

static const char *power_name(uint8_t power) {
  switch (power) {
    case POWER_ON:
      return "on";
    case POWER_STANDBY:
      return "standby";
    case POWER_TO_ON:
      return "standby->on";
    case POWER_TO_STANDBY:
      return "on->standby";
    default:
      return "?";
  }
}

std::string format_physical_address(uint16_t pa) {
  if (pa == PA_UNKNOWN)
    return "?";
  char buf[12];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (pa >> 12) & 0xF, (pa >> 8) & 0xF, (pa >> 4) & 0xF, pa & 0xF);
  return buf;
}

const char *vendor_name(uint32_t vendor_id) {
  switch (vendor_id) {
    case 0x000039:
      return "Toshiba";
    case 0x0000F0:
      return "Samsung";
    case 0x0005CD:
      return "Denon";
    case 0x000678:
      return "Marantz";
    case 0x000982:
      return "Loewe";
    case 0x0009B0:
      return "Onkyo";
    case 0x000CB8:
      return "Medion";
    case 0x0010FA:
      return "Apple";
    case 0x001582:
      return "Pulse-Eight";
    case 0x008045:
      return "Panasonic";
    case 0x00903E:
      return "Philips";
    case 0x00A0DE:
      return "Yamaha";
    case 0x00E036:
      return "Pioneer";
    case 0x00E091:
      return "LG";
    case 0x08001F:
      return "Sharp";
    case 0x080046:
      return "Sony";
    case 0x6B746D:
      return "Vizio";
    default:
      return nullptr;
  }
}

const char *opcode_name(uint8_t opcode) {
  switch (opcode) {
    case 0x00:
      return "Feature Abort";
    case 0x04:
      return "Image View On";
    case 0x05:
      return "Tuner Step Increment";
    case 0x06:
      return "Tuner Step Decrement";
    case 0x07:
      return "Tuner Device Status";
    case 0x08:
      return "Give Tuner Device Status";
    case 0x0D:
      return "Text View On";
    case 0x1A:
      return "Give Deck Status";
    case 0x1B:
      return "Deck Status";
    case 0x32:
      return "Set Menu Language";
    case 0x35:
      return "Clear Analogue Timer";
    case 0x36:
      return "Standby";
    case 0x41:
      return "Play";
    case 0x42:
      return "Deck Control";
    case 0x44:
      return "User Control Pressed";
    case 0x45:
      return "User Control Released";
    case 0x46:
      return "Give OSD Name";
    case 0x47:
      return "Set OSD Name";
    case 0x64:
      return "Set OSD String";
    case 0x70:
      return "System Audio Mode Request";
    case 0x71:
      return "Give Audio Status";
    case 0x72:
      return "Set System Audio Mode";
    case 0x7A:
      return "Report Audio Status";
    case 0x7D:
      return "Give System Audio Mode Status";
    case 0x7E:
      return "System Audio Mode Status";
    case 0x80:
      return "Routing Change";
    case 0x81:
      return "Routing Information";
    case 0x82:
      return "Active Source";
    case 0x83:
      return "Give Physical Address";
    case 0x84:
      return "Report Physical Address";
    case 0x85:
      return "Request Active Source";
    case 0x86:
      return "Set Stream Path";
    case 0x87:
      return "Device Vendor ID";
    case 0x89:
      return "Vendor Command";
    case 0x8A:
      return "Vendor Remote Button Down";
    case 0x8B:
      return "Vendor Remote Button Up";
    case 0x8C:
      return "Give Device Vendor ID";
    case 0x8D:
      return "Menu Request";
    case 0x8E:
      return "Menu Status";
    case 0x8F:
      return "Give Device Power Status";
    case 0x90:
      return "Report Power Status";
    case 0x91:
      return "Get Menu Language";
    case 0x9A:
      return "Set Audio Rate";
    case 0x9D:
      return "Inactive Source";
    case 0x9E:
      return "CEC Version";
    case 0x9F:
      return "Get CEC Version";
    case 0xA0:
      return "Vendor Command With ID";
    case 0xA3:
      return "Report Short Audio Descriptor";
    case 0xA4:
      return "Request Short Audio Descriptor";
    case 0xA5:
      return "Give Features";
    case 0xA6:
      return "Report Features";
    case 0xA7:
      return "Request Current Latency";
    case 0xA8:
      return "Report Current Latency";
    case 0xC0:
      return "Initiate ARC";
    case 0xC1:
      return "Report ARC Initiated";
    case 0xC2:
      return "Report ARC Terminated";
    case 0xC3:
      return "Request ARC Initiation";
    case 0xC4:
      return "Request ARC Termination";
    case 0xC5:
      return "Terminate ARC";
    case 0xFF:
      return "Abort";
    default:
      return "?";
  }
}

// ---------------------------------------------------------------------------
// CecDevice
// ---------------------------------------------------------------------------

void CecDevice::publish(const DeviceState &state) {
#ifdef USE_BINARY_SENSOR
  if (this->present_sensor_ != nullptr)
    this->present_sensor_->publish_state(state.present);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->osd_name_sensor_ != nullptr && !state.osd_name.empty() && state.osd_name != this->last_osd_name_) {
    this->last_osd_name_ = state.osd_name;
    this->osd_name_sensor_->publish_state(state.osd_name);
  }
  if (this->pa_sensor_ != nullptr && state.physical_address != PA_UNKNOWN) {
    std::string pa = format_physical_address(state.physical_address);
    if (pa != this->last_pa_) {
      this->last_pa_ = pa;
      this->pa_sensor_->publish_state(pa);
    }
  }
  if (this->vendor_sensor_ != nullptr && state.vendor_known) {
    char buf[40];
    const char *name = vendor_name(state.vendor_id);
    if (name != nullptr) {
      snprintf(buf, sizeof(buf), "%s (%06" PRIX32 ")", name, state.vendor_id);
    } else {
      snprintf(buf, sizeof(buf), "%06" PRIX32, state.vendor_id);
    }
    if (this->last_vendor_ != buf) {
      this->last_vendor_ = buf;
      this->vendor_sensor_->publish_state(this->last_vendor_);
    }
  }
#endif
#ifdef USE_SWITCH
  if (this->power_switch_ != nullptr)
    this->power_switch_->update_from_power(state.power);
#endif
}

// ---------------------------------------------------------------------------
// Triggers
// ---------------------------------------------------------------------------

MessageTrigger::MessageTrigger(HdmiCecRmt *parent) { parent->add_message_trigger(this); }

void MessageTrigger::process(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data) {
  if (this->source_.has_value() && *this->source_ != source)
    return;
  if (this->destination_.has_value() && *this->destination_ != destination)
    return;
  if (this->opcode_.has_value() && (data.empty() || data[0] != *this->opcode_))
    return;
  if (this->data_.has_value() && *this->data_ != data)
    return;
  this->trigger(source, destination, data);
}

DeviceDiscoveredTrigger::DeviceDiscoveredTrigger(HdmiCecRmt *parent) { parent->add_discovered_trigger(this); }

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------

void HdmiCecRmt::add_device(CecDevice *device) {
  device->set_parent(this);
  this->devices_.push_back(device);
}

void HdmiCecRmt::setup() {
  if (!this->bus_.setup(this->pin_, this->monitor_mode_)) {
    this->mark_failed();
    return;
  }
  if (this->device_type_ == 0xFF)
    this->device_type_ = default_device_type(this->configured_la_);
  this->states_[0].physical_address = 0x0000;  // the TV is always 0.0.0.0
  for (auto &st : this->states_)
    st.dirty = true;

  int before = this->bus_.level_before_driver(), after = this->bus_.level_after_driver();
  if (before == 0) {
    ESP_LOGW(TAG, "CEC line is LOW before the driver touched it: held low externally (wiring, or a device on the bus)");
  } else if (after == 0) {
    ESP_LOGE(TAG, "CEC line was high, but is LOW after driver setup: the driver itself pulls it down (bug)");
  }

  if (this->monitor_mode_) {
    this->la_ = CEC_BROADCAST;
    ESP_LOGI(TAG, "Monitor mode: listening only, never driving the bus");
  } else {
    this->set_timeout(500, [this]() { this->allocate_logical_address_(); });
    if (this->scan_interval_ != NEVER)
      this->set_interval("scan", this->scan_interval_, [this]() { this->scan(); });
    if (this->power_poll_interval_ != NEVER) {
      this->set_interval("power_poll", this->power_poll_interval_, [this]() {
        for (auto *dev : this->devices_) {
          if (this->states_[dev->get_logical_address()].present)
            this->request_power_status(dev->get_logical_address());
        }
      });
    }
  }
  if (this->stats_interval_ != NEVER)
    this->set_interval("stats", this->stats_interval_, [this]() { this->publish_stats_(); });
}

void HdmiCecRmt::dump_config() {
  ESP_LOGCONFIG(TAG, "HDMI-CEC (RMT):");
  LOG_PIN("  Pin: ", this->pin_);
  ESP_LOGCONFIG(TAG,
                "  Logical address: %u (in use: %u)\n"
                "  Physical address: %s\n"
                "  Device type: %s\n"
                "  OSD name: %s\n"
                "  Vendor ID: %06" PRIX32 "\n"
                "  Promiscuous: %s\n"
                "  Monitor mode: %s\n"
                "  Line level now: %s (self-test at boot: before driver %s, after driver %s)",
                this->configured_la_, this->la_, format_physical_address(this->physical_address_).c_str(),
                device_type_name(this->device_type_), this->osd_name_.c_str(), this->vendor_id_,
                YESNO(this->promiscuous_), YESNO(this->monitor_mode_),
                this->is_failed() ? "?" : (this->bus_.line_is_low() ? "LOW (!)" : "high"),
                this->bus_.level_before_driver() == 1 ? "high" : (this->bus_.level_before_driver() == 0 ? "LOW" : "?"),
                this->bus_.level_after_driver() == 1 ? "high" : (this->bus_.level_after_driver() == 0 ? "LOW" : "?"));
  for (auto *dev : this->devices_)
    ESP_LOGCONFIG(TAG, "  Device '%s' at logical address %u", dev->get_name().c_str(), dev->get_logical_address());
  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup failed (RMT/GPTimer allocation)");
}

void HdmiCecRmt::loop() {
  this->bus_.service();
  CecFrame frame;
  for (uint8_t i = 0; i < 8 && this->bus_.read_frame(frame); i++)
    this->handle_frame_(frame);
  this->process_tx_();
  this->route_tick_();
  this->publish_states_();
  if (this->tx_in_flight_ || !this->tx_queue_.empty()) {
    this->high_freq_.start();
  } else {
    this->high_freq_.stop();
  }
}

// ---------------------------------------------------------------------------
// Address allocation
// ---------------------------------------------------------------------------

void HdmiCecRmt::allocate_logical_address_() {
  uint8_t la = this->configured_la_;
  if (la == CEC_BROADCAST) {
    this->la_ = CEC_BROADCAST;
    this->allocated_ = true;
    return;
  }
  // Poll our own address: if someone ACKs, it is taken.
  TxRequest req;
  req.frame.data[0] = static_cast<uint8_t>((la << 4) | la);
  req.frame.len = 1;
  req.attempts = 2;
  req.done = [this, la](bool acked) {
    if (acked) {
      ESP_LOGW(TAG, "Logical address %u is already in use by another device; continuing as unregistered (15)", la);
      this->la_ = CEC_BROADCAST;
    } else {
      this->la_ = la;
      ESP_LOGI(TAG, "Claimed logical address %u", la);
    }
    this->bus_.set_logical_address(this->la_);
    this->allocated_ = true;
    if (this->la_ != CEC_BROADCAST)
      this->report_physical_address_();
    // Ask who is active right now; the active source answers with <Active Source>.
    this->enqueue_frame_(this->la_, CEC_BROADCAST, {OP_REQUEST_ACTIVE_SOURCE});
    if (this->scan_interval_ != NEVER)
      this->set_timeout("initial_scan", 1000, [this]() { this->scan(); });
  };
  this->enqueue_(std::move(req));
}

void HdmiCecRmt::report_physical_address_() {
  this->enqueue_frame_(this->la_, CEC_BROADCAST,
                       {OP_REPORT_PHYSICAL_ADDRESS, static_cast<uint8_t>(this->physical_address_ >> 8),
                        static_cast<uint8_t>(this->physical_address_ & 0xFF), this->device_type_});
}

// ---------------------------------------------------------------------------
// TX queue
// ---------------------------------------------------------------------------

void HdmiCecRmt::enqueue_(TxRequest &&req, bool front) {
  if (this->monitor_mode_) {
    ESP_LOGW(TAG, "Monitor mode is enabled; not sending anything");
    return;
  }
  if (this->tx_queue_.size() >= TX_QUEUE_MAX) {
    ESP_LOGW(TAG, "TX queue full, dropping frame");
    return;
  }
  if (front) {
    this->tx_queue_.push_front(std::move(req));
  } else {
    this->tx_queue_.push_back(std::move(req));
  }
  this->high_freq_.start();
}

void HdmiCecRmt::enqueue_frame_(uint8_t source, uint8_t destination, std::initializer_list<uint8_t> data,
                                uint8_t attempts, std::function<void(bool)> done, bool front) {
  TxRequest req;
  req.frame.data[0] = static_cast<uint8_t>(((source & 0x0F) << 4) | (destination & 0x0F));
  uint8_t i = 1;
  for (uint8_t b : data) {
    if (i >= CEC_MAX_BLOCKS)
      break;
    req.frame.data[i++] = b;
  }
  req.frame.len = i;
  req.attempts = attempts;
  req.done = std::move(done);
  this->enqueue_(std::move(req), front);
}

void HdmiCecRmt::enqueue_marker_(std::function<void()> &&marker) {
  if (this->monitor_mode_)
    return;
  TxRequest req;
  req.marker = std::move(marker);
  this->tx_queue_.push_back(std::move(req));
}

void HdmiCecRmt::process_tx_() {
  if (this->tx_in_flight_) {
    TxResult result;
    if (this->bus_.poll_transmit(result))
      this->on_tx_done_(result);
    return;
  }
  while (!this->tx_queue_.empty() && this->tx_queue_.front().marker) {
    auto marker = std::move(this->tx_queue_.front().marker);
    this->tx_queue_.pop_front();
    marker();
  }
  if (this->tx_queue_.empty())
    return;
  if (!this->bus_.bus_free_for(this->next_signal_free_bits_ * BIT_PERIOD_US)) {
    uint32_t now = millis();
    if (this->busy_since_ == 0) {
      this->busy_since_ = now;
    } else if (!this->busy_warned_ && now - this->busy_since_ > 2000) {
      this->busy_warned_ = true;
      ESP_LOGW(TAG, "Cannot transmit for 2 s: %s", this->bus_.busy_reason());
    }
    return;
  }
  this->busy_since_ = 0;
  if (this->busy_warned_) {
    this->busy_warned_ = false;
    ESP_LOGI(TAG, "Bus is free again");
  }

  this->current_ = std::move(this->tx_queue_.front());
  this->tx_queue_.pop_front();
  if (this->bus_.start_transmit(this->current_.frame)) {
    this->tx_in_flight_ = true;
  } else if (this->current_.done) {
    this->current_.done(false);
  }
}

void HdmiCecRmt::on_tx_done_(TxResult result) {
  this->tx_in_flight_ = false;
  const CecFrame &f = this->current_.frame;
  this->record_(f, result == TxResult::OK ? 1 : result == TxResult::NACK ? 2 : result == TxResult::ARB_LOST ? 4 : 3);
  std::string desc = this->frame_to_string_(f);
  const char *name = f.has_opcode() ? opcode_name(f.opcode()) : "Poll";

  switch (result) {
    case TxResult::OK:
      ESP_LOGD(TAG, "TX %X->%X  %-24s %s%s", f.initiator(), f.destination(), desc.c_str(), name,
               f.is_broadcast() ? "" : "  [ACK]");
      this->next_signal_free_bits_ = 7;  // spec: same initiator sending again waits 7 bit periods
      if (this->current_.done)
        this->current_.done(true);
      return;
    case TxResult::ARB_LOST:
      ESP_LOGD(TAG, "TX %s: lost arbitration, will retry", desc.c_str());
      this->next_signal_free_bits_ = 5;
      this->tx_queue_.push_front(std::move(this->current_));
      return;
    case TxResult::NACK:
    case TxResult::ERROR:
    default:
      break;
  }

  if (this->current_.attempts > 1) {
    this->current_.attempts--;
    ESP_LOGV(TAG, "TX %s: %s, retrying", desc.c_str(), result == TxResult::NACK ? "no ACK" : "error");
    this->next_signal_free_bits_ = 3;  // spec: retransmission waits 3 bit periods
    this->tx_queue_.push_front(std::move(this->current_));
    return;
  }
  if (result == TxResult::NACK) {
    if (f.len == 1) {
      ESP_LOGV(TAG, "TX %X->%X  Poll: no device", f.initiator(), f.destination());
    } else {
      ESP_LOGD(TAG, "TX %X->%X  %-24s %s  [NACK: %s]", f.initiator(), f.destination(), desc.c_str(), name,
               f.is_broadcast() ? "rejected" : "no device acknowledged");
    }
  } else {
    ESP_LOGW(TAG, "TX %X->%X  %s %s: failed (bus error / readback mismatch)", f.initiator(), f.destination(),
             desc.c_str(), name);
  }
  this->next_signal_free_bits_ = 5;
  if (this->current_.done)
    this->current_.done(false);
}

// ---------------------------------------------------------------------------
// RX
// ---------------------------------------------------------------------------

std::string HdmiCecRmt::frame_to_string_(const CecFrame &frame) const {
  std::string out;
  char buf[4];
  for (uint8_t i = 0; i < frame.len; i++) {
    snprintf(buf, sizeof(buf), i == 0 ? "%02X" : ":%02X", frame.data[i]);
    out += buf;
  }
  return out;
}

void HdmiCecRmt::handle_frame_(const CecFrame &frame) {
  if (frame.flags & FRAME_OWN)
    return;  // our own transmission; reported in on_tx_done_()
  this->record_(frame, 0);

  uint8_t src = frame.initiator();
  uint8_t dst = frame.destination();
  bool complete = frame.flags & FRAME_COMPLETE;
  std::string desc = this->frame_to_string_(frame);
  const char *name = frame.has_opcode() ? opcode_name(frame.opcode()) : "Poll";
  const char *status = "";
  if (frame.flags & FRAME_ERROR) {
    status = "  [ERROR]";
  } else if (!complete) {
    status = (!frame.is_broadcast() && (frame.ack_bits & 1)) ? "  [NACK, aborted]" : "  [incomplete]";
  } else if (!frame.is_broadcast()) {
    status = frame.acknowledged() ? "  [ACK]" : "  [NACK]";
  } else if (!frame.acknowledged()) {
    status = "  [rejected]";
  }
  ESP_LOGD(TAG, "RX %X->%X  %-24s %s%s", src, dst, desc.c_str(), name, status);

#ifdef USE_TEXT_SENSOR
  if (this->last_message_sensor_ != nullptr) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%X->%X %s %s%s", src, dst, desc.c_str(), name, status);
    this->last_message_sensor_->publish_state(buf);
  }
#endif

  if (frame.flags & FRAME_ERROR)
    return;

  // Presence: whoever sends is there; a directed frame tells us whether the destination is.
  if (src != CEC_BROADCAST)
    this->mark_seen_(src);
  if (!frame.is_broadcast() && frame.len >= 1) {
    if (frame.ack_bits & 1) {
      // A NACKed poll means nobody is at that address -- except when a device polls its
      // own address to claim it (src == dst): then the NACK is the answer it wants.
      if (frame.len == 1 && src != dst)
        this->mark_absent_(dst);
    } else {
      this->mark_seen_(dst);
    }
  }

  if (!complete)
    return;

  this->update_state_(frame);
  this->respond_(frame);
  this->fire_triggers_(frame);
}

void HdmiCecRmt::update_state_(const CecFrame &f) {
  if (!f.has_opcode())
    return;
  uint8_t src = f.initiator();
  uint8_t dst = f.destination();
  DeviceState &s = this->states_[src];
  const uint8_t *d = f.data;

  switch (f.opcode()) {
    case OP_REPORT_PHYSICAL_ADDRESS:
      if (f.len >= 5) {
        s.physical_address = static_cast<uint16_t>((d[2] << 8) | d[3]);
        s.device_type = d[4];
        s.dirty = true;
        this->active_dirty_ = true;  // the name for the active address may resolve now
      }
      break;
    case OP_SET_OSD_NAME: {
      std::string name(reinterpret_cast<const char *>(&d[2]), f.len - 2);
      while (!name.empty() && (name.back() == '\0' || name.back() == ' '))
        name.pop_back();  // some devices pad the name with NULs
      if (name != s.osd_name) {
        s.osd_name = name;
        s.dirty = true;
        this->active_dirty_ = true;
      }
      break;
    }
    case OP_DEVICE_VENDOR_ID:
      if (f.len >= 5) {
        s.vendor_id = (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 8) | d[4];
        s.vendor_known = true;
        s.dirty = true;
      }
      break;
    case OP_REPORT_POWER_STATUS:
      if (f.len >= 3 && s.power != d[2]) {
        s.power = d[2];
        s.dirty = true;
      }
      break;
    case OP_ACTIVE_SOURCE:
      if (f.len >= 4) {
        uint16_t pa = static_cast<uint16_t>((d[2] << 8) | d[3]);
        if (s.physical_address != pa) {
          s.physical_address = pa;
          s.dirty = true;
        }
        if (s.power != POWER_ON) {
          s.power = POWER_ON;
          s.dirty = true;
        }
        this->set_active_pa_(pa);
        this->last_active_la_ = src;
        this->last_active_ms_ = millis();
        if (this->route_phase_ != ROUTE_IDLE && src != this->route_la_)
          ESP_LOGW(TAG, "%s claimed the source (%s) while switching to %s", this->display_name(src).c_str(),
                   format_physical_address(pa).c_str(), format_physical_address(this->route_pa_).c_str());
      }
      break;
    case OP_SET_STREAM_PATH:
    case OP_ROUTING_INFORMATION:
      if (f.len >= 4)
        this->set_active_pa_(static_cast<uint16_t>((d[2] << 8) | d[3]));
      this->schedule_state_refresh_();  // routing messages are hints; confirm who is really active
      break;
    case OP_ROUTING_CHANGE:
      if (f.len >= 6)
        this->set_active_pa_(static_cast<uint16_t>((d[4] << 8) | d[5]));
      this->schedule_state_refresh_();
      break;
    case OP_SET_SYSTEM_AUDIO_MODE:
      if (f.len >= 3 && d[2] == 0x01 && s.power != POWER_ON) {
        s.power = POWER_ON;  // only a powered audio system turns system audio on
        s.dirty = true;
      }
      break;
    case OP_INACTIVE_SOURCE:
      if (f.len >= 4 && static_cast<uint16_t>((d[2] << 8) | d[3]) == this->active_pa_)
        this->set_active_pa_(PA_UNKNOWN);
      break;
    case OP_STANDBY:
      if (f.is_broadcast()) {
        for (auto &st : this->states_) {
          if (st.present && st.power != POWER_STANDBY) {
            st.power = POWER_STANDBY;
            st.dirty = true;
          }
        }
        this->set_active_pa_(PA_UNKNOWN);
      } else if (this->states_[dst].power != POWER_STANDBY) {
        this->states_[dst].power = POWER_STANDBY;
        this->states_[dst].dirty = true;
      }
      this->schedule_state_refresh_();
      break;
    case OP_IMAGE_VIEW_ON:
    case OP_TEXT_VIEW_ON:
      if (dst == 0 && this->states_[0].power != POWER_ON) {
        this->states_[0].power = POWER_ON;
        this->states_[0].dirty = true;
      }
      this->schedule_state_refresh_();  // one-touch play: other devices usually wake too
      break;
    case OP_FEATURE_ABORT:
      if (f.len >= 4)
        ESP_LOGD(TAG, "%s refused <%s> (reason %u)", this->display_name(src).c_str(), opcode_name(d[2]), d[3]);
      break;
    default:
      break;
  }
}

void HdmiCecRmt::respond_(const CecFrame &f) {
  if (this->monitor_mode_ || this->la_ == CEC_BROADCAST || !f.has_opcode())
    return;
  if (f.destination() != this->la_)
    return;  // we only answer requests addressed to us
  uint8_t src = f.initiator();
  uint8_t op = f.opcode();

  switch (op) {
    case OP_GIVE_PHYSICAL_ADDRESS:
      this->enqueue_frame_(this->la_, CEC_BROADCAST,
                           {OP_REPORT_PHYSICAL_ADDRESS, static_cast<uint8_t>(this->physical_address_ >> 8),
                            static_cast<uint8_t>(this->physical_address_ & 0xFF), this->device_type_},
                           5, nullptr, true);
      break;
    case OP_GIVE_OSD_NAME: {
      TxRequest req;
      req.frame.data[0] = static_cast<uint8_t>((this->la_ << 4) | src);
      req.frame.data[1] = OP_SET_OSD_NAME;
      uint8_t n = 2;
      for (char c : this->osd_name_) {
        if (n >= CEC_MAX_BLOCKS)
          break;
        req.frame.data[n++] = static_cast<uint8_t>(c);
      }
      req.frame.len = n;
      this->enqueue_(std::move(req), true);
      break;
    }
    case OP_GIVE_DEVICE_VENDOR_ID:
      this->enqueue_frame_(this->la_, CEC_BROADCAST,
                           {OP_DEVICE_VENDOR_ID, static_cast<uint8_t>(this->vendor_id_ >> 16),
                            static_cast<uint8_t>(this->vendor_id_ >> 8), static_cast<uint8_t>(this->vendor_id_)},
                           5, nullptr, true);
      break;
    case OP_GET_CEC_VERSION:
      this->enqueue_frame_(this->la_, src, {OP_CEC_VERSION, 0x05}, 5, nullptr, true);  // 1.4
      break;
    case OP_GIVE_DEVICE_POWER_STATUS:
      this->enqueue_frame_(this->la_, src, {OP_REPORT_POWER_STATUS, POWER_ON}, 5, nullptr, true);
      break;
    default:
      if (needs_feature_abort(op))
        this->enqueue_frame_(this->la_, src, {OP_FEATURE_ABORT, op, 0x00}, 5, nullptr, true);  // unrecognized
      break;
  }
}

void HdmiCecRmt::fire_triggers_(const CecFrame &f) {
  if (this->message_triggers_.empty())
    return;
  uint8_t dst = f.destination();
  if (!this->promiscuous_ && dst != this->la_ && dst != CEC_BROADCAST)
    return;
  std::vector<uint8_t> data(f.data + 1, f.data + f.len);
  for (auto *t : this->message_triggers_)
    t->process(f.initiator(), dst, data);
}

// ---------------------------------------------------------------------------
// Device table
// ---------------------------------------------------------------------------

void HdmiCecRmt::mark_seen_(uint8_t la) {
  if (la >= CEC_BROADCAST || la == this->la_)
    return;
  DeviceState &s = this->states_[la];
  if (!s.present) {
    s.present = true;
    s.dirty = true;
    if (!this->scanning_ && this->allocated_ && !this->monitor_mode_)
      this->query_device_(la);
  }
}

void HdmiCecRmt::mark_absent_(uint8_t la) {
  if (la >= CEC_BROADCAST || la == this->la_)
    return;
  DeviceState &s = this->states_[la];
  if (s.present) {
    s.present = false;
    s.power = POWER_UNKNOWN;
    s.dirty = true;
  }
}

void HdmiCecRmt::query_device_(uint8_t la) {
  uint16_t bit = 1u << la;
  if (this->query_pending_ & bit)
    return;  // a query round for this device is already queued
  this->query_pending_ |= bit;
  this->enqueue_frame_(this->la_, la, {OP_GIVE_PHYSICAL_ADDRESS}, 2, [this, la](bool acked) {
    if (!acked)
      this->mark_absent_(la);
  });
  this->enqueue_frame_(this->la_, la, {OP_GIVE_OSD_NAME}, 2);
  this->enqueue_frame_(this->la_, la, {OP_GIVE_DEVICE_VENDOR_ID}, 2);
  this->enqueue_frame_(this->la_, la, {OP_GIVE_DEVICE_POWER_STATUS}, 2,
                       [this, bit](bool) { this->query_pending_ &= ~bit; });
}

void HdmiCecRmt::scan() {
  if (this->monitor_mode_ || this->scanning_ || !this->allocated_)
    return;
  this->scanning_ = true;
  this->query_pending_ = 0;  // scan re-queries everything that answers
  ESP_LOGI(TAG, "Scanning CEC bus...");
  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    if (la == this->la_)
      continue;
    this->enqueue_frame_(this->la_, la, {}, 2, [this, la](bool acked) {
      if (acked) {
        this->mark_seen_(la);
        this->query_device_(la);
      } else {
        this->mark_absent_(la);
      }
    });
  }
  // The poll callbacks queue their queries behind this first marker; the second
  // marker therefore lands after all of them.
  this->enqueue_marker_([this]() {
    this->enqueue_marker_([this]() { this->set_timeout("scan_done", 1500, [this]() { this->finish_scan_(); }); });
  });
}

void HdmiCecRmt::finish_scan_() {
  this->scanning_ = false;
  ESP_LOGI(TAG, "CEC bus scan complete:");
  ESP_LOGI(TAG, "  LA  Name              Phys.addr  Type          Vendor        Power");
  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    const DeviceState &s = this->states_[la];
    if (la == this->la_) {
      ESP_LOGI(TAG, "  %2u  %-16s  %-9s  %-12s  (this device)", la, this->osd_name_.c_str(),
               format_physical_address(this->physical_address_).c_str(), device_type_name(this->device_type_));
      continue;
    }
    if (!s.present)
      continue;
    char vendor[24];
    const char *vn = vendor_name(s.vendor_id);
    if (!s.vendor_known) {
      snprintf(vendor, sizeof(vendor), "?");
    } else if (vn != nullptr) {
      snprintf(vendor, sizeof(vendor), "%s", vn);
    } else {
      snprintf(vendor, sizeof(vendor), "%06" PRIX32, s.vendor_id);
    }
    ESP_LOGI(TAG, "  %2u  %-16s  %-9s  %-12s  %-12s  %s", la, this->display_name(la).c_str(),
             format_physical_address(s.physical_address).c_str(), device_type_name(s.device_type), vendor,
             power_name(s.power));
  }

  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    DeviceState &s = this->states_[la];
    if (!s.present || s.announced || la == this->la_)
      continue;
    s.announced = true;
    bool configured = false;
    for (auto *dev : this->devices_)
      configured |= dev->get_logical_address() == la;
    if (configured)
      continue;
    for (auto *t : this->discovered_triggers_)
      t->trigger(la, s.physical_address, s.osd_name);
  }
}

std::string HdmiCecRmt::display_name(uint8_t la) const {
  la &= 0x0F;
  for (auto *dev : this->devices_) {
    if (dev->get_logical_address() == la)
      return dev->get_name();
  }
  if (!this->states_[la].osd_name.empty())
    return this->states_[la].osd_name;
  return default_la_name(la);
}

void HdmiCecRmt::set_active_pa_(uint16_t pa) {
  if (pa != this->active_pa_) {
    this->active_pa_ = pa;
    this->active_dirty_ = true;
  }
}

void HdmiCecRmt::publish_states_() {
  bool any = false;
  for (auto &s : this->states_)
    any |= s.dirty;
  if (any) {
    for (auto *dev : this->devices_) {
      const DeviceState &s = this->states_[dev->get_logical_address()];
      if (s.dirty)
        dev->publish(s);
    }
    for (auto &s : this->states_)
      s.dirty = false;
  }

  if (!this->active_dirty_)
    return;
  this->active_dirty_ = false;
#ifdef USE_TEXT_SENSOR
  if (this->active_source_sensor_ != nullptr) {
    std::string text;
    if (this->active_pa_ == PA_UNKNOWN) {
      text = "Unknown";
    } else {
      for (uint8_t la = 0; la < CEC_BROADCAST && text.empty(); la++) {
        if (this->states_[la].physical_address == this->active_pa_ && (la == 0 || this->states_[la].present))
          text = this->display_name(la);
      }
#ifdef USE_SELECT
      if (text.empty() && this->source_select_ != nullptr) {
        const char *n = this->source_select_->name_for_address(this->active_pa_);
        if (n != nullptr)
          text = n;
      }
#endif
      if (text.empty())
        text = "HDMI " + format_physical_address(this->active_pa_);
    }
    if (text != this->last_active_text_) {
      this->last_active_text_ = text;
      this->active_source_sensor_->publish_state(text);
    }
  }
#endif
#ifdef USE_SELECT
  if (this->source_select_ != nullptr)
    this->source_select_->update_active(this->active_pa_);
#endif
}

void HdmiCecRmt::publish_stats_() {
#ifdef USE_SENSOR
  CecBusStats st = this->bus_.get_stats();
  if (this->frames_received_sensor_ != nullptr)
    this->frames_received_sensor_->publish_state(st.frames_received);
  if (this->frames_sent_sensor_ != nullptr)
    this->frames_sent_sensor_->publish_state(st.frames_sent);
  if (this->tx_nack_sensor_ != nullptr)
    this->tx_nack_sensor_->publish_state(st.tx_nack);
  if (this->tx_arbitration_lost_sensor_ != nullptr)
    this->tx_arbitration_lost_sensor_->publish_state(st.tx_arbitration_lost);
  if (this->tx_errors_sensor_ != nullptr)
    this->tx_errors_sensor_->publish_state(st.tx_errors);
  if (this->rx_errors_sensor_ != nullptr)
    this->rx_errors_sensor_->publish_state(st.rx_errors);
  if (this->acks_sent_sensor_ != nullptr)
    this->acks_sent_sensor_->publish_state(st.acks_sent);
  if (this->rx_ack_missed_sensor_ != nullptr)
    this->rx_ack_missed_sensor_->publish_state(st.acks_missed);
  if (this->watchdog_sensor_ != nullptr)
    this->watchdog_sensor_->publish_state(st.watchdog_releases);
#endif
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void HdmiCecRmt::send(uint8_t destination, const std::vector<uint8_t> &data, int source) {
  TxRequest req;
  uint8_t src = source < 0 ? this->la_ : static_cast<uint8_t>(source & 0x0F);
  req.frame.data[0] = static_cast<uint8_t>((src << 4) | (destination & 0x0F));
  uint8_t n = 1;
  for (uint8_t b : data) {
    if (n >= CEC_MAX_BLOCKS)
      break;
    req.frame.data[n++] = b;
  }
  req.frame.len = n;
  req.attempts = n == 1 ? 2 : 5;
  this->enqueue_(std::move(req));
}

void HdmiCecRmt::power_on(uint8_t destination) {
  destination &= 0x0F;
  if (destination < CEC_BROADCAST)
    this->power_on_sent_ms_[destination] = millis() | 1;
  if (destination == 0 || destination == CEC_BROADCAST) {
    this->enqueue_frame_(this->la_, 0, {OP_IMAGE_VIEW_ON});
    this->schedule_power_check_(0);
  } else {
    this->user_control(destination, UI_POWER_ON_FUNCTION);
    this->schedule_power_check_(destination);
  }
  this->schedule_state_refresh_(true);
}

void HdmiCecRmt::standby(uint8_t destination) {
  destination &= 0x0F;
  this->enqueue_frame_(this->la_, destination, {OP_STANDBY});
  this->schedule_state_refresh_(true);
  if (destination == CEC_BROADCAST) {
    for (auto *dev : this->devices_)
      this->schedule_power_check_(dev->get_logical_address());
  } else {
    this->schedule_power_check_(destination);
  }
}

void HdmiCecRmt::user_control(uint8_t destination, uint8_t key) {
  destination &= 0x0F;
  this->enqueue_frame_(this->la_, destination, {OP_USER_CONTROL_PRESSED, key});
  this->enqueue_frame_(this->la_, destination, {OP_USER_CONTROL_RELEASED});
}

void HdmiCecRmt::request_power_status(uint8_t destination) {
  destination &= 0x0F;
  this->enqueue_frame_(this->la_, destination, {OP_GIVE_DEVICE_POWER_STATUS}, 2, [this, destination](bool acked) {
    if (!acked)
      this->mark_absent_(destination);
  });
}

void HdmiCecRmt::schedule_state_refresh_(bool ours) {
  if (this->monitor_mode_ || !this->allocated_)
    return;
  // Bus-triggered refreshes have a cooldown: our own Request Active Source can make devices
  // send routing/system-audio messages, which must not trigger the next refresh in a loop.
  if (!ours && this->last_refresh_ms_ != 0 && millis() - this->last_refresh_ms_ < 15000)
    return;
  // Debounced: a burst of wake/standby/routing messages leads to one refresh at +2 s and one at +8 s.
  this->set_timeout(ID_REFRESH_1, 2000, [this]() { this->refresh_state_(); });
  this->set_timeout(ID_REFRESH_2, 8000, [this]() { this->refresh_state_(); });
}

void HdmiCecRmt::refresh_state_() {
  this->last_refresh_ms_ = millis();
  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    if (la != this->la_ && this->states_[la].present)
      this->request_power_status(la);
  }
  // Whoever is the active source answers with <Active Source>. Skipped right after we switched
  // the source ourselves: the previous source may still claim it, and a receiver would follow
  // that claim and switch straight back.
  if ((int32_t) (millis() - this->suppress_active_request_until_) >= 0)
    this->enqueue_frame_(this->la_, CEC_BROADCAST, {OP_REQUEST_ACTIVE_SOURCE});
}

void HdmiCecRmt::note_own_switch_(uint16_t pa) {
  this->suppress_active_request_until_ = millis() + 12000;
  this->set_active_pa_(pa);  // inputs without CEC never announce themselves; we know where we routed
}

uint8_t HdmiCecRmt::active_source_logical_address() const {
  if (this->active_pa_ == PA_UNKNOWN)
    return 0xFF;
  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    if (la != this->la_ && this->states_[la].present && this->states_[la].physical_address == this->active_pa_)
      return la;
  }
  return 0xFF;
}

void HdmiCecRmt::set_stream_path(uint16_t physical_address) {
  ESP_LOGI(TAG, "Set Stream Path to %s", format_physical_address(physical_address).c_str());
  this->enqueue_frame_(this->la_, CEC_BROADCAST,
                       {OP_SET_STREAM_PATH, static_cast<uint8_t>(physical_address >> 8),
                        static_cast<uint8_t>(physical_address & 0xFF)});
  this->note_own_switch_(physical_address);
  this->schedule_state_refresh_(true);
}

bool HdmiCecRmt::route_needs_wait_(uint8_t la) const {
  const DeviceState &s = this->states_[la];
  if (s.power == POWER_ON)
    return false;
  if (s.power == POWER_TO_ON)
    return true;
  // Woken by us a moment ago and not yet reported on (it may not have answered a poll yet).
  return this->power_on_sent_ms_[la] != 0 && millis() - this->power_on_sent_ms_[la] < 15000;
}

void HdmiCecRmt::route_to(uint16_t physical_address, uint8_t la) {
  this->route_pa_ = physical_address;
  this->route_la_ = la;
  this->route_resent_ = false;
  this->route_started_ = millis();
  this->route_last_poll_ = 0;
  this->route_awake_ms_ = 0;
  this->suppress_active_request_until_ = millis() + 25000;  // nobody gets asked while we switch
  bool cec_source = la != 0xFF && la != 0 && la < CEC_BROADCAST;
  if (cec_source && this->states_[la].power != POWER_ON) {
    ESP_LOGI(TAG, "Input %s: waking %s first", format_physical_address(physical_address).c_str(),
             this->display_name(la).c_str());
    this->power_on(la);
  }
  // Wait for everything that is still starting up: the source itself, and the TV and audio
  // system, which do the switching. A TV or receiver that is still booting ignores
  // <Set Stream Path>, and when it finishes it picks an input of its own (often the port of
  // whoever woke it), so routing before then gets undone.
  this->route_wait_mask_ = 0;
  if (cec_source && this->states_[la].power != POWER_ON)
    this->route_wait_mask_ |= 1u << la;
  for (uint8_t sw : {(uint8_t) 0, (uint8_t) 5}) {
    if (sw != la && this->states_[sw].present && this->route_needs_wait_(sw))
      this->route_wait_mask_ |= 1u << sw;
  }
  if (this->route_wait_mask_ != 0) {
    this->route_phase_ = ROUTE_WAIT_POWER;
    return;
  }
  this->set_stream_path(physical_address);
  this->route_phase_ = ROUTE_WAIT_CONFIRM;
  this->route_started_ = millis();
}

void HdmiCecRmt::route_tick_() {
  if (this->route_phase_ == ROUTE_IDLE)
    return;
  uint32_t now = millis();
  if (this->route_phase_ == ROUTE_WAIT_POWER) {
    for (uint8_t i = 0; i < CEC_BROADCAST; i++) {
      if ((this->route_wait_mask_ & (1u << i)) && this->states_[i].power == POWER_ON)
        this->route_wait_mask_ &= ~(1u << i);
    }
    bool awake = this->route_wait_mask_ == 0;
    if (!awake && now - this->route_started_ < 15000) {
      if (now - this->route_last_poll_ >= 1000) {
        this->route_last_poll_ = now;
        for (uint8_t i = 0; i < CEC_BROADCAST; i++) {
          if (this->route_wait_mask_ & (1u << i))
            this->request_power_status(i);
        }
      }
      return;
    }
    // Give a device that just came up a moment to finish its own start-up switching.
    if (awake && this->route_awake_ms_ == 0)
      this->route_awake_ms_ = now;
    if (awake && now - this->route_awake_ms_ < 1500)
      return;
    ESP_LOGI(TAG, "%s, routing to %s", awake ? "Everything is awake" : "Not everything reported power on in 15 s",
             format_physical_address(this->route_pa_).c_str());
    this->set_stream_path(this->route_pa_);
    this->route_phase_ = ROUTE_WAIT_CONFIRM;
    this->route_started_ = now;
    return;
  }
  // ROUTE_WAIT_CONFIRM
  bool cec_source = this->route_la_ != 0xFF && this->route_la_ != 0 && this->route_la_ < CEC_BROADCAST;
  if (cec_source && this->last_active_la_ == this->route_la_ &&
      (int32_t) (this->last_active_ms_ - this->route_started_) >= 0) {
    ESP_LOGI(TAG, "%s confirmed as active source", this->display_name(this->route_la_).c_str());
    this->route_phase_ = ROUTE_IDLE;
    return;
  }
  if (now - this->route_started_ < 3000)
    return;
  if (!cec_source) {
    // An input without CEC can't confirm anything. Say it once more: harmless if the switch
    // already happened, and it overrides a late start-up switch of the TV or receiver.
    ESP_LOGI(TAG, "Repeating Set Stream Path to %s", format_physical_address(this->route_pa_).c_str());
    this->set_stream_path(this->route_pa_);
    this->route_phase_ = ROUTE_IDLE;
    return;
  }
  // Some playback devices never answer <Set Stream Path>. Then the bus has no
  // active source and a receiver may fall back to its previous input. Announce it on the
  // device's behalf, which is what the device should have done itself.
  ESP_LOGI(TAG, "%s did not announce itself; sending <Active Source> on its behalf",
           this->display_name(this->route_la_).c_str());
  this->enqueue_frame_(this->route_la_, CEC_BROADCAST,
                       {OP_ACTIVE_SOURCE, static_cast<uint8_t>(this->route_pa_ >> 8),
                        static_cast<uint8_t>(this->route_pa_ & 0xFF)});
  this->route_phase_ = ROUTE_IDLE;
}

void HdmiCecRmt::record_(const CecFrame &frame, uint8_t kind) {
  // Leave out our own routine traffic (polls, power and scan queries) so the history covers
  // more of what matters. The answers to those queries are still recorded.
  if (kind != 0 && frame.len >= 1) {
    if (frame.len == 1)
      return;
    switch (frame.data[1]) {
      case OP_GIVE_DEVICE_POWER_STATUS:
      case 0x83:  // Give Physical Address
      case 0x46:  // Give OSD Name
      case 0x8C:  // Give Device Vendor ID
        return;
      default:
        break;
    }
  }
  HistoryEntry &e = this->history_[this->history_head_];
  e.ms = millis();
  e.frame = frame;
  e.kind = kind;
  this->history_head_ = (this->history_head_ + 1) % HISTORY_SIZE;
  if (this->history_count_ < HISTORY_SIZE)
    this->history_count_++;
}

void HdmiCecRmt::dump_history() {
  static const char *const KINDS[] = {"RX", "TX", "TX NACK", "TX ERR", "TX ARB"};
  uint32_t now = millis();
  ESP_LOGI(TAG, "Last %u frames (oldest first, seconds before now):", this->history_count_);
  uint8_t start = (this->history_head_ + HISTORY_SIZE - this->history_count_) % HISTORY_SIZE;
  for (uint8_t i = 0; i < this->history_count_; i++) {
    const HistoryEntry &e = this->history_[(start + i) % HISTORY_SIZE];
    const CecFrame &f = e.frame;
    std::string hex = this->frame_to_string_(f);
    const char *ack = "";
    if (e.kind == 0 && !f.is_broadcast())
      ack = (f.ack_bits & 1) ? " [NACK]" : " [ACK]";
    ESP_LOGI(TAG, "  -%7.3f  %-7s %X->%X  %-24s %s%s", (now - e.ms) / 1000.0f, KINDS[e.kind < 5 ? e.kind : 0],
             f.initiator(), f.destination(), hex.c_str(), f.has_opcode() ? opcode_name(f.opcode()) : "Poll", ack);
    delay(2);  // keep the log/API buffer from overflowing
  }
}

void HdmiCecRmt::schedule_power_check_(uint8_t la) {
  if (la >= CEC_BROADCAST)
    return;
  // Devices report "in transition" for a few seconds; ask twice.
  this->set_timeout(ID_POWER_CHECK_1 + la, 3000, [this, la]() { this->request_power_status(la); });
  this->set_timeout(ID_POWER_CHECK_2 + la, 10000, [this, la]() { this->request_power_status(la); });
}

void HdmiCecRmt::set_active_source(uint8_t destination) {
  destination &= 0x0F;
  if (destination >= CEC_BROADCAST)
    return;
  uint16_t pa = destination == 0 ? 0x0000 : this->states_[destination].physical_address;
  if (pa != PA_UNKNOWN) {
    this->do_set_active_source_(destination, pa);
    return;
  }
  // Physical address unknown: ask for it, then try once more.
  ESP_LOGI(TAG, "Physical address of %s unknown, requesting it first", this->display_name(destination).c_str());
  this->enqueue_frame_(this->la_, destination, {OP_GIVE_PHYSICAL_ADDRESS}, 2, [this, destination](bool acked) {
    if (!acked) {
      ESP_LOGW(TAG, "%s does not respond; cannot switch to it", this->display_name(destination).c_str());
      return;
    }
    this->set_timeout(ID_SET_SOURCE_RETRY, 500, [this, destination]() {
      uint16_t pa = this->states_[destination].physical_address;
      if (pa == PA_UNKNOWN) {
        ESP_LOGW(TAG, "%s did not report its physical address", this->display_name(destination).c_str());
      } else {
        this->do_set_active_source_(destination, pa);
      }
    });
  });
}

void HdmiCecRmt::do_set_active_source_(uint8_t la, uint16_t pa) {
  uint8_t hi = static_cast<uint8_t>(pa >> 8), lo = static_cast<uint8_t>(pa & 0xFF);
  ESP_LOGI(TAG, "Switching to %s (%s)", this->display_name(la).c_str(), format_physical_address(pa).c_str());
  this->enqueue_frame_(this->la_, 0, {OP_IMAGE_VIEW_ON});  // make sure the TV is on
  if (this->source_switch_method_ != SourceSwitchMethod::ACTIVE_SOURCE)
    this->enqueue_frame_(this->la_, CEC_BROADCAST, {OP_SET_STREAM_PATH, hi, lo});
  if (this->source_switch_method_ != SourceSwitchMethod::SET_STREAM_PATH && la != 0)
    this->enqueue_frame_(la, CEC_BROADCAST, {OP_ACTIVE_SOURCE, hi, lo});  // on behalf of the device
  this->note_own_switch_(pa);
  this->schedule_state_refresh_(true);
}

}  // namespace hdmi_cec_rmt
}  // namespace esphome

#endif  // USE_ESP32
