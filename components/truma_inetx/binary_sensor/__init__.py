import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TRUMA_INETX_CHILD_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

TrumaInetXBinarySensor = truma_inetx_ns.class_(
    "TrumaInetXBinarySensor", binary_sensor.BinarySensor, cg.Component
)

# Without topic/parameter: ON while the iNet X session is ready (connection status).
# With topic/parameter: ON when the value is not 0.
CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(TrumaInetXBinarySensor)
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
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])
    if CONF_TOPIC in config:
        cg.add(var.set_source(config[CONF_TOPIC], config[CONF_PARAMETER]))
