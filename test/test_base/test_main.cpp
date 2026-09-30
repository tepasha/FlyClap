// Хост-тест основи: ARM і disarm(), статус-LED, конфлікти пінів і таймерів.
#include <FlyDefense.h>
#include <unity.h>
#include "fly_test.h"

struct Probe : fly::Module {
  uint8_t pin;
  fly::Health h = fly::Health::Ok;
  int disarms = 0, ticks = 0, loops = 0;
  explicit Probe(uint8_t p) : pin(p) {}
  const __FlashStringHelper *name() const override { return F("Probe"); }
  bool begin(fly::Base &b) override { return b.claimPin(pin, name()); }
  void tick(fly::Base &, uint32_t) override { ticks++; }
  void loop(fly::Base &) override { loops++; }
  void disarm(fly::Base &) override { disarms++; }
  fly::Health health() const override { return h; }
};

static void run(fly::Base &b, uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    b.update();
    g_ms++;
  }
}

// Скільки мс з ms горів LED
static int ledOnMs(fly::Base &b, uint32_t ms) {
  int on = 0;
  for (uint32_t i = 0; i < ms; i++) {
    b.update();
    on += g_pins[13];
    g_ms++;
  }
  return on;
}

static void test_base() {
  // 1. ARM і LED
  {
    fly::Base base(A0, 13);
    Probe probe(5);
    base.add(probe);
    g_pins[A0] = HIGH;
    CHECK(base.begin(), "begin failed");
    int on = ledOnMs(base, 2000);
    CHECK(!base.armed() && on > 0 && on < 200, "SAFE heartbeat: armed=%d led on %d ms", base.armed(), on);

    g_pins[A0] = LOW;
    run(base, 1);
    CHECK(base.armed() && g_log.find("ARM on") != std::string::npos, "ARM not detected");
    CHECK(ledOnMs(base, 1000) == 1000, "LED must be solid when armed and ready");
    probe.h = fly::Health::Busy;
    on = ledOnMs(base, 1600);
    CHECK(on > 600 && on < 1000, "busy blink on %d ms", on);
    probe.h = fly::Health::Fault;
    on = ledOnMs(base, 1000);
    CHECK(on > 400 && on < 600, "fault blink on %d ms", on);

    g_pins[A0] = HIGH;
    run(base, 1);
    CHECK(!base.armed() && probe.disarms == 1, "disarm not propagated (%d)", probe.disarms);
    CHECK(probe.ticks > 0 && probe.loops > 0, "module not driven");
  }

  // 2. Два модулі на одному піні — помилка конфігурації, пристрій не озброюється
  {
    g_log.clear();
    fly::Base base(A0, 13);
    Probe a(7), b(7);
    base.add(a);
    base.add(b);
    CHECK(!base.begin(), "pin conflict accepted");
    CHECK(g_log.find("CONFIG ERROR: pin 7") != std::string::npos, "conflict not explained: %s", g_log.c_str());
    g_pins[A0] = LOW;
    run(base, 100);
    CHECK(!base.armed() && b.ticks == 0 && b.loops == 0, "misconfigured device armed or running");
  }

  // 3. Модуль на піні основи (LED) і спільний таймер
  {
    g_log.clear();
    fly::Base base(A0, 13);
    Probe onLed(13);
    base.add(onLed);
    CHECK(!base.begin(), "LED pin conflict accepted");
    CHECK(base.claim(fly::Res::Timer2, F("first")) && !base.claim(fly::Res::Timer2, F("second")),
          "timer conflict not detected");
  }

}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_base);
  return UNITY_END();
}
