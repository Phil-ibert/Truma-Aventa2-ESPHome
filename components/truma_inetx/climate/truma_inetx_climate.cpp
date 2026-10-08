#include "truma_inetx_climate.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.climate";

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void TrumaInetXClimate::setup() {
  this->mode = climate::CLIMATE_MODE_OFF;

  // Custom fan labels from the base mapping and every override (deduplicated).
  auto add_labels = [this](const FanSource &source) {
    for (const auto &f : source.custom) {
      bool known = false;
      for (const char *label : this->custom_fan_labels_)
        known |= strcmp(label, f.first) == 0;
      if (!known)
        this->custom_fan_labels_.push_back(f.first);
    }
  };
  if (this->fan_.param.configured()) {
    add_labels(this->fan_);
    for (const auto &o : this->fan_overrides_)
      add_labels(o.second);
  }
  if (!this->custom_fan_labels_.empty())
    this->set_supported_custom_fan_modes(this->custom_fan_labels_);

  // Any change of any involved parameter rebuilds the state (values depend on the active mode).
  this->listen_(this->mode_param_);
  this->listen_(this->target_param_);
  for (const auto &o : this->target_overrides_)
    this->listen_(o.second);
  this->listen_(this->current_param_);
  this->listen_(this->fan_.param);
  for (const auto &o : this->fan_overrides_)
    this->listen_(o.second.param);
  this->listen_(this->preset_param_);
  for (const auto &a : this->action_sources_)
    this->listen_(a.param);

  this->parent_->add_on_state_callback([this](SessionState state) {
    if (state == SessionState::DISCONNECTED) {
      this->current_temperature = NAN;
      this->schedule_publish_();
    }
  });
}

