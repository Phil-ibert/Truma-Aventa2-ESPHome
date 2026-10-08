#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/select/select.h"
#include "../truma_inetx.h"

#include <vector>

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

/// Enumerated Topic.Parameter (e.g. AirCooling.Mode: 0 = comfort, 1 = fast).
class TrumaInetXSelect : public select::Select, public Component, public Parented<TrumaInetX> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_topic(const std::string &topic) { this->topic_ = topic; }
  void set_parameter(const std::string &parameter) { this->parameter_ = parameter; }
  /// Wire values, in the same order as the options.
  void add_value(int64_t value) { this->values_.push_back(value); }

 protected:
  void control(size_t index) override;

  std::string topic_;
  std::string parameter_;
  std::vector<int64_t> values_;
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
