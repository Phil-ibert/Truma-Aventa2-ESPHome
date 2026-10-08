#include "truma_inetx_text_sensor.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.text_sensor";

void TrumaInetXTextSensor::setup() {
  if (this->topic_.empty()) {
    this->publish_state(session_state_to_string(this->parent_->get_state()));
    this->parent_->add_on_state_callback(
        [this](SessionState state) { this->publish_state(session_state_to_string(state)); });
    return;
  }
  this->parent_->register_listener(this->topic_, this->parameter_, [this](const cbor::Value &value) {
    if (value.is_text()) {
      this->publish_state(value.str);
    } else {
      this->publish_state(value.to_string(200));
    }
  });
}

void TrumaInetXTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "Truma iNet X Text Sensor", this);
  if (this->topic_.empty()) {
    ESP_LOGCONFIG(TAG, "  Source: session state");
  } else {
    ESP_LOGCONFIG(TAG, "  Parameter: %s.%s", this->topic_.c_str(), this->parameter_.c_str());
  }
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
