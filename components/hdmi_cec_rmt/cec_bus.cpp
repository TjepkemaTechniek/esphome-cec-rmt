#ifdef USE_ESP32

#include "cec_bus.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/soc_caps.h"

namespace esphome {
namespace hdmi_cec_rmt {

static const char *const TAG = "hdmi_cec_rmt.bus";

// ---- CEC timing (microseconds), HDMI 1.4b CEC spec section 5.2 ----
// Transmit (nominal)
static constexpr uint32_t TX_START_LOW = 3700;
static constexpr uint32_t TX_START_HIGH = 800;
static constexpr uint32_t TX_ZERO_LOW = 1500;
static constexpr uint32_t TX_ZERO_HIGH = 900;
static constexpr uint32_t TX_ONE_LOW = 600;
static constexpr uint32_t TX_ONE_HIGH = 1800;
static constexpr uint32_t ACK_LOW = 1500;  // follower holds the ACK bit low until here
// Receive (tolerant)
static constexpr int64_t RX_GLITCH = 100;          // shorter pulses are ignored
static constexpr int64_t RX_SAMPLE_POINT = 1050;   // low < this -> '1', else '0'
static constexpr int64_t RX_ZERO_LOW_MAX = 2050;
static constexpr int64_t RX_START_LOW_MIN = 3300;
static constexpr int64_t RX_START_LOW_MAX = 4100;
static constexpr int64_t RX_BIT_PERIOD_MIN = 1800;   // spec 2050
static constexpr int64_t RX_BIT_PERIOD_MAX = 2900;   // spec 2750
static constexpr int64_t RX_START_PERIOD_MAX = 5000; // spec 4700
static constexpr int64_t RX_STALL = 5000;            // no edges for this long -> frame over
static constexpr int64_t STUCK_LOW = 20000;          // longest legal low is 3.7 ms; 20 ms = something is wrong

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool CecBus::setup(InternalGPIOPin *pin, bool monitor_only) {
  this->pin_ = pin;
  this->gpio_num_ = pin->get_pin();
  this->monitor_only_ = monitor_only;
  auto gpio = static_cast<gpio_num_t>(this->gpio_num_);

  // Self-test: sample the line as a plain input with pull-up, before we drive anything.
  {
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << this->gpio_num_;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    esp_rom_delay_us(2000);
    int highs = 0;
    for (int i = 0; i < 20; i++) {
      highs += gpio_ll_get_level(&GPIO, this->gpio_num_);
      esp_rom_delay_us(100);
    }
    this->level_before_driver_ = highs >= 10 ? 1 : 0;
  }

  // Freeze the pad in the released state while the RMT channel is created and primed.
  // On the ESP32-S3 the RMT idle level (init_level) only takes effect once a transmission
  // has started (conf_update), so until then the RMT output idles LOW and would pull the
  // bus down. Holding the pad hides that; the dummy transmission below latches the idle level.
  {
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << this->gpio_num_;
    io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level(gpio, 1);
    gpio_hold_en(gpio);
  }

  // RMT TX channel. init_level=1 keeps the (open-drain) line released while idle.
  rmt_tx_channel_config_t cfg{};
  cfg.gpio_num = gpio;
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = 1000000;  // 1 tick = 1 us
  cfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
  cfg.trans_queue_depth = 1;
  cfg.flags.init_level = 1;
  esp_err_t err = rmt_new_tx_channel(&cfg, &this->rmt_channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
    return false;
  }
  rmt_copy_encoder_config_t enc_cfg{};
  err = rmt_new_copy_encoder(&enc_cfg, &this->rmt_encoder_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
    return false;
  }
  rmt_tx_event_callbacks_t rmt_cbs{};
  rmt_cbs.on_trans_done = CecBus::rmt_done_cb_;
  rmt_tx_register_event_callbacks(this->rmt_channel_, &rmt_cbs, this);
  err = rmt_enable(this->rmt_channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
    gpio_hold_dis(gpio);
    return false;
  }

  // Prime: one all-high symbol makes the hardware latch idle level = high (released).
  {
    static rmt_symbol_word_t prime;
    prime.level0 = 1;
    prime.duration0 = 10;
    prime.level1 = 1;
    prime.duration1 = 10;
    rmt_transmit_config_t pcfg{};
    pcfg.loop_count = 0;
    pcfg.flags.eot_level = 1;
    if (rmt_transmit(this->rmt_channel_, this->rmt_encoder_, &prime, sizeof(prime), &pcfg) == ESP_OK)
      rmt_tx_wait_all_done(this->rmt_channel_, 50);
    this->tx_rmt_done_ = false;
  }

  // Open drain + input so we can read the bus (and our own transmissions) back.
  gpio_ll_od_enable(&GPIO, this->gpio_num_);
  gpio_input_enable(gpio);
  if (pin->get_flags() & gpio::FLAG_PULLUP) {
    gpio_pullup_en(gpio);
  } else {
    gpio_pullup_dis(gpio);
  }
  gpio_pulldown_dis(gpio);

  // Two GPIO-matrix configurations for the pin: driven by RMT (normal), or driven
  // by the GPIO_OUT latch (used for ACK and for aborting a transmission).
  this->out_sel_rmt_ = GPIO.func_out_sel_cfg[this->gpio_num_].val;
  this->out_sel_gpio_ = SIG_GPIO_OUT_IDX;
  gpio_ll_set_level(&GPIO, this->gpio_num_, 0);
  gpio_ll_output_enable(&GPIO, this->gpio_num_);

  // RMT now idles high: let the pad follow it.
  gpio_hold_dis(gpio);

  // One-shot timer that releases the ACK pulse.
  gptimer_config_t tcfg{};
  tcfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  tcfg.direction = GPTIMER_COUNT_UP;
  tcfg.resolution_hz = 1000000;
  err = gptimer_new_timer(&tcfg, &this->ack_timer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
    return false;
  }
  gptimer_event_callbacks_t tcbs{};
  tcbs.on_alarm = CecBus::gptimer_alarm_cb_;
  gptimer_register_event_callbacks(this->ack_timer_, &tcbs, this);
  gptimer_enable(this->ack_timer_);

  esp_rom_delay_us(2000);
  this->level_after_driver_ = gpio_ll_get_level(&GPIO, this->gpio_num_);
  this->line_low_ = this->level_after_driver_ == 0;
  this->last_edge_us_ = esp_timer_get_time();
  pin->attach_interrupt(&CecBus::gpio_isr_, this, gpio::INTERRUPT_ANY_EDGE);
  return true;
}

bool CecBus::line_is_low() { return gpio_ll_get_level(&GPIO, this->gpio_num_) == 0; }

const char *CecBus::busy_reason() {
  if (gpio_ll_get_level(&GPIO, this->gpio_num_) == 0)
    return "CEC line is held LOW (missing pull-up, wiring, or a device holding the bus)";
  if (this->tx_active_)
    return "previous transmission still active";
  if (this->ack_driving_)
    return "sending an ACK";
  if (this->rx_state_ != RX_IDLE)
    return "a frame is being received (or the decoder is stuck mid-frame)";
  return "waiting for signal-free time (continuous bus traffic)";
}

// ---------------------------------------------------------------------------
// ISRs
// ---------------------------------------------------------------------------

void IRAM_ATTR CecBus::gpio_isr_(CecBus *bus) {
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&bus->lock_);
  if (gpio_ll_get_level(&GPIO, bus->gpio_num_)) {
    bus->on_rise_(now);
  } else {
    bus->on_fall_(now);
  }
  portEXIT_CRITICAL_ISR(&bus->lock_);
}

bool IRAM_ATTR CecBus::gptimer_alarm_cb_(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata,
                                         void *arg) {
  auto *bus = static_cast<CecBus *>(arg);
  portENTER_CRITICAL_ISR(&bus->lock_);
  bus->release_ack_();
  portEXIT_CRITICAL_ISR(&bus->lock_);
  gptimer_stop(timer);
  return false;
}

bool IRAM_ATTR CecBus::rmt_done_cb_(rmt_channel_handle_t channel, const rmt_tx_done_event_data_t *edata, void *arg) {
  auto *bus = static_cast<CecBus *>(arg);
  bus->tx_rmt_done_ = true;
  return false;
}

// All of the following run inside gpio_isr_ with lock_ held.

void IRAM_ATTR CecBus::on_fall_(int64_t now) {
  if (this->line_low_) {
    // Two falling edges in a row: we missed a rising edge.
    this->rx_error_();
  }
  this->line_low_ = true;
  int64_t since = now - this->last_fall_us_;
  this->prev_fall_us_ = this->last_fall_us_;
  this->prev_state_ = this->rx_state_;
  this->last_fall_us_ = now;
  this->last_edge_us_ = now;

  if (this->rx_state_ == RX_BITS) {
    int64_t max_period = this->after_start_ ? RX_START_PERIOD_MAX : RX_BIT_PERIOD_MAX;
    if (since > max_period) {
      // The previous frame stopped early (initiator gave up after a NACK).
      this->finish_frame_(0);
      this->rx_state_ = RX_START_LOW;
      return;
    }
    if (!this->after_start_ && since < RX_BIT_PERIOD_MIN) {
      this->rx_error_();
      this->rx_state_ = RX_START_LOW;
      return;
    }
    if (this->bit_in_block_ == 9 && this->ack_this_frame_)
      this->assert_ack_(now);
    return;
  }
  this->rx_state_ = RX_START_LOW;
}

void IRAM_ATTR CecBus::on_rise_(int64_t now) {
  if (!this->line_low_)
    return;
  this->line_low_ = false;
  int64_t low = now - this->last_fall_us_;
  this->last_edge_us_ = now;

  if (low < RX_GLITCH) {
    // Ignore the pulse entirely.
    this->last_fall_us_ = this->prev_fall_us_;
    this->rx_state_ = this->prev_state_;
    return;
  }

  switch (this->rx_state_) {
    case RX_START_LOW:
      if (low >= RX_START_LOW_MIN && low <= RX_START_LOW_MAX) {
        this->start_frame_();
      } else {
        this->stats_.rx_errors++;
        this->rx_state_ = RX_IDLE;
      }
      break;
    case RX_BITS:
      if (low < RX_SAMPLE_POINT) {
        this->handle_bit_(1);
      } else if (low <= RX_ZERO_LOW_MAX) {
        this->handle_bit_(0);
      } else if (low >= RX_START_LOW_MIN && low <= RX_START_LOW_MAX) {
        this->finish_frame_(FRAME_ERROR);
        this->start_frame_();
      } else {
        this->rx_error_();
      }
      break;
    default:
      break;
  }
}

void IRAM_ATTR CecBus::start_frame_() {
  this->rx_state_ = RX_BITS;
  this->bit_in_block_ = 0;
  this->cur_byte_ = 0;
  this->eom_ = 0;
  this->rx_frame_ = CecFrame{};
  this->frame_is_own_ = this->tx_active_ && !this->tx_aborted_;
  this->ack_this_frame_ = false;
  this->after_start_ = true;
}

void IRAM_ATTR CecBus::handle_bit_(uint8_t bit) {
  this->after_start_ = false;
  uint8_t idx = this->rx_frame_.len;  // index of the block being received
  bool checking = this->frame_is_own_ && this->tx_fail_ == 0;

  if (this->bit_in_block_ < 8) {
    if (checking && idx < this->tx_frame_.len) {
      uint8_t expected = (this->tx_frame_.data[idx] >> (7 - this->bit_in_block_)) & 1;
      if (expected != bit) {
        if (idx == 0 && this->bit_in_block_ < 4 && expected == 1) {
          // Another initiator with a lower address is transmitting: back off.
          this->tx_fail_ = static_cast<uint8_t>(TxResult::ARB_LOST) + 1;
          this->stats_.tx_arbitration_lost++;
          this->frame_is_own_ = false;  // keep decoding it as their frame
        } else {
          this->tx_fail_ = static_cast<uint8_t>(TxResult::ERROR) + 1;
        }
        this->abort_tx_();
      }
    }
    this->cur_byte_ = static_cast<uint8_t>((this->cur_byte_ << 1) | bit);
    if (this->bit_in_block_ == 7) {
      this->rx_frame_.data[idx] = this->cur_byte_;
      if (idx == 0) {
        uint8_t dest = this->cur_byte_ & 0x0F;
        uint8_t la = this->logical_address_;
        this->ack_this_frame_ = !this->monitor_only_ && !this->frame_is_own_ && la != CEC_BROADCAST && dest == la;
      }
    }
    this->bit_in_block_++;
    return;
  }

  if (this->bit_in_block_ == 8) {
    this->eom_ = bit;
    if (checking && idx < this->tx_frame_.len) {
      uint8_t expected = (idx == this->tx_frame_.len - 1) ? 1 : 0;
      if (expected != bit) {
        this->tx_fail_ = static_cast<uint8_t>(TxResult::ERROR) + 1;
        this->abort_tx_();
      }
    }
    this->bit_in_block_++;
    return;
  }

  // ACK slot
  if (bit)
    this->rx_frame_.ack_bits |= static_cast<uint16_t>(1u << idx);
  this->rx_frame_.len = idx + 1;
  if (this->ack_this_frame_) {
    if (bit) {
      this->stats_.acks_missed++;
    } else {
      this->stats_.acks_sent++;
    }
  }
  if (checking) {
    bool ok = this->rx_frame_.is_broadcast() ? bit == 1 : bit == 0;
    if (!ok) {
      this->tx_fail_ = static_cast<uint8_t>(TxResult::NACK) + 1;
      if (!this->eom_)
        this->abort_tx_();  // don't send the remaining blocks
    }
  }
  this->bit_in_block_ = 0;
  this->cur_byte_ = 0;
  if (this->eom_ || this->rx_frame_.len >= CEC_MAX_BLOCKS)
    this->finish_frame_(this->eom_ ? FRAME_COMPLETE : 0);
}

void IRAM_ATTR CecBus::finish_frame_(uint8_t flags) {
  CecFrame &f = this->rx_frame_;
  f.flags = flags | (this->frame_is_own_ ? FRAME_OWN : 0);
  if (f.len > 0) {
    if (this->frame_is_own_) {
      this->tx_readback_ = f;
      this->tx_readback_done_ = true;
    } else {
      this->stats_.frames_received++;
    }
    this->push_frame_(f);
  }
  this->rx_state_ = RX_IDLE;
  this->frame_is_own_ = false;
  this->ack_this_frame_ = false;
}

void IRAM_ATTR CecBus::rx_error_() {
  this->stats_.rx_errors++;
  if (this->frame_is_own_ && this->tx_fail_ == 0) {
    this->tx_fail_ = static_cast<uint8_t>(TxResult::ERROR) + 1;
    this->abort_tx_();
  }
  if (this->rx_state_ == RX_BITS) {
    this->finish_frame_(FRAME_ERROR);
  } else {
    this->rx_state_ = RX_IDLE;
  }
}

void IRAM_ATTR CecBus::assert_ack_(int64_t edge_time) {
  // The initiator holds the line low for ~0.6 ms. If it is already high again we
  // are too late; pulling now would create a bogus extra bit.
  if (gpio_ll_get_level(&GPIO, this->gpio_num_) != 0)
    return;
  gpio_ll_set_level(&GPIO, this->gpio_num_, 0);
  GPIO.func_out_sel_cfg[this->gpio_num_].val = this->out_sel_gpio_;
  this->ack_driving_ = true;

  int64_t elapsed = esp_timer_get_time() - edge_time;
  uint64_t hold = elapsed < (int64_t) ACK_LOW - 100 ? ACK_LOW - elapsed : 100;
  gptimer_set_raw_count(this->ack_timer_, 0);
  gptimer_alarm_config_t alarm{};
  alarm.alarm_count = hold;
  gptimer_set_alarm_action(this->ack_timer_, &alarm);
  gptimer_start(this->ack_timer_);
}

void IRAM_ATTR CecBus::release_ack_() {
  if (!this->ack_driving_)
    return;
  if (this->tx_aborted_) {
    gpio_ll_set_level(&GPIO, this->gpio_num_, 1);  // stay on the latch, released
  } else {
    GPIO.func_out_sel_cfg[this->gpio_num_].val = this->out_sel_rmt_;
  }
  this->ack_driving_ = false;
}

void IRAM_ATTR CecBus::abort_tx_() {
  if (this->tx_aborted_ || !this->tx_active_)
    return;
  // Take the pin away from RMT and release it. RMT keeps "sending" into nothing
  // until its waveform ends; poll_transmit() hands the pin back afterwards.
  gpio_ll_set_level(&GPIO, this->gpio_num_, 1);
  GPIO.func_out_sel_cfg[this->gpio_num_].val = this->out_sel_gpio_;
  this->tx_aborted_ = true;
}

void IRAM_ATTR CecBus::push_frame_(const CecFrame &frame) {
  uint8_t next = (this->ring_head_ + 1) % RING_SIZE;
  if (next == this->ring_tail_)
    return;  // full, drop
  this->ring_[this->ring_head_] = frame;
  this->ring_head_ = next;
}

// ---------------------------------------------------------------------------
// Task context
// ---------------------------------------------------------------------------

void CecBus::service() {
  int64_t now = esp_timer_get_time();
  bool level_low = gpio_ll_get_level(&GPIO, this->gpio_num_) == 0;
  int wd_event = 0;  // 1 = released, 2 = was us, 3 = external
  portENTER_CRITICAL(&this->lock_);
  if (this->line_low_ && !level_low && now - this->last_fall_us_ > STUCK_LOW) {
    // We missed the rising edge; the line is actually high.
    this->line_low_ = false;
    this->last_edge_us_ = now;
  }
  if (!this->line_low_ && now - this->last_edge_us_ > RX_STALL) {
    if (this->rx_state_ == RX_BITS) {
      this->finish_frame_(0);
    } else if (this->rx_state_ == RX_START_LOW) {
      this->rx_state_ = RX_IDLE;
    }
  }
  // Watchdog: the line has been low far longer than any legal CEC pulse.
  // Release everything we could be driving, then see whether the line recovers.
  if (level_low && this->line_low_ && now - this->last_fall_us_ > STUCK_LOW && !this->wd_released_ &&
      !this->monitor_only_) {
    gpio_ll_set_level(&GPIO, this->gpio_num_, 1);
    GPIO.func_out_sel_cfg[this->gpio_num_].val = this->out_sel_gpio_;  // pin on the latch, released
    if (this->ack_driving_) {
      this->ack_driving_ = false;
      gptimer_stop(this->ack_timer_);
    }
    if (this->tx_active_) {
      this->tx_aborted_ = true;
      if (this->tx_fail_ == 0)
        this->tx_fail_ = static_cast<uint8_t>(TxResult::ERROR) + 1;
    }
    this->wd_released_ = true;
    this->wd_reported_ = false;
    this->wd_release_us_ = now;
    this->stats_.watchdog_releases++;
    wd_event = 1;
  } else if (this->wd_released_ && !this->wd_reported_ && now - this->wd_release_us_ > 2000) {
    this->wd_reported_ = true;
    wd_event = level_low ? 3 : 2;
  }
  if (this->wd_released_ && !level_low && this->wd_reported_)
    this->wd_released_ = false;  // armed again; pin stays on the latch until the next transmit
  portEXIT_CRITICAL(&this->lock_);

  if (wd_event == 2) {
    ESP_LOGE(TAG, "Bus was stuck low and recovered after releasing our pin: this driver was holding it (bug)");
  } else if (wd_event == 3) {
    ESP_LOGW(TAG, "Bus is held low by another device (not us); waiting for it to release");
  }
}

bool CecBus::read_frame(CecFrame &out) {
  bool got = false;
  portENTER_CRITICAL(&this->lock_);
  if (this->ring_tail_ != this->ring_head_) {
    out = this->ring_[this->ring_tail_];
    this->ring_tail_ = (this->ring_tail_ + 1) % RING_SIZE;
    got = true;
  }
  portEXIT_CRITICAL(&this->lock_);
  return got;
}

bool CecBus::bus_free_for(uint32_t us) {
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&this->lock_);
  bool free = this->rx_state_ == RX_IDLE && !this->line_low_ && !this->ack_driving_ && !this->tx_active_ &&
              (now - this->last_edge_us_) >= (int64_t) us;
  portEXIT_CRITICAL(&this->lock_);
  return free && gpio_ll_get_level(&GPIO, this->gpio_num_) != 0;
}

