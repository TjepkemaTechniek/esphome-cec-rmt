import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_DEVICE, CecDevice

DEPENDENCIES = ["hdmi_cec_rmt"]

CONF_PRESENT = "present"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_DEVICE): cv.use_id(CecDevice),
        cv.Optional(CONF_PRESENT): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
    }
)


async def to_code(config):
    dev = await cg.get_variable(config[CONF_DEVICE])
    if conf := config.get(CONF_PRESENT):
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(dev.set_present_binary_sensor(sens))
