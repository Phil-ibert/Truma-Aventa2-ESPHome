import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TOPIC_PARAMETER_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

CONF_ON_VALUE = "on_value"
CONF_OFF_VALUE = "off_value"

TrumaInetXSwitch = truma_inetx_ns.class_("TrumaInetXSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = (
    # never write anything to the device at boot
    switch.switch_schema(TrumaInetXSwitch, default_restore_mode="DISABLED")
    .extend(TOPIC_PARAMETER_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_ON_VALUE, default=1): cv.int_,
            cv.Optional(CONF_OFF_VALUE, default=0): cv.int_,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])
    cg.add(var.set_topic(config[CONF_TOPIC]))
    cg.add(var.set_parameter(config[CONF_PARAMETER]))
    cg.add(var.set_values(config[CONF_ON_VALUE], config[CONF_OFF_VALUE]))
