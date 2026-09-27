#!/usr/bin/env bash
# Збирає лендінг у _site/: сторінка + картинки з репозиторію (без дублювання PNG у git).
# Запуск з кореня репозиторію: site/build.sh   → відкрити _site/index.html у браузері.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=_site
rm -rf "$OUT"
mkdir -p "$OUT/img"
cp site/index.html site/donate.config.js "$OUT/"
cp schematics/png/flyclap-device.png           "$OUT/img/flyclap-device.png"
cp schematics/png/flysonar-arduino-device.png  "$OUT/img/flysonar-arduino-device.png"
cp schematics/png/flysonar-esp32-device.png    "$OUT/img/flysonar-esp32-device.png"
cp schematics/png/flyclap-wiring.png           "$OUT/img/flyclap-wiring.png"
cp cad/flyclap/preview/1_assembly.png          "$OUT/img/flyclap-case.png"
cp cad/flyclap/preview/3_section_front.png     "$OUT/img/flyclap-section.png"
cp cad/flysonar/preview/1_arduino.png          "$OUT/img/flysonar-arduino-turret.png"
cp cad/flysonar/preview/2_esp32.png            "$OUT/img/flysonar-esp32-turret.png"
cp cad/flysonar/preview/5_led_stand.png        "$OUT/img/led-stand.png"
# кожна картинка, на яку посилається сторінка, має існувати
missing=0
for f in $(grep -o 'img/[a-z0-9_-]*\.png' "$OUT/index.html" | sort -u); do
  [ -f "$OUT/$f" ] || { echo "НЕМАЄ: $f"; missing=1; }
done
[ $missing -eq 0 ] && echo "Готово: $OUT/index.html"
exit $missing
