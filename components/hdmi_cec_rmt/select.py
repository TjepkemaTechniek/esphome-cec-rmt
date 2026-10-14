import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_NAME

from . import CONF_HDMI_CEC_RMT_ID, CecDevice, HdmiCecRmt, get_device_name, hdmi_cec_rmt_ns

DEPENDENCIES = ["hdmi_cec_rmt"]

CONF_DEVICES = "devices"
CONF_INPUTS = "inputs"
CONF_DEVICE = "device"
CONF_PHYSICAL_ADDRESS = "physical_address"
CONF_STANDBY_PREVIOUS_SOURCE = "standby_previous_source"

CecSourceSelect = hdmi_cec_rmt_ns.class_("CecSourceSelect", select.Select)

INPUT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Required(CONF_PHYSICAL_ADDRESS): cv.hex_uint16_t,
        # Optional: the CEC device on this input (woken when selected, recognised as active)
        cv.Optional(CONF_DEVICE): cv.use_id(CecDevice),
    }
)


def validate(config):
    if not config.get(CONF_DEVICES) and not config.get(CONF_INPUTS):
        raise cv.Invalid(f"Specify '{CONF_DEVICES}' and/or '{CONF_INPUTS}'")
    return config


CONFIG_SCHEMA = cv.All(
    select.select_schema(CecSourceSelect, icon="mdi:video-input-hdmi").extend(
        {
            cv.GenerateID(CONF_HDMI_CEC_RMT_ID): cv.use_id(HdmiCecRmt),
            cv.Optional(CONF_DEVICES, default=[]): cv.ensure_list(cv.use_id(CecDevice)),
            cv.Optional(CONF_INPUTS, default=[]): cv.ensure_list(INPUT_SCHEMA),
            cv.Optional(CONF_STANDBY_PREVIOUS_SOURCE, default=False): cv.boolean,
        }
    ),
    validate,
)


async def to_code(config):
    options = [get_device_name(dev_id) for dev_id in config[CONF_DEVICES]]
    options += [inp[CONF_NAME] for inp in config[CONF_INPUTS]]
    var = await select.new_select(config, options=options)
    parent = await cg.get_variable(config[CONF_HDMI_CEC_RMT_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_source_select(var))
    cg.add(var.set_standby_previous_source(config[CONF_STANDBY_PREVIOUS_SOURCE]))
    for dev_id in config[CONF_DEVICES]:
        dev = await cg.get_variable(dev_id)
        cg.add(var.add_device(dev))
    for inp in config[CONF_INPUTS]:
        dev = cg.nullptr
        if CONF_DEVICE in inp:
            dev = await cg.get_variable(inp[CONF_DEVICE])
        cg.add(var.add_input(inp[CONF_NAME], inp[CONF_PHYSICAL_ADDRESS], dev))