void TrumaInetXClimate::listen_(const Param &param) {
  if (!param.configured())
    return;
  this->parent_->register_listener(param.topic, param.param, [this](const cbor::Value &) { this->recompute_(); });
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const cbor::Value *TrumaInetXClimate::get_(const Param &param) const {
  if (!param.configured())
    return nullptr;
  const cbor::Value *value = this->parent_->get_value(param.topic, param.param);
  return (value != nullptr && value->is_number()) ? value : nullptr;
}

const TrumaInetXClimate::Param &TrumaInetXClimate::target_param_for_(climate::ClimateMode mode) const {
  for (const auto &o : this->target_overrides_) {
    if (o.first == mode)
      return o.second;
  }
  return this->target_param_;
}

const TrumaInetXClimate::FanSource &TrumaInetXClimate::fan_source_for_(climate::ClimateMode mode) const {
  for (const auto &o : this->fan_overrides_) {
    if (o.first == mode)
      return o.second;
  }
  return this->fan_;
}

TrumaInetXClimate::FanSource *TrumaInetXClimate::find_fan_override_(climate::ClimateMode mode) {
  for (auto &o : this->fan_overrides_) {
    if (o.first == mode)
      return &o.second;
  }
  return nullptr;
}

void TrumaInetXClimate::warn_once_(const Param &param, int64_t value, const char *what) {
  std::string key = param.topic + "." + param.param + "=" + std::to_string(value);
  if (!this->warned_.insert(key).second)
    return;
  ESP_LOGW(TAG, "%s.%s = %lld has no %s mapping: add it in the climate configuration", param.topic.c_str(),
           param.param.c_str(), (long long) value, what);
}

void TrumaInetXClimate::schedule_publish_() {
  // coalesce several parameter updates received together into one state publish
  this->defer("publish", [this]() { this->publish_state(); });
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void TrumaInetXClimate::recompute_() {
  // 1. mode
  if (const auto *v = this->get_(this->mode_param_)) {
    int64_t wire = v->as_int();
    bool found = false;
    for (const auto &m : this->mode_values_) {
      if (m.second == wire) {
        this->mode = m.first;
        found = true;
        break;
      }
    }
    if (!found)
      this->warn_once_(this->mode_param_, wire, "mode");
  }

  // 2. setpoint of the active mode
  if (const auto *v = this->get_(this->target_param_for_(this->mode)))
    this->target_temperature = static_cast<float>(v->as_double()) * this->multiplier_;

  // 3. measured temperature
  if (const auto *v = this->get_(this->current_param_))
    this->current_temperature = static_cast<float>(v->as_double()) * this->multiplier_;

  // 4. fan speed of the active mode
  if (this->fan_.param.configured()) {
    const FanSource &source = this->fan_source_for_(this->mode);
    const FanSource &values = this->fan_values_for_(source);
    if (const auto *v = this->get_(source.param)) {
      int64_t wire = v->as_int();
      bool found = false;
      for (const auto &f : values.standard) {
        if (f.second == wire) {
          this->set_fan_mode_(f.first);
          found = true;
          break;
        }
      }
      for (size_t i = 0; !found && i < values.custom.size(); i++) {
        if (values.custom[i].second == wire) {
          this->set_custom_fan_mode_(values.custom[i].first);
          found = true;
        }
      }
      if (!found)
        this->warn_once_(source.param, wire, "fan mode");
    }
  }

  // 5. preset
  if (const auto *v = this->get_(this->preset_param_)) {
    int64_t wire = v->as_int();
    bool found = false;
    for (const auto &p : this->preset_values_) {
      if (p.second == wire) {
        this->set_preset_(p.first);
        found = true;
        break;
      }
    }
    if (!found)
      this->warn_once_(this->preset_param_, wire, "preset");
  }

  // 6. action: the first source reporting a running action wins, otherwise idle while on
  if (!this->action_sources_.empty()) {
    climate::ClimateAction action = climate::CLIMATE_ACTION_IDLE;
    for (const auto &source : this->action_sources_) {
      const auto *v = this->get_(source.param);
      if (v == nullptr)
        continue;
      int64_t wire = v->as_int();
      bool running = false;
      for (const auto &a : source.values) {
        if (a.second == wire && a.first != climate::CLIMATE_ACTION_IDLE && a.first != climate::CLIMATE_ACTION_OFF) {
          action = a.first;
          running = true;
          break;
        }
      }
      if (running)
        break;
    }
    this->action = this->mode == climate::CLIMATE_MODE_OFF ? climate::CLIMATE_ACTION_OFF : action;
  }

  this->schedule_publish_();
}

climate::ClimateTraits TrumaInetXClimate::traits() {
  auto traits = climate::ClimateTraits();
  if (this->current_param_.configured())
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  if (!this->action_sources_.empty())
    traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION);
  for (const auto &m : this->mode_values_)
    traits.add_supported_mode(m.first);
  if (this->fan_.param.configured()) {
    for (const auto &f : this->fan_.standard)
      traits.add_supported_fan_mode(f.first);
    for (const auto &o : this->fan_overrides_) {
      for (const auto &f : o.second.standard)
        traits.add_supported_fan_mode(f.first);
    }
  }
  for (const auto &p : this->preset_values_)
    traits.add_supported_preset(p.first);
  // Defaults matching the Aventa (16-30 degrees C, whole degrees like the remote);
  // override with `visual:` in YAML.
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_target_temperature_step(1.0f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void TrumaInetXClimate::control(const climate::ClimateCall &call) {
  auto *hub = this->parent_;
  // Setpoint and fan go to the parameters of the mode being set (or the current one).
  climate::ClimateMode target_mode = call.get_mode().has_value() ? *call.get_mode() : this->mode;

  if (call.get_mode().has_value()) {
    bool found = false;
    for (const auto &m : this->mode_values_) {
      if (m.first == target_mode) {
        hub->write_int(this->mode_param_.topic, this->mode_param_.param, m.second);
        found = true;
        break;
      }
    }
    if (!found)
      ESP_LOGW(TAG, "Mode %s is not mapped", LOG_STR_ARG(climate::climate_mode_to_string(target_mode)));
  }

  if (call.get_target_temperature().has_value()) {
    const Param &param = this->target_param_for_(target_mode);
    int64_t wire = static_cast<int64_t>(std::llround(*call.get_target_temperature() / this->multiplier_));
    hub->write_int(param.topic, param.param, wire);
  }

  if (this->fan_.param.configured() && (call.get_fan_mode().has_value() || call.has_custom_fan_mode())) {
    const FanSource &source = this->fan_source_for_(target_mode);
    const FanSource &values = this->fan_values_for_(source);
    bool found = false;
    if (call.get_fan_mode().has_value()) {
      for (const auto &f : values.standard) {
        if (f.first == *call.get_fan_mode()) {
          hub->write_int(source.param.topic, source.param.param, f.second);
          found = true;
          break;
        }
      }
    } else {
      auto label = call.get_custom_fan_mode();
      for (const auto &f : values.custom) {
        if (label.size() == strlen(f.first) && strncmp(label.c_str(), f.first, label.size()) == 0) {
          hub->write_int(source.param.topic, source.param.param, f.second);
          found = true;
          break;
        }
      }
    }
    if (!found)
      ESP_LOGW(TAG, "This fan mode is not available in mode %s",
               LOG_STR_ARG(climate::climate_mode_to_string(target_mode)));
  }

  if (call.get_preset().has_value() && this->preset_param_.configured()) {
    for (const auto &p : this->preset_values_) {
      if (p.first == *call.get_preset()) {
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
  for (const auto &o : this->target_overrides_)
    ESP_LOGCONFIG(TAG, "    in %s: %s.%s", LOG_STR_ARG(climate::climate_mode_to_string(o.first)),
                  o.second.topic.c_str(), o.second.param.c_str());
  if (this->current_param_.configured())
    ESP_LOGCONFIG(TAG, "  Current temperature: %s.%s", this->current_param_.topic.c_str(),
                  this->current_param_.param.c_str());
  if (this->fan_.param.configured()) {
    ESP_LOGCONFIG(TAG, "  Fan mode: %s.%s (%u standard, %u custom)", this->fan_.param.topic.c_str(),
                  this->fan_.param.param.c_str(), (unsigned) this->fan_.standard.size(),
                  (unsigned) this->fan_.custom.size());
    for (const auto &o : this->fan_overrides_)
      ESP_LOGCONFIG(TAG, "    in %s: %s.%s", LOG_STR_ARG(climate::climate_mode_to_string(o.first)),
                    o.second.param.topic.c_str(), o.second.param.param.c_str());
  }
  if (this->preset_param_.configured())
    ESP_LOGCONFIG(TAG, "  Preset: %s.%s", this->preset_param_.topic.c_str(), this->preset_param_.param.c_str());
  for (const auto &a : this->action_sources_)
    ESP_LOGCONFIG(TAG, "  Action source: %s.%s", a.param.topic.c_str(), a.param.param.c_str());
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
