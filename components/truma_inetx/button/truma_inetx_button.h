#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/button/button.h"
#include "../truma_inetx.h"

#ifdef USE_ESP32

namespace esphome {
namespace truma_inetx {

enum class TrumaInetXButtonType : uint8_t {
  PAIR,
  FORGET_PAIRING,
  REFRESH,
  DUMP_PARAMETERS,
};

/// Maintenance buttons: first pairing, forget pairing, refresh, dump parameters.
class TrumaInetXButton : public button::Button, public Component, public Parented<TrumaInetX> {
 public:
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_type(TrumaInetXButtonType type) { this->type_ = type; }

 protected:
  void press_action() override;

  TrumaInetXButtonType type_{TrumaInetXButtonType::REFRESH};
};

}  // namespace truma_inetx
}  // namespace esphome

#endif  // USE_ESP32
