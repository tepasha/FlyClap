# fly-defense на Rust

Ті самі три прошивки, що й у [`firmware/`](../firmware/), переписані на Rust. Поведінка,
константи й пінаут збігаються з C++-версією: той самий пристрій можна прошити
будь-якою з них.

```
rust/
├── flycore/          # уся логіка без заліза (no_std), тестується на ПК
│   └── src/
│       ├── flyclap.rs    # машина станів хлопавки
│       ├── sonar.rs      # сонарний скан: фон, змійка, ширина, центроїд, "сидить"
│       ├── link.rs       # протокол ESP32 → Arduino + контролер режиму камери
│       ├── turret.rs     # коли можна стріляти (ESP32 і Arduino-камера)
│       ├── vision.rs     # зір: фон, плями, трекер, афінне калібрування
│       ├── servo.rs, time.rs
├── flyclap/          # Arduino Uno / Nano (AVR)
├── flysonar/         # Arduino Uno / Nano (AVR): сонар або цілі від ESP32
└── flysonar-esp32/   # ESP32-CAM / ESP32-S3-CAM
```

Прошивки тонкі: читають входи, викликають `flycore` і застосовують результат до
пінів. Тому все, що може зламатися логічно, покрито тестами на ПК.

## Що змінилося порівняно з C++

- **Спільне ядро замість копій.** У C++ логіка пострілу була двічі: `turret.h`
  (ESP32) і режим камери у `flysonar.ino`. Тут це один `flycore::turret`, а Arduino
  в режимі камери — `Turret` плюс тайм-аут зв'язку (`flycore::link::CameraLink`).
  Протокол `A/M/F/B` теж один: ESP32 форматує `Command`, Arduino його ж розбирає.
- **Серви на апаратному PWM Timer1** (0.5 мкс на тік) замість програмної
  бібліотеки `Servo`. «Відключення» серво FlyClap — це відключення виходу OC1B:
  пін одразу падає в LOW, навіть посеред імпульсу.
- **Arduino в режимі камери приймає UART через переривання** в кільцевий буфер
  (64 байти, як у `Serial`), тож команди від ESP32 не губляться, поки лог пострілу
  блокує передачу.
- **Паніка = безпечний стан.** Обробник паніки знеструмлює соленоїд (FlyClap)
  або помпу/клапан (FlySonar), а не лишає пін як був.
- **`pulseIn` сонара міряє Timer1**, а не підрахунком тактів CPU.
- Без чисел з рухомою комою на AVR: балістична поправка — у цілих (сотих градуса
  на см) з тим самим округленням, що й у C++.
- Лічильник FlyClap в EEPROM і калібрування ESP32 в NVS зберігаються в тому ж
  форматі, що й у C++-версії: при переході прошивки дані не губляться.

## Тести (на ПК)

```bash
cd rust
cargo test
```

38 тестів: усі сценарії з `tools/*_test.cpp` (зір на синтетичних кадрах, турель,
режим камери, симуляція сонара у віртуальній кімнаті) плюс нові тести машини
станів FlyClap. Для деталей симуляції сонара: `cargo test -- --nocapture`.

## Збірка й прошивка

### FlyClap і FlySonar (AVR)

Потрібні `avr-gcc` (лінкер) та [`ravedude`](https://github.com/Rahix/avr-hal/tree/main/ravedude)
(прошивка через avrdude). Тулчейн (nightly + `rust-src`) rustup поставить сам із
`rust-toolchain.toml`.

```bash
cargo install ravedude

cd rust/flyclap
cargo run --release                         # Uno: зібрати, прошити, відкрити монітор
cargo run --release --features tssp         # завіса на TSSP4038

cd rust/flysonar
cargo run --release                         # сонар, помпа
cargo run --release --features valve        # сонар, клапан під тиском
cargo run --release --features camera       # цілі від ESP32 ("очі")
```

Для **Nano**: `--no-default-features --features nano` і `board = "nano"` (або
`"nano-new"`) у `Ravedude.toml`. Інші фічі FlySonar: `moving-targets` (сонар
стріляє й по цілі в польоті).

### FlySonar ESP32

Потрібні тулчейн Xtensa з [`espup`](https://github.com/esp-rs/espup), `ldproxy` і
`espflash`. ESP-IDF і драйвер камери `esp32-camera` build-скрипт завантажить сам.

```bash
cargo install espup ldproxy espflash
espup install

cd rust/flysonar-esp32
cargo run --release                                         # ESP32-CAM (AI-Thinker), самостійна
cargo run --release --features eyes                         # роль "очі" для Arduino
cargo run --release --target xtensa-esp32s3-espidf \
    --no-default-features --features s3-eye                 # ESP32-S3-EYE / Freenove S3
```

Фіча `pump` — помпа R385 замість клапана. Керування й калібрування з консолі —
ті самі клавіші, що в [docs/flysonar-esp32.md](../docs/flysonar-esp32.md).
