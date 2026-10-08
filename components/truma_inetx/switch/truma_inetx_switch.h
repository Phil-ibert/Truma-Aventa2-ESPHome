#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/switch/switch.h"
#include "../truma_inetx.h"

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

/// On/off Topic.Parameter (e.g. AirCooling.Active).
class TrumaInetXSwitch : public switch_::Switch, public Component, public Parented<TrumaInetX> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_topic(const std::string &topic) { this->topic_ = topic; }
  void set_parameter(const std::string &parameter) { this->parameter_ = parameter; }
  void set_values(int64_t on_value, int64_t off_value) {
    this->on_value_ = on_value;
    this->off_value_ = off_value;
  }

 protected:
  void write_state(bool state) override;

  std::string topic_;
  std::string parameter_;
  int64_t on_value_{1};
  int64_t off_value_{0};
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
