/*
 * FlyDefense — модульні DIY-пристрої проти мух.
 *
 * Основа (fly::Base) + модулі на вибір:
 *
 *   Тригери й сенсори          Виконавці                 Зв'язок між платами
 *   ─────────────────          ─────────                 ───────────────────
 *   IrCurtain   (AVR)          ClapLatch  (хлопавка)     LinkOut   ("очі" → UART)
 *   Sonar                      Turret = PanTilt + Nozzle EyesLink  (UART → турель)
 *   CameraEyes  (ESP32)
 *
 * Готові збірки — прошивки в src/ (по одній на середовище PlatformIO), опис
 * модулів, роз'єми й як написати свій модуль — docs/modules.md.
 */
#pragma once

#include "fly/Base.h"
#include "fly/Module.h"

#include "fly/ClapLatch.h"
#include "fly/IrCurtain.h"
#include "fly/Trigger.h"

#include "fly/Nozzle.h"
#include "fly/PanTilt.h"
#include "fly/Sonar.h"
#include "fly/Turret.h"

#include "fly/EyesLink.h"
#include "fly/LinkOut.h"

#if defined(ESP32)
#include "fly/CameraEyes.h"
#endif
