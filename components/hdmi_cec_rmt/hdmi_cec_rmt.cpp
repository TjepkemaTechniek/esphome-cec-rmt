#ifdef USE_ESP32

#include "hdmi_cec_rmt.h"
#include "cec_entities.h"

#include <cinttypes>
#include <cstdlib>

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
static constexpr uint32_t ID_SETTLED_1 = 0x1302;
static constexpr uint32_t ID_SETTLED_2 = 0x1303;
static constexpr uint32_t ID_INFO_QUERY = 0x1400;  // + logical address

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

// <User Control Pressed> keys (HDMI 1.4b CEC table 27). Keep in sync with KEYS in __init__.py.
struct KeyEntry {
  const char *name;
  uint8_t code;
};
static const KeyEntry KEYS[] = {
    {"select", 0x00},        {"ok", 0x00},           {"up", 0x01},
    {"down", 0x02},          {"left", 0x03},         {"right", 0x04},
    {"root_menu", 0x09},     {"home", 0x09},         {"setup_menu", 0x0A},
    {"contents_menu", 0x0B}, {"favorite_menu", 0x0C}, {"back", 0x0D},
    {"exit", 0x0D},          {"menu", 0x0D},         {"number_0", 0x20},
    {"number_1", 0x21},      {"number_2", 0x22},     {"number_3", 0x23},
    {"number_4", 0x24},      {"number_5", 0x25},     {"number_6", 0x26},
    {"number_7", 0x27},      {"number_8", 0x28},     {"number_9", 0x29},
    {"enter", 0x2B},         {"clear", 0x2C},        {"channel_up", 0x30},
    {"channel_down", 0x31},  {"previous_channel", 0x32}, {"input_select", 0x34},
    {"info", 0x35},          {"help", 0x36},         {"page_up", 0x37},
    {"page_down", 0x38},     {"power", 0x40},        {"volume_up", 0x41},
    {"volume_down", 0x42},   {"mute", 0x43},         {"play", 0x44},
    {"stop", 0x45},          {"pause", 0x46},        {"record", 0x47},
    {"rewind", 0x48},        {"fast_forward", 0x49}, {"eject", 0x4A},
    {"next", 0x4B},          {"forward", 0x4B},      {"previous", 0x4C},
    {"backward", 0x4C},      {"subtitle", 0x51},     {"epg", 0x53},
    {"play_function", 0x60}, {"pause_play", 0x61},   {"stop_function", 0x64},
    {"mute_function", 0x65}, {"restore_volume", 0x66}, {"select_media", 0x68},
    {"select_av_input", 0x69}, {"select_audio_input", 0x6A}, {"power_toggle", 0x6B},
    {"power_off", 0x6C},     {"power_on", 0x6D},     {"blue", 0x71},
    {"red", 0x72},           {"green", 0x73},        {"yellow", 0x74},
};

static std::string normalize_name(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (c == ' ' || c == '-') {
      c = '_';
    } else if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    out += c;
  }
  while (!out.empty() && out.back() == '_')
    out.pop_back();
  return out;
}

static int parse_number(const std::string &s) {
  if (s.empty())
    return -1;
  char *end = nullptr;
  long v = strtol(s.c_str(), &end, 0);  // decimal, or hex with 0x
  if (end == nullptr || *end != '\0' || v < 0 || v > 255)
    return -1;
  return static_cast<int>(v);
}

int key_code(const std::string &name) {
  std::string n = normalize_name(name);
  for (const auto &k : KEYS) {
    if (n == k.name)
      return k.code;
  }
  return parse_number(n);
}

const char *key_name(uint8_t code) {
  for (const auto &k : KEYS) {
    if (k.code == code)
      return k.name;  // the first name listed is the main one
  }
  return "?";
}

// ---------------------------------------------------------------------------
// CecDevice
// ---------------------------------------------------------------------------

uint8_t CecDevice::match_score(const DeviceState &s) const {
  bool confirmed = false;
  if (this->match_vendor_ && s.vendor_known) {
    if (s.vendor_id != this->match_vendor_id_)
      return 0;
    confirmed = true;
  }
  if (!this->match_osd_name_.empty() && !s.osd_name.empty()) {
    if (normalize_name(s.osd_name) != normalize_name(this->match_osd_name_))
      return 0;
    confirmed = true;
  }
  if (this->match_device_type_ != 0xFF && s.device_type != 0xFF) {
    if (s.device_type != this->match_device_type_)
      return 0;
    confirmed = true;
  }
  return confirmed ? 2 : 1;
}

