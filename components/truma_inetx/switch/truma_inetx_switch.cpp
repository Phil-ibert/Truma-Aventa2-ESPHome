#include "truma_inetx_switch.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.switch";

void TrumaInetXSwitch::setup() {
  this->parent_->register_listener(this->topic_, this->parameter_, [this](const cbor::Value &value) {
    if (!value.is_number()) {
      ESP_LOGW(TAG, "'%s': %s.%s is not numeric (%s)", this->get_name().c_str(), this->topic_.c_str(),
               this->parameter_.c_str(), value.to_string(64).c_str());
      return;
    }
    // Any value other than off_value counts as ON (e.g. Active = 2 "idle" while regulating).
    this->publish_state(value.as_int() != this->off_value_);
  });
}

void TrumaInetXSwitch::write_state(bool state) {
  this->parent_->write_int(this->topic_, this->parameter_, state ? this->on_value_ : this->off_value_);
}

void TrumaInetXSwitch::dump_config() {
  LOG_SWITCH("", "Truma iNet X Switch", this);
  ESP_LOGCONFIG(TAG, "  Parameter: %s.%s (on=%lld, off=%lld)", this->topic_.c_str(), this->parameter_.c_str(),
                (long long) this->on_value_, (long long) this->off_value_);
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
