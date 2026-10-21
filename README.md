# esphome-cec-rmt

HDMI-CEC for ESPHome on ESP32 (ESP-IDF 5.5+), with proper ACK handling.

- **TX** – an RMT TX channel generates the bit waveforms in hardware.
- **RX** – a GPIO any-edge ISR timestamps edges and decodes bits/frames.
- **ACK** – on the falling edge of an ACK slot in a frame addressed to us, the ISR takes
  the pin away from RMT (one GPIO-matrix register write) and pulls it low; a GPTimer
  alarm hands it back to RMT at 1.5 ms. The same trick aborts our own transmission on
  lost arbitration or a NACK.
- The pin is open-drain with input enabled, so every transmission is read back: ACK/NACK
  detection, arbitration and readback verification come for free.
- **Passive by default** – it never polls the bus. Devices, names, power state and the active
  source are learned from the traffic between the other devices. It ACKs and answers what is
  addressed to it, and only transmits for commands you give.
- **Devices are found by who they are**, not by a fixed logical address: an Apple TV is the
  Apple TV whether it ends up at playback 1, 2 or 3.

Developed on ESP32-S3; the code only uses driver/HAL APIs that also exist on ESP32-P4.

## Hardware

| HDMI pin | Connect to |
|---|---|
| 13 (CEC) | GPIO (e.g. GPIO17) + **27 kΩ pull-up to 3.3 V** |
| 17 (DDC/CEC ground) | GND |

Avoid strapping pins (0, 3, 45, 46 on the S3).

## Usage

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

external_components:
  - source: github://TjepkemaTechniek/esphome-cec-rmt@main
    components: [hdmi_cec_rmt]

hdmi_cec_rmt:
  pin: GPIO17
  logical_address: 14          # 14 = "Specific use"
  physical_address: 0x3000     # 3.0.0.0
  osd_name: "Home Assistant"
  devices:
    - id: tv
      logical_address: 0
      name: TV
    - id: apple_tv               # no logical_address: found on the bus
      name: Apple TV
      vendor_id: 0x0010FA
      osd_name: Apple TV
```

A complete Home Assistant controller config is in [`examples/cec-controller.yaml`](examples/cec-controller.yaml).

### Options

| Option | Default | |
|---|---|---|
| `pin` | required | Bus pin (open-drain is configured by the component) |
| `logical_address` | `14` | Claimed at boot by polling it; falls back to 15 (unregistered) if taken |
| `physical_address` | required | e.g. `0x3000` = 3.0.0.0 |
| `device_type` | from address | Reported in `<Report Physical Address>` |
| `osd_name` | `ESPHome` | Max 14 chars |
| `vendor_id` | `0` | 24-bit IEEE OUI |
| `promiscuous` | `true` | Decode and report traffic between other devices (needed to track state) |
| `passive` | `true` | Never poll: no boot-time announcements, scans, power polls or state refreshes. Still ACKs and answers requests addressed to us. After a command it only asks the addressed device for its power status |
| `monitor_mode` | `false` | Never drive the bus: no ACK, no TX |
| `scan_interval` | `never` (passive) / `5min` | Full poll of addresses 0–14; also runs at boot |
| `power_poll_interval` | `never` (passive) / `60s` | `<Give Device Power Status>` to configured devices |
| `source_switch_method` | `set_stream_path` | `set_stream_path`, `active_source` (sends it on behalf of the device), or `both` |
| `devices` | `[]` | Devices that get entities: `id`, `name`, and either `logical_address` or any of `vendor_id`, `osd_name`, `device_type` (`tv`, `recorder`, `tuner`, `playback`, `audio_system`) to find it on the bus. Every property that is known must match; the last address is remembered across reboots |
| `on_message` | | Trigger with `source`, `destination`, `data` (opcode first). Filters: `source`, `destination`, `opcode`, `data` |
| `on_device_discovered` | | A device not in `devices:` showed up in a scan: `logical_address`, `physical_address`, `osd_name` |

### Actions

Every action that takes a `destination` (logical address) also takes `device:` (an id from
`devices:`), and `key` takes names as well as codes.

```yaml
- hdmi_cec_rmt.command:         # by name; meant for Home Assistant actions
    device: "Apple TV"          # YAML name, CEC OSD name, "TV", "Playback 1", "all", or 0-15
    command: play               # on, standby, switch_to, power_status, or a key name
- hdmi_cec_rmt.user_control:
    device: apple_tv
    key: select
- hdmi_cec_rmt.send:            # raw frame; data = opcode + operands, [] = poll
    destination: 0
    data: [0x04]
    # source: 4                 # optional, defaults to our own address
- hdmi_cec_rmt.power_on:        # TV: <Image View On>; others: <Power On Function> key
    destination: 4
- hdmi_cec_rmt.standby:         # 15 = everything
    destination: 15
- hdmi_cec_rmt.set_active_source:
    destination: 4
- hdmi_cec_rmt.user_control:    # press + release
    destination: 5
    key: 0x41                   # volume up
- hdmi_cec_rmt.request_power_status:
    destination: 0
- hdmi_cec_rmt.scan:
```

All `destination`/`key`/`data`/`device`/`command` fields accept lambdas.

Key names: `select` (`ok`), `up`, `down`, `left`, `right`, `home` (`root_menu`), `setup_menu`,
`contents_menu`, `back` (`exit`, `menu`), `number_0`–`number_9`, `enter`, `channel_up`,
`channel_down`, `info`, `power`, `volume_up`, `volume_down`, `mute`, `play`, `stop`, `pause`,
`record`, `rewind`, `fast_forward`, `next`, `previous`, `power_toggle`, `power_off`, `power_on`,
`red`, `green`, `yellow`, `blue`, and a few more (see `KEYS` in `__init__.py`).

### Home Assistant

The example config exposes one action, `cec_command(device, command)`, and fires
`esphome.hdmi_cec_message` for every frame with `source_name`, `destination_name`, `command`
(e.g. `Active Source`, `Standby`) and `key` (for remote keys). For example:

```yaml
# Pause the Apple TV when the doorbell rings
actions:
  - action: esphome.cec_controller_cec_command
    data: {device: "Apple TV", command: pause}

# Do something when the Apple TV becomes the active source
triggers:
  - trigger: event
    event_type: esphome.hdmi_cec_message
    event_data: {source_name: "Apple TV", command: "Active Source"}
```

### Entities

| Platform | Keys |
|---|---|
| `switch` | `device:` — power on/off, state from `<Report Power Status>` |
| `binary_sensor` | `device:`, `present:` |
| `text_sensor` | per device: `osd_name`, `physical_address`, `vendor_id`; bus: `active_source`, `last_message` |
| `select` | `devices: [...]` and/or `inputs: [{name, physical_address, device?}]`, `standby_previous_source` — routes the receiver/TV with <Set Stream Path>; works for inputs without CEC; follows the bus |
| `sensor` | `frames_received`, `frames_sent`, `tx_nack`, `tx_arbitration_lost`, `tx_errors`, `rx_errors`, `acks_sent`, `rx_ack_missed` |

## Debugging

With `logger: level: DEBUG` every frame is logged, decoded:

```
[D][hdmi_cec_rmt]: RX 4->F  4F:82:10:00              Active Source
[D][hdmi_cec_rmt]: TX E->0  E0:8F                    Give Device Power Status  [ACK]
```

and after each scan a table of all devices on the bus. `rx_ack_missed` > 0 means we were
too late to ACK a frame addressed to us.
