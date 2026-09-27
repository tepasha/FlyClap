# 🪰 fly-defense

Два DIY-проєкти на Arduino для боротьби з мухами:

| Проєкт | Принцип | Сенсор | Виконавчий механізм | Реакція |
|---|---|---|---|---|
| [**FlyClap**](docs/flyclap.md) | «Хлопавка»: муха пролітає між пластинами → вони схлопуються | ІЧ-завіса (4 промені) + LM339 | Соленоїд-засувка + пружини, серво взводить | ~25 мс |
| [**FlySonar**](docs/flysonar.md) | Турель: ехолокатор сканує зону, водомет стріляє по цілі | HC-SR04 на pan-tilt | Помпа або клапан + сопло, 2 серви | ~3 с скан + ~1–2 с уточнення |
| [**FlySonar + FlyVision**](docs/flyvision.md) | Та сама турель, але очі — камера над підсвіченим фоном; бачить і мошку | ESP32-CAM + LED-панель | Клапан + розпилювач, 2 серви | ~70–90 мс |

## Структура

```
fly-defense/
├── firmware/
│   ├── flyclap/flyclap.ino     # ІЧ-завіса + хлопавка
│   ├── flysonar/flysonar.ino   # турель з водометом (сонар або цілі від камери)
│   └── flyvision/              # ESP32-CAM: зір, трекер, калібрування
│       ├── flyvision.ino
│       └── vision.h            # ядро зору без залежностей (тестується на ПК)
├── docs/
│   ├── flyclap.md              # механіка, BOM, схема, налаштування
│   ├── flysonar.md
│   ├── flyvision.md            # режим камери: фон, підключення, калібрування
│   └── shopping-list.md        # зведений список покупок для обох проєктів
├── tools/                      # хост-тести: зір, симуляція турелі
├── platformio.ini
└── LICENSE
```

Що купити — див. [docs/shopping-list.md](docs/shopping-list.md).

## Збірка

### Arduino IDE
Відкрити `firmware/flyclap/flyclap.ino` або `firmware/flysonar/flysonar.ino`, обрати плату **Arduino Uno** (Tools → Board → Arduino AVR Boards → Arduino Uno) і прошити. Потрібні лише стандартні бібліотеки `Servo` та `EEPROM`.

### PlatformIO
```bash
pio run -e flyclap -t upload
pio run -e flysonar -t upload
pio device monitor -b 115200
```

> Клони Uno на CH340 потребують драйвера CH340 (Windows/macOS), якщо плату не видно як COM/tty-порт.
>
> Прошивки використовують лише ресурси ATmega328P і працюють без змін і на Nano. Для нього є окремі середовища: `pio run -e flyclap_nano -t upload`, `pio run -e flysonar_nano -t upload`.

Інші варіанти: `flyclap_tssp` (завіса TSSP4038), `flysonar_camera` (+ `_nano`) і `flyvision` / `flyvision_s3` (ESP32) — див. [docs/flyvision.md](docs/flyvision.md).

### Тести
```bash
tools/run_tests.sh   # потрібен лише g++
```
Ядро зору FlyVision на синтетичних кадрах, логіка FlySonar у режимі камери та симуляція режиму сонара (віртуальна кімната + модель пелюстки HC-SR04).

## Безпека

- **FlyClap**: механізм у корпусі з сіткою ~8 мм, пальці не пролазять. Соленоїд живиться лише короткими імпульсами.
- **FlySonar**: фільтр великих об'єктів не стріляє по руках і тваринах, а режим SAFE (тумблер ARM вимкнено) тільки логує цілі. Електроніку тримайте вище за рівень води й за перегородкою.
- Жодних лазерів і високої напруги: свідоме архітектурне рішення, пояснене в [docs/flyclap.md](docs/flyclap.md).

## Ліцензія

MIT, див. [LICENSE](LICENSE).
