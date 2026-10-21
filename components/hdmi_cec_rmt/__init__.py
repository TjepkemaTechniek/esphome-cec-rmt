"""HDMI-CEC for ESP32 using RMT for transmission and a GPIO ISR + GPTimer for receive/ACK."""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA,
    CONF_ID,
    CONF_NAME,
    CONF_PIN,
    CONF_SOURCE,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE

CODEOWNERS = ["@TjepkemaTechniek"]
DEPENDENCIES = ["esp32"]

CONF_HDMI_CEC_RMT_ID = "hdmi_cec_rmt_id"
CONF_LOGICAL_ADDRESS = "logical_address"
CONF_PHYSICAL_ADDRESS = "physical_address"
CONF_DEVICE_TYPE = "device_type"
CONF_OSD_NAME = "osd_name"
CONF_VENDOR_ID = "vendor_id"
CONF_PROMISCUOUS = "promiscuous"
CONF_MONITOR_MODE = "monitor_mode"
CONF_PASSIVE = "passive"
CONF_COMMAND = "command"
CONF_SCAN_INTERVAL = "scan_interval"
CONF_POWER_POLL_INTERVAL = "power_poll_interval"
CONF_STATS_INTERVAL = "stats_interval"
CONF_SOURCE_SWITCH_METHOD = "source_switch_method"
CONF_DEVICES = "devices"
CONF_DEVICE = "device"
CONF_ON_MESSAGE = "on_message"
CONF_ON_DEVICE_DISCOVERED = "on_device_discovered"
CONF_DESTINATION = "destination"
CONF_OPCODE = "opcode"
CONF_KEY = "key"

hdmi_cec_rmt_ns = cg.esphome_ns.namespace("hdmi_cec_rmt")
HdmiCecRmt = hdmi_cec_rmt_ns.class_("HdmiCecRmt", cg.Component)
CecDevice = hdmi_cec_rmt_ns.class_("CecDevice")
MessageTrigger = hdmi_cec_rmt_ns.class_(
    "MessageTrigger",
    automation.Trigger.template(cg.uint8, cg.uint8, cg.std_vector.template(cg.uint8)),
)
DeviceDiscoveredTrigger = hdmi_cec_rmt_ns.class_(
    "DeviceDiscoveredTrigger",
    automation.Trigger.template(cg.uint8, cg.uint16, cg.std_string),
)

SendAction = hdmi_cec_rmt_ns.class_("SendAction", automation.Action)
PowerOnAction = hdmi_cec_rmt_ns.class_("PowerOnAction", automation.Action)
StandbyAction = hdmi_cec_rmt_ns.class_("StandbyAction", automation.Action)
SetActiveSourceAction = hdmi_cec_rmt_ns.class_("SetActiveSourceAction", automation.Action)
UserControlAction = hdmi_cec_rmt_ns.class_("UserControlAction", automation.Action)
RequestPowerStatusAction = hdmi_cec_rmt_ns.class_(
    "RequestPowerStatusAction", automation.Action
)
ScanAction = hdmi_cec_rmt_ns.class_("ScanAction", automation.Action)
CommandAction = hdmi_cec_rmt_ns.class_("CommandAction", automation.Action)
SetStreamPathAction = hdmi_cec_rmt_ns.class_("SetStreamPathAction", automation.Action)
DumpHistoryAction = hdmi_cec_rmt_ns.class_("DumpHistoryAction", automation.Action)

SourceSwitchMethod = hdmi_cec_rmt_ns.enum("SourceSwitchMethod", is_class=True)
SOURCE_SWITCH_METHODS = {
    "set_stream_path": SourceSwitchMethod.SET_STREAM_PATH,
    "active_source": SourceSwitchMethod.ACTIVE_SOURCE,
    "both": SourceSwitchMethod.BOTH,
}

uint4 = cv.int_range(min=0, max=15)
byte = cv.hex_uint8_t

