#if defined(ESP32)
#include "CameraEyes.h"
#include <Preferences.h>
#include "Base.h"
#include "esp_camera.h"

namespace fly {

const CameraPins CAM_AI_THINKER = {32, -1, 0, 26, 27, {5, 18, 19, 21, 36, 39, 34, 35}, 25, 23, 22};
const CameraPins CAM_ESP32S3_EYE = {-1, -1, 15, 4, 5, {11, 9, 8, 10, 12, 18, 17, 16}, 6, 7, 13};

namespace {
const uint32_t EXPOSURE_LOCK_MS = 2500;  // дати автоекспозиції встановитись, потім заморозити
const int16_t JOG_FINE10 = 5;            // 0.5°
const int16_t JOG_COARSE10 = 50;         // 5°
const char *PREFS_NS = "flyvision";
}  // namespace

bool CameraEyes::begin(Base &base) {
  const int8_t used[] = {pins_.pwdn, pins_.reset, pins_.xclk, pins_.sda, pins_.scl, pins_.d[0], pins_.d[1],
                         pins_.d[2], pins_.d[3], pins_.d[4], pins_.d[5], pins_.d[6], pins_.d[7], pins_.vsync,
                         pins_.href, pins_.pclk};
  for (int8_t p : used)
    if (p >= 0 && !base.claimPin((uint8_t)p, name())) return false;
  if (!base.claim(Res::LedcCamera, name())) return false;

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = pins_.d[0]; c.pin_d1 = pins_.d[1]; c.pin_d2 = pins_.d[2]; c.pin_d3 = pins_.d[3];
  c.pin_d4 = pins_.d[4]; c.pin_d5 = pins_.d[5]; c.pin_d6 = pins_.d[6]; c.pin_d7 = pins_.d[7];
  c.pin_xclk = pins_.xclk;
  c.pin_pclk = pins_.pclk;
  c.pin_vsync = pins_.vsync;
  c.pin_href = pins_.href;
  c.pin_sccb_sda = pins_.sda;
  c.pin_sccb_scl = pins_.scl;
  c.pin_pwdn = pins_.pwdn;
  c.pin_reset = pins_.reset;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_GRAYSCALE;  // 1 байт/піксель, без JPEG — одразу в алгоритм
  c.frame_size = FRAMESIZE_QVGA;         // 320×240: ~1 мм/піксель на полі 30 см
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;      // завжди найсвіжіший кадр — менша затримка
  if (esp_camera_init(&c) != ESP_OK) {
    base.configError(name(), F("camera init failed - check camera pins and PSRAM"));
    return false;
  }
  if (!vision_.begin(320, 240)) {
    base.configError(name(), F("not enough memory for vision buffers"));
    return false;
  }
  loadCal(base);
  base.log().println(F("Camera ready. Keys: h - help"));
  bootMs_ = fpsMark_ = millis();
  return true;
}

// Після встановлення експозиції фіксуємо її: автоекспозиція, що "дихає",
// ламає модель фону й дає хибні цілі.
void CameraEyes::lockExposure(Base &base) {
  exposureLocked_ = true;
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_exposure_ctrl(s, 0);
    s->set_gain_ctrl(s, 0);
    s->set_whitebal(s, 0);
  }
  vision_.resetBackground();
  base.log().println(F("Exposure locked, background re-learned"));
}

void CameraEyes::loadCal(Base &base) {
  Preferences prefs;
  prefs.begin(PREFS_NS, true);
  if (prefs.getBytesLength("cal") == sizeof(cal_)) prefs.getBytes("cal", &cal_, sizeof(cal_));
  prefs.end();
  base.log().println(cal_.valid ? F("Calibration loaded") : F("NOT CALIBRATED - press 'm' to calibrate"));
}

void CameraEyes::saveCal() {
  Preferences prefs;
  prefs.begin(PREFS_NS, false);
  prefs.putBytes("cal", &cal_, sizeof(cal_));
  prefs.end();
}

void CameraEyes::jog(Base &base, int16_t dp, int16_t dt) {
  jogPan10_ = constrain(jogPan10_ + dp, 0, 1800);
  jogTilt10_ = constrain(jogTilt10_ + dt, 0, 1800);
  out_.moveTo(jogPan10_, jogTilt10_);
  base.log().printf("aim pan=%.1f tilt=%.1f\n", jogPan10_ / 10.0f, jogTilt10_ / 10.0f);
}

void CameraEyes::printStatus(Base &base) {
  base.log().printf("fps=%.1f cal=%s points=%d mode=%s fireMoving=%d blobs=%d track=%d hits=%u v=%.0fpx/s\n", fps_,
                    cal_.valid ? "ok" : "NONE", calCount_, calMode_ ? "CAL" : "RUN", fireMoving, blobCount_,
                    track_.active, track_.hits, track_.speed());
  if (cal_.valid)
    base.log().printf("pan10 = %.3f*x + %.3f*y + %.1f ; tilt10 = %.3f*x + %.3f*y + %.1f\n", cal_.a[0], cal_.a[1],
                      cal_.a[2], cal_.b[0], cal_.b[1], cal_.b[2]);
}

