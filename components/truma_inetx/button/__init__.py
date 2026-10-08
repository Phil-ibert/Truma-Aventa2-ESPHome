import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import (
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_TYPE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from .. import CONF_TRUMA_INETX_ID, TRUMA_INETX_CHILD_SCHEMA, truma_inetx_ns

DEPENDENCIES = ["truma_inetx"]

TrumaInetXButton = truma_inetx_ns.class_("TrumaInetXButton", button.Button, cg.Component)
ButtonType = truma_inetx_ns.enum("TrumaInetXButtonType", is_class=True)

BUTTON_TYPES = {
    # find the nearest Truma device, connect and bond (first pairing)
    "pair": ButtonType.PAIR,
    # remove the bond and the remembered address
    "forget_pairing": ButtonType.FORGET_PAIRING,
    # ask every known device for all its parameters again
    "refresh": ButtonType.REFRESH,
    # log every known parameter
    "dump_parameters": ButtonType.DUMP_PARAMETERS,
}

TYPE_DEFAULTS = {
    "pair": ("mdi:bluetooth-settings", ENTITY_CATEGORY_CONFIG),
    "forget_pairing": ("mdi:link-variant-off", ENTITY_CATEGORY_CONFIG),
    "refresh": ("mdi:refresh", ENTITY_CATEGORY_DIAGNOSTIC),
    "dump_parameters": ("mdi:text-box-search-outline", ENTITY_CATEGORY_DIAGNOSTIC),
}


def _apply_type_defaults(config):
    """Fill icon / entity category from the button type before validation."""
    if not isinstance(config, dict):
        return config
    defaults = TYPE_DEFAULTS.get(str(config.get(CONF_TYPE, "")).lower())
    if defaults is None:
        return config
    config = dict(config)
    config.setdefault(CONF_ICON, defaults[0])
    config.setdefault(CONF_ENTITY_CATEGORY, defaults[1])
    return config


CONFIG_SCHEMA = cv.All(
    _apply_type_defaults,
    button.button_schema(TrumaInetXButton)
    .extend(TRUMA_INETX_CHILD_SCHEMA)
    .extend({cv.Required(CONF_TYPE): cv.one_of(*BUTTON_TYPES, lower=True)})
    .extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])
    cg.add(var.set_type(BUTTON_TYPES[config[CONF_TYPE]]))
