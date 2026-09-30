/*
 * Камера ESP32, що бачить мошку (сенсор FlySonar ESP32).
 *
 * Камера дивиться на яскравий підсвічений фон; комаха — темна точка (навіть
 * 2–4 пікселі). Модуль шукає точки, веде ціль, прогнозує її положення на час
 * пострілу, перераховує пікселі в кути й віддає рішення в AimTarget:
 *   - Turret на цій же платі (самостійна збірка), або
 *   - LinkOut → Arduino з EyesLink (роль "очі").
 *
 * Калібрування "піксель → кути" — з USB-консолі (клавіша h — довідка), зберігається
 * у flash (Preferences). Потрібна плата з PSRAM: ESP32-CAM або ESP32-S3 CAM.
 */
#pragma once
#if defined(ESP32)
#include "AimTarget.h"
#include "Module.h"
#include "Vision.h"

namespace fly {

// Піни камери (з camera_pins.h прикладу CameraWebServer)
struct CameraPins {
  int8_t pwdn, reset, xclk, sda, scl;
  int8_t d[8];  // Y2..Y9
  int8_t vsync, href, pclk;
};
extern const CameraPins CAM_AI_THINKER;    // ESP32-CAM (AI-Thinker)
extern const CameraPins CAM_ESP32S3_EYE;   // ESP32-S3-EYE / Freenove ESP32-S3-WROOM CAM

class CameraEyes : public Module {
 public:
  uint32_t latencyMs = 70;      // кадр + обробка + доворот серв + політ струменя
  uint16_t lockHits = 5;        // стільки кадрів поспіль бачимо ціль, перш ніж дозволити постріл
  float stillPxPerS = 15.0f;    // повільніше — ціль "сидить"
  uint32_t bigHoldMs = 3000;    // після великого об'єкта мовчимо стільки
  uint16_t testShotMs = 40;
  bool fireMoving = false;      // стріляти й по цілях у польоті (з упередженням); клавіша t

  CameraEyes(AimTarget &out, const CameraPins &pins) : out_(out), pins_(pins) {}

  const __FlashStringHelper *name() const override { return F("CameraEyes"); }
  bool begin(Base &base) override;
  void loop(Base &base) override;
  Health health() const override { return exposureLocked_ ? Health::Ok : Health::Busy; }

 private:
  static const uint8_t MAX_CAL_POINTS = 9;
  static const uint8_t MAX_BLOBS = 8;

  void handleKey(Base &base, char k);
  void jog(Base &base, int16_t dp, int16_t dt);
  void printStatus(Base &base);
  void loadCal(Base &base);
  void saveCal();
  void lockExposure(Base &base);

  AimTarget &out_;
  const CameraPins &pins_;
  Vision vision_;
  Track track_;
  TrackerConfig trackCfg_;
  Affine cal_;
  CalPoint calPts_[MAX_CAL_POINTS];
  int calCount_ = 0;
  bool calMode_ = false, exposureLocked_ = false;
  int16_t jogPan10_ = 900, jogTilt10_ = 900;
  uint32_t bigUntil_ = 0, bootMs_ = 0, frames_ = 0, fpsMark_ = 0;
  float fps_ = 0;
  Blob blobs_[MAX_BLOBS];
  int blobCount_ = 0;
};

}  // namespace fly
#endif