bool CecBus::start_transmit(const CecFrame &frame) {
  if (this->tx_active_ || this->monitor_only_ || frame.len == 0 || frame.len > CEC_MAX_BLOCKS)
    return false;

  size_t n = 0;
  auto add = [this, &n](uint32_t low, uint32_t high) {
    rmt_symbol_word_t &s = this->tx_symbols_[n++];
    s.level0 = 0;
    s.duration0 = low;
    s.level1 = 1;
    s.duration1 = high;
  };
  auto add_bit = [&add](bool one) {
    if (one) {
      add(TX_ONE_LOW, TX_ONE_HIGH);
    } else {
      add(TX_ZERO_LOW, TX_ZERO_HIGH);
    }
  };
  add(TX_START_LOW, TX_START_HIGH);
  for (uint8_t b = 0; b < frame.len; b++) {
    for (int i = 7; i >= 0; i--)
      add_bit((frame.data[b] >> i) & 1);
    add_bit(b == frame.len - 1);  // EOM
    add_bit(true);                // ACK slot: initiator sends '1', follower may pull it to '0'
  }

  this->tx_frame_ = frame;
  this->tx_fail_ = 0;
  this->tx_rmt_done_ = false;
  this->tx_readback_done_ = false;
  this->tx_aborted_ = false;
  this->tx_expected_ms_ = (4500 + frame.len * 10 * 2400) / 1000 + 1;
  this->tx_started_ms_ = millis();

  portENTER_CRITICAL(&this->lock_);
  if (GPIO.func_out_sel_cfg[this->gpio_num_].val != this->out_sel_rmt_ && !this->ack_driving_) {
    // Parked on the latch by the watchdog: hand the pin back to RMT (idles high), latch back to 0 for ACKs.
    GPIO.func_out_sel_cfg[this->gpio_num_].val = this->out_sel_rmt_;
    gpio_ll_set_level(&GPIO, this->gpio_num_, 0);
  }
  this->tx_active_ = true;
  portEXIT_CRITICAL(&this->lock_);

  rmt_transmit_config_t tcfg{};
  tcfg.loop_count = 0;
  tcfg.flags.eot_level = 1;
  esp_err_t err = rmt_transmit(this->rmt_channel_, this->rmt_encoder_, this->tx_symbols_,
                               n * sizeof(rmt_symbol_word_t), &tcfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
    portENTER_CRITICAL(&this->lock_);
    this->tx_active_ = false;
    portEXIT_CRITICAL(&this->lock_);
    this->stats_.tx_errors++;
    return false;
  }
  return true;
}

