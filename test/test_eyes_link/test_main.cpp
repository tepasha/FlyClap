// Хост-тест збірки "турель на Arduino + очі ESP32" (приклад FlySonarTurret):
// протокол, серії, кулдаун, великий об'єкт, втрата зв'язку, ARM, тестовий постріл.
#include <FlyDefense.h>
#include <unity.h>
#include "fly_test.h"

static fly::Base base(A0, 13);
static fly::PanTilt head(9, 10);
static fly::Nozzle gun(5, fly::Nozzle::PUMP);
static fly::Turret turret(head, gun);
static fly::EyesLink eyes(turret);

static void send(const char *s) {
  for (; *s; s++) g_rx.push_back(*s);
}

// loop() з кроком 1 мс; ESP шле line кожні 33 мс (~30 кадр/с)
static void run(uint32_t ms, const char *line) {
  for (uint32_t i = 0; i < ms; i++) {
    if (line && i % 33 == 0) send(line);
    base.update();
    g_ms++;
  }
}

// Друк у рядок — для перевірки формату команд
struct StrPrint : Print {
  std::string s;
  size_t write(uint8_t c) override {
    s += (char)c;
    return 1;
  }
};

static void protocolRoundTrip() {
  const char *lines[] = {"A 973 -20 1", "M 0 1800", "F 40", "B"};
  for (const char *l : lines) {
    fly::Command c;
    CHECK(c.parse(l), "parse '%s'", l);
    StrPrint p;
    c.send(p);
    CHECK(p.s == std::string(l) + "\n", "round trip '%s' -> '%s'", l, p.s.c_str());
  }
  fly::Command c;
  CHECK(!c.parse("A 12") && !c.parse("XYZ") && !c.parse("F"), "garbage accepted");
}

static void test_eyes_link() {
  protocolRoundTrip();

  g_shotPin = 5;
  g_pins[A0] = HIGH;  // SAFE
  base.add(turret);
  base.add(eyes);
  CHECK(base.begin(), "begin failed");

  run(1000, "A 900 900 1\n");
  CHECK(g_shotStarts == 0, "fired while SAFE: %d", g_shotStarts);

  g_pins[A0] = LOW;  // ARM
  run(100, "A 900 900 1\n");
  CHECK(g_shotStarts == 1, "expected first shot, got %d", g_shotStarts);
  run(2000, "A 900 900 1\n");
  CHECK(g_shotStarts == 3, "burst should cap at 3, got %d", g_shotStarts);
  run(1100, "A 900 900 1\n");  // серія ~0.7 с + кулдаун 2.5 с ще не минули
  CHECK(g_shotStarts == 3, "fired during cooldown: %d", g_shotStarts);
  run(200, "A 900 900 1\n");
  CHECK(g_shotStarts == 4, "after cooldown expected 4th shot, got %d", g_shotStarts);
  CHECK(g_log.find("PSSHT! pan=90.0 tilt=90.0 total=4") != std::string::npos, "shot not logged");

  // Один постріл триває стільки, скільки задає профіль ствола
  {
    uint32_t on = 0;
    int i = 0;
    for (; i < 4000 && !g_pins[5]; i++) run(1, i % 33 == 0 ? "A 900 900 1\n" : nullptr);
    for (; i < 4000 && g_pins[5]; i++) {
      on++;
      run(1, i % 33 == 0 ? "A 900 900 1\n" : nullptr);
    }
    CHECK(on >= 118 && on <= 122, "shot length %u ms, expected 120", on);
  }

  // Великий об'єкт: мовчимо, навіть коли A просить стріляти
  run(3000, nullptr);
  send("B\n");
  int s = g_shotStarts;
  run(2900, "A 900 900 1\n");
  CHECK(g_shotStarts == s, "fired during big-object hold");
  run(400, "A 900 900 1\n");
  CHECK(g_shotStarts > s, "did not resume after hold");

  // Великий поворот: стріляємо лише після того, як серво доїхало
  run(3000, nullptr);
  s = g_shotStarts;
  uint32_t t0 = g_ms;
  while (g_shotStarts == s && g_ms - t0 < 1000) run(1, (g_ms - t0) % 33 == 0 ? "A 300 900 1\n" : nullptr);
  CHECK(g_ms - t0 >= 130, "fired %u ms after a 60 deg move, before servo settled", g_ms - t0);
  CHECK(head.panUs() == fly::ServoOut::deg10ToUs(300), "pan us=%d", head.panUs());

  // Застарілий дозвіл: одна команда A, далі тиша — не стріляємо; зв'язок втрачено
  run(3000, nullptr);
  send("A 300 900 1\n");
  run(10, nullptr);
  int after = g_shotStarts;
  run(3000, nullptr);
  CHECK(g_shotStarts == after, "fired on a stale request (%d extra)", g_shotStarts - after);
  CHECK(!eyes.linkOk() && eyes.health() == fly::Health::Fault, "link loss not reported");

  // Сміття й задовгі рядки не ламають парсер і не рухають серво
  int before = head.panUs();
  send("A 12\nXYZ\nA 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18\n");
  run(50, nullptr);
  CHECK(head.panUs() == before, "garbage moved servo");
  send("M 1200 900\n");
  run(5, nullptr);
  CHECK(head.panUs() == fly::ServoOut::deg10ToUs(1200), "M not applied");

  // ARM вимкнули посеред пострілу — ствол закривається одразу
  run(3000, nullptr);
  t0 = g_ms;
  while (!g_pins[5] && g_ms - t0 < 500) run(1, (g_ms - t0) % 33 == 0 ? "A 1200 900 1\n" : nullptr);
  CHECK(g_pins[5], "no shot to interrupt");
  g_pins[A0] = HIGH;
  run(1, nullptr);
  CHECK(!g_pins[5], "pump still on after disarm");

  // Тестовий постріл: лише з ARM і не довше testShotMaxMs
  s = g_shotStarts;
  send("F 150\n");
  run(300, nullptr);
  CHECK(g_shotStarts == s, "test shot while SAFE");
  g_pins[A0] = LOW;
  run(1, nullptr);
  send("F 5000\n");
  uint32_t on = 0;
  for (int i = 0; i < 600; i++) {
    run(1, nullptr);
    if (g_pins[5]) on++;
  }
  CHECK(on >= 198 && on <= 202, "test shot %u ms, expected 200", on);

}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_eyes_link);
  return UNITY_END();
}
