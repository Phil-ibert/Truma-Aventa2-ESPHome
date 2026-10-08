"""Truma iNet X over Bluetooth LE (Truma Aventa 2nd generation, iNet X panels).

Protocol: GATT service F47BBBAC-..., "TruMessageV3" frames carrying CBOR maps
such as {"tn": "AirCooling", "pn": "TgtTemp", "v": 220}. Reverse engineering
credits: https://github.com/daaaaan/truma-inetx-ble
"""

import uuid

import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID, CONF_TX_POWER, SCHEDULER_DONT_RUN
from esphome.core import CORE

DEPENDENCIES = ["ble_client", "esp32_ble_tracker"]
MULTI_CONF = True

CONF_TRUMA_INETX_ID = "truma_inetx_id"
CONF_TOPIC = "topic"
CONF_PARAMETER = "parameter"
CONF_PIN = "pin"
CONF_ENCRYPTION = "encryption"
CONF_USER_NAME = "user_name"
CONF_MUID = "muid"
CONF_UUID = "uuid"
CONF_SEND_IDENTITY = "send_identity"
CONF_TOPICS = "topics"
CONF_DISCOVERY_ADDRESSES = "discovery_addresses"
CONF_DEFAULT_DESTINATION = "default_destination"
CONF_DESTINATIONS = "destinations"
CONF_AUTO_DISCOVERY = "auto_discovery"
CONF_OPTIMISTIC = "optimistic"
CONF_LOG_FRAMES = "log_frames"
CONF_FRAME_DELAY = "frame_delay"
CONF_DEVICE_NAME = "device_name"
CONF_REMEMBER_ADDRESS = "remember_address"
CONF_LOG_ADVERTISEMENTS = "log_advertisements"
CONF_POLL_INTERVAL = "poll_interval"

truma_inetx_ns = cg.esphome_ns.namespace("truma_inetx")
TrumaInetX = truma_inetx_ns.class_(
    "TrumaInetX",
    cg.Component,
    ble_client.BLEClientNode,
    esp32_ble_tracker.ESPBTDeviceListener,
)

# Topic list sent by the official iNet X app (subscribed in batches of 10).
DEFAULT_TOPICS = [
    "AirCirculation",
    "AirCooling",
    "AirHeating",
    "DeviceManagement",
    "EnergySrc",
    "ErrorReset",
    "FreshWater",
    "GasBtl",
    "GasControl",
    "GreyWater",
    "Identify",
    "L1Bat",
    "L2Bat",
    "LinePower",
    "MobileIdentity",
    "PowerSupply",
    "RoomClimate",
    "Switches",
    "Temperature",
    "Transfer",
    "VBat",
    "WaterHeating",
    "AmbientLight",
    "Panel",
    "BatteryMngmt",
    "Install",
    "Connect",
    "TimerConfig",
    "BleDeviceManagement",
    "BluetoothDevice",
    "System",
    "Resources",
    "PowerMgmt",
    # Aventa 2 topics missing from the app list above: without them, the dehumidifier
    # state was only seen by the periodic poll (the action stayed "drying" for up to a minute)
    "AirDehumid",
    "ACCAirCooling",
    "ACCAirHeating",
    "BleRemoteControl",  # the Bluetooth remote (0x0602)
    # Deliberately left out (still read by the poll): Eol (factory test voltages) and
    # TimeAndDate (the unit's clock), which change all the time and are of no use here.
]

# Aventa 2 (observed): 0x0101 = built-in "iNet X Interface AC", 0x0801 = the air conditioner.
# (iNet X panel setups use 0x0101 = panel, 0x0201 = heater.) Addresses seen in INFO messages
# are queried automatically, so this list only speeds up the first connection.
DEFAULT_DISCOVERY_ADDRESSES = [0x0101, 0x0801]

# dBm values accepted by every ESP32 variant (converted to esp_power_level_t in C++)
TX_POWER_LEVELS = [-12, -9, -6, -3, 0, 3, 6, 9]


def validate_tx_power(value):
    """Bluetooth TX power in dBm (-12..9, steps of 3), or "default" to keep ESP-IDF's (+3 dBm)."""
    if value is False or (
        isinstance(value, str) and value.strip().lower() in ("default", "none", "false")
    ):
        return None
    if isinstance(value, str):
        value = value.strip().lower()
        for suffix in ("dbm", "db"):
            if value.endswith(suffix):
                value = value[: -len(suffix)].strip()
                break
    value = cv.int_(value)
    if value not in TX_POWER_LEVELS:
        raise cv.Invalid(
            f"tx_power must be one of {', '.join(str(v) for v in TX_POWER_LEVELS)} dBm"
        )
    return value


# Stable namespace so that the generated identity never changes for a given device name.
IDENTITY_NAMESPACE = uuid.UUID("6f0b6c3e-2d1a-4b8e-9c55-7d3f0e2a9b41")


def validate_pin(value):
    """6-digit Bluetooth passkey; empty / "none" means no PIN (handy with package vars)."""
    if value is None or str(value).strip().lower() in ("", "none", "no", "false"):
        return None
    return cv.int_range(min=0, max=999999)(cv.int_(value))