void CameraEyes::handleKey(Base &base, char k) {
  Print &log = base.log();
  switch (k) {
    case 'm':
      calMode_ = !calMode_;
      log.println(calMode_ ? F("CAL mode: shoot (f), put a dark marker on the wet spot, capture (c)") : F("RUN mode"));
      if (calMode_) out_.moveTo(jogPan10_, jogTilt10_);
      break;
    case 'a': if (calMode_) jog(base, -JOG_FINE10, 0); break;
    case 'd': if (calMode_) jog(base, +JOG_FINE10, 0); break;
    case 'w': if (calMode_) jog(base, 0, +JOG_FINE10); break;
    case 's': if (calMode_) jog(base, 0, -JOG_FINE10); break;
    case 'A': if (calMode_) jog(base, -JOG_COARSE10, 0); break;
    case 'D': if (calMode_) jog(base, +JOG_COARSE10, 0); break;
    case 'W': if (calMode_) jog(base, 0, +JOG_COARSE10); break;
    case 'S': if (calMode_) jog(base, 0, -JOG_COARSE10); break;
    case 'f':
      if (calMode_ && !out_.testShot(testShotMs))
        log.println(F("Test shot refused: ARM off, servo moving or big object hold"));
      break;
    case 'c':
      if (!calMode_) break;
      if (blobCount_ != 1) {
        log.printf("Need exactly one dark dot in view, see %d\n", blobCount_);
        break;
      }
      if (calCount_ >= MAX_CAL_POINTS) {
        log.println(F("Too many points, solve with 'v'"));
        break;
      }
      calPts_[calCount_++] = {blobs_[0].x, blobs_[0].y, (float)jogPan10_, (float)jogTilt10_};
      log.printf("Point %d: px=(%.1f, %.1f) -> pan=%.1f tilt=%.1f\n", calCount_, blobs_[0].x, blobs_[0].y,
                 jogPan10_ / 10.0f, jogTilt10_ / 10.0f);
      break;
    case 'u':
      if (calCount_) log.printf("Points: %d\n", --calCount_);
      break;
    case 'v': {
      Affine a;
      if (!solveAffine(calPts_, calCount_, a)) {
        log.println(F("Need >=3 points not on one line"));
        break;
      }
      cal_ = a;
      saveCal();
      float worst = 0;  // залишкова похибка по точках
      for (int i = 0; i < calCount_; i++) {
        float p, t;
        cal_.apply(calPts_[i].x, calPts_[i].y, p, t);
        worst = fmaxf(worst, fmaxf(fabsf(p - calPts_[i].pan10), fabsf(t - calPts_[i].tilt10)));
      }
      log.printf("Saved. Worst residual %.2f deg. Remove markers and press 'b'.\n", worst / 10.0f);
      calCount_ = 0;
      break;
    }
    case 'b':
      vision_.resetBackground();
      log.println(F("Background reset"));
      break;
    case 't':
      fireMoving = !fireMoving;
      log.printf("fireMoving=%d\n", fireMoving);
      break;
    case 'p': printStatus(base); break;
    case 'h':
    case '?':
      log.println(F("Keys:\n"
                    "  m        calibration mode on/off\n"
                    "  a d w s  jog 0.5 deg (A D W S = 5 deg)   [cal mode]\n"
                    "  f        test shot (ARM must be on)      [cal mode]\n"
                    "  c        capture point: marker on the wet spot, exactly one dark dot in view\n"
                    "  u        undo last point,  v  solve & save (>=3 points)\n"
                    "  b        re-learn background (clear backdrop first)\n"
                    "  t        toggle firing at moving targets (lead aim)\n"
                    "  p        status"));
      break;
  }
}

void CameraEyes::loop(Base &base) {
  while (Serial.available()) handleKey(base, (char)Serial.read());

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  const uint32_t now = millis();
  bool big = false;
  blobCount_ = vision_.process(fb->buf, blobs_, MAX_BLOBS, big);
  esp_camera_fb_return(fb);

  if (!exposureLocked_ && now - bootMs_ > EXPOSURE_LOCK_MS) lockExposure(base);

  frames_++;
  if (now - fpsMark_ >= 1000) {
    fps_ = frames_ * 1000.0f / (now - fpsMark_);
    frames_ = 0;
    fpsMark_ = now;
  }

  if (big) {
    if (reached(now, bigUntil_)) base.log().println(F("Big object - holding fire"));
    bigUntil_ = now + bigHoldMs;
    out_.bigObject();
  }

  if (calMode_ || !exposureLocked_) return;  // у калібруванні приціл веде людина

  trackUpdate(track_, blobs_, blobCount_, now, trackCfg_);
  if (!track_.active || !cal_.valid) return;

  const bool still = track_.speed() < stillPxPerS;
  // По нерухомій цілі цілимось у поточну точку, по рухомій — з упередженням
  const float x = still ? track_.x : track_.predictX(latencyMs);
  const float y = still ? track_.y : track_.predictY(latencyMs);
  float pan10, tilt10;
  cal_.apply(x, y, pan10, tilt10);

  const bool fire = track_.hits >= lockHits && track_.misses == 0 && reached(now, bigUntil_) && (still || fireMoving);
  out_.aim((int)lroundf(pan10), (int)lroundf(tilt10), fire);
}

}  // namespace fly
#endif
