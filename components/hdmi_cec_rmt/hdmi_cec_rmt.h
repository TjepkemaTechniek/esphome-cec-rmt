#pragma once

#ifdef USE_ESP32

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "cec_bus.h"

namespace esphome {
namespace hdmi_cec_rmt {

class HdmiCecRmt;
class CecPowerSwitch;
class CecSourceSelect;

static constexpr uint8_t POWER_ON = 0x00;
static constexpr uint8_t POWER_STANDBY = 0x01;
static constexpr uint8_t POWER_TO_ON = 0x02;
static constexpr uint8_t POWER_TO_STANDBY = 0x03;
static constexpr uint8_t POWER_UNKNOWN = 0xFF;
static constexpr uint16_t PA_UNKNOWN = 0xFFFF;

enum class SourceSwitchMethod : uint8_t { SET_STREAM_PATH, ACTIVE_SOURCE, BOTH };

/// What we know about one logical address on the bus.
struct DeviceState {
  bool present{false};
  uint16_t physical_address{PA_UNKNOWN};
  uint8_t device_type{0xFF};
  std::string osd_name;
  bool vendor_known{false};
  uint32_t vendor_id{0};
  uint8_t power{POWER_UNKNOWN};
  bool announced{false};
  bool dirty{false};
};

std::string format_physical_address(uint16_t pa);
const char *vendor_name(uint32_t vendor_id);
const char *opcode_name(uint8_t opcode);

/// A device declared in YAML (`devices:`); entities attach to it.
class CecDevice {
 public:
  CecDevice(uint8_t logical_address, const char *name) : logical_address_(logical_address), name_(name) {}

  uint8_t get_logical_address() const { return this->logical_address_; }
  const std::string &get_name() const { return this->name_; }
  void set_parent(HdmiCecRmt *parent) { this->parent_ = parent; }
  HdmiCecRmt *get_parent() const { return this->parent_; }

#ifdef USE_BINARY_SENSOR
  void set_present_binary_sensor(binary_sensor::BinarySensor *s) { this->present_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_osd_name_text_sensor(text_sensor::TextSensor *s) { this->osd_name_sensor_ = s; }
  void set_physical_address_text_sensor(text_sensor::TextSensor *s) { this->pa_sensor_ = s; }
  void set_vendor_text_sensor(text_sensor::TextSensor *s) { this->vendor_sensor_ = s; }
#endif
  void set_power_switch(CecPowerSwitch *s) { this->power_switch_ = s; }

  void publish(const DeviceState &state);

 protected:
  uint8_t logical_address_;
  std::string name_;
  HdmiCecRmt *parent_{nullptr};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *present_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *osd_name_sensor_{nullptr};
  text_sensor::TextSensor *pa_sensor_{nullptr};
  text_sensor::TextSensor *vendor_sensor_{nullptr};
  std::string last_osd_name_;
  std::string last_pa_;
  std::string last_vendor_;
#endif
  CecPowerSwitch *power_switch_{nullptr};
};

class MessageTrigger : public Trigger<uint8_t, uint8_t, std::vector<uint8_t>> {
 public:
  explicit MessageTrigger(HdmiCecRmt *parent);
  void set_source(uint8_t source) { this->source_ = source; }
  void set_destination(uint8_t destination) { this->destination_ = destination; }
  void set_opcode(uint8_t opcode) { this->opcode_ = opcode; }
  void set_data(const std::vector<uint8_t> &data) { this->data_ = data; }
  /// `data` = everything after the header byte (opcode first); empty for a poll.
  void process(uint8_t source, uint8_t destination, const std::vector<uint8_t> &data);

