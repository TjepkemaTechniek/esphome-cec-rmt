#pragma once

#ifdef USE_ESP32

#include "esphome/core/defines.h"
#include "hdmi_cec_rmt.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif

namespace esphome {
namespace hdmi_cec_rmt {

#ifdef USE_SWITCH
/// ON = power on (Image View On for the TV, <Power On Function> key for others), OFF = <Standby>.
/// State follows <Report Power Status>.
class CecPowerSwitch : public switch_::Switch {
 public:
  void set_device(CecDevice *device) {
    this->device_ = device;
    device->set_power_switch(this);
  }
  void update_from_power(uint8_t power) {
    if (power == POWER_ON || power == POWER_TO_ON) {
      if (!this->has_published_ || !this->state) {
        this->has_published_ = true;
        this->publish_state(true);
      }
    } else if (power == POWER_STANDBY || power == POWER_TO_STANDBY) {
      if (!this->has_published_ || this->state) {
        this->has_published_ = true;
        this->publish_state(false);
      }
    }
  }

 protected:
  void write_state(bool state) override {
    auto *parent = this->device_->get_parent();
    if (state) {
      parent->power_on(this->device_->get_logical_address());
    } else {
      parent->standby(this->device_->get_logical_address());
    }
    this->has_published_ = true;
    this->publish_state(state);  // optimistic; corrected by the power status checks that follow
  }

  CecDevice *device_{nullptr};
  bool has_published_{false};
};
#endif

#ifdef USE_SELECT
/// Source select. Each option is either a CEC device (address learned from the bus) or a
/// fixed HDMI input by physical address (works for sources without CEC, e.g. a game PC on
/// a receiver input). Selecting one routes the bus there with <Set Stream Path>.
class CecSourceSelect : public select::Select, public Parented<HdmiCecRmt> {
 public:
  struct Entry {
    std::string name;
    CecDevice *device{nullptr};
    uint16_t physical_address{PA_UNKNOWN};
  };

  void add_device(CecDevice *device) { this->entries_.push_back({device->get_name(), device, PA_UNKNOWN}); }
  void add_input(const char *name, uint16_t physical_address, CecDevice *device) {
    this->entries_.push_back({name, device, physical_address});
  }
  void set_standby_previous_source(bool standby) { this->standby_previous_ = standby; }

  /// Physical address of an entry: fixed, or learned from the bus for CEC devices.
  uint16_t entry_address(size_t i) const {
    const Entry &e = this->entries_[i];
    if (e.physical_address != PA_UNKNOWN)
      return e.physical_address;
    if (e.device == nullptr)
      return PA_UNKNOWN;
    uint8_t la = e.device->get_logical_address();
    return la == 0 ? 0x0000 : this->parent_->get_state(la).physical_address;
  }
  /// Name of the entry routed to this address, or nullptr.
  const char *name_for_address(uint16_t pa) const {
    for (size_t i = 0; i < this->entries_.size(); i++) {
      if (this->entry_address(i) == pa)
        return this->entries_[i].name.c_str();
    }
    return nullptr;
  }

  void update_active(uint16_t pa) {
    if (pa == PA_UNKNOWN)
      return;
    for (size_t i = 0; i < this->entries_.size(); i++) {
      if (this->entry_address(i) == pa) {
        if (this->last_index_ != (int) i) {
          this->last_index_ = (int) i;
          this->publish_state(i);
        }
        return;
      }
    }
  }

 protected:
  void control(size_t index) override {
    if (index >= this->entries_.size())
      return;
    const Entry &e = this->entries_[index];
    uint16_t pa = this->entry_address(index);
    uint8_t target_la = e.device != nullptr ? e.device->get_logical_address() : 0xFF;
    if (pa == PA_UNKNOWN) {
      if (target_la != 0xFF)
        this->parent_->set_active_source(target_la);  // asks the device for its address first
      return;
    }
    // Put the current source to sleep, so it can't grab the input back when the bus asks.
    // If we don't know which device is active (e.g. right after boot), put every powered-on
    // source device to sleep except the target.
    if (this->standby_previous_) {
      uint8_t prev = this->parent_->active_source_logical_address();
      if (prev != 0xFF) {
        if (prev != target_la && is_source_address(prev))
          this->parent_->standby(prev);
      } else {
        for (uint8_t la = 1; la < 15; la++) {
          const DeviceState &st = this->parent_->get_state(la);
          if (la != target_la && la != this->parent_->get_logical_address() && is_source_address(la) &&
              st.present && (st.power == POWER_ON || st.power == POWER_TO_ON))
            this->parent_->standby(la);
        }
      }
    }
    // Wake (if needed) -> route -> confirm; see HdmiCecRmt::route_to().
    this->parent_->route_to(pa, target_la, this->standby_previous_);
    // Not optimistic: the state follows once the switch is reflected on the bus / in our tracking.
  }

  static bool is_source_address(uint8_t la) {
    // Recorders, tuners and playback devices. Never the TV (0) or the audio system (5).
    switch (la) {
      case 1: case 2: case 3: case 4: case 6: case 7: case 8: case 9: case 10: case 11:
        return true;
      default:
        return false;
    }
  }

  std::vector<Entry> entries_;
  bool standby_previous_{false};
  int last_index_{-1};
};
#endif

}  // namespace hdmi_cec_rmt
}  // namespace esphome

#endif  // USE_ESP32
