#pragma once

// Low-level HDMI-CEC transport for ESP32 (ESP-IDF >= 5.5).
//
//  - TX: RMT TX channel generates the bit waveforms (no CPU involvement).
//  - RX: GPIO any-edge ISR timestamps edges and decodes bits. It has to track
//        every bit anyway to know where the ACK slots are, so it is also the
//        frame decoder.
//  - ACK: on the falling edge of an ACK slot in a frame addressed to us, the
//        ISR takes the pin away from RMT (one GPIO-matrix register write) and
//        drives it low. A GPTimer alarm hands it back to RMT at 1.5 ms.
//
// The pin is open-drain with input enabled, so the ISR sees every edge on the
// bus, including our own transmissions. That gives readback for ACK detection
// and arbitration.

#ifdef USE_ESP32

#include <cstdint>
#include <cstring>

#include "esphome/core/gpio.h"

#include "driver/gptimer.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"

namespace esphome {
namespace hdmi_cec_rmt {

static constexpr uint8_t CEC_MAX_BLOCKS = 16;
static constexpr uint8_t CEC_BROADCAST = 0x0F;

// Frame flags
static constexpr uint8_t FRAME_COMPLETE = 0x01;  // ended with EOM
static constexpr uint8_t FRAME_OWN = 0x02;       // we transmitted it
static constexpr uint8_t FRAME_ERROR = 0x04;     // timing violation / truncated

struct CecFrame {
  uint8_t data[CEC_MAX_BLOCKS]{};
  uint8_t len{0};         // number of complete blocks (header included)
  uint16_t ack_bits{0};   // bit n = level of the ACK bit of block n as seen on the bus
  uint8_t flags{0};

  uint8_t initiator() const { return this->data[0] >> 4; }
  uint8_t destination() const { return this->data[0] & 0x0F; }
  bool is_broadcast() const { return this->destination() == CEC_BROADCAST; }
  bool has_opcode() const { return this->len >= 2; }
  uint8_t opcode() const { return this->data[1]; }
  /// Directed: every block ACKed (bit read 0). Broadcast: nobody rejected (bit read 1).
  bool acknowledged() const {
    for (uint8_t i = 0; i < this->len; i++) {
      bool bit = (this->ack_bits >> i) & 1;
      if (this->is_broadcast() ? !bit : bit)
        return false;
    }
    return this->len > 0;
  }
};

enum class TxResult : uint8_t {
  OK,
  NACK,          // directed frame not acknowledged / broadcast rejected
  ARB_LOST,      // another initiator won arbitration
  ERROR,         // readback mismatch, timing error, timeout
};

struct CecBusStats {
  uint32_t frames_received{0};
  uint32_t frames_sent{0};
  uint32_t tx_nack{0};
  uint32_t tx_arbitration_lost{0};
  uint32_t tx_errors{0};
  uint32_t rx_errors{0};
  uint32_t acks_sent{0};
  uint32_t acks_missed{0};
  uint32_t watchdog_releases{0};
};

class CecBus {
 public:
  bool setup(InternalGPIOPin *pin, bool monitor_only);

  void set_logical_address(uint8_t la) { this->logical_address_ = la; }
  uint8_t get_logical_address() const { return this->logical_address_; }

  /// Call from loop(): finalizes frames that stopped mid-way (no more edges).
  void service();
  /// Pop one received frame (including readback of our own frames, flagged FRAME_OWN).
  bool read_frame(CecFrame &out);

  /// True when the bus has been idle (high, no frame in progress) for at least `us`.
  bool bus_free_for(uint32_t us);
  bool tx_busy() const { return this->tx_active_; }
  bool start_transmit(const CecFrame &frame);
  /// Returns true once the current transmission has finished; `result` is set.
  bool poll_transmit(TxResult &result);

  CecBusStats get_stats();
  bool line_is_low();
  /// Human-readable reason why bus_free_for() is false (for diagnostics).
  const char *busy_reason();
  /// Line level sampled at setup: before the driver touched the pin (input + pull-up only) and after.
  int level_before_driver() const { return this->level_before_driver_; }
  int level_after_driver() const { return this->level_after_driver_; }

 protected:
  enum RxState : uint8_t { RX_IDLE, RX_START_LOW, RX_BITS };

  static void gpio_isr_(CecBus *bus);
  static bool gptimer_alarm_cb_(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg);
  static bool rmt_done_cb_(rmt_channel_handle_t channel, const rmt_tx_done_event_data_t *edata, void *arg);

  void on_fall_(int64_t now);
  void on_rise_(int64_t now);
  void handle_bit_(uint8_t bit);
  void start_frame_();
  void finish_frame_(uint8_t flags);
  void rx_error_();
  void assert_ack_(int64_t edge_time);
  void release_ack_();
  void abort_tx_();
  void push_frame_(const CecFrame &frame);

  InternalGPIOPin *pin_{nullptr};
  uint8_t gpio_num_{0};
  bool monitor_only_{false};
  volatile uint8_t logical_address_{CEC_BROADCAST};

  // GPIO matrix output config: pin driven by RMT vs. pin driven by GPIO_OUT latch
  uint32_t out_sel_rmt_{0};
  uint32_t out_sel_gpio_{0};

  rmt_channel_handle_t rmt_channel_{nullptr};
  rmt_encoder_handle_t rmt_encoder_{nullptr};
  gptimer_handle_t ack_timer_{nullptr};
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;

  // ---- RX decoder state (ISR) ----
  volatile RxState rx_state_{RX_IDLE};
  RxState prev_state_{RX_IDLE};
  bool after_start_{false};
  volatile bool line_low_{false};
  volatile int64_t last_fall_us_{0};
  volatile int64_t prev_fall_us_{0};
  volatile int64_t last_edge_us_{0};
  uint8_t bit_in_block_{0};
  uint8_t cur_byte_{0};
  uint8_t eom_{0};
  CecFrame rx_frame_{};
  bool frame_is_own_{false};
  bool ack_this_frame_{false};
  bool ack_asserted_this_block_{false};
  volatile bool ack_driving_{false};

  // ---- TX state ----
  rmt_symbol_word_t tx_symbols_[1 + CEC_MAX_BLOCKS * 10]{};
  CecFrame tx_frame_{};
  volatile bool tx_active_{false};
  volatile bool tx_rmt_done_{false};
  volatile bool tx_readback_done_{false};
  volatile bool tx_aborted_{false};
  volatile uint8_t tx_fail_{0};  // 0 = none, else TxResult+1
  CecFrame tx_readback_{};
  uint32_t tx_started_ms_{0};
  uint32_t tx_expected_ms_{0};

  // ---- RX frame ring buffer (ISR -> loop) ----
  static constexpr uint8_t RING_SIZE = 16;
  CecFrame ring_[RING_SIZE]{};
  volatile uint8_t ring_head_{0};
  volatile uint8_t ring_tail_{0};

  CecBusStats stats_{};
  int level_before_driver_{-1};
  // Stuck-low watchdog
  bool wd_released_{false};      // we forced the pin released
  bool wd_reported_{false};      // result of the last release has been logged
  int64_t wd_release_us_{0};
  int level_after_driver_{-1};
};

}  // namespace hdmi_cec_rmt
}  // namespace esphome

#endif  // USE_ESP32
