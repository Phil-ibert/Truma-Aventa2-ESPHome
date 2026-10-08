#include "truma_inetx_sensor.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.sensor";

void TrumaInetXSensor::setup() {
  this->parent_->register_listener(this->topic_, this->parameter_, [this](const cbor::Value &value) {
    if (!value.is_number()) {
      ESP_LOGW(TAG, "'%s': %s.%s is not numeric (%s)", this->get_name().c_str(), this->topic_.c_str(),
               this->parameter_.c_str(), value.to_string(64).c_str());
      return;
    }
    this->publish_state(static_cast<float>(value.as_double()) * this->multiplier_);
  });
  this->parent_->add_on_state_callback([this](SessionState state) {
    if (state == SessionState::DISCONNECTED)
      this->publish_state(NAN);
  });
}

void TrumaInetXSensor::dump_config() {
  LOG_SENSOR("", "Truma iNet X Sensor", this);
  ESP_LOGCONFIG(TAG, "  Parameter: %s.%s (x%g)", this->topic_.c_str(), this->parameter_.c_str(), this->multiplier_);
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
