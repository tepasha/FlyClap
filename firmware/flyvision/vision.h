/*
 * FlyVision — ядро машинного зору без залежностей від Arduino/ESP-IDF.
 * Той самий код компілюється під ESP32 і на ПК (tools/vision_test.cpp).
 *
 * Сцена: яскравий підсвічений фон (LED-панель + розсіювач), комаха на ньому —
 * темна точка. Кадр у відтінках сірого, 1 байт на піксель.
 *
 *   1) Фон — повільне ковзне середнє (фіксована кома 8.8). Під темними
 *      пікселями фон не оновлюється, тож муха, що сіла, не "розчиняється".
 *   2) Маска: піксель на яскравій частині фону (>= backdropMin) і темніший
 *      за фон щонайменше на darkDelta.
 *   3) Зв'язні області (4-зв'язність, заливка стеком). Малі — цілі,
 *      великі (рука, кіт, людина) — прапорець big, по ньому не стріляємо.
 *   4) Трекер альфа-бета: згладжена позиція + швидкість у пікселях/с,
 *      прогноз на час затримки пострілу.
 *   5) Калібрування "піксель -> кути" афінною моделлю методом найменших
 *      квадратів. Точки знімаються тестовими пострілами, тож зміщення сопла
 *      й падіння струменя входять у модель автоматично.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace fv {

struct VisionConfig {
  uint8_t  darkDelta   = 28;   // на скільки піксель темніший за фон, щоб бути "комахою"
  uint8_t  backdropMin = 110;  // фон яскравіший за це = робоча зона (панель)
  uint8_t  bgShift     = 5;    // швидкість адаптації фону: 1/32 за кадр
  uint16_t minPix      = 2;    // менше — шум сенсора
  uint16_t maxSidePx   = 14;   // бокс більший за це — вже не комаха
  uint32_t bigTotalPix = 400;  // стільки темних пікселів у кадрі — великий об'єкт
};

struct Blob {
  float    x, y;               // центроїд, пікселі
  uint16_t pix;                // площа
  uint16_t w, h;               // габарити
};

class Vision {
 public:
  VisionConfig cfg;

  Vision() = default;
  Vision(const Vision &) = delete;             // володіє буферами — не копіюємо
  Vision &operator=(const Vision &) = delete;
  ~Vision() { release(); }

  bool begin(int width, int height) {
    release();
    w_ = width; h_ = height;
    size_t n = (size_t)w_ * h_;
    bg_    = (uint16_t *)malloc(n * sizeof(uint16_t));
    mask_  = (uint8_t  *)malloc(n);
    stack_ = (uint32_t *)malloc(n * sizeof(uint32_t));
    haveBg_ = false;
    return bg_ && mask_ && stack_;
  }

  // Примусово перевчити фон з наступного кадру
  void resetBackground() { haveBg_ = false; }
  bool hasBackground() const { return haveBg_; }

  // Обробити кадр. Повертає кількість малих цілей (<= maxOut), big — великий об'єкт.
  int process(const uint8_t *img, Blob *out, int maxOut, bool &big) {
    big = false;
    const size_t n = (size_t)w_ * h_;
    if (!haveBg_) {
      for (size_t i = 0; i < n; i++) bg_[i] = (uint16_t)img[i] << 8;
      haveBg_ = true;
      return 0;
    }

    // 1-2. Маска темних пікселів + адаптація фону там, де темного немає
    uint32_t dark = 0;
    for (size_t i = 0; i < n; i++) {
      uint8_t b = bg_[i] >> 8;
      uint8_t m = 0;
      if (b >= cfg.backdropMin && img[i] + cfg.darkDelta < b) { m = 1; dark++; }
      mask_[i] = m;
      if (!m) bg_[i] = (uint16_t)((int32_t)bg_[i] + ((((int32_t)img[i] << 8) - (int32_t)bg_[i]) >> cfg.bgShift));
    }
    if (dark == 0) return 0;
    if (dark > cfg.bigTotalPix) { big = true; return 0; }

    // 3. Зв'язні області
    int found = 0;
    for (size_t start = 0; start < n; start++) {
      if (!mask_[start]) continue;
      uint32_t sp = 0, pix = 0;
      uint64_t sx = 0, sy = 0;
      int minX = w_, maxX = -1, minY = h_, maxY = -1;
      stack_[sp++] = (uint32_t)start;
      mask_[start] = 0;
      while (sp) {
        uint32_t i = stack_[--sp];
        int x = i % w_, y = i / w_;
        pix++; sx += x; sy += y;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
        if (x > 0      && mask_[i - 1])  { mask_[i - 1]  = 0; stack_[sp++] = i - 1; }
        if (x < w_ - 1 && mask_[i + 1])  { mask_[i + 1]  = 0; stack_[sp++] = i + 1; }
        if (y > 0      && mask_[i - w_]) { mask_[i - w_] = 0; stack_[sp++] = i - w_; }
        if (y < h_ - 1 && mask_[i + w_]) { mask_[i + w_] = 0; stack_[sp++] = i + w_; }
      }
      int bw = maxX - minX + 1, bh = maxY - minY + 1;
      if (bw > cfg.maxSidePx || bh > cfg.maxSidePx) { big = true; continue; }
      if (pix < cfg.minPix) continue;
      if (found < maxOut) {
        out[found].x = (float)sx / pix;
        out[found].y = (float)sy / pix;
        out[found].pix = (uint16_t)pix;
        out[found].w = (uint16_t)bw;
        out[found].h = (uint16_t)bh;
        found++;
      }
    }
    return found;
  }

 private:
  void release() {
    free(bg_); free(mask_); free(stack_);
    bg_ = nullptr; mask_ = nullptr; stack_ = nullptr;
  }
  int w_ = 0, h_ = 0;
  uint16_t *bg_ = nullptr;
  uint8_t  *mask_ = nullptr;
  uint32_t *stack_ = nullptr;
  bool haveBg_ = false;
};

// ---------------- Трекер ----------------
struct TrackerConfig {
  float    alpha      = 0.6f;  // довіра до виміру позиції
  float    beta       = 0.2f;  // довіра до виміру швидкості
  float    gatePx     = 30.0f; // далі від прогнозу — інша ціль
  uint8_t  maxMisses  = 4;     // кадрів без цілі до скидання
  float    maxSpeed   = 2000;  // пікс/с — обмеження проти стрибків
};

struct Track {
  bool     active = false;
  float    x = 0, y = 0, vx = 0, vy = 0;
  uint32_t lastMs = 0;
  uint16_t hits = 0;           // скільки кадрів поспіль підтверджено
  uint8_t  misses = 0;

  float speed() const { return sqrtf(vx * vx + vy * vy); }
  float predictX(float dtMs) const { return x + vx * dtMs * 0.001f; }
  float predictY(float dtMs) const { return y + vy * dtMs * 0.001f; }
};

inline void trackUpdate(Track &t, const Blob *b, int n, uint32_t nowMs, const TrackerConfig &c) {
  if (!t.active) {
    if (n <= 0) return;
    int best = 0;                                 // нова ціль: найбільша пляма
    for (int i = 1; i < n; i++) if (b[i].pix > b[best].pix) best = i;
    t.active = true; t.x = b[best].x; t.y = b[best].y;
    t.vx = t.vy = 0; t.lastMs = nowMs; t.hits = 1; t.misses = 0;
    return;
  }
  float dt = (nowMs - t.lastMs) * 0.001f;
  if (dt <= 0) dt = 0.001f;
  float px = t.x + t.vx * dt, py = t.y + t.vy * dt;

  int best = -1; float bestD2 = c.gatePx * c.gatePx;
  for (int i = 0; i < n; i++) {
    float dx = b[i].x - px, dy = b[i].y - py, d2 = dx * dx + dy * dy;
    if (d2 <= bestD2) { bestD2 = d2; best = i; }
  }
  if (best < 0) {
    if (++t.misses > c.maxMisses) { t.active = false; t.hits = 0; }
    return;                                       // позицію/час не чіпаємо — наступний кадр порахує dt від останнього виміру
  }
  float rx = b[best].x - px, ry = b[best].y - py;
  t.x = px + c.alpha * rx;
  t.y = py + c.alpha * ry;
  t.vx += c.beta * rx / dt;
  t.vy += c.beta * ry / dt;
  float s = t.speed();
  if (s > c.maxSpeed) { t.vx *= c.maxSpeed / s; t.vy *= c.maxSpeed / s; }
  t.lastMs = nowMs;
  t.misses = 0;
  if (t.hits < 0xFFFF) t.hits++;
}

// ---------------- Калібрування піксель -> кути ----------------
// pan = a0*x + a1*y + a2,  tilt = b0*x + b1*y + b2  (кути в десятих градуса)
struct Affine {
  float a[3] = {0, 0, 900};
  float b[3] = {0, 0, 900};
  bool  valid = false;

  void apply(float x, float y, float &pan10, float &tilt10) const {
    pan10  = a[0] * x + a[1] * y + a[2];
    tilt10 = b[0] * x + b[1] * y + b[2];
  }
};

struct CalPoint { float x, y, pan10, tilt10; };

// Найменші квадрати для 3 і більше точок (не на одній прямій). false — вироджено.
inline bool solveAffine(const CalPoint *p, int n, Affine &out) {
  if (n < 3) return false;
  double m[3][3] = {{0}}, rp[3] = {0}, rt[3] = {0};
  for (int i = 0; i < n; i++) {
    double v[3] = {p[i].x, p[i].y, 1.0};
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 3; c++) m[r][c] += v[r] * v[c];
      rp[r] += v[r] * p[i].pan10;
      rt[r] += v[r] * p[i].tilt10;
    }
  }
  double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
             - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
             + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (fabs(det) < 1e-6) return false;
  // Крамер для двох правих частин
  for (int k = 0; k < 2; k++) {
    double *r = k == 0 ? rp : rt;
    float  *o = k == 0 ? out.a : out.b;
    for (int col = 0; col < 3; col++) {
      double t[3][3];
      memcpy(t, m, sizeof(t));
      for (int row = 0; row < 3; row++) t[row][col] = r[row];
      double d = t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1])
               - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0])
               + t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
      o[col] = (float)(d / det);
    }
  }
  out.valid = true;
  return true;
}

}  // namespace fv