# <User Control Pressed> key names. Keep in sync with KEYS in hdmi_cec_rmt.cpp.
KEYS = {
    "select": 0x00, "ok": 0x00, "up": 0x01, "down": 0x02, "left": 0x03, "right": 0x04,
    "root_menu": 0x09, "home": 0x09, "setup_menu": 0x0A, "contents_menu": 0x0B,
    "favorite_menu": 0x0C, "back": 0x0D, "exit": 0x0D, "menu": 0x0D,
    **{f"number_{i}": 0x20 + i for i in range(10)},
    "enter": 0x2B, "clear": 0x2C, "channel_up": 0x30, "channel_down": 0x31,
    "previous_channel": 0x32, "input_select": 0x34, "info": 0x35, "help": 0x36,
    "page_up": 0x37, "page_down": 0x38, "power": 0x40, "volume_up": 0x41,
    "volume_down": 0x42, "mute": 0x43, "play": 0x44, "stop": 0x45, "pause": 0x46,
    "record": 0x47, "rewind": 0x48, "fast_forward": 0x49, "eject": 0x4A,
    "next": 0x4B, "forward": 0x4B, "previous": 0x4C, "backward": 0x4C,
    "subtitle": 0x51, "epg": 0x53, "play_function": 0x60, "pause_play": 0x61,
    "stop_function": 0x64, "mute_function": 0x65, "restore_volume": 0x66,
    "select_media": 0x68, "select_av_input": 0x69, "select_audio_input": 0x6A,
    "power_toggle": 0x6B, "power_off": 0x6C, "power_on": 0x6D,
    "blue": 0x71, "red": 0x72, "green": 0x73, "yellow": 0x74,
}


def validate_key(value):
    """A key code (0x44) or a key name ("play", "volume up")."""
    if isinstance(value, str):
        name = value.strip().lower().replace(" ", "_").replace("-", "_")
        if name in KEYS:
            return KEYS[name]
        try:
            return byte(value)
        except cv.Invalid:
            raise cv.Invalid(
                f"Unknown key '{value}'. Use a code (e.g. 0x44) or one of: {', '.join(sorted(KEYS))}"
            ) from None
    return byte(value)


def validate_osd_name(value):
    value = cv.string_strict(value)
    if len(value) > 14:
        raise cv.Invalid("OSD name can be at most 14 characters")
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in value):
        raise cv.Invalid("OSD name must be printable ASCII")
    return value


def validate_payload(value):
    if isinstance(value, list):
        value = [byte(v) for v in value]
        if len(value) > 15:
            raise cv.Invalid("A CEC frame carries at most 15 bytes after the header")
        return value
    raise cv.Invalid("Expected a list of bytes")


def validate_device(config):
    if CONF_LOGICAL_ADDRESS in config:
        return config
    if not any(k in config for k in (CONF_VENDOR_ID, CONF_OSD_NAME, CONF_DEVICE_TYPE)):
        raise cv.Invalid(
            f"Give '{CONF_LOGICAL_ADDRESS}', or let the device be found on the bus by "
            f"'{CONF_VENDOR_ID}', '{CONF_OSD_NAME}' and/or '{CONF_DEVICE_TYPE}'"
        )
    return config


DEVICE_TYPES = {
    "tv": 0,
    "recorder": 1,
    "tuner": 3,
    "playback": 4,
    "audio_system": 5,
    "switch": 6,
    "processor": 7,
}

DEVICE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.declare_id(CecDevice),
            cv.Required(CONF_NAME): cv.string_strict,
            # Fixed address, or omit it and match the device by what it says about itself:
            cv.Optional(CONF_LOGICAL_ADDRESS): cv.int_range(min=0, max=14),
            cv.Optional(CONF_VENDOR_ID): cv.int_range(min=0, max=0xFFFFFF),
            cv.Optional(CONF_OSD_NAME): cv.string_strict,
            cv.Optional(CONF_DEVICE_TYPE): cv.enum(DEVICE_TYPES, lower=True, space="_"),
        }
    ),
    validate_device,
)


