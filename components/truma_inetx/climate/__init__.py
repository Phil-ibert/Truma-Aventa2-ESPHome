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
CONF_PER_MODE = "per_mode"
CONF_MODE_PARAMETER = "mode_parameter"
CONF_TARGET_TEMPERATURE_PARAMETER = "target_temperature_parameter"
CONF_CURRENT_TEMPERATURE_PARAMETER = "current_temperature_parameter"
CONF_FAN_MODE_PARAMETER = "fan_mode_parameter"
CONF_PRESET_PARAMETER = "preset_parameter"
CONF_ACTION_PARAMETER = "action_parameter"
CONF_TEMPERATURE_MULTIPLIER = "temperature_multiplier"
CONF_CUSTOM_FAN_MODES = "custom_fan_modes"

TrumaInetXClimate = truma_inetx_ns.class_(
    "TrumaInetXClimate", climate.Climate, cg.Component
)

# ---------------------------------------------------------------------------
# Aventa 2nd generation defaults, observed on a real unit (no iNet X panel):
#   0x0101 "iNet X Interface AC": RoomClimate.Mode / RoomClimate.TgtTemp (setpoint used in ACC)
#   0x0801 the air conditioner:   AirCooling.* / AirHeating.* / AirCirculation.* / AirDehumid.*
# The remote changes the setpoint and the fan speed of the *active* function, so these
# parameters depend on the climate mode (`per_mode`).
# ---------------------------------------------------------------------------

# RoomClimate.Mode enum reported by the Aventa:
# Off=0 ACC=1 Cooling=2 HeatingAC=4 Ventilating=5 Dehumidifying=6
DEFAULT_MODE_VALUES = {
    "OFF": 0,
    "AUTO": 1,
    "COOL": 2,
    "HEAT": 4,
    "FAN_ONLY": 5,
    "DRY": 6,
}

DEFAULT_TARGET = {
    CONF_TOPIC: "RoomClimate",
    CONF_PARAMETER: "TgtTemp",
    CONF_PER_MODE: {
        "COOL": {CONF_TOPIC: "AirCooling", CONF_PARAMETER: "TgtTemp"},
        "HEAT": {CONF_TOPIC: "AirHeating", CONF_PARAMETER: "TgtTemp"},
    },
}

DEFAULT_CURRENT = {CONF_TOPIC: "AirCooling", CONF_PARAMETER: "Temp"}

# AirCooling.Mode / AirHeating.Mode enum: Auto=0 Low=1 Mid=2 High=3 Night=4
# AirCirculation.FanLevel (ventilation): 0..3
DEFAULT_FAN = {
    CONF_TOPIC: "AirCooling",
    CONF_PARAMETER: "Mode",
    CONF_VALUES: {"AUTO": 0, "LOW": 1, "MEDIUM": 2, "HIGH": 3, "QUIET": 4},
    CONF_PER_MODE: {
        "HEAT": {CONF_TOPIC: "AirHeating", CONF_PARAMETER: "Mode"},
        "FAN_ONLY": {
            CONF_TOPIC: "AirCirculation",
            CONF_PARAMETER: "FanLevel",
            CONF_VALUES: {"LOW": 1, "MEDIUM": 2, "HIGH": 3},
        },
    },
}

# Active: 0=off 1=running 2=idle (regulating, nothing to do)
DEFAULT_ACTION = [
    {CONF_TOPIC: "AirCooling", CONF_PARAMETER: "Active", CONF_VALUES: {"COOLING": 1, "IDLE": 2}},
    {CONF_TOPIC: "AirHeating", CONF_PARAMETER: "Active", CONF_VALUES: {"HEATING": 1, "IDLE": 2}},
    {CONF_TOPIC: "AirDehumid", CONF_PARAMETER: "Active", CONF_VALUES: {"DRYING": 1}},
]


# ---------------------------------------------------------------------------
# Schema helpers
# ---------------------------------------------------------------------------


def _topic_parameter(default_topic=None, default_parameter=None):
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
    return {topic: cv.string_strict, parameter: cv.string_strict}


def _values(key_validator, required=True):
    validator = cv.All(cv.Schema({key_validator: cv.int_}), cv.Length(min=1), _unique_values)
    if required:
        return {cv.Required(CONF_VALUES): validator}
    return {cv.Optional(CONF_VALUES): validator}


def _unique_values(values):
    seen = {}
    for name, wire in values.items():
        if wire in seen:
            raise cv.Invalid(
                f"'{name}' and '{seen[wire]}' use the same value {wire}: each value must map to one mode"
            )
        seen[wire] = name
    return values


def _fan_mode_key(value):
    """Standard fan mode name (AUTO, LOW, MEDIUM, HIGH, QUIET...) or any custom label."""
    value = cv.string_strict(str(value)) if isinstance(value, int) else cv.string_strict(value)
    if not value.strip():
        raise cv.Invalid("Fan mode names cannot be empty")
    if value.upper() in climate.CLIMATE_FAN_MODES:
        return value.upper()
    return value


def _disableable(validator):
    """Accept the given config, or `false` to disable an optional parameter."""

    def wrapped(value):
        if value is False or (
            isinstance(value, str) and value.lower() in ("false", "none", "off")
        ):
            return None
        return validator(value)

    return wrapped


