import esphome.codegen as cg
from esphome.components import climate
import esphome.config_validation as cv

from .. import (
    CONF_PARAMETER,
    CONF_TOPIC,
    CONF_TRUMA_INETX_ID,
    TRUMA_INETX_CHILD_SCHEMA,
    truma_inetx_ns,
)

DEPENDENCIES = ["truma_inetx"]

CONF_VALUES = "values"
CONF_MODE_PARAMETER = "mode_parameter"
CONF_TARGET_TEMPERATURE_PARAMETER = "target_temperature_parameter"
CONF_CURRENT_TEMPERATURE_PARAMETER = "current_temperature_parameter"
CONF_FAN_MODE_PARAMETER = "fan_mode_parameter"
CONF_PRESET_PARAMETER = "preset_parameter"
CONF_ACTION_PARAMETER = "action_parameter"
CONF_TEMPERATURE_MULTIPLIER = "temperature_multiplier"

TrumaInetXClimate = truma_inetx_ns.class_(
    "TrumaInetXClimate", climate.Climate, cg.Component
)

# iNet X "RoomClimate.Mode": 0=OFF 1=ACC(auto) 2=COOLING 3=HEATING 4=HEATING_AC 5=VENTING 6=DEHUMIDIFYING
# Best guesses for an Aventa without panel: check the logs while using the original remote.
DEFAULT_MODE_VALUES = {
    "OFF": 0,
    "AUTO": 1,
    "COOL": 2,
    "HEAT": 4,
    "FAN_ONLY": 5,
    "DRY": 6,
}


def _mapping_schema(key_validator, default_topic=None, default_parameter=None, default_values=None):
    topic = (
        cv.Optional(CONF_TOPIC, default=default_topic)
        if default_topic
        else cv.Required(CONF_TOPIC)
    )
    parameter = (
        cv.Optional(CONF_PARAMETER, default=default_parameter)
        if default_parameter
        else cv.Required(CONF_PARAMETER)
    )
    values = (
        cv.Optional(CONF_VALUES, default=default_values)
        if default_values is not None
        else cv.Required(CONF_VALUES)
    )
    return cv.Schema(
        {
            topic: cv.string_strict,
            parameter: cv.string_strict,
            values: cv.All(cv.Schema({key_validator: cv.int_}), cv.Length(min=1)),
        }
    )


def _parameter_schema(default_topic=None, default_parameter=None):
    topic = (
        cv.Optional(CONF_TOPIC, default=default_topic)
        if default_topic
        else cv.Required(CONF_TOPIC)
    )
    parameter = (
        cv.Optional(CONF_PARAMETER, default=default_parameter)
        if default_parameter
        else cv.Required(CONF_PARAMETER)
    )
    return cv.Schema({topic: cv.string_strict, parameter: cv.string_strict})


def _validate_off_mode(config):
    if "OFF" not in config[CONF_MODE_PARAMETER][CONF_VALUES]:
        raise cv.Invalid("mode_parameter.values must contain OFF")
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(TrumaInetXClimate)
    .extend(TRUMA_INETX_CHILD_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_MODE_PARAMETER, default={}): _mapping_schema(
                climate.validate_climate_mode,
                "RoomClimate",
                "Mode",
                DEFAULT_MODE_VALUES,
            ),
            cv.Optional(
                CONF_TARGET_TEMPERATURE_PARAMETER, default={}
            ): _parameter_schema("RoomClimate", "TgtTemp"),
            cv.Optional(CONF_CURRENT_TEMPERATURE_PARAMETER): _parameter_schema(),
            cv.Optional(CONF_FAN_MODE_PARAMETER): _mapping_schema(
                climate.validate_climate_fan_mode
            ),
            cv.Optional(CONF_PRESET_PARAMETER): _mapping_schema(
                climate.validate_climate_preset
            ),
            cv.Optional(CONF_ACTION_PARAMETER): _mapping_schema(
                climate.validate_climate_action
            ),
            # wire value x multiplier = degrees C (iNet X uses tenths of a degree)
            cv.Optional(CONF_TEMPERATURE_MULTIPLIER, default=0.1): cv.positive_float,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_off_mode,
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_TRUMA_INETX_ID])

    cg.add(var.set_temperature_multiplier(config[CONF_TEMPERATURE_MULTIPLIER]))

    mode = config[CONF_MODE_PARAMETER]
    cg.add(var.set_mode_parameter(mode[CONF_TOPIC], mode[CONF_PARAMETER]))
    for key, wire in mode[CONF_VALUES].items():
        cg.add(var.add_mode(climate.CLIMATE_MODES[key], wire))

    target = config[CONF_TARGET_TEMPERATURE_PARAMETER]
    cg.add(var.set_target_parameter(target[CONF_TOPIC], target[CONF_PARAMETER]))

    if current := config.get(CONF_CURRENT_TEMPERATURE_PARAMETER):
        cg.add(var.set_current_parameter(current[CONF_TOPIC], current[CONF_PARAMETER]))

    if fan := config.get(CONF_FAN_MODE_PARAMETER):
        cg.add(var.set_fan_mode_parameter(fan[CONF_TOPIC], fan[CONF_PARAMETER]))
        for key, wire in fan[CONF_VALUES].items():
            cg.add(var.add_fan_mode(climate.CLIMATE_FAN_MODES[key], wire))

    if preset := config.get(CONF_PRESET_PARAMETER):
        cg.add(var.set_preset_parameter(preset[CONF_TOPIC], preset[CONF_PARAMETER]))
        for key, wire in preset[CONF_VALUES].items():
            cg.add(var.add_preset(climate.CLIMATE_PRESETS[key], wire))

    if action := config.get(CONF_ACTION_PARAMETER):
        cg.add(var.set_action_parameter(action[CONF_TOPIC], action[CONF_PARAMETER]))
        for key, wire in action[CONF_VALUES].items():
            cg.add(var.add_action(climate.CLIMATE_ACTIONS[key], wire))
