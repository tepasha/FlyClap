#include "ServoOut.h"
#include "Base.h"

#if defined(ESP32)
#include "driver/ledc.h"

namespace {
const ledc_mode_t SERVO_MODE = LEDC_LOW_SPEED_MODE;
const ledc_timer_t SERVO_TIMER = LEDC_TIMER_2;  // камера — LEDC_TIMER_0 / канал 0
const uint32_t SERVO_HZ = 50;
const uint32_t SERVO_BITS = 14;                 // 20 мс / 16384 ≈ 1.2 мкс ≈ 0.12°
const int8_t FIRST_CHANNEL = 4, LAST_CHANNEL = 7;
int8_t nextChannel = FIRST_CHANNEL;
bool timerReady = false;

uint32_t dutyOf(uint16_t us) { return (uint32_t)us * (1u << SERVO_BITS) / (1000000u / SERVO_HZ); }
}  // namespace
#endif

namespace fly {

bool ServoOut::begin(Base &base, const __FlashStringHelper *owner) {
  if (!base.claimPin(pin_, owner)) return false;
#if defined(ESP32)
  if (nextChannel > LAST_CHANNEL) {
    base.configError(owner, F("too many servos (max 4 on ESP32)"));
    return false;
  }
  channel_ = nextChannel++;
  if (!timerReady) {
    ledc_timer_config_t tc = {};
    tc.speed_mode = SERVO_MODE;
    tc.duty_resolution = (ledc_timer_bit_t)SERVO_BITS;
    tc.timer_num = SERVO_TIMER;
    tc.freq_hz = SERVO_HZ;
    tc.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&tc) != ESP_OK) {
      base.configError(owner, F("LEDC timer config failed"));
      return false;
    }
    timerReady = true;
  }
#endif
  pinMode(pin_, OUTPUT);
  digitalWrite(pin_, LOW);
  return true;
}

void ServoOut::writeUs(uint16_t us) {
  us_ = us;
#if defined(ESP32)
  if (channel_ < 0) return;
  if (!attached_) {
    ledc_channel_config_t c = {};
    c.gpio_num = pin_;
    c.speed_mode = SERVO_MODE;
    c.channel = (ledc_channel_t)channel_;
    c.timer_sel = SERVO_TIMER;
    c.duty = dutyOf(us);
    c.hpoint = 0;
    ledc_channel_config(&c);
    attached_ = true;
    return;
  }
  ledc_set_duty(SERVO_MODE, (ledc_channel_t)channel_, dutyOf(us));
  ledc_update_duty(SERVO_MODE, (ledc_channel_t)channel_);
#else
  servo_.writeMicroseconds(us);  // спершу кут, щоб attach() не смикнув у 90°
  if (!attached_) {
    servo_.attach(pin_);
    attached_ = true;
  }
#endif
}

void ServoOut::detach() {
  if (!attached_) return;
#if defined(ESP32)
  ledc_stop(SERVO_MODE, (ledc_channel_t)channel_, 0);
#else
  servo_.detach();
  digitalWrite(pin_, LOW);  // detach() посеред імпульсу лишає пін у HIGH
#endif
  attached_ = false;
}

}  // namespace fly
