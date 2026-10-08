#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/climate/climate.h"
#include "../truma_inetx.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

/// Home Assistant climate entity built on iNet X parameters (by default RoomClimate.Mode / RoomClimate.TgtTemp).
/// Every mapping (HA mode <-> wire value) is configurable from YAML.
class TrumaInetXClimate : public climate::Climate, public Component, public Parented<TrumaInetX> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_temperature_multiplier(float multiplier) { this->multiplier_ = multiplier; }
  void set_mode_parameter(const std::string &topic, const std::string &param) { this->mode_param_ = {topic, param}; }
  void add_mode(climate::ClimateMode mode, int64_t value) { this->mode_values_.emplace_back(mode, value); }
  void set_target_parameter(const std::string &topic, const std::string &param) { this->target_param_ = {topic, param}; }
  void set_current_parameter(const std::string &topic, const std::string &param) { this->current_param_ = {topic, param}; }
  void set_fan_mode_parameter(const std::string &topic, const std::string &param) { this->fan_param_ = {topic, param}; }
  void add_fan_mode(climate::ClimateFanMode mode, int64_t value) { this->fan_values_.emplace_back(mode, value); }
  /// Custom fan mode shown with its own label in Home Assistant (label must have static storage).
  void add_custom_fan_mode(const char *label, int64_t value) { this->custom_fan_values_.emplace_back(label, value); }
  void set_preset_parameter(const std::string &topic, const std::string &param) { this->preset_param_ = {topic, param}; }
  void add_preset(climate::ClimatePreset preset, int64_t value) { this->preset_values_.emplace_back(preset, value); }
  void set_action_parameter(const std::string &topic, const std::string &param) { this->action_param_ = {topic, param}; }
  void add_action(climate::ClimateAction action, int64_t value) { this->action_values_.emplace_back(action, value); }

 protected:
  struct Param {
    std::string topic;
    std::string param;
    bool configured() const { return !this->topic.empty(); }
  };

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  void schedule_publish_();

  float multiplier_{0.1f};
  Param mode_param_;
  Param target_param_;
  Param current_param_;
  Param fan_param_;
  Param preset_param_;
  Param action_param_;
  std::vector<std::pair<climate::ClimateMode, int64_t>> mode_values_;
  std::vector<std::pair<climate::ClimateFanMode, int64_t>> fan_values_;
  std::vector<std::pair<const char *, int64_t>> custom_fan_values_;
  std::vector<std::pair<climate::ClimatePreset, int64_t>> preset_values_;
  std::vector<std::pair<climate::ClimateAction, int64_t>> action_values_;
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