def _per_mode(entry_schema):
    return {cv.Optional(CONF_PER_MODE, default={}): cv.Schema({climate.validate_climate_mode: entry_schema})}


MODE_SCHEMA = cv.Schema(
    {
        **_topic_parameter("RoomClimate", "Mode"),
        cv.Optional(CONF_VALUES, default=DEFAULT_MODE_VALUES): cv.All(
            cv.Schema({climate.validate_climate_mode: cv.int_}), cv.Length(min=1), _unique_values
        ),
    }
)

TARGET_SCHEMA = cv.Schema(
    {
        **_topic_parameter("RoomClimate", "TgtTemp"),
        **_per_mode(cv.Schema(_topic_parameter())),
    }
)

CURRENT_SCHEMA = cv.Schema(_topic_parameter())

FAN_SCHEMA = cv.Schema(
    {
        **_topic_parameter(),
        **_values(_fan_mode_key),
        **_per_mode(cv.Schema({**_topic_parameter(), **_values(_fan_mode_key, required=False)})),
    }
)

PRESET_SCHEMA = cv.Schema({**_topic_parameter(), **_values(climate.validate_climate_preset)})

ACTION_SOURCE_SCHEMA = cv.Schema({**_topic_parameter(), **_values(climate.validate_climate_action)})


def _validate_off_mode(config):
    if "OFF" not in config[CONF_MODE_PARAMETER][CONF_VALUES]:
        raise cv.Invalid("mode_parameter.values must contain OFF")
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(TrumaInetXClimate)
    .extend(TRUMA_INETX_CHILD_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_MODE_PARAMETER, default={}): MODE_SCHEMA,
            cv.Optional(CONF_TARGET_TEMPERATURE_PARAMETER, default=DEFAULT_TARGET): TARGET_SCHEMA,
            cv.Optional(
                CONF_CURRENT_TEMPERATURE_PARAMETER, default=DEFAULT_CURRENT
            ): _disableable(CURRENT_SCHEMA),
            cv.Optional(CONF_FAN_MODE_PARAMETER, default=DEFAULT_FAN): _disableable(FAN_SCHEMA),
            cv.Optional(CONF_PRESET_PARAMETER): _disableable(PRESET_SCHEMA),
            # one source or a list: the first source reporting a running action wins
            cv.Optional(CONF_ACTION_PARAMETER, default=DEFAULT_ACTION): _disableable(
                cv.ensure_list(ACTION_SOURCE_SCHEMA)
            ),
            # false = keep only the standard fan modes, hide custom labels
            cv.Optional(CONF_CUSTOM_FAN_MODES, default=True): cv.boolean,
            # wire value x multiplier = degrees C (iNet X uses tenths of a degree)
            cv.Optional(CONF_TEMPERATURE_MULTIPLIER, default=0.1): cv.positive_float,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_off_mode,
)


def _fan_value_calls(var, add_standard, add_custom, values, allow_custom):
    for key, wire in values.items():
        if key in climate.CLIMATE_FAN_MODES:
            cg.add(add_standard(climate.CLIMATE_FAN_MODES[key], wire))
        elif allow_custom:
            # custom fan mode: the label is shown as-is in Home Assistant
            cg.add(add_custom(key, wire))


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
    for mode_key, entry in target[CONF_PER_MODE].items():
        cg.add(
            var.add_target_override(
                climate.CLIMATE_MODES[mode_key], entry[CONF_TOPIC], entry[CONF_PARAMETER]
            )
        )

    if current := config.get(CONF_CURRENT_TEMPERATURE_PARAMETER):
        cg.add(var.set_current_parameter(current[CONF_TOPIC], current[CONF_PARAMETER]))

    if fan := config.get(CONF_FAN_MODE_PARAMETER):
        allow_custom = config[CONF_CUSTOM_FAN_MODES]
        cg.add(var.set_fan_mode_parameter(fan[CONF_TOPIC], fan[CONF_PARAMETER]))
        _fan_value_calls(var, var.add_fan_mode, var.add_custom_fan_mode, fan[CONF_VALUES], allow_custom)
        for mode_key, entry in fan[CONF_PER_MODE].items():
            climate_mode = climate.CLIMATE_MODES[mode_key]
            cg.add(var.add_fan_override(climate_mode, entry[CONF_TOPIC], entry[CONF_PARAMETER]))
            if CONF_VALUES in entry:
                _fan_value_calls(
                    var,
                    lambda f, w, m=climate_mode: var.add_fan_override_mode(m, f, w),
                    lambda label, w, m=climate_mode: var.add_fan_override_custom_mode(m, label, w),
                    entry[CONF_VALUES],
                    allow_custom,
                )

    if preset := config.get(CONF_PRESET_PARAMETER):
        cg.add(var.set_preset_parameter(preset[CONF_TOPIC], preset[CONF_PARAMETER]))
        for key, wire in preset[CONF_VALUES].items():
            cg.add(var.add_preset(climate.CLIMATE_PRESETS[key], wire))

    if sources := config.get(CONF_ACTION_PARAMETER):
        for source in sources:
            cg.add(var.add_action_source(source[CONF_TOPIC], source[CONF_PARAMETER]))
            for key, wire in source[CONF_VALUES].items():
                cg.add(var.add_action_value(climate.CLIMATE_ACTIONS[key], wire))