void CecDevice::restore() {
  if (!this->is_auto())
    return;
  this->pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("hdmi_cec_rmt_dev_" + this->name_));
  uint8_t la;
  if (this->pref_.load(&la) && la < CEC_BROADCAST) {
    this->saved_la_ = la;
    this->la_ = la;  // provisional until the device confirms it, so commands work right after boot
  }
}

bool CecDevice::bind(uint8_t la, bool remember) {
  if (!this->is_auto())
    return false;
  if (remember && la != LA_NONE && la != this->saved_la_) {
    this->saved_la_ = la;
    this->pref_.save(&la);
  }
  if (la == this->la_)
    return false;
  this->la_ = la;
  this->force_publish_ = true;
  return true;
}

void CecDevice::publish(const DeviceState &state) {
  this->force_publish_ = false;
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
  for (auto *dev : this->devices_)
    dev->restore();

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
          uint8_t la = dev->get_logical_address();
          if (la < CEC_BROADCAST && this->states_[la].present)
            this->request_power_status(la);
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
                "  Passive: %s\n"
                "  Monitor mode: %s\n"
                "  Line level now: %s (self-test at boot: before driver %s, after driver %s)",
                this->configured_la_, this->la_, format_physical_address(this->physical_address_).c_str(),
                device_type_name(this->device_type_), this->osd_name_.c_str(), this->vendor_id_,
                YESNO(this->promiscuous_), YESNO(this->passive_), YESNO(this->monitor_mode_),
                this->is_failed() ? "?" : (this->bus_.line_is_low() ? "LOW (!)" : "high"),
                this->bus_.level_before_driver() == 1 ? "high" : (this->bus_.level_before_driver() == 0 ? "LOW" : "?"),
                this->bus_.level_after_driver() == 1 ? "high" : (this->bus_.level_after_driver() == 0 ? "LOW" : "?"));
  for (auto *dev : this->devices_) {
    if (!dev->is_auto()) {
      ESP_LOGCONFIG(TAG, "  Device '%s' at logical address %u", dev->get_name().c_str(), dev->get_logical_address());
    } else if (dev->get_logical_address() == LA_NONE) {
      ESP_LOGCONFIG(TAG, "  Device '%s': auto-detect, not seen yet", dev->get_name().c_str());
    } else {
      ESP_LOGCONFIG(TAG, "  Device '%s': auto-detect, at logical address %u", dev->get_name().c_str(),
                    dev->get_logical_address());
    }
  }
  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup failed (RMT/GPTimer allocation)");
}

