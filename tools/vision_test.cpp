// Хост-тест ядра зору FlyVision на синтетичних кадрах.
//   g++ -std=c++11 -O2 -Wall -Wextra -I firmware/flyvision tools/vision_test.cpp -o /tmp/vision_test && /tmp/vision_test
#include "vision.h"
#include <stdio.h>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

const int W = 320, H = 240;

struct Frame {
  std::vector<uint8_t> px;
  Frame(uint8_t level) : px(W * H, level) {}
  void noise(unsigned &seed, int amp) {
    for (auto &p : px) {
      seed = seed * 1103515245u + 12345u;
      int v = p + (int)((seed >> 16) % (2 * amp + 1)) - amp;
      p = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
  // темна пляма size×size з лівим верхнім кутом (x, y)
  void spot(int x, int y, int size, uint8_t level) {
    for (int j = 0; j < size; j++)
      for (int i = 0; i < size; i++)
        if (x + i >= 0 && x + i < W && y + j >= 0 && y + j < H) px[(y + j) * W + x + i] = level;
  }
};

int main() {
  unsigned seed = 1;
  fv::Blob blobs[8];
  bool big;

  // 1. Чистий фон з шумом — жодних цілей
  {
    fv::Vision v; v.begin(W, H);
    int falsePos = 0;
    for (int f = 0; f < 100; f++) {
      Frame fr(200); fr.noise(seed, 6);
      int n = v.process(fr.px.data(), blobs, 8, big);
      falsePos += n + (big ? 1 : 0);
    }
    CHECK(falsePos == 0, "false positives on clean backdrop: %d", falsePos);
  }

  // 2. Мошка 2×2 пікселі летить 3 пікс/кадр при 30 кадр/с; трекер оцінює швидкість
  {
    fv::Vision v; v.begin(W, H);
    fv::Track t; fv::TrackerConfig tc;
    Frame bg(200); bg.noise(seed, 4);
    v.process(bg.px.data(), blobs, 8, big);
    int detected = 0; float maxErr = 0;
    for (int f = 0; f < 30; f++) {
      Frame fr(200); fr.noise(seed, 4);
      int x = 100 + 3 * f, y = 120;
      fr.spot(x, y, 2, 80);
      int n = v.process(fr.px.data(), blobs, 8, big);
      CHECK(!big, "small midge flagged as big at frame %d", f);
      if (n == 1) {
        detected++;
        float err = fabsf(blobs[0].x - (x + 0.5f)) + fabsf(blobs[0].y - (y + 0.5f));
        if (err > maxErr) maxErr = err;
      }
      fv::trackUpdate(t, blobs, n, 1000 + f * 33, tc);
    }
    CHECK(detected == 30, "midge detected in %d/30 frames", detected);
    CHECK(maxErr < 0.01f, "centroid error %.3f px", maxErr);
    CHECK(t.active && t.hits >= 25, "track not locked (hits=%d)", t.hits);
    CHECK(fabsf(t.vx - 90.9f) < 10 && fabsf(t.vy) < 5, "velocity %.1f,%.1f px/s, expected ~91,0", t.vx, t.vy);
    float lead = t.predictX(60) - t.x;
    CHECK(fabsf(lead - 5.45f) < 1.0f, "60 ms lead %.2f px, expected ~5.5", lead);
    printf("midge: %d/30 frames, v=(%.1f, %.1f) px/s, 60ms lead=%.2f px\n", detected, t.vx, t.vy, lead);
  }

  // 3. Одиночний "битий" піксель — шум, не ціль
  {
    fv::Vision v; v.begin(W, H);
    Frame fr(200); v.process(fr.px.data(), blobs, 8, big);
    fr.spot(50, 50, 1, 20);
    int n = v.process(fr.px.data(), blobs, 8, big);
    CHECK(n == 0 && !big, "single hot pixel detected (n=%d big=%d)", n, big);
  }

  // 4. Рука/кіт — великий об'єкт: big, жодних цілей
  {
    fv::Vision v; v.begin(W, H);
    Frame fr(200); v.process(fr.px.data(), blobs, 8, big);
    fr.spot(100, 60, 40, 60);
    int n = v.process(fr.px.data(), blobs, 8, big);
    CHECK(n == 0 && big, "hand not flagged (n=%d big=%d)", n, big);

    // пляма середнього розміру (20×3: палець) — теж big, хоча загалом пікселів мало
    fv::Vision v2; v2.begin(W, H);
    Frame g(200); v2.process(g.px.data(), blobs, 8, big);
    g.spot(10, 10, 3, 60); g.spot(13, 10, 3, 60); g.spot(16, 10, 3, 60);
    g.spot(19, 10, 3, 60); g.spot(22, 10, 3, 60); g.spot(25, 10, 3, 60);
    n = v2.process(g.px.data(), blobs, 8, big);
    CHECK(n == 0 && big, "finger-like strip not flagged (n=%d big=%d)", n, big);
  }

  // 5. Темна зона поза панеллю ігнорується
  {
    fv::Vision v; v.begin(W, H);
    Frame fr(200);
    for (int y = 0; y < H; y++) for (int x = 0; x < 60; x++) fr.px[y * W + x] = 50;  // рамка/стіна
    v.process(fr.px.data(), blobs, 8, big);
    fr.spot(20, 100, 3, 5);
    int n = v.process(fr.px.data(), blobs, 8, big);
    CHECK(n == 0 && !big, "target outside backdrop detected (n=%d)", n);
  }

  // 6. Муха сіла й сидить 300 кадрів — фон її не "з'їдає"
  {
    fv::Vision v; v.begin(W, H);
    Frame bg(200); v.process(bg.px.data(), blobs, 8, big);
    int detected = 0;
    for (int f = 0; f < 300; f++) {
      Frame fr(200); fr.noise(seed, 4); fr.spot(200, 150, 4, 70);
      detected += v.process(fr.px.data(), blobs, 8, big) == 1;
    }
    CHECK(detected == 300, "landed fly detected in %d/300 frames", detected);
  }

  // 7. Повільна зміна яскравості панелі (200 -> 170) без хибних спрацювань
  {
    fv::Vision v; v.begin(W, H);
    int falsePos = 0;
    for (int f = 0; f < 600; f++) {
      Frame fr((uint8_t)(200 - f / 20)); fr.noise(seed, 4);
      falsePos += v.process(fr.px.data(), blobs, 8, big) + (big ? 1 : 0);
    }
    CHECK(falsePos == 0, "false positives on brightness drift: %d", falsePos);
  }

  // 8. Калібрування: відновлення афінного перетворення з 4 точок; вироджений випадок
  {
    fv::CalPoint p[4];
    const float xs[4] = {40, 280, 160, 60}, ys[4] = {30, 40, 210, 190};
    for (int i = 0; i < 4; i++) {
      p[i].x = xs[i]; p[i].y = ys[i];
      p[i].pan10  = -2.5f * xs[i] + 0.1f * ys[i] + 1300;
      p[i].tilt10 =  0.05f * xs[i] + 2.2f * ys[i] + 600;
    }
    fv::Affine a;
    CHECK(fv::solveAffine(p, 4, a), "solveAffine failed");
    float pan, tilt; a.apply(123, 77, pan, tilt);
    float ep = -2.5f * 123 + 0.1f * 77 + 1300, et = 0.05f * 123 + 2.2f * 77 + 600;
    CHECK(fabsf(pan - ep) < 0.5f && fabsf(tilt - et) < 0.5f, "affine %.1f/%.1f vs %.1f/%.1f", pan, tilt, ep, et);

    fv::CalPoint line[3] = {{0, 0, 900, 900}, {10, 10, 950, 950}, {20, 20, 1000, 1000}};
    fv::Affine b;
    CHECK(!fv::solveAffine(line, 3, b), "collinear points accepted");
  }

  printf(failures ? "%d FAILURE(S)\n" : "ALL TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
