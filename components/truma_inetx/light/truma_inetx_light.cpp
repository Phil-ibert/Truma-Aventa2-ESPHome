#include "truma_inetx_light.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.light";

void TrumaInetXLight::setup() {
  this->parent_->register_listener(this->active_topic_, this->active_param_,
                                   [this](const cbor::Value &value) { this->on_active_(value); });
  if (this->has_brightness_()) {
    this->parent_->register_listener(this->level_topic_, this->level_param_,
                                     [this](const cbor::Value &value) { this->on_level_(value); });
  }
  this->parent_->add_on_state_callback([this](SessionState session) {
    if (session == SessionState::DISCONNECTED) {
      // unknown until the device reports again after reconnection
      this->device_on_.reset();
      this->device_level_.reset();
    } else if (session == SessionState::READY) {
      this->show_device_state_();
    }
  });
}

void TrumaInetXLight::show_device_state_() {
  // A change made in Home Assistant while the session was not ready was not sent: show the
  // state of the device again.
  if (this->state_ == nullptr || !this->device_on_.has_value() ||
      this->state_->remote_values.is_on() == *this->device_on_)
    return;
  auto call = this->state_->make_call();
  call.set_state(*this->device_on_);
  if (*this->device_on_ && this->has_brightness_() && this->device_level_.has_value() && *this->device_level_ > 0)
    call.set_brightness(this->to_brightness_(*this->device_level_));
  this->apply_(call);
}

light::LightTraits TrumaInetXLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes(
      {this->has_brightness_() ? light::ColorMode::BRIGHTNESS : light::ColorMode::ON_OFF});
  return traits;
}

int64_t TrumaInetXLight::to_level_(float brightness) const {
  auto level = static_cast<int64_t>(std::lround(brightness * static_cast<float>(this->max_level_)));
  return std::min(std::max(level, this->min_level_), this->max_level_);
}

float TrumaInetXLight::to_brightness_(int64_t level) const {
  float brightness = static_cast<float>(level) / static_cast<float>(this->max_level_);
  return std::min(std::max(brightness, 0.0f), 1.0f);
}

void TrumaInetXLight::apply_(light::LightCall &call) {
  if (this->has_brightness_())
    call.set_transition_length(0);
  call.perform();
}

// ---------------------------------------------------------------------------
// Device -> Home Assistant
// ---------------------------------------------------------------------------

void TrumaInetXLight::on_active_(const cbor::Value &value) {
  if (this->state_ == nullptr || !value.is_number())
    return;
  const bool on = value.as_int() != this->off_value_;
  if (this->device_on_.has_value() && *this->device_on_ == on)
    return;  // unchanged, or the echo of our own write
  this->device_on_ = on;

  auto call = this->state_->make_call();
  call.set_state(on);
  const bool level_known = this->device_level_.has_value() && *this->device_level_ > 0;
  if (on && this->has_brightness_() && level_known)
    call.set_brightness(this->to_brightness_(*this->device_level_));
  this->apply_(call);

  // Switched on before its level is reported (the remote sends Active, then LightStep):
  // keep the brightness shown in Home Assistant without writing it to the device. The
  // level the device reports next replaces it.
  if (on && this->has_brightness_() && !level_known)
    this->device_level_ = this->to_level_(this->state_->remote_values.get_brightness());
}

void TrumaInetXLight::on_level_(const cbor::Value &value) {
  if (this->state_ == nullptr || !value.is_number())
    return;
  const int64_t level = value.as_int();
  if (this->device_level_.has_value() && *this->device_level_ == level)
    return;
  this->device_level_ = level;
  // 0 is reported while off: keep the last brightness for the next time it is switched on.
  if (level <= 0 || !this->state_->remote_values.is_on())
    return;
  auto call = this->state_->make_call();
  call.set_brightness(this->to_brightness_(level));
  this->apply_(call);
}

// ---------------------------------------------------------------------------
// Home Assistant -> device
// ---------------------------------------------------------------------------

void TrumaInetXLight::write_state(light::LightState *state) {
  if (state->is_transformer_active())
    return;  // only the final value of a transition is sent
  if (!this->parent_->is_ready()) {
    ESP_LOGD(TAG, "'%s': not connected, the state of the device will be shown once connected",
             state->get_name().c_str());
    return;
  }

  const bool on = state->current_values.is_on();
  if (!this->device_on_.has_value() || *this->device_on_ != on) {
    this->device_on_ = on;  // set first: the optimistic echo of the write must be ignored
    if (!this->parent_->write_int(this->active_topic_, this->active_param_, on ? this->on_value_ : this->off_value_)) {
      this->device_on_.reset();
      return;
    }
  }

  if (on && this->has_brightness_()) {
    const int64_t level = this->to_level_(state->current_values.get_brightness());
    if (!this->device_level_.has_value() || *this->device_level_ != level) {
      this->device_level_ = level;
      if (!this->parent_->write_int(this->level_topic_, this->level_param_, level))
        this->device_level_.reset();
    }
  }
}

void TrumaInetXLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Truma iNet X Light:");
  ESP_LOGCONFIG(TAG, "  On/off: %s.%s (on=%lld, off=%lld)", this->active_topic_.c_str(),
                this->active_param_.c_str(), (long long) this->on_value_, (long long) this->off_value_);
  if (this->has_brightness_()) {
    ESP_LOGCONFIG(TAG, "  Brightness: %s.%s (%lld..%lld)", this->level_topic_.c_str(), this->level_param_.c_str(),
                  (long long) this->min_level_, (long long) this->max_level_);
  } else {
    ESP_LOGCONFIG(TAG, "  Brightness: none (on/off only)");
  }
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