void HdmiCecRmt::loop() {
  this->bus_.service();
  CecFrame frame;
  for (uint8_t i = 0; i < 8 && this->bus_.read_frame(frame); i++)
    this->handle_frame_(frame);
  if (this->identity_dirty_) {
    this->identity_dirty_ = false;
    this->bind_devices_();
    if (!this->scanning_)
      this->announce_new_devices_();
  }
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
    if (this->passive_) {
      // Passive: claim the address (so we ACK what is sent to us) and stay quiet. Everything else
      // is learned from the traffic; we only answer when asked.
      ESP_LOGI(TAG, "Passive mode: learning devices from bus traffic, not polling");
      return;
    }
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
        this->identity_dirty_ = true;
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
        this->identity_dirty_ = true;
        this->active_dirty_ = true;
      }
      break;
    }
    case OP_DEVICE_VENDOR_ID:
      if (f.len >= 5) {
        s.vendor_id = (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 8) | d[4];
        s.vendor_known = true;
        s.dirty = true;
        this->identity_dirty_ = true;
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
        if (src != this->la_ && pa != this->proxy_pa_)
          this->proxy_pa_ = PA_UNKNOWN;
        this->off_guard_check_(src);
        if (src != this->route_la_ && pa != this->route_pa_ && src != 0 && src != 5 && src != this->la_ &&
            src < CEC_BROADCAST)
          this->route_intruder_(src, pa);
      }
      break;
    case OP_SET_STREAM_PATH:
    case OP_ROUTING_INFORMATION:
      if (src == 0 && f.opcode() == OP_SET_STREAM_PATH && this->states_[0].power != POWER_ON) {
        this->states_[0].power = POWER_ON;  // a TV only picks an input while it is on
        this->states_[0].dirty = true;
      }
      if (f.len >= 4) {
        uint16_t pa = static_cast<uint16_t>((d[2] << 8) | d[3]);
        // The TV routing somewhere else (e.g. the user picked a source) ends our stand-in role,
        // except right after our own switch, when the guard deals with it.
        if (src != this->la_ && f.opcode() == OP_SET_STREAM_PATH && pa != this->proxy_pa_ &&
            (int32_t) (millis() - this->route_guard_until_) >= 0 && this->route_phase_ == ROUTE_IDLE)
          this->proxy_pa_ = PA_UNKNOWN;
        this->set_active_pa_(pa);
      }
      this->schedule_state_refresh_();  // routing messages are hints; confirm who is really active
      break;
    case OP_ROUTING_CHANGE:
      if (src == 0 && this->states_[0].power != POWER_ON) {
        this->states_[0].power = POWER_ON;
        this->states_[0].dirty = true;
      }
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
        this->proxy_pa_ = PA_UNKNOWN;
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
      // The TV is going to standby (told by anyone, e.g. a playback device's remote, or the TV
      // putting the system to sleep): some TVs wake a playback device on the way down, which
      // then turns everything back on. Guard against that, like after our own "everything off".
      if ((f.is_broadcast() || dst == 0 || src == 0) && (int32_t) (millis() - this->off_guard_until_) >= 0) {
        this->off_guard_until_ = millis() + 20000;
        this->off_guard_count_ = 0;
      }
      this->schedule_state_refresh_();
      break;
    case OP_IMAGE_VIEW_ON:
    case OP_TEXT_VIEW_ON:
      if (dst == 0 && this->states_[0].power != POWER_ON) {
        this->states_[0].power = POWER_ON;
        this->states_[0].dirty = true;
      }
      this->off_guard_check_(src);
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
  if (f.opcode() == OP_REQUEST_ACTIVE_SOURCE && f.is_broadcast() && f.initiator() != this->la_ &&
      this->proxy_pa_ != PA_UNKNOWN) {
    // We switched to an input without CEC: answer for it, so the TV doesn't pick a source of its own.
    this->enqueue_frame_(this->la_, CEC_BROADCAST,
                         {OP_ACTIVE_SOURCE, static_cast<uint8_t>(this->proxy_pa_ >> 8),
                          static_cast<uint8_t>(this->proxy_pa_ & 0xFF)},
                         5, nullptr, true);
    return;
  }
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
    this->identity_dirty_ = true;
    // Passive: don't ask a new device anything; it tells who it is when it talks to the TV.
    if (!this->scanning_ && this->allocated_ && !this->monitor_mode_ && !this->passive_)
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
    this->identity_dirty_ = true;
  }
}

void HdmiCecRmt::bind_devices_() {
  uint16_t taken = 0;  // addresses owned by fixed devices or already bound auto devices
  for (auto *dev : this->devices_) {
    if (!dev->is_auto())
      taken |= 1u << dev->get_logical_address();
  }
  for (auto *dev : this->devices_) {
    if (!dev->is_auto())
      continue;
    uint8_t cur = dev->get_logical_address();
    uint8_t cur_score = 0;
    if (cur < CEC_BROADCAST && !(taken & (1u << cur))) {
      cur_score = dev->match_score(this->states_[cur]);
      if (!this->states_[cur].present && cur_score == 2)
        cur_score = 1;  // gone from there: look elsewhere, but keep it if it isn't found
    }
    uint8_t best = LA_NONE;
    if (cur_score < 2) {
      // Look for an address where the device has identified itself.
      for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
        if (la == this->la_ || (taken & (1u << la)) || !this->states_[la].present)
          continue;
        if (dev->match_score(this->states_[la]) == 2) {
          best = la;
          break;
        }
      }
    }
    uint8_t target;
    bool confirmed;
    if (best != LA_NONE) {
      target = best;
      confirmed = true;
    } else if (cur_score >= 1) {
      target = cur;  // keep (confirmed, or remembered and not contradicted)
      confirmed = cur_score == 2;
    } else {
      target = LA_NONE;
      confirmed = false;
    }
    if (dev->bind(target, confirmed)) {
      if (target == LA_NONE) {
        ESP_LOGI(TAG, "'%s' is no longer at logical address %u", dev->get_name().c_str(), cur);
      } else {
        ESP_LOGI(TAG, "Found '%s' at logical address %u", dev->get_name().c_str(), target);
      }
      this->active_dirty_ = true;  // display names may have changed
    }
    if (target < CEC_BROADCAST)
      taken |= 1u << target;
  }
}

