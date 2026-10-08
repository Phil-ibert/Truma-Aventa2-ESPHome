#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "../truma_inetx.h"

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

/// Connection status (no topic) or "value != 0" of a Topic.Parameter.
class TrumaInetXBinarySensor : public binary_sensor::BinarySensor, public Component, public Parented<TrumaInetX> {
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