def validate_devices(config):
    seen = {}
    for dev in config.get(CONF_DEVICES, []):
        if CONF_LOGICAL_ADDRESS not in dev:
            continue
        la = dev[CONF_LOGICAL_ADDRESS]
        if la in seen:
            raise cv.Invalid(
                f"Logical address {la} is used by both '{seen[la]}' and '{dev[CONF_NAME]}'"
            )
        seen[la] = dev[CONF_NAME]
        if la == config[CONF_LOGICAL_ADDRESS]:
            raise cv.Invalid(
                f"Device '{dev[CONF_NAME]}' uses logical address {la}, which is this controller's own address"
            )
    return config


def apply_passive_defaults(config):
    passive = config[CONF_PASSIVE]
    config.setdefault(CONF_SCAN_INTERVAL, cv.update_interval("never" if passive else "5min"))
    config.setdefault(CONF_POWER_POLL_INTERVAL, cv.update_interval("never" if passive else "60s"))
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HdmiCecRmt),
            cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_LOGICAL_ADDRESS, default=14): uint4,
            cv.Required(CONF_PHYSICAL_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_DEVICE_TYPE): cv.int_range(min=0, max=7),
            cv.Optional(CONF_OSD_NAME, default="ESPHome"): validate_osd_name,
            cv.Optional(CONF_VENDOR_ID, default=0): cv.int_range(min=0, max=0xFFFFFF),
            cv.Optional(CONF_PROMISCUOUS, default=True): cv.boolean,
            cv.Optional(CONF_MONITOR_MODE, default=False): cv.boolean,
            # Passive: never poll; learn everything from bus traffic, answer when asked, and only
            # transmit for actions you trigger. Scans and power polls default to off.
            cv.Optional(CONF_PASSIVE, default=True): cv.boolean,
            cv.Optional(CONF_SCAN_INTERVAL): cv.update_interval,
            cv.Optional(CONF_POWER_POLL_INTERVAL): cv.update_interval,
            cv.Optional(CONF_STATS_INTERVAL, default="60s"): cv.update_interval,
            cv.Optional(CONF_SOURCE_SWITCH_METHOD, default="set_stream_path"): cv.enum(
                SOURCE_SWITCH_METHODS, lower=True
            ),
            cv.Optional(CONF_DEVICES, default=[]): cv.ensure_list(DEVICE_SCHEMA),
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MessageTrigger),
                    cv.Optional(CONF_SOURCE): uint4,
                    cv.Optional(CONF_DESTINATION): uint4,
                    cv.Optional(CONF_OPCODE): byte,
                    cv.Optional(CONF_DATA): validate_payload,
                }
            ),
            cv.Optional(CONF_ON_DEVICE_DISCOVERED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        DeviceDiscoveredTrigger
                    ),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.require_framework_version(esp_idf=cv.Version(5, 5, 0)),
    validate_devices,
    apply_passive_defaults,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))
    cg.add(var.set_logical_address(config[CONF_LOGICAL_ADDRESS]))
    cg.add(var.set_physical_address(config[CONF_PHYSICAL_ADDRESS]))
    if CONF_DEVICE_TYPE in config:
        cg.add(var.set_device_type(config[CONF_DEVICE_TYPE]))
    cg.add(var.set_osd_name(config[CONF_OSD_NAME]))
    cg.add(var.set_vendor_id(config[CONF_VENDOR_ID]))
    cg.add(var.set_promiscuous(config[CONF_PROMISCUOUS]))
    cg.add(var.set_monitor_mode(config[CONF_MONITOR_MODE]))
    cg.add(var.set_passive(config[CONF_PASSIVE]))
    cg.add(var.set_scan_interval(config[CONF_SCAN_INTERVAL]))
    cg.add(var.set_power_poll_interval(config[CONF_POWER_POLL_INTERVAL]))
    cg.add(var.set_stats_interval(config[CONF_STATS_INTERVAL]))
    cg.add(var.set_source_switch_method(config[CONF_SOURCE_SWITCH_METHOD]))

    for dev_conf in config[CONF_DEVICES]:
        dev = cg.new_Pvariable(
            dev_conf[CONF_ID], dev_conf.get(CONF_LOGICAL_ADDRESS, 0xFF), dev_conf[CONF_NAME]
        )
        if CONF_VENDOR_ID in dev_conf:
            cg.add(dev.set_match_vendor_id(dev_conf[CONF_VENDOR_ID]))
        if CONF_OSD_NAME in dev_conf:
            cg.add(dev.set_match_osd_name(dev_conf[CONF_OSD_NAME]))
        if CONF_DEVICE_TYPE in dev_conf:
            cg.add(dev.set_match_device_type(dev_conf[CONF_DEVICE_TYPE]))
        cg.add(var.add_device(dev))

    for conf in config.get(CONF_ON_MESSAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        if CONF_SOURCE in conf:
            cg.add(trigger.set_source(conf[CONF_SOURCE]))
        if CONF_DESTINATION in conf:
            cg.add(trigger.set_destination(conf[CONF_DESTINATION]))
        if CONF_OPCODE in conf:
            cg.add(trigger.set_opcode(conf[CONF_OPCODE]))
        if CONF_DATA in conf:
            cg.add(trigger.set_data(conf[CONF_DATA]))
        await automation.build_automation(
            trigger,
            [
                (cg.uint8, "source"),
                (cg.uint8, "destination"),
                (cg.std_vector.template(cg.uint8), "data"),
            ],
            conf,
        )

    for conf in config.get(CONF_ON_DEVICE_DISCOVERED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [
                (cg.uint8, "logical_address"),
                (cg.uint16, "physical_address"),
                (cg.std_string, "osd_name"),
            ],
            conf,
        )

    # The ACK-release timer must keep running while flash is busy (NVS writes, OTA),
    # otherwise an ACK pulse could hold the bus low for the duration of a flash erase.
    esp32.add_idf_sdkconfig_option("CONFIG_GPTIMER_ISR_CACHE_SAFE", True)
    esp32.add_idf_sdkconfig_option("CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM", True)
    # Keep the RMT ping-pong refill running during flash writes too.
    esp32.add_idf_sdkconfig_option("CONFIG_RMT_TX_ISR_CACHE_SAFE", True)


# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------

# Destination: a logical address (`destination: 4`) or a device from `devices:` (`device: apple_tv`).
DESTINATION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(HdmiCecRmt),
        cv.Exclusive(CONF_DESTINATION, "destination"): cv.templatable(uint4),
        cv.Exclusive(CONF_DEVICE, "destination"): cv.use_id(CecDevice),
    }
)


