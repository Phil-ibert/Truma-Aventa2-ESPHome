import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TRUMA_INETX_CHILD_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

TrumaInetXTextSensor = truma_inetx_ns.class_(
    "TrumaInetXTextSensor", text_sensor.TextSensor, cg.Component
)

# Without topic/parameter: session state (connecting, ready...).
# With topic/parameter: raw value of the parameter.
CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(TrumaInetXTextSensor)
    .extend(TRUMA_INETX_CHILD_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_TOPIC): cv.string_strict,
            cv.Optional(CONF_PARAMETER): cv.string_strict,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.has_none_or_all_keys(CONF_TOPIC, CONF_PARAMETER),
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])
    if CONF_TOPIC in config:
        cg.add(var.set_source(config[CONF_TOPIC], config[CONF_PARAMETER]))
