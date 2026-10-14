import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_DEVICE, CONF_HDMI_CEC_RMT_ID, CecDevice, HdmiCecRmt

DEPENDENCIES = ["hdmi_cec_rmt"]

CONF_OSD_NAME = "osd_name"
CONF_PHYSICAL_ADDRESS = "physical_address"
CONF_VENDOR_ID = "vendor_id"
CONF_ACTIVE_SOURCE = "active_source"
CONF_LAST_MESSAGE = "last_message"

DEVICE_KEYS = (CONF_OSD_NAME, CONF_PHYSICAL_ADDRESS, CONF_VENDOR_ID)


def validate(config):
    has_device_keys = any(k in config for k in DEVICE_KEYS)
    if has_device_keys and CONF_DEVICE not in config:
        raise cv.Invalid(f"'{CONF_DEVICE}' is required for {', '.join(DEVICE_KEYS)}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_HDMI_CEC_RMT_ID): cv.use_id(HdmiCecRmt),
            cv.Optional(CONF_DEVICE): cv.use_id(CecDevice),
            cv.Optional(CONF_OSD_NAME): text_sensor.text_sensor_schema(
                icon="mdi:rename"
            ),
            cv.Optional(CONF_PHYSICAL_ADDRESS): text_sensor.text_sensor_schema(
                icon="mdi:video-input-hdmi",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_VENDOR_ID): text_sensor.text_sensor_schema(
                icon="mdi:factory",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_ACTIVE_SOURCE): text_sensor.text_sensor_schema(
                icon="mdi:video-input-hdmi"
            ),
            cv.Optional(CONF_LAST_MESSAGE): text_sensor.text_sensor_schema(
                icon="mdi:message-text",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ),
    validate,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HDMI_CEC_RMT_ID])
    if CONF_DEVICE in config:
        dev = await cg.get_variable(config[CONF_DEVICE])
        if conf := config.get(CONF_OSD_NAME):
            cg.add(dev.set_osd_name_text_sensor(await text_sensor.new_text_sensor(conf)))
        if conf := config.get(CONF_PHYSICAL_ADDRESS):
            cg.add(
                dev.set_physical_address_text_sensor(
                    await text_sensor.new_text_sensor(conf)
                )
            )
        if conf := config.get(CONF_VENDOR_ID):
            cg.add(dev.set_vendor_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_ACTIVE_SOURCE):
        cg.add(
            parent.set_active_source_text_sensor(await text_sensor.new_text_sensor(conf))
        )
    if conf := config.get(CONF_LAST_MESSAGE):
        cg.add(
            parent.set_last_message_text_sensor(await text_sensor.new_text_sensor(conf))
        )
