#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/optional.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "../truma_inetx.h"

#include <string>

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

/// Light on an iNet X on/off parameter (AmbientLight.Active), with optional brightness
/// (AmbientLight.LightStep). Changes made with the remote are reflected in Home Assistant.
class TrumaInetXLight : public light::LightOutput, public Component, public Parented<TrumaInetX> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_active_parameter(const std::string &topic, const std::string &param, int64_t on_value,
                            int64_t off_value) {
    this->active_topic_ = topic;
    this->active_param_ = param;
    this->on_value_ = on_value;
    this->off_value_ = off_value;
  }
  void set_brightness_parameter(const std::string &topic, const std::string &param, int64_t min_value,
                                int64_t max_value) {
    this->level_topic_ = topic;
    this->level_param_ = param;
    this->min_level_ = min_value;
    this->max_level_ = max_value;
  }

  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override { this->state_ = state; }
  void write_state(light::LightState *state) override;

 protected:
  bool has_brightness_() const { return !this->level_topic_.empty(); }
  int64_t to_level_(float brightness) const;
  float to_brightness_(int64_t level) const;
  void on_active_(const cbor::Value &value);
  void on_level_(const cbor::Value &value);
  void apply_(light::LightCall &call);
  void show_device_state_();

  light::LightState *state_{nullptr};
  std::string active_topic_;
  std::string active_param_;
  int64_t on_value_{1};
  int64_t off_value_{0};
  std::string level_topic_;
  std::string level_param_;
  int64_t min_level_{1};
  int64_t max_level_{100};

  // What the device is known (or was just told) to have. Writes are only sent when Home
  // Assistant asks for something different, so values coming from the remote are never echoed.
  optional<bool> device_on_;
  optional<int64_t> device_level_;
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
