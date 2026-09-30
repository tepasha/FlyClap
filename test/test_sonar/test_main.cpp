// Хост-симуляція збірки FlySonar (сонар + турель): віртуальна кімната зі стіною,
// ціль і модель пелюстки HC-SR04. Перевіряє, що уточнення центроїдом наводить
// точніше за крок сітки, і що фільтри не стріляють по руці, по цілі в польоті та
// в режимі SAFE.
#include "Servo.h"
#include <FlyDefense.h>
#include <unity.h>
#include "fly_test.h"

static fly::Base base(A0, 13);
static fly::PanTilt head(9, 10);
static fly::Nozzle gun(5, fly::Nozzle::PUMP);
static fly::Turret turret(head, gun, 900, 800);
static fly::Sonar sonar(turret, 2, 3, 4);

const float WALL_CM = 80;
const float BEAM_HALF_DEG = 8;  // ефективна напівширина пелюстки для малої цілі

struct Obj {
  bool on;
  float pan, tilt, halfDeg, cm;
  bool moving;
};
static Obj target;
static unsigned pingNo = 0;

static unsigned long echo() {
  pingNo++;
  const float p = servoDeg(head.panUs()), t = servoDeg(head.tiltUs());
  if (target.on) {
    const float reach = BEAM_HALF_DEG + target.halfDeg;
    if (fabsf(p - target.pan) <= reach && fabsf(t - target.tilt) <= reach) {
      const float cm = target.moving ? target.cm + ((pingNo % 2) ? 4.0f : -4.0f) : target.cm;
      return (unsigned long)(cm * 58);
    }
  }
  return (unsigned long)(WALL_CM * 58);
}

static float shotPan, shotTilt;

// Прогнати скан до першої серії пострілів (або timeoutMs віртуального часу)
static bool runUntilShot(uint32_t timeoutMs) {
  const int s0 = g_shotStarts;
  const uint32_t t0 = g_ms;
  while (g_shotStarts == s0 && g_ms - t0 < timeoutMs) base.update();
  return g_shotStarts != s0;
}

static void holdAndReset(uint32_t ms) {
  sonar.holdAllRows(millis() + ms);  // добити попередню серію
  g_ms += ms;
}

static void test_sonar() {
  g_autoTick = true;
  g_pulseHook = echo;
  g_shotPin = 5;
  g_onShot = []() {
    shotPan = servoDeg(head.panUs());
    shotTilt = servoDeg(head.tiltUs());
  };
  g_pins[A0] = LOW;  // ARM
  g_pins[4] = HIGH;  // кнопка RECAL не натиснута
  g_pins[3] = LOW;   // ECHO

  base.add(turret);
  base.add(sonar);
  CHECK(base.begin(), "begin failed");
  base.update();  // калібрування фону по порожній кімнаті
  CHECK(g_log.find("Background ready") != std::string::npos, "not calibrated");

  const float expectLift = (gun.ballisticLift10(30) + fly::Sonar::TILT_NOZZLE_OFFSET10) / 10.0f;

  // 1. Муха сидить між вузлами сітки (97.3°/101°), 30 см
  {
    target = {true, 97.3f, 101.0f, 0.5f, 30, false};
    CHECK(runUntilShot(120000), "no shot at a still fly");
    const float ePan = fabsf(shotPan - 97.3f), eTilt = fabsf(shotTilt - expectLift - 101.0f);
    printf("still fly: aim err pan=%.2f tilt=%.2f deg (grid-only would be %.1f / %.1f)\n", ePan, eTilt,
           fabsf(95 - 97.3f), fabsf(95 - 101.0f));
    CHECK(ePan <= 1.0f, "pan error %.2f deg", ePan);
    CHECK(eTilt <= 1.5f, "tilt error %.2f deg", eTilt);
  }

  // 1b. Різні положення в полі огляду — похибка стабільно мала
  {
    const float pos[][2] = {{45.5f, 82}, {62, 90}, {88.8f, 104}, {120.2f, 86}, {141, 111}, {33, 96}, {74.4f, 108.7f}};
    float worstPan = 0, worstTilt = 0;
    for (auto &q : pos) {
      holdAndReset(3000);
      target = {true, q[0], q[1], 0.5f, 30, false};
      const bool shot = runUntilShot(120000);
      CHECK(shot, "no shot at %.1f/%.1f", q[0], q[1]);
      if (!shot) continue;
      worstPan = fmaxf(worstPan, fabsf(shotPan - q[0]));
      worstTilt = fmaxf(worstTilt, fabsf(shotTilt - expectLift - q[1]));
    }
    printf("7 positions: worst aim err pan=%.2f tilt=%.2f deg\n", worstPan, worstTilt);
    CHECK(worstPan <= 1.0f && worstTilt <= 1.5f, "worst error pan=%.2f tilt=%.2f", worstPan, worstTilt);
  }

  // 2. Рука (широкий об'єкт) — не стріляємо
  holdAndReset(3000);
  target = {true, 90, 95, 20, 30, false};
  CHECK(!runUntilShot(30000), "fired at a hand");
  CHECK(g_log.find("holding fire") != std::string::npos, "hand not reported");

  // 2b. Рука далі (50 см, вужча в кутах) — теж не стріляємо
  holdAndReset(3000);
  target = {true, 70, 95, 6, 50, false};
  CHECK(!runUntilShot(30000), "fired at a hand at 50 cm");

  // 3. Ціль у польоті (відстань скаче) — при staticOnly не стріляємо
  holdAndReset(3000);
  target = {true, 110, 95, 0.5f, 30, true};
  CHECK(!runUntilShot(30000), "fired at a moving target with staticOnly");

  // 4. SAFE — не стріляємо навіть по ідеальній цілі
  holdAndReset(3000);
  g_pins[A0] = HIGH;
  target = {true, 97.3f, 101.0f, 0.5f, 30, false};
  CHECK(!runUntilShot(30000), "fired while SAFE");
  CHECK(g_log.find("[SAFE] not firing") != std::string::npos, "SAFE not reported");

}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_sonar);
  return UNITY_END();
}
