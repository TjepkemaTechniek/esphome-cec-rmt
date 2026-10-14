#pragma once

#ifdef USE_ESP32

#include <vector>

#include "esphome/core/automation.h"
#include "hdmi_cec_rmt.h"

namespace esphome {
namespace hdmi_cec_rmt {

template<typename... Ts> class SendAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint8_t, destination)
  TEMPLATABLE_VALUE(uint8_t, source)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, data)
  void set_has_source(bool has_source) { this->has_source_ = has_source; }

  void play(const Ts &...x) override {
    int source = this->has_source_ ? static_cast<int>(this->source_.value(x...)) : -1;
    this->parent_->send(this->destination_.value(x...), this->data_.value(x...), source);
  }

 protected:
  bool has_source_{false};
};

template<typename... Ts> class PowerOnAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint8_t, destination)
  void play(const Ts &...x) override { this->parent_->power_on(this->destination_.value(x...)); }
};

template<typename... Ts> class StandbyAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint8_t, destination)
  void play(const Ts &...x) override { this->parent_->standby(this->destination_.value(x...)); }
};

template<typename... Ts> class SetActiveSourceAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint8_t, destination)
  void play(const Ts &...x) override { this->parent_->set_active_source(this->destination_.value(x...)); }
};

template<typename... Ts> class UserControlAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint8_t, destination)
  TEMPLATABLE_VALUE(uint8_t, key)
  void play(const Ts &...x) override {
    this->parent_->user_control(this->destination_.value(x...), this->key_.value(x...));
  }
};

template<typename... Ts> class RequestPowerStatusAction : public Action<Ts...>, public Parented<HdmiCecRmt> {
 public:
  TEMPLATABLE_VALUE(uint8_t, destination)
  void play(const Ts &...x) override { this->parent_->request_power_status(this->destination_.value(x...)); }
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