def _require_destination(config):
    if CONF_DESTINATION not in config and CONF_DEVICE not in config:
        raise cv.Invalid(f"Specify '{CONF_DESTINATION}' or '{CONF_DEVICE}'")
    return config


async def _destination_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_DEVICE in config:
        cg.add(var.set_device(await cg.get_variable(config[CONF_DEVICE])))
    else:
        templ = await cg.templatable(config[CONF_DESTINATION], args, cg.uint8)
        cg.add(var.set_destination(templ))
    return var


@automation.register_action(
    "hdmi_cec_rmt.send",
    SendAction,
    cv.All(
        DESTINATION_SCHEMA.extend(
            {
                cv.Optional(CONF_SOURCE): cv.templatable(uint4),
                cv.Required(CONF_DATA): cv.templatable(validate_payload),
            }
        ),
        _require_destination,
    ),
    synchronous=True,
)
async def send_action_to_code(config, action_id, template_arg, args):
    var = await _destination_action(config, action_id, template_arg, args)
    if CONF_SOURCE in config:
        templ = await cg.templatable(config[CONF_SOURCE], args, cg.uint8)
        cg.add(var.set_source(templ))
        cg.add(var.set_has_source(True))
    data = config[CONF_DATA]
    if cg.is_template(data):
        templ = await cg.templatable(data, args, cg.std_vector.template(cg.uint8))
        cg.add(var.set_data(templ))
    else:
        literal = ", ".join(f"0x{b:02X}" for b in data)
        cg.add(var.set_data(cg.RawExpression(f"std::vector<uint8_t>{{{literal}}}")))
    return var


