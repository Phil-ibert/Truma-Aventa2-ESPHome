import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TOPIC_PARAMETER_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

CONF_MULTIPLIER = "multiplier"

TrumaInetXSensor = truma_inetx_ns.class_(
    "TrumaInetXSensor", sensor.Sensor, cg.Component
)

CONFIG_SCHEMA = (
    sensor.sensor_schema(TrumaInetXSensor)
    .extend(TOPIC_PARAMETER_SCHEMA)
    .extend({cv.Optional(CONF_MULTIPLIER, default=1.0): cv.float_})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])
    cg.add(var.set_topic(config[CONF_TOPIC]))
    cg.add(var.set_parameter(config[CONF_PARAMETER]))
    cg.add(var.set_multiplier(config[CONF_MULTIPLIER]))
