import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import (
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_GAMMA_CORRECT,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_OUTPUT_ID,
)

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TRUMA_INETX_CHILD_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

CONF_ACTIVE_PARAMETER = "active_parameter"
CONF_BRIGHTNESS_PARAMETER = "brightness_parameter"
CONF_ON_VALUE = "on_value"
CONF_OFF_VALUE = "off_value"

TrumaInetXLight = truma_inetx_ns.class_(
    "TrumaInetXLight", light.LightOutput, cg.Component
)

# Aventa 2nd generation, observed on a real unit (device 0x0801):
#   AmbientLight.Active    0 = off, 1 = on
#   AmbientLight.LightStep 0..100 (the remote switched it on at 25)
ACTIVE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TOPIC, default="AmbientLight"): cv.string_strict,
        cv.Optional(CONF_PARAMETER, default="Active"): cv.string_strict,
        cv.Optional(CONF_ON_VALUE, default=1): cv.int_,
        cv.Optional(CONF_OFF_VALUE, default=0): cv.int_,
    }
)


def _validate_range(config):
    if config[CONF_MIN_VALUE] > config[CONF_MAX_VALUE]:
        raise cv.Invalid("min_value must be lower than max_value")
    return config


BRIGHTNESS_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_TOPIC, default="AmbientLight"): cv.string_strict,
            cv.Optional(CONF_PARAMETER, default="LightStep"): cv.string_strict,
            # device value for 100 % brightness, and the lowest value sent while on
            cv.Optional(CONF_MAX_VALUE, default=100): cv.int_range(min=1),
            cv.Optional(CONF_MIN_VALUE, default=1): cv.int_range(min=1),
        }
    ),
    _validate_range,
)


def _disableable(validator):
    """Accept the given config, or `false` for an on/off light without brightness."""

    def wrapped(value):
        if value is False or (
            isinstance(value, str) and value.lower() in ("false", "none", "off")
        ):
            return None
        return validator(value)

    return wrapped


CONFIG_SCHEMA = (
    light.BRIGHTNESS_ONLY_LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(TrumaInetXLight),
            cv.Optional(CONF_ACTIVE_PARAMETER, default={}): ACTIVE_SCHEMA,
            cv.Optional(CONF_BRIGHTNESS_PARAMETER, default={}): _disableable(
                BRIGHTNESS_SCHEMA
            ),
            # brightness % maps linearly to the device level, applied at once like the remote
            cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.positive_float,
            cv.Optional(
                CONF_DEFAULT_TRANSITION_LENGTH, default="0s"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(TRUMA_INETX_CHILD_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])

    active = config[CONF_ACTIVE_PARAMETER]
    cg.add(
        var.set_active_parameter(
            active[CONF_TOPIC],
            active[CONF_PARAMETER],
            active[CONF_ON_VALUE],
            active[CONF_OFF_VALUE],
        )
    )
    if brightness := config.get(CONF_BRIGHTNESS_PARAMETER):
        cg.add(
            var.set_brightness_parameter(
                brightness[CONF_TOPIC],
                brightness[CONF_PARAMETER],
                brightness[CONF_MIN_VALUE],
                brightness[CONF_MAX_VALUE],
            )
        )