bool CecBus::poll_transmit(TxResult &result) {
  if (!this->tx_active_)
    return false;
  uint32_t elapsed = millis() - this->tx_started_ms_;
  bool timeout = elapsed > this->tx_expected_ms_ + 250;
  if (!this->tx_rmt_done_ && !timeout)
    return false;
  // The readback normally completes on the last rising edge, before RMT finishes the
  // final high period. Give it a little slack anyway.
  if (!this->tx_readback_done_ && this->tx_fail_ == 0 && !timeout && elapsed < this->tx_expected_ms_ + 30)
    return false;
  if (timeout && !this->tx_rmt_done_) {
    ESP_LOGW(TAG, "Transmission timed out");
    rmt_tx_wait_all_done(this->rmt_channel_, 100);
  }

  portENTER_CRITICAL(&this->lock_);
  if (this->ack_driving_) {
    // We are ACKing someone else's frame right now; hand the pin back afterwards.
    portEXIT_CRITICAL(&this->lock_);
    return false;
  }
  if (this->tx_aborted_) {
    GPIO.func_out_sel_cfg[this->gpio_num_].val = this->out_sel_rmt_;  // RMT idles high
    gpio_ll_set_level(&GPIO, this->gpio_num_, 0);                    // latch ready for ACK
    this->tx_aborted_ = false;
  }
  uint8_t fail = this->tx_fail_;
  bool readback_ok = this->tx_readback_done_;
  CecFrame readback = this->tx_readback_;
  this->tx_active_ = false;
  portEXIT_CRITICAL(&this->lock_);

  if (fail != 0) {
    result = static_cast<TxResult>(fail - 1);
  } else if (!readback_ok || readback.len != this->tx_frame_.len ||
             memcmp(readback.data, this->tx_frame_.data, readback.len) != 0) {
    result = TxResult::ERROR;
  } else {
    result = TxResult::OK;
  }

  switch (result) {
    case TxResult::OK:
      this->stats_.frames_sent++;
      break;
    case TxResult::NACK:
      this->stats_.tx_nack++;
      break;
    case TxResult::ERROR:
      this->stats_.tx_errors++;
      break;
    default:
      break;
  }
  return true;
}

CecBusStats CecBus::get_stats() {
  portENTER_CRITICAL(&this->lock_);
  CecBusStats s = this->stats_;
  portEXIT_CRITICAL(&this->lock_);
  return s;
}

}  // namespace hdmi_cec_rmt
}  // namespace esphome

#endif  // USE_ESP32
