/*
 * FlyClap — автоматична "хлопавка" для мух (Arduino Uno / Nano).
 *
 * Муха перетинає ІЧ-завісу → переривання одразу відкриває соленоїд-засувку →
 * пружинні пластини схлопуються → серво знову взводить механізм.
 * Опис механіки й складання — docs/flyclap.md.
 *
 * Модулі:  основа + IrCurtain (тригер) + ClapLatch (виконавець)
 * Збірка:  pio run -e flyclap -t upload   (flyclap_tssp — завіса TSSP4038, flyclap_nano — Nano)
 *
 * Пінаут:
 *   D3      — (лише TSSP) несуча 38 кГц на ключ ІЧ-світлодіодів
 *   D4..D7  — промені завіси (цілий = LOW, перекритий = HIGH)
 *   D8      — кінцевик "взведено" (до GND)
 *   D9      — затвор MOSFET соленоїда (IRLZ44N)
 *   D10     — серво взведення (MG996R)
 *   D11     — бузер: пасивний; для TSSP — активний
 *   D13     — статус-LED
 *   A0      — тумблер ARM (до GND)
 */
#include <Arduino.h>
#include <FlyDefense.h>

// Завіса: 0 — фототранзистори + LM339, 1 — приймачі TSSP4038 (не бояться сонця)
#ifndef BEAM_SENSOR_TSSP
#define BEAM_SENSOR_TSSP 0
#endif

fly::Base base(A0, 13);  // тумблер ARM, статус-LED
fly::IrCurtain curtain(4, 5, 6, 7, BEAM_SENSOR_TSSP ? fly::IrCurtain::TSSP : fly::IrCurtain::COMPARATOR);
fly::ClapLatch clap(curtain, 9, 10, 8);  // соленоїд, серво, кінцевик

void setup() {
  base.buzzer(11, BEAM_SENSOR_TSSP);  // TSSP займає Timer2, тож бузер активний
  base.add(curtain);
  base.add(clap);
  base.begin();
}

void loop() { base.update(); }
