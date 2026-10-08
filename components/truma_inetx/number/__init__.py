import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TOPIC_PARAMETER_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

CONF_MULTIPLIER = "multiplier"

TrumaInetXNumber = truma_inetx_ns.class_("TrumaInetXNumber", number.Number, cg.Component)


def validate_min_max(config):
    if config[CONF_MIN_VALUE] >= config[CONF_MAX_VALUE]:
        raise cv.Invalid("min_value must be lower than max_value")
    if config[CONF_MULTIPLIER] == 0:
        raise cv.Invalid("multiplier cannot be 0")
    return config


# Displayed value = wire value x multiplier (e.g. 0.1 for temperatures in tenths of a degree).
CONFIG_SCHEMA = cv.All(
    number.number_schema(TrumaInetXNumber)
    .extend(TOPIC_PARAMETER_SCHEMA)
    .extend(
        {
            cv.Required(CONF_MIN_VALUE): cv.float_,
            cv.Required(CONF_MAX_VALUE): cv.float_,
            cv.Optional(CONF_STEP, default=1): cv.positive_float,
            cv.Optional(CONF_MULTIPLIER, default=1.0): cv.float_,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    validate_min_max,
)


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])
    cg.add(var.set_topic(config[CONF_TOPIC]))
    cg.add(var.set_parameter(config[CONF_PARAMETER]))
    cg.add(var.set_multiplier(config[CONF_MULTIPLIER]))
