#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "../truma_inetx.h"

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

/// Session state (no topic) or raw value of a Topic.Parameter.
class TrumaInetXTextSensor : public text_sensor::TextSensor, public Component, public Parented<TrumaInetX> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_source(const std::string &topic, const std::string &parameter) {
    this->topic_ = topic;
    this->parameter_ = parameter;
  }

 protected:
  std::string topic_;
  std::string parameter_;
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