def validate_uuid(value):
    value = cv.string_strict(value)
    try:
        return str(uuid.UUID(value))
    except ValueError as err:
        raise cv.Invalid(f"Invalid UUID: {value}") from err


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TrumaInetX),
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_PIN): validate_pin,
            cv.Optional(CONF_ENCRYPTION, default=True): cv.boolean,
            cv.Optional(CONF_USER_NAME, default="ESPHome"): cv.string_strict,
            cv.Optional(CONF_MUID): validate_uuid,
            cv.Optional(CONF_UUID): validate_uuid,
            cv.Optional(CONF_SEND_IDENTITY, default=True): cv.boolean,
            cv.Optional(CONF_TOPICS, default=DEFAULT_TOPICS): cv.All(
                cv.ensure_list(cv.string_strict), cv.Length(min=1)
            ),
            cv.Optional(
                CONF_DISCOVERY_ADDRESSES, default=DEFAULT_DISCOVERY_ADDRESSES
            ): cv.ensure_list(cv.hex_uint16_t),
            cv.Optional(CONF_DEFAULT_DESTINATION, default=0x0101): cv.hex_uint16_t,
            cv.Optional(CONF_DESTINATIONS, default={}): cv.Schema(
                {cv.string_strict: cv.hex_uint16_t}
            ),
            cv.Optional(CONF_AUTO_DISCOVERY, default=True): cv.boolean,
            cv.Optional(CONF_OPTIMISTIC, default=True): cv.boolean,
            cv.Optional(CONF_LOG_FRAMES, default=False): cv.boolean,
            cv.Optional(
                CONF_FRAME_DELAY, default="100ms"
            ): cv.positive_time_period_milliseconds,
            # Addresses: Bluedroid resolves the rotating private addresses (RPA) of a
            # bonded device by itself; the address it reports after a reboot is the one
            # stored with the bond, remembered here so the link survives power cuts.
            cv.Optional(CONF_REMEMBER_ADDRESS, default=True): cv.boolean,
            # Fallback for a device changing address without bonding: follow its name.
            cv.Optional(CONF_DEVICE_NAME): cv.string_strict,
            cv.Optional(CONF_LOG_ADVERTISEMENTS, default=True): cv.boolean,
            # Re-read every parameter periodically, in case the device does not push changes
            # made with its remote ("never" to disable).
            cv.Optional(CONF_POLL_INTERVAL, default="60s"): cv.update_interval,
            # Bluetooth TX power of the ESP32 in dBm (whole BLE radio, proxy included). Not set:
            # ESP-IDF default (+3 dBm). Raise it if the unit does not answer connection requests
            # (HCI error 0x3E); 9 dBm is the maximum.
            cv.Optional(CONF_TX_POWER): validate_tx_power,
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# Shared by every entity platform
TRUMA_INETX_CHILD_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TRUMA_INETX_ID): cv.use_id(TrumaInetX),
    }
)
TOPIC_PARAMETER_SCHEMA = TRUMA_INETX_CHILD_SCHEMA.extend(
    {
        cv.Required(CONF_TOPIC): cv.string_strict,
        cv.Required(CONF_PARAMETER): cv.string_strict,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)

    # Clock of the unit: the time_id given, otherwise the first time source of the configuration.
    time_id = config.get(CONF_TIME_ID)
    if time_id is None and (clocks := CORE.config.get("time")):
        time_id = clocks[0][CONF_ID]
    if time_id is not None:
        clock = await cg.get_variable(time_id)
        cg.add(var.set_time(clock))

    if config.get(CONF_PIN) is not None:
        cg.add(var.set_pin(config[CONF_PIN]))
    cg.add(var.set_encryption(config[CONF_ENCRYPTION]))

    # A stable identity is required: the device remembers clients by Muid/Uuid.
    muid = config.get(CONF_MUID) or str(
        uuid.uuid5(IDENTITY_NAMESPACE, f"{CORE.name}:muid")
    )
    app_uuid = config.get(CONF_UUID) or str(
        uuid.uuid5(IDENTITY_NAMESPACE, f"{CORE.name}:uuid")
    )
    cg.add(var.set_user_name(config[CONF_USER_NAME]))
    cg.add(var.set_identity(muid.upper(), app_uuid.lower()))
    cg.add(var.set_send_identity(config[CONF_SEND_IDENTITY]))

    for topic in config[CONF_TOPICS]:
        cg.add(var.add_topic(topic))
    for address in config[CONF_DISCOVERY_ADDRESSES]:
        cg.add(var.add_discovery_address(address))
    cg.add(var.set_default_destination(config[CONF_DEFAULT_DESTINATION]))
    for topic, address in config[CONF_DESTINATIONS].items():
        cg.add(var.set_topic_destination(topic, address))

    cg.add(var.set_auto_discovery(config[CONF_AUTO_DISCOVERY]))
    cg.add(var.set_optimistic(config[CONF_OPTIMISTIC]))
    cg.add(var.set_log_frames(config[CONF_LOG_FRAMES]))
    cg.add(var.set_frame_delay(config[CONF_FRAME_DELAY].total_milliseconds))
    cg.add(var.set_remember_address(config[CONF_REMEMBER_ADDRESS]))
    cg.add(var.set_log_advertisements(config[CONF_LOG_ADVERTISEMENTS]))
    poll_ms = config[CONF_POLL_INTERVAL].total_milliseconds
    cg.add(var.set_poll_interval(0 if poll_ms >= SCHEDULER_DONT_RUN else poll_ms))
    if CONF_DEVICE_NAME in config:
        cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    if (tx_power := config.get(CONF_TX_POWER)) is not None:
        cg.add(var.set_tx_power(tx_power))