 protected:
  optional<uint8_t> source_;
  optional<uint8_t> destination_;
  optional<uint8_t> opcode_;
  optional<std::vector<uint8_t>> data_;
};

class DeviceDiscoveredTrigger : public Trigger<uint8_t, uint16_t, std::string> {
 public:
  explicit DeviceDiscoveredTrigger(HdmiCecRmt *parent);
};

class HdmiCecRmt : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- configuration ----
  void set_pin(InternalGPIOPin *pin) { this->pin_ = pin; }
  void set_logical_address(uint8_t la) { this->configured_la_ = la; }
  void set_physical_address(uint16_t pa) { this->physical_address_ = pa; }
  void set_device_type(uint8_t type) { this->device_type_ = type; }
  void set_osd_name(const std::string &name) { this->osd_name_ = name; }
  void set_vendor_id(uint32_t vendor_id) { this->vendor_id_ = vendor_id; }
  void set_promiscuous(bool promiscuous) { this->promiscuous_ = promiscuous; }
  void set_monitor_mode(bool monitor) { this->monitor_mode_ = monitor; }
  void set_scan_interval(uint32_t ms) { this->scan_interval_ = ms; }
  void set_power_poll_interval(uint32_t ms) { this->power_poll_interval_ = ms; }
  void set_stats_interval(uint32_t ms) { this->stats_interval_ = ms; }
  void set_source_switch_method(SourceSwitchMethod method) { this->source_switch_method_ = method; }
  void add_device(CecDevice *device);
  void add_message_trigger(MessageTrigger *trigger) { this->message_triggers_.push_back(trigger); }
  void add_discovered_trigger(DeviceDiscoveredTrigger *trigger) { this->discovered_triggers_.push_back(trigger); }
  void set_source_select(CecSourceSelect *select) { this->source_select_ = select; }

#ifdef USE_TEXT_SENSOR
  void set_active_source_text_sensor(text_sensor::TextSensor *s) { this->active_source_sensor_ = s; }
  void set_last_message_text_sensor(text_sensor::TextSensor *s) { this->last_message_sensor_ = s; }
#endif
#ifdef USE_SENSOR
  void set_frames_received_sensor(sensor::Sensor *s) { this->frames_received_sensor_ = s; }
  void set_frames_sent_sensor(sensor::Sensor *s) { this->frames_sent_sensor_ = s; }
  void set_tx_nack_sensor(sensor::Sensor *s) { this->tx_nack_sensor_ = s; }
  void set_tx_arbitration_lost_sensor(sensor::Sensor *s) { this->tx_arbitration_lost_sensor_ = s; }
  void set_tx_errors_sensor(sensor::Sensor *s) { this->tx_errors_sensor_ = s; }
  void set_rx_errors_sensor(sensor::Sensor *s) { this->rx_errors_sensor_ = s; }
  void set_acks_sent_sensor(sensor::Sensor *s) { this->acks_sent_sensor_ = s; }
  void set_rx_ack_missed_sensor(sensor::Sensor *s) { this->rx_ack_missed_sensor_ = s; }
  void set_watchdog_sensor(sensor::Sensor *s) { this->watchdog_sensor_ = s; }
#endif

  // ---- actions ----
  /// Queue a raw frame. `data` is opcode + operands (empty = poll). source < 0 = our own address.
  void send(uint8_t destination, const std::vector<uint8_t> &data, int source = -1);
  void power_on(uint8_t destination);
  void standby(uint8_t destination);
  void set_active_source(uint8_t destination);
  void user_control(uint8_t destination, uint8_t key);
  void request_power_status(uint8_t destination);
  /// Broadcast <Set Stream Path>: asks switches (receivers) and devices to route to this address.
  void set_stream_path(uint16_t physical_address);
  /// Switch to an input: wakes the CEC device on it first (if given and asleep), waits until it
  /// reports power on, then routes with <Set Stream Path> and re-sends once if the device does
  /// not announce itself as active source. la = 0xFF for inputs without a CEC device.
  /// Switch the TV/receiver to an input. With standby_intruders, a source device that
  /// claims the input while we switch (e.g. one the TV wakes along with itself) is put back in standby.
  void route_to(uint16_t physical_address, uint8_t la, bool standby_intruders = false);
  /// Print the last frames (RX and TX results) with timestamps to the log.
  void dump_history();
  void scan();

  // ---- state ----
  uint8_t get_logical_address() const { return this->la_; }
  const DeviceState &get_state(uint8_t la) const { return this->states_[la & 0x0F]; }
  uint16_t get_active_physical_address() const { return this->active_pa_; }
  /// Logical address of the present device at the active physical address, or 0xFF.
  uint8_t active_source_logical_address() const;
  /// Display name for a logical address: YAML name, else OSD name, else "LA n".
  std::string display_name(uint8_t la) const;

 protected:
  struct TxRequest {
    CecFrame frame{};
    uint8_t attempts{5};
    std::function<void(bool)> done{nullptr};
    std::function<void()> marker{nullptr};  // set -> no frame, just a callback when reached
  };

  void enqueue_(TxRequest &&req, bool front = false);
  void enqueue_frame_(uint8_t source, uint8_t destination, std::initializer_list<uint8_t> data, uint8_t attempts = 5,
                      std::function<void(bool)> done = nullptr, bool front = false);
  void enqueue_marker_(std::function<void()> &&marker);
  void process_tx_();
  void on_tx_done_(TxResult result);

