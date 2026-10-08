#include "truma_inetx_climate.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.climate";

void TrumaInetXClimate::setup() {
  this->mode = climate::CLIMATE_MODE_OFF;
  auto *hub = this->parent_;

  hub->register_listener(this->mode_param_.topic, this->mode_param_.param, [this](const cbor::Value &value) {
    int64_t wire = value.as_int();
    for (const auto &m : this->mode_values_) {
      if (m.second == wire) {
        this->mode = m.first;
        this->schedule_publish_();
        return;
      }
    }
    ESP_LOGW(TAG, "%s.%s = %lld has no Home Assistant mode: add it under mode_parameter.values",
             this->mode_param_.topic.c_str(), this->mode_param_.param.c_str(), (long long) wire);
  });

  hub->register_listener(this->target_param_.topic, this->target_param_.param, [this](const cbor::Value &value) {
    if (!value.is_number())
      return;
    this->target_temperature = static_cast<float>(value.as_double()) * this->multiplier_;
    this->schedule_publish_();
  });

  if (this->current_param_.configured()) {
    hub->register_listener(this->current_param_.topic, this->current_param_.param, [this](const cbor::Value &value) {
      if (!value.is_number())
        return;
      this->current_temperature = static_cast<float>(value.as_double()) * this->multiplier_;
      this->schedule_publish_();
    });
  }

  if (this->fan_param_.configured()) {
    hub->register_listener(this->fan_param_.topic, this->fan_param_.param, [this](const cbor::Value &value) {
      int64_t wire = value.as_int();
      for (const auto &f : this->fan_values_) {
        if (f.second == wire) {
          this->fan_mode = f.first;
          this->schedule_publish_();
          return;
        }
      }
      ESP_LOGW(TAG, "%s.%s = %lld has no fan mode: add it under fan_mode_parameter.values",
               this->fan_param_.topic.c_str(), this->fan_param_.param.c_str(), (long long) wire);
    });
  }

  if (this->preset_param_.configured()) {
    hub->register_listener(this->preset_param_.topic, this->preset_param_.param, [this](const cbor::Value &value) {
      int64_t wire = value.as_int();
      for (const auto &p : this->preset_values_) {
        if (p.second == wire) {
          this->preset = p.first;
          this->schedule_publish_();
          return;
        }
      }
      ESP_LOGW(TAG, "%s.%s = %lld has no preset: add it under preset_parameter.values",
               this->preset_param_.topic.c_str(), this->preset_param_.param.c_str(), (long long) wire);
    });
  }

  if (this->action_param_.configured()) {
    hub->register_listener(this->action_param_.topic, this->action_param_.param, [this](const cbor::Value &value) {
      int64_t wire = value.as_int();
      for (const auto &a : this->action_values_) {
        if (a.second == wire) {
          this->action = a.first;
          this->schedule_publish_();
          return;
        }
      }
      ESP_LOGD(TAG, "%s.%s = %lld has no action mapping", this->action_param_.topic.c_str(),
               this->action_param_.param.c_str(), (long long) wire);
    });
  }

  hub->add_on_state_callback([this](SessionState state) {
    if (state == SessionState::DISCONNECTED) {
      this->current_temperature = NAN;
      this->schedule_publish_();
    }
  });
}

void TrumaInetXClimate::schedule_publish_() {
  // coalesce several parameter updates received together into one state publish
  this->defer("publish", [this]() { this->publish_state(); });
}

climate::ClimateTraits TrumaInetXClimate::traits() {
  auto traits = climate::ClimateTraits();
  if (this->current_param_.configured())
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  if (this->action_param_.configured())
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION);
  for (const auto &m : this->mode_values_)
    traits.add_supported_mode(m.first);
  for (const auto &f : this->fan_values_)
    traits.add_supported_fan_mode(f.first);
  for (const auto &p : this->preset_values_)
    traits.add_supported_preset(p.first);
  // Defaults matching the iNet X RoomClimate range; override with `visual:` in YAML.
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void TrumaInetXClimate::control(const climate::ClimateCall &call) {
  auto *hub = this->parent_;

  if (call.get_mode().has_value()) {
    climate::ClimateMode mode = *call.get_mode();
    bool found = false;
    for (const auto &m : this->mode_values_) {
      if (m.first == mode) {
        hub->write_int(this->mode_param_.topic, this->mode_param_.param, m.second);
        found = true;
        break;
      }
    }
    if (!found)
      ESP_LOGW(TAG, "Mode %s is not mapped", LOG_STR_ARG(climate::climate_mode_to_string(mode)));
  }

  if (call.get_target_temperature().has_value()) {
    float target = *call.get_target_temperature();
    int64_t wire = static_cast<int64_t>(std::llround(target / this->multiplier_));
    hub->write_int(this->target_param_.topic, this->target_param_.param, wire);
  }

  if (call.get_fan_mode().has_value() && this->fan_param_.configured()) {
    climate::ClimateFanMode fan = *call.get_fan_mode();
    for (const auto &f : this->fan_values_) {
      if (f.first == fan) {
        hub->write_int(this->fan_param_.topic, this->fan_param_.param, f.second);
        break;
      }
    }
  }

  if (call.get_preset().has_value() && this->preset_param_.configured()) {
    climate::ClimatePreset preset = *call.get_preset();
    for (const auto &p : this->preset_values_) {
      if (p.first == preset) {
        hub->write_int(this->preset_param_.topic, this->preset_param_.param, p.second);
        break;
      }
    }
  }
  // State is published when the device (or the optimistic echo) reports the new values.
}

void TrumaInetXClimate::dump_config() {
  LOG_CLIMATE("", "Truma iNet X Climate", this);
  ESP_LOGCONFIG(TAG, "  Mode: %s.%s (%u modes)", this->mode_param_.topic.c_str(), this->mode_param_.param.c_str(),
                (unsigned) this->mode_values_.size());
  ESP_LOGCONFIG(TAG, "  Target temperature: %s.%s (x%g)", this->target_param_.topic.c_str(),
                this->target_param_.param.c_str(), this->multiplier_);
  if (this->current_param_.configured())
    ESP_LOGCONFIG(TAG, "  Current temperature: %s.%s", this->current_param_.topic.c_str(),
                  this->current_param_.param.c_str());
  if (this->fan_param_.configured())
    ESP_LOGCONFIG(TAG, "  Fan mode: %s.%s", this->fan_param_.topic.c_str(), this->fan_param_.param.c_str());
  if (this->preset_param_.configured())
    ESP_LOGCONFIG(TAG, "  Preset: %s.%s", this->preset_param_.topic.c_str(), this->preset_param_.param.c_str());
  if (this->action_param_.configured())
    ESP_LOGCONFIG(TAG, "  Action: %s.%s", this->action_param_.topic.c_str(), this->action_param_.param.c_str());
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
