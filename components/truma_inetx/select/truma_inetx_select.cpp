#include "truma_inetx_select.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.select";

void TrumaInetXSelect::setup() {
  this->parent_->register_listener(this->topic_, this->parameter_, [this](const cbor::Value &value) {
    if (!value.is_number()) {
      ESP_LOGW(TAG, "'%s': %s.%s is not numeric (%s)", this->get_name().c_str(), this->topic_.c_str(),
               this->parameter_.c_str(), value.to_string(64).c_str());
      return;
    }
    int64_t wire = value.as_int();
    for (size_t i = 0; i < this->values_.size(); i++) {
      if (this->values_[i] == wire) {
        this->publish_state(i);
        return;
      }
    }
    ESP_LOGW(TAG, "'%s': value %lld of %s.%s has no option, add it to `options:`", this->get_name().c_str(),
             (long long) wire, this->topic_.c_str(), this->parameter_.c_str());
  });
}

void TrumaInetXSelect::control(size_t index) {
  if (index >= this->values_.size())
    return;
  this->parent_->write_int(this->topic_, this->parameter_, this->values_[index]);
}

void TrumaInetXSelect::dump_config() {
  LOG_SELECT("", "Truma iNet X Select", this);
  ESP_LOGCONFIG(TAG, "  Parameter: %s.%s", this->topic_.c_str(), this->parameter_.c_str());
  for (size_t i = 0; i < this->values_.size(); i++)
    ESP_LOGCONFIG(TAG, "  %lld = %s", (long long) this->values_[i], this->option_at(i));
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
