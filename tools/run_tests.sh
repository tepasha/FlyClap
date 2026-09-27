#!/usr/bin/env bash
# Хост-тести: зір і турель ESP32-версії, логіка Arduino-версії FlySonar на макеті Arduino API.
# Потрібен лише g++ (C++11). Запуск з кореня репозиторію: tools/run_tests.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
FLAGS="-std=gnu++11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined"

g++ $FLAGS -I firmware/flysonar_esp32 tools/vision_test.cpp -o "$OUT/vision" && "$OUT/vision"
g++ $FLAGS -I firmware/flysonar_esp32 tools/turret_test.cpp -o "$OUT/turret" && "$OUT/turret"
g++ $FLAGS -Wno-unused-function -I tools/mock tools/flysonar_camera_test.cpp -o "$OUT/camera" && "$OUT/camera"
g++ $FLAGS -Wno-unused-function -I tools/mock tools/flysonar_sonar_test.cpp -o "$OUT/sonar" && "$OUT/sonar"