void HdmiCecRmt::announce_new_devices_() {
  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    DeviceState &s = this->states_[la];
    if (!s.present || s.announced || la == this->la_)
      continue;
    bool configured = false;
    for (auto *dev : this->devices_)
      configured |= dev->get_logical_address() == la;
    if (configured) {
      s.announced = true;
      continue;
    }
    // Outside a scan, wait until the device has said something about itself.
    if (!this->scanning_ && s.osd_name.empty() && s.physical_address == PA_UNKNOWN)
      continue;
    s.announced = true;
    for (auto *t : this->discovered_triggers_)
      t->trigger(la, s.physical_address, s.osd_name);
  }
}

void HdmiCecRmt::query_device_(uint8_t la) {
  uint16_t bit = 1u << la;
  if (this->query_pending_ & bit)
    return;  // a query round for this device is already queued
  this->query_pending_ |= bit;
  DeviceState &s = this->states_[la];
  if (la == 0 && s.physical_address == PA_UNKNOWN) {
    s.physical_address = 0x0000;  // the TV is always 0.0.0.0
    s.dirty = true;
  }
  // Power status and OSD name are answered directly to us. Only ask what we don't know yet.
  this->enqueue_frame_(this->la_, la, {OP_GIVE_DEVICE_POWER_STATUS}, 2, [this, la](bool acked) {
    if (!acked)
      this->mark_absent_(la);
  });
  if (s.osd_name.empty())
    this->enqueue_frame_(this->la_, la, {OP_GIVE_OSD_NAME}, 2);
  // <Report Physical Address> and <Device Vendor ID> are broadcasts, and some TVs treat them as
  // "a device was just connected" and switch that device on. So ask them only once, and only
  // while the device is on anyway; otherwise they are learned from the device's own messages.
  this->enqueue_marker_([this, la, bit]() {
    this->set_timeout(ID_INFO_QUERY + la, 1500, [this, la, bit]() {
      this->query_pending_ &= ~bit;
      const DeviceState &st = this->states_[la];
      if (!st.present || st.power != POWER_ON)
        return;
      if (st.physical_address == PA_UNKNOWN)
        this->enqueue_frame_(this->la_, la, {OP_GIVE_PHYSICAL_ADDRESS}, 2);
      if (!st.vendor_known)
        this->enqueue_frame_(this->la_, la, {OP_GIVE_DEVICE_VENDOR_ID}, 2);
    });
  });
}