  void handle_frame_(const CecFrame &frame);
  void update_state_(const CecFrame &frame);
  void respond_(const CecFrame &frame);
  void fire_triggers_(const CecFrame &frame);
  void mark_seen_(uint8_t la);
  void mark_absent_(uint8_t la);
  void query_device_(uint8_t la);
  void finish_scan_();
  void set_active_pa_(uint16_t pa);
  void publish_states_();
  void publish_stats_();
  void schedule_power_check_(uint8_t la);
  void schedule_state_refresh_(bool ours = false);
  void refresh_state_();
  void allocate_logical_address_();
  void report_physical_address_();
  void do_set_active_source_(uint8_t la, uint16_t pa);
  void note_own_switch_(uint16_t pa);
  void route_tick_();
  void record_(const CecFrame &frame, uint8_t kind);

  // History of recent frames, for dump_history()
  struct HistoryEntry {
    uint32_t ms;
    CecFrame frame;
    uint8_t kind;  // 0 = RX, 1 = TX ok, 2 = TX NACK, 3 = TX error, 4 = TX lost arbitration
  };
  static constexpr uint8_t HISTORY_SIZE = 96;
  HistoryEntry history_[HISTORY_SIZE];
  uint8_t history_head_{0};
  uint8_t history_count_{0};

  // Input switch in progress (route_to)
  enum RoutePhase : uint8_t { ROUTE_IDLE, ROUTE_WAIT_POWER, ROUTE_WAIT_CONFIRM };
  RoutePhase route_phase_{ROUTE_IDLE};
  uint16_t route_pa_{PA_UNKNOWN};
  uint8_t route_la_{0xFF};
  uint32_t route_started_{0};
  uint32_t route_last_poll_{0};
  bool route_resent_{false};
  uint16_t route_wait_mask_{0};
  uint16_t route_intruders_{0};   // source devices that claimed the input during the switch
  bool route_standby_intruders_{false};
  uint32_t route_guard_until_{0};  // after a switch: sources that grab the input are sent back to standby
  uint32_t route_guard_last_ms_{0};
  void route_intruder_(uint8_t src, uint16_t pa);
  void route_finish_();  // devices that must be awake before we route (bit = logical address)
  uint32_t route_awake_ms_{0};   // when they all were
  uint32_t power_on_sent_ms_[16]{};  // last power-on command per logical address
  bool route_needs_wait_(uint8_t la) const;
  uint8_t last_active_la_{0xFF};
  uint32_t last_active_ms_{0};
  std::string frame_to_string_(const CecFrame &frame) const;

  InternalGPIOPin *pin_{nullptr};
  CecBus bus_;
  HighFrequencyLoopRequester high_freq_;

  uint8_t configured_la_{14};
  uint8_t la_{CEC_BROADCAST};
  uint16_t physical_address_{0x1000};
  uint8_t device_type_{0xFF};
  std::string osd_name_{"ESPHome"};
  uint32_t vendor_id_{0};
  bool promiscuous_{true};
  bool monitor_mode_{false};
  uint32_t scan_interval_{300000};
  uint32_t power_poll_interval_{60000};
  uint32_t stats_interval_{60000};
  SourceSwitchMethod source_switch_method_{SourceSwitchMethod::SET_STREAM_PATH};

  std::vector<CecDevice *> devices_;
  std::vector<MessageTrigger *> message_triggers_;
  std::vector<DeviceDiscoveredTrigger *> discovered_triggers_;
  CecSourceSelect *source_select_{nullptr};

  DeviceState states_[16];
  uint16_t active_pa_{PA_UNKNOWN};
  bool active_dirty_{true};
  bool scanning_{false};
  bool allocated_{false};

  std::deque<TxRequest> tx_queue_;
  bool tx_in_flight_{false};
  TxRequest current_{};
  uint8_t next_signal_free_bits_{5};
  uint32_t busy_since_{0};
  uint16_t query_pending_{0};
  uint32_t last_refresh_ms_{0};
  uint32_t suppress_active_request_until_{0};
  bool busy_warned_{false};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *active_source_sensor_{nullptr};
  text_sensor::TextSensor *last_message_sensor_{nullptr};
  std::string last_active_text_;
#endif
#ifdef USE_SENSOR
  sensor::Sensor *frames_received_sensor_{nullptr};
  sensor::Sensor *frames_sent_sensor_{nullptr};
  sensor::Sensor *tx_nack_sensor_{nullptr};
  sensor::Sensor *tx_arbitration_lost_sensor_{nullptr};
  sensor::Sensor *tx_errors_sensor_{nullptr};
  sensor::Sensor *rx_errors_sensor_{nullptr};
  sensor::Sensor *acks_sent_sensor_{nullptr};
  sensor::Sensor *rx_ack_missed_sensor_{nullptr};
  sensor::Sensor *watchdog_sensor_{nullptr};
#endif
};

}  // namespace hdmi_cec_rmt
}  // namespace esphome

#endif  // USE_ESP32
