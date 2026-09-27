// Хост-симуляція FlySonar у режимі сонара: віртуальна кімната зі стіною,
// ціль і модель пелюстки HC-SR04. Перевіряє, що уточнення центроїдом
// наводить точніше за крок сітки, і що фільтри не стріляють по руці,
// по цілі в польоті та в режимі SAFE. Запуск: tools/run_tests.sh
#include <Arduino.h>
#include <math.h>
#include "mock_globals.h"
#include "../firmware/flysonar/flysonar.ino"

const float WALL_CM = 80;
const float BEAM_HALF_DEG = 8;          // ефективна напівширина пелюстки для малої цілі

struct Obj { bool on; float pan, tilt, halfDeg, cm; bool moving; };
static Obj target;
static unsigned pingNo = 0;

static unsigned long echo() {
  pingNo++;
  float p = panServo.deg(), t = tiltServo.deg();
  if (target.on) {
    float reach = BEAM_HALF_DEG + target.halfDeg;
    if (fabsf(p - target.pan) <= reach && fabsf(t - target.tilt) <= reach) {
      float cm = target.moving ? target.cm + ((pingNo % 2) ? 4.0f : -4.0f) : target.cm;
      return (unsigned long)(cm * 58);
    }
  }
  return (unsigned long)(WALL_CM * 58);
}

// Прогнати скан до першого пострілу (або timeoutMs віртуального часу)
static bool runUntilShot(uint32_t timeoutMs, float &shotPan, float &shotTilt) {
  int s0 = g_shotStarts;
  g_onShot = [&]() { shotPan = panServo.deg(); shotTilt = tiltServo.deg(); };
  uint32_t t0 = g_ms;
  while (g_shotStarts == s0 && g_ms - t0 < timeoutMs) loop();
  g_onShot = nullptr;
  return g_shotStarts != s0;
}

int main() {
  g_autoTick = true;
  g_pulseHook = echo;
  g_pins[PIN_ARM] = LOW;                        // ARM
  g_pins[PIN_RECAL] = HIGH;
  g_pins[PIN_ECHO] = LOW;
  setup();                                      // калібрування фону по порожній кімнаті

  const float expectLift = (int)(BALLISTIC_DEG_PER_CM * 30 * 10 + 0.5f) / 10.0f + TILT_NOZZLE_OFFSET10 / 10.0f;

  // 1. Муха сидить між вузлами сітки (97.3°/101°), 30 см
  {
    target = { true, 97.3f, 101.0f, 0.5f, 30, false };
    float sp = 0, st = 0;
    bool shot = runUntilShot(120000, sp, st);
    CHECK(shot, "no shot at a still fly");
    float ePan = fabsf(sp - 97.3f), eTilt = fabsf(st - expectLift - 101.0f);
    printf("still fly: aim err pan=%.2f tilt=%.2f deg (grid-only would be %.1f / %.1f)\n",
           ePan, eTilt, fabsf(95 - 97.3f), fabsf(95 - 101.0f));
    CHECK(ePan <= 1.0f, "pan error %.2f deg", ePan);
    CHECK(eTilt <= 1.5f, "tilt error %.2f deg", eTilt);
  }

  // 1b. Різні положення в полі огляду — похибка стабільно мала
  {
    const float pos[][2] = { {45.5f, 82}, {62, 90}, {88.8f, 104}, {120.2f, 86}, {141, 111}, {33, 96}, {74.4f, 108.7f} };
    float worstPan = 0, worstTilt = 0;
    for (auto &q : pos) {
      for (uint8_t t = 0; t < TILT_CELLS; t++) cooldownUntil[t] = millis() + 3000;  // добити попередню серію
      g_ms += 3000;
      target = { true, q[0], q[1], 0.5f, 30, false };
      float sp = 0, st = 0;
      bool shot = runUntilShot(120000, sp, st);
      CHECK(shot, "no shot at %.1f/%.1f", q[0], q[1]);
      if (!shot) continue;
      worstPan  = fmaxf(worstPan,  fabsf(sp - q[0]));
      worstTilt = fmaxf(worstTilt, fabsf(st - expectLift - q[1]));
    }
    printf("7 positions: worst aim err pan=%.2f tilt=%.2f deg\n", worstPan, worstTilt);
    CHECK(worstPan <= 1.0f && worstTilt <= 1.5f, "worst error pan=%.2f tilt=%.2f", worstPan, worstTilt);
  }

  // 2. Рука (широкий об'єкт) — не стріляємо
  {
    for (uint8_t t = 0; t < TILT_CELLS; t++) cooldownUntil[t] = millis();
    target = { true, 90, 95, 20, 30, false };
    float sp, st;
    CHECK(!runUntilShot(30000, sp, st), "fired at a hand");
  }

  // 2b. Рука далі (50 см, вужча в кутах) — теж не стріляємо
  {
    for (uint8_t t = 0; t < TILT_CELLS; t++) cooldownUntil[t] = millis();
    target = { true, 70, 95, 6, 50, false };
    float sp, st;
    CHECK(!runUntilShot(30000, sp, st), "fired at a hand at 50 cm");
  }

  // 3. Ціль у польоті (відстань скаче) — при STATIC_ONLY не стріляємо
  {
    for (uint8_t t = 0; t < TILT_CELLS; t++) cooldownUntil[t] = millis();
    target = { true, 110, 95, 0.5f, 30, true };
    float sp, st;
    CHECK(!runUntilShot(30000, sp, st), "fired at a moving target with STATIC_ONLY");
  }

  // 4. SAFE — не стріляємо навіть по ідеальній цілі
  {
    for (uint8_t t = 0; t < TILT_CELLS; t++) cooldownUntil[t] = millis();
    g_pins[PIN_ARM] = HIGH;
    target = { true, 97.3f, 101.0f, 0.5f, 30, false };
    float sp, st;
    CHECK(!runUntilShot(30000, sp, st), "fired while SAFE");
  }

  printf(failures ? "flysonar sonar: %d FAILURE(S)\n" : "flysonar sonar: ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