void HdmiCecRmt::scan() {
  if (this->monitor_mode_ || this->scanning_ || !this->allocated_)
    return;
  this->scanning_ = true;
  this->query_pending_ = 0;
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
  this->bind_devices_();
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

  this->announce_new_devices_();
  this->scanning_ = false;
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
  for (auto *dev : this->devices_)
    any |= dev->needs_publish();
  if (any) {
    static const DeviceState NOT_FOUND{};
    for (auto *dev : this->devices_) {
      uint8_t la = dev->get_logical_address();
      const DeviceState &s = la < CEC_BROADCAST ? this->states_[la] : NOT_FOUND;
      if (s.dirty || dev->needs_publish())
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

uint8_t HdmiCecRmt::find_device(const std::string &name) const {
  std::string n = normalize_name(name);
  if (n.empty())
    return LA_NONE;
  if (n == "all" || n == "broadcast")
    return CEC_BROADCAST;
  for (auto *dev : this->devices_) {
    if (normalize_name(dev->get_name()) == n)
      return dev->get_logical_address();  // LA_NONE if not found on the bus yet
  }
  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    if (!this->states_[la].osd_name.empty() && normalize_name(this->states_[la].osd_name) == n)
      return la;
  }
  for (uint8_t la = 0; la < CEC_BROADCAST; la++) {
    if (normalize_name(default_la_name(la)) == n)
      return la;
  }
  int num = parse_number(n);
  return num >= 0 && num <= CEC_BROADCAST ? static_cast<uint8_t>(num) : LA_NONE;
}

bool HdmiCecRmt::command(const std::string &device, const std::string &command) {
  uint8_t la = this->find_device(device);
  if (la == LA_NONE) {
    ESP_LOGW(TAG, "Command '%s': device '%s' is unknown or not found on the bus yet", command.c_str(),
             device.c_str());
    return false;
  }
  std::string c = normalize_name(command);
  if (c == "on" || c == "wake" || c == "turn_on") {
    this->power_on(la);
  } else if (c == "off" || c == "standby" || c == "turn_off") {
    this->standby(la);
  } else if (c == "switch_to" || c == "active_source" || c == "source") {
    this->set_active_source(la);
  } else if (c == "power_status" || c == "refresh") {
    this->request_power_status(la);
  } else {
    int key = key_code(c);
    if (key < 0) {
      ESP_LOGW(TAG, "Unknown command '%s'", command.c_str());
      return false;
    }
    this->user_control(la, static_cast<uint8_t>(key));
  }
  ESP_LOGD(TAG, "Command '%s' -> %s (logical address %u)", c.c_str(), this->display_name(la).c_str(), la);
  return true;
}

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
  this->off_guard_until_ = millis();  // the user wants something on
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
  if (destination == CEC_BROADCAST && (int32_t) (millis() - this->off_guard_until_) >= 0) {
    this->off_guard_until_ = millis() + 20000;
    this->off_guard_count_ = 0;
  }
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
  // Passive: never ask every device for its state. After our own commands, only the device we
  // addressed is asked (schedule_power_check_), the rest follows from the bus.
  if (this->monitor_mode_ || !this->allocated_ || this->passive_)
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

void HdmiCecRmt::route_to(uint16_t physical_address, uint8_t la, bool standby_intruders) {
  this->route_pa_ = physical_address;
  this->route_intruders_ = 0;
  this->route_standby_intruders_ = standby_intruders;
  this->route_were_off_ = 0;
  for (uint8_t i = 1; i < CEC_BROADCAST; i++) {
    if (i != 5 && i != la && i != this->la_ && this->states_[i].present && this->states_[i].power != POWER_ON &&
        this->states_[i].power != POWER_TO_ON)
      this->route_were_off_ |= 1u << i;
  }
  this->cancel_timeout(ID_SETTLED_1);
  this->cancel_timeout(ID_SETTLED_2);
  this->route_guard_until_ = millis();  // a new switch ends the guard of the previous one
  this->route_guard_count_ = 0;
  this->proxy_pa_ = PA_UNKNOWN;
  this->off_guard_until_ = millis();
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
    if (!cec_source && physical_address != 0x0000) {
      // One Touch Play for an input without CEC: announce it now, so a TV that is starting up
      // knows where to go (else it goes back to the source it remembers).
      this->proxy_pa_ = physical_address;
      this->enqueue_frame_(this->la_, CEC_BROADCAST,
                           {OP_ACTIVE_SOURCE, static_cast<uint8_t>(physical_address >> 8),
                            static_cast<uint8_t>(physical_address & 0xFF)});
    }
    this->route_phase_ = ROUTE_WAIT_POWER;
    return;
  }
  this->announce_route_();
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
    this->announce_route_();
    this->route_phase_ = ROUTE_WAIT_CONFIRM;
    this->route_started_ = now;
    return;
  }
  // ROUTE_WAIT_CONFIRM
  bool cec_source = this->route_la_ != 0xFF && this->route_la_ != 0 && this->route_la_ < CEC_BROADCAST;
  if (cec_source && this->last_active_la_ == this->route_la_ &&
      (int32_t) (this->last_active_ms_ - this->route_started_) >= 0) {
    ESP_LOGI(TAG, "%s confirmed as active source", this->display_name(this->route_la_).c_str());
    this->route_finish_();
    return;
  }
  if (now - this->route_started_ < 3000)
    return;
  if (!cec_source) {
    // An input without CEC can't confirm anything. Say it once more: harmless if the switch
    // already happened, and it overrides a late start-up switch of the TV or receiver.
    ESP_LOGI(TAG, "Repeating the switch to %s", format_physical_address(this->route_pa_).c_str());
    this->announce_route_();
    this->route_finish_();
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
  this->route_finish_();
}

void HdmiCecRmt::route_intruder_(uint8_t src, uint16_t pa) {
  if (this->route_phase_ != ROUTE_IDLE) {
    ESP_LOGW(TAG, "%s claimed the source (%s) while switching to %s", this->display_name(src).c_str(),
             format_physical_address(pa).c_str(), format_physical_address(this->route_pa_).c_str());
    this->route_intruders_ |= 1u << src;
    return;
  }
  if (!this->route_standby_intruders_ || (int32_t) (millis() - this->route_guard_until_) >= 0)
    return;
  this->route_intruders_ |= 1u << src;
  if (this->route_guard_count_ >= 2 || millis() - this->route_guard_last_ms_ <= 1000)
    return;
  // Shortly after a switch: a device that was woken along with the TV announces itself late.
  // Take the input back now; the device itself is put to sleep once the TV has settled
  // (a TV that is still starting up wakes it again otherwise).
  ESP_LOGW(TAG, "%s claimed the source (%s) right after the switch to %s; routing again",
           this->display_name(src).c_str(), format_physical_address(pa).c_str(),
           format_physical_address(this->route_pa_).c_str());
  this->route_guard_count_++;
  this->route_guard_last_ms_ = millis();
  this->announce_route_();
}

void HdmiCecRmt::off_guard_check_(uint8_t src) {
  if (src == this->la_ || (int32_t) (millis() - this->off_guard_until_) >= 0 || this->off_guard_count_ >= 2 ||
      millis() - this->off_guard_last_ms_ < 3000)
    return;
  ESP_LOGW(TAG, "%s woke up right after everything was switched off; standby again", this->display_name(src).c_str());
  this->off_guard_count_++;
  this->off_guard_last_ms_ = millis();
  this->enqueue_frame_(this->la_, CEC_BROADCAST, {OP_STANDBY});
}

void HdmiCecRmt::route_finish_() {
  this->route_phase_ = ROUTE_IDLE;
  this->route_guard_until_ = millis() + 30000;
  if ((int32_t) (this->route_guard_until_ - this->suppress_active_request_until_) > 0)
    this->suppress_active_request_until_ = this->route_guard_until_;
  if (!this->route_standby_intruders_)
    return;
  // Devices woken along the way (typically by a TV that powers up its last source) are put back
  // in standby once the TV has finished starting up, and checked once more after that.
  this->set_timeout(ID_SETTLED_1, 30000, [this]() { this->route_settled_standby_(); });
  this->set_timeout(ID_SETTLED_2, 60000, [this]() { this->route_settled_standby_(); });
}

void HdmiCecRmt::route_settled_standby_() {
  if (this->active_pa_ != this->route_pa_) {
    ESP_LOGD(TAG, "Input changed since the switch; leaving other devices alone");
    return;
  }
  uint16_t candidates = this->route_intruders_ | this->route_were_off_;
  for (uint8_t i = 1; i < CEC_BROADCAST; i++) {
    if (!(candidates & (1u << i)) || i == this->route_la_ || i == 5 || i == this->la_)
      continue;
    const DeviceState &st = this->states_[i];
    if (st.present && (st.power == POWER_ON || st.power == POWER_TO_ON)) {
      ESP_LOGI(TAG, "%s woke up during the switch; putting it back in standby", this->display_name(i).c_str());
      this->standby(i);
    }
  }
}

void HdmiCecRmt::announce_route_() {
  bool cec_source = this->route_la_ != 0xFF && this->route_la_ != 0 && this->route_la_ < CEC_BROADCAST;
  if (!cec_source && this->route_pa_ != 0x0000) {
    // An input without CEC can't do One Touch Play itself, so we do it on its behalf: announce it
    // as the active source. Without an active source a TV that just woke up routes to the last
    // source it remembers (and wakes that device), or to whoever sent it <Image View On>.
    this->proxy_pa_ = this->route_pa_;
    this->enqueue_frame_(this->la_, CEC_BROADCAST,
                         {OP_ACTIVE_SOURCE, static_cast<uint8_t>(this->route_pa_ >> 8),
                          static_cast<uint8_t>(this->route_pa_ & 0xFF)});
  }
  this->set_stream_path(this->route_pa_);
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
