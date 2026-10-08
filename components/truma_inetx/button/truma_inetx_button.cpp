#include "truma_inetx_button.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace truma_inetx {

static const char *const TAG = "truma_inetx.button";

static const char *button_type_to_string(TrumaInetXButtonType type) {
  switch (type) {
    case TrumaInetXButtonType::PAIR:
      return "pair";
    case TrumaInetXButtonType::FORGET_PAIRING:
      return "forget_pairing";
    case TrumaInetXButtonType::REFRESH:
      return "refresh";
    case TrumaInetXButtonType::DUMP_PARAMETERS:
      return "dump_parameters";
    default:
      return "unknown";
  }
}

void TrumaInetXButton::press_action() {
  switch (this->type_) {
    case TrumaInetXButtonType::PAIR:
      this->parent_->start_pairing();
      break;
    case TrumaInetXButtonType::FORGET_PAIRING:
      this->parent_->forget_pairing();
      break;
    case TrumaInetXButtonType::REFRESH:
      this->parent_->refresh();
      break;
    case TrumaInetXButtonType::DUMP_PARAMETERS:
      this->parent_->dump_parameters();
      break;
  }
}

void TrumaInetXButton::dump_config() {
  LOG_BUTTON("", "Truma iNet X Button", this);
  ESP_LOGCONFIG(TAG, "  Type: %s", button_type_to_string(this->type_));
}

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
