import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_OPTIONS

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TOPIC_PARAMETER_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

TrumaInetXSelect = truma_inetx_ns.class_("TrumaInetXSelect", select.Select, cg.Component)


def validate_options(value):
    value = cv.Schema({cv.int_: cv.string_strict})(value)
    if not value:
        raise cv.Invalid("At least one option is required")
    labels = list(value.values())
    if len(set(labels)) != len(labels):
        raise cv.Invalid("Option labels must be unique")
    return value


# options: wire value -> label, e.g. {0: "Confort", 1: "Rapide"}
CONFIG_SCHEMA = (
    select.select_schema(TrumaInetXSelect)
    .extend(TOPIC_PARAMETER_SCHEMA)
    .extend({cv.Required(CONF_OPTIONS): validate_options})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    options = config[CONF_OPTIONS]
    var = await select.new_select(config, options=list(options.values()))
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])
    cg.add(var.set_topic(config[CONF_TOPIC]))
    cg.add(var.set_parameter(config[CONF_PARAMETER]))
    for wire_value in options:
        cg.add(var.add_value(wire_value))
