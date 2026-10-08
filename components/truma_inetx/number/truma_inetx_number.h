#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/number/number.h"
#include "../truma_inetx.h"

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

/// Writable numeric Topic.Parameter (e.g. AirCirculation.FanLevel).
class TrumaInetXNumber : public number::Number, public Component, public Parented<TrumaInetX> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_topic(const std::string &topic) { this->topic_ = topic; }
  void set_parameter(const std::string &parameter) { this->parameter_ = parameter; }
  void set_multiplier(float multiplier) { this->multiplier_ = multiplier; }

 protected:
  void control(float value) override;

  std::string topic_;
  std::string parameter_;
  float multiplier_{1.0f};
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