@automation.register_action(
    "hdmi_cec_rmt.power_on", PowerOnAction, cv.All(DESTINATION_SCHEMA, _require_destination), synchronous=True
)
async def power_on_action_to_code(config, action_id, template_arg, args):
    return await _destination_action(config, action_id, template_arg, args)


@automation.register_action(
    "hdmi_cec_rmt.standby", StandbyAction, cv.All(DESTINATION_SCHEMA, _require_destination), synchronous=True
)
async def standby_action_to_code(config, action_id, template_arg, args):
    return await _destination_action(config, action_id, template_arg, args)


@automation.register_action(
    "hdmi_cec_rmt.set_active_source",
    SetActiveSourceAction,
    cv.All(DESTINATION_SCHEMA, _require_destination),
    synchronous=True,
)
async def set_active_source_action_to_code(config, action_id, template_arg, args):
    return await _destination_action(config, action_id, template_arg, args)


@automation.register_action(
    "hdmi_cec_rmt.request_power_status",
    RequestPowerStatusAction,
    cv.All(DESTINATION_SCHEMA, _require_destination),
    synchronous=True,
)
async def request_power_status_action_to_code(config, action_id, template_arg, args):
    return await _destination_action(config, action_id, template_arg, args)


@automation.register_action(
    "hdmi_cec_rmt.user_control",
    UserControlAction,
    cv.All(
        DESTINATION_SCHEMA.extend({cv.Required(CONF_KEY): cv.templatable(validate_key)}),
        _require_destination,
    ),
    synchronous=True,
)
async def user_control_action_to_code(config, action_id, template_arg, args):
    var = await _destination_action(config, action_id, template_arg, args)
    templ = await cg.templatable(config[CONF_KEY], args, cg.uint8)
    cg.add(var.set_key(templ))
    return var


@automation.register_action(
    "hdmi_cec_rmt.command",
    CommandAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HdmiCecRmt),
            cv.Required(CONF_DEVICE): cv.templatable(cv.string),
            cv.Required(CONF_COMMAND): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def command_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_device(await cg.templatable(config[CONF_DEVICE], args, cg.std_string)))
    cg.add(var.set_command(await cg.templatable(config[CONF_COMMAND], args, cg.std_string)))
    return var


@automation.register_action(
    "hdmi_cec_rmt.set_stream_path",
    SetStreamPathAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HdmiCecRmt),
            cv.Required(CONF_PHYSICAL_ADDRESS): cv.templatable(cv.hex_uint16_t),
        }
    ),
    synchronous=True,
)
async def set_stream_path_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(config[CONF_PHYSICAL_ADDRESS], args, cg.uint16)
    cg.add(var.set_physical_address(templ))
    return var


@automation.register_action(
    "hdmi_cec_rmt.scan",
    ScanAction,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(HdmiCecRmt)}),
    synchronous=True,
)
async def scan_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "hdmi_cec_rmt.dump_history",
    DumpHistoryAction,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(HdmiCecRmt)}),
    synchronous=True,
)
async def dump_history_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


def get_device_name(device_id):
    """Look up the YAML name of a device by its ID (used by the select platform)."""
    for dev in CORE.config.get("hdmi_cec_rmt", {}).get(CONF_DEVICES, []):
        if dev[CONF_ID].id == device_id.id:
            return dev[CONF_NAME]
    raise cv.Invalid(f"Unknown CEC device '{device_id}'")
