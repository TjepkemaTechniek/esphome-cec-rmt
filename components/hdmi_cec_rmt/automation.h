#pragma once

#ifdef USE_ESP32

#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/log.h"
#include "hdmi_cec_rmt.h"

namespace esphome {
namespace hdmi_cec_rmt {

/// Destination given as a logical address, or as a device from `devices:` (resolved when the
/// action runs, so auto-detected devices work wherever they are on the bus).
template<typename... Ts> class DestinationAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint8_t, destination)
  void set_device(CecDevice *device) { this->device_ = device; }

 protected:
  /// LA_NONE (and a warning) if the device has not been seen on the bus yet.
  uint8_t resolve_(const Ts &...x) {
    if (this->device_ == nullptr)
      return this->destination_.value(x...);
    uint8_t la = this->device_->get_logical_address();
    if (la == LA_NONE)
      ESP_LOGW("hdmi_cec_rmt", "'%s' has not been seen on the bus yet", this->device_->get_name().c_str());
    return la;
  }
  CecDevice *device_{nullptr};
};

template<typename... Ts> class SendAction : public DestinationAction<Ts...> {
 public:
  TEMPLATABLE_VALUE(uint8_t, source)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)
  void set_has_source(bool has_source) { this->has_source_ = has_source; }

  void play(const Ts &...x) override {
    uint8_t dest = this->resolve_(x...);
    if (dest == LA_NONE)
      return;
    int source = this->has_source_ ? static_cast<int>(this->source_.value(x...)) : -1;
    this->parent_->send(dest, this->data_.value(x...), source);
  }

 protected:
  bool has_source_{false};
};

template<typename... Ts> class PowerOnAction : public DestinationAction<Ts...> {
 public:
  void play(const Ts &...x) override {
    uint8_t dest = this->resolve_(x...);
    if (dest != LA_NONE)
      this->parent_->power_on(dest);
  }
};

template<typename... Ts> class StandbyAction : public DestinationAction<Ts...> {
 public:
  void play(const Ts &...x) override {
    uint8_t dest = this->resolve_(x...);
    if (dest != LA_NONE)
      this->parent_->standby(dest);
  }
};

template<typename... Ts> class SetActiveSourceAction : public DestinationAction<Ts...> {
 public:
  void play(const Ts &...x) override {
    uint8_t dest = this->resolve_(x...);
    if (dest != LA_NONE)
      this->parent_->set_active_source(dest);
  }
};

template<typename... Ts> class UserControlAction : public DestinationAction<Ts...> {
 public:
  TEMPLATABLE_VALUE(uint8_t, key)
  void play(const Ts &...x) override {
    uint8_t dest = this->resolve_(x...);
    if (dest != LA_NONE)
      this->parent_->user_control(dest, this->key_.value(x...));
  }
};

/// Command by name to a device by name; see HdmiCecRmt::command(). Meant for Home Assistant
/// actions: `device: "Apple TV"`, `command: "play"`.
template<typename... Ts> class CommandAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(std::string, device)
  TEMPLATABLE_VALUE(std::string, command)
  void play(const Ts &...x) override { this->parent_->command(this->device_.value(x...), this->command_.value(x...)); }
};

template<typename... Ts> class RequestPowerStatusAction : public DestinationAction<Ts...> {
 public:
  void play(const Ts &...x) override {
    uint8_t dest = this->resolve_(x...);
    if (dest != LA_NONE)
      this->parent_->request_power_status(dest);
  }
};

template<typename... Ts> class SetStreamPathAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint16_t, physical_address)
  void play(const Ts &...x) override { this->parent_->set_stream_path(this->physical_address_.value(x...)); }
};

template<typename... Ts> class DumpHistoryAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  void play(const Ts &...x) override { this->parent_->dump_history(); }
};

template<typename... Ts> class ScanAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  void play(const Ts &...x) override { this->parent_->scan(); }
};

}  // namespace hdmi_cec_rmt
}  // namespace esphome

#endif  // USE_ESP32
