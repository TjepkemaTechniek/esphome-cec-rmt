import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_UPDATE_INTERVAL,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
)

from . import CONF_HDMI_CEC_RMT_ID, HdmiCecRmt

DEPENDENCIES = ["hdmi_cec_rmt"]

# key -> setter on HdmiCecRmt
SENSORS = {
    "frames_received": "set_frames_received_sensor",
    "frames_sent": "set_frames_sent_sensor",
    "tx_nack": "set_tx_nack_sensor",
    "tx_arbitration_lost": "set_tx_arbitration_lost_sensor",
    "tx_errors": "set_tx_errors_sensor",
    "rx_errors": "set_rx_errors_sensor",
    "acks_sent": "set_acks_sent_sensor",
    "rx_ack_missed": "set_rx_ack_missed_sensor",
    "watchdog_releases": "set_watchdog_sensor",
}


def _counter_schema():
    return sensor.sensor_schema(
        icon="mdi:counter",
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HDMI_CEC_RMT_ID): cv.use_id(HdmiCecRmt),
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.update_interval,
        **{cv.Optional(key): _counter_schema() for key in SENSORS},
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HDMI_CEC_RMT_ID])
    cg.add(parent.set_stats_interval(config[CONF_UPDATE_INTERVAL]))
    for key, setter in SENSORS.items():
        if conf := config.get(key):
            sens = await sensor.new_sensor(conf)
            cg.add(getattr(parent, setter)(sens))
