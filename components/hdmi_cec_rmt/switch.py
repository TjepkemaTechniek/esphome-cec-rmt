import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv

from . import CONF_DEVICE, CecDevice, hdmi_cec_rmt_ns

DEPENDENCIES = ["hdmi_cec_rmt"]

CecPowerSwitch = hdmi_cec_rmt_ns.class_("CecPowerSwitch", switch.Switch)

# Restore is disabled on purpose: restoring would send Standby / power on at every boot.
CONFIG_SCHEMA = switch.switch_schema(
    CecPowerSwitch, icon="mdi:power", default_restore_mode="DISABLED"
).extend({cv.Required(CONF_DEVICE): cv.use_id(CecDevice)})


async def to_code(config):
    var = await switch.new_switch(config)
    dev = await cg.get_variable(config[CONF_DEVICE])
    cg.add(var.set_device(dev))
