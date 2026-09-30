// Хост-тест збірки FlyClap: машина станів ClapLatch з модельною механікою
// (засувка, соленоїд, серво взведення) і тригером, що "стріляє" як ISR завіси.
#include "EEPROM.h"
#include "Servo.h"
#include <FlyDefense.h>
#include <unity.h>
#include "fly_test.h"

// Тригер-замінник: blocked задає тест, cross() — те, що робить ISR завіси
struct FakeCurtain : fly::Trigger {
  bool isBlocked = false;
  const __FlashStringHelper *name() const override { return F("FakeCurtain"); }
  bool blocked() const override { return isBlocked; }
  void cross() { crossedFromIsr(); }
};

const int SOLENOID = 9, SERVO = 10, COCKED = 8, ARM = A0;

static fly::Base base(ARM, 13);
static FakeCurtain curtain;
static fly::ClapLatch clap(curtain, SOLENOID, SERVO, COCKED, 16);

static bool latchWorks = true;

// Механіка: серво на куті взведення зачіплює засувку, соленоїд її зриває
static void run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    if (latchWorks && g_servoUs[SERVO] && fabsf(servoDeg(g_servoUs[SERVO]) - fly::ClapLatch::SERVO_COCK_DEG) < 1)
      g_pins[COCKED] = LOW;
    if (g_pins[SOLENOID]) g_pins[COCKED] = HIGH;
    base.update();
    g_ms++;
  }
}

static void setArm(bool on) { g_pins[ARM] = on ? LOW : HIGH; }

static uint32_t storedClaps() {
  uint32_t v;
  return EEPROM.get(16, v);
}

static void test_clap() {
  uint32_t seven = 7;
  EEPROM.put(16, seven);  // лічильник з минулих запусків
  g_pins[COCKED] = HIGH;  // не взведено
  setArm(false);
  base.buzzer(11);
  base.add(curtain);
  base.add(clap);
  CHECK(base.begin(), "begin failed");
  CHECK(g_log.find("Total claps: 7") != std::string::npos, "claps not loaded from EEPROM");

  // 1. Повний цикл: ARM → взведення → чиста завіса → ARMED → муха → хлопок → знову ARMED
  run(1000);
  CHECK(clap.state() == fly::ClapLatch::DISARMED, "state %d", clap.state());
  CHECK(g_servoUs[SERVO] == 0, "servo must detach at rest");

  setArm(true);
  run(1);
  CHECK(clap.state() == fly::ClapLatch::COCKING, "not cocking: %d", clap.state());
  run(1);
  CHECK(clap.state() == fly::ClapLatch::SERVO_RETURN, "not returning: %d", clap.state());
  run(fly::ClapLatch::SERVO_RETURN_MS + fly::ClapLatch::BEAMS_CLEAR_MS + 5);
  CHECK(clap.state() == fly::ClapLatch::ARMED, "not armed: %d", clap.state());
  CHECK(curtain.shotEnabled(), "trigger not enabled");
  CHECK(g_servoUs[SERVO] == 0, "servo must detach once armed");

  curtain.cross();
  CHECK(g_pins[SOLENOID] == HIGH, "solenoid not fired from ISR");
  run(1);
  CHECK(clap.state() == fly::ClapLatch::FIRED && clap.claps() == 8, "clap not counted");
  CHECK(storedClaps() == 8, "claps not saved: %u", storedClaps());
  run(fly::ClapLatch::SOLENOID_PULSE_MS);
  CHECK(g_pins[SOLENOID] == LOW, "solenoid held longer than the pulse");
  run(fly::ClapLatch::CLAP_SETTLE_MS);
  CHECK(clap.state() == fly::ClapLatch::SERVO_RETURN, "not re-cocked: %d", clap.state());
  run(fly::ClapLatch::SERVO_RETURN_MS + fly::ClapLatch::BEAMS_CLEAR_MS + 5);
  CHECK(clap.state() == fly::ClapLatch::ARMED, "not re-armed: %d", clap.state());

  // 2. Засувка зірвалась сама (вібрація) — перевзвод без зарахування хлопка
  latchWorks = false;
  g_pins[COCKED] = HIGH;
  run(1);
  CHECK(clap.state() == fly::ClapLatch::COCKING && !curtain.shotEnabled(), "latch loss not handled");
  CHECK(clap.claps() == 8, "latch loss counted as a clap");

  // 3. Засувка не чіпляється — FAULT за таймаутом, серво у спокій, ISR заборонено
  run(fly::ClapLatch::COCK_TIMEOUT_MS + 5);
  CHECK(clap.state() == fly::ClapLatch::FAULT, "no cocking timeout fault: %d", clap.state());
  CHECK(clap.health() == fly::Health::Fault, "fault not reported to base");
  CHECK(g_toneHz == 400, "fault beep %d Hz", g_toneHz);

  // FAULT тримається, доки не вимкнуть ARM
  latchWorks = true;
  run(2000);
  CHECK(clap.state() == fly::ClapLatch::FAULT, "fault cleared by itself");
  setArm(false);
  run(1);
  CHECK(clap.state() == fly::ClapLatch::DISARMED, "ARM off did not clear fault");

  // 4. Прилипла муха: завіса перекрита довше BEAMS_STUCK_MS — FAULT, ARMED не буває
  curtain.isBlocked = true;
  setArm(true);
  run(fly::ClapLatch::BEAMS_STUCK_MS);
  CHECK(clap.state() == fly::ClapLatch::WAIT_CLEAR, "armed with a blocked beam: %d", clap.state());
  run(fly::ClapLatch::SERVO_RETURN_MS + 10);
  CHECK(clap.state() == fly::ClapLatch::FAULT, "stuck beam not detected: %d", clap.state());
  curtain.isBlocked = false;
  setArm(false);
  run(1);

  // 5. ARM вимкнули одразу після спрацювання — соленоїд знеструмлено, ISR заборонено
  setArm(true);
  run(1000);
  CHECK(clap.state() == fly::ClapLatch::ARMED, "not armed again: %d", clap.state());
  curtain.cross();
  setArm(false);
  run(1);
  CHECK(g_pins[SOLENOID] == LOW && !curtain.shotEnabled(), "disarm did not stop the latch");

}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_clap);
  return UNITY_END();
}
