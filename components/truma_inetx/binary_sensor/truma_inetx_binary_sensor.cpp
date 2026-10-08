#include "truma_inetx_binary_sensor.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.binary_sensor";

void TrumaInetXBinarySensor::setup() {
  if (this->topic_.empty()) {
    this->publish_initial_state(false);
    this->parent_->add_on_state_callback(
        [this](SessionState state) { this->publish_state(state == SessionState::READY); });
    return;
  }
  this->parent_->register_listener(this->topic_, this->parameter_, [this](const cbor::Value &value) {
    if (!value.is_number()) {
      ESP_LOGW(TAG, "'%s': %s.%s is not numeric (%s)", this->get_name().c_str(), this->topic_.c_str(),
               this->parameter_.c_str(), value.to_string(64).c_str());
      return;
    }
    this->publish_state(value.as_int() != 0);
  });
}

void TrumaInetXBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "Truma iNet X Binary Sensor", this);
  if (this->topic_.empty()) {
    ESP_LOGCONFIG(TAG, "  Source: session status");
  } else {
    ESP_LOGCONFIG(TAG, "  Parameter: %s.%s", this->topic_.c_str(), this->parameter_.c_str());
  }
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
