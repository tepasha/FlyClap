# 🪰 fly-defense

Модульні DIY-пристрої на Arduino та ESP32 для боротьби з мухами. Спершу збирається **основа** (плата, живлення, тумблер ARM, статус-LED), до неї роз'ємами JST-XH підключаються **модулі** на вибір: ІЧ-завіса, хлопавка, турель, сонар, камера. Три готові пристрої — типові комбінації модулів, див. [docs/modules.md](docs/modules.md).

| Проєкт | Принцип | Сенсор | Виконавчий механізм | Реакція |
|---|---|---|---|---|
| [**FlyClap**](docs/flyclap.md) | «Хлопавка»: муха пролітає між пластинами → вони схлопуються | ІЧ-завіса (4 промені) + LM339 | Соленоїд-засувка + пружини, серво взводить | ~25 мс |
| [**FlySonar — Arduino**](docs/flysonar.md) | Турель: ехолокатор сканує зону, водомет стріляє по цілі | HC-SR04 на pan-tilt | Помпа або клапан + сопло, 2 серви | ~3 с скан + ~1–2 с уточнення |
| [**FlySonar — ESP32**](docs/flysonar-esp32.md) | Та сама турель на одній ESP32-CAM: камера над підсвіченим фоном, бачить і мошку | ESP32-CAM + LED-панель | Клапан + розпилювач, 2 серви | ~70–90 мс |

## Структура

```
fly-defense/
├── platformio.ini              # проєкт PlatformIO (фреймворк Arduino): плати й середовища
├── lib/FlyDefense/             # бібліотека: основа (Base) і модулі
│   └── src/fly/                #   IrCurtain, ClapLatch, Turret, PanTilt, Nozzle, Sonar,
│                               #   CameraEyes, EyesLink, LinkOut
├── src/                        # прошивки = готові збірки модулів (по одній на середовище)
│   ├── flyclap/                # ІЧ-завіса + хлопавка
│   ├── flysonar/               # турель із сонаром (Arduino)
│   ├── flysonar_turret/        # турель на Arduino, цілі від ESP32 "очей"
│   ├── flysonar_eyes/          # ESP32-CAM як "очі" для Arduino
│   ├── flysonar_esp32/         # турель на одній ESP32-CAM
│   └── mybuild/                # шаблон своєї збірки
├── test/                       # тести модулів на ПК (pio test -e native), макет Arduino API
├── docs/
│   ├── modules.md              # основа + модулі, роз'єми JST-XH, свій модуль
│   ├── flyclap.md              # механіка, BOM, схема, налаштування
│   ├── flysonar.md             # Arduino-версія
│   ├── flysonar-esp32.md       # ESP32-версія: фон, підключення, калібрування
│   └── shopping-list.md        # зведений список покупок для обох проєктів
├── schematics/                 # схеми підключення, роз'єми JST-XH, ілюстрації (SVG + PNG)
├── cad/flyclap/                # корпус FlyClap для 3D-друку: STEP (FreeCAD), STL, модель CadQuery
├── cad/flysonar/               # турель FlySonar (обидві версії) і коробка основи для 3D-друку
├── tools/diagrams.py           # генератор схем
└── LICENSE
```

Що купити — див. [docs/shopping-list.md](docs/shopping-list.md). Для 3D-друку: корпус FlyClap — [cad/flyclap](cad/flyclap/README.md), турель FlySonar — [cad/flysonar](cad/flysonar/README.md).

## Як виглядають пристрої

| FlyClap | FlySonar — Arduino | FlySonar — ESP32 |
|---|---|---|
| ![FlyClap](schematics/flyclap-device.svg) | ![FlySonar Arduino](schematics/flysonar-arduino-device.svg) | ![FlySonar ESP32](schematics/flysonar-esp32-device.svg) |
| [схема підключення](schematics/flyclap-wiring.svg) · [варіант TSSP4038](schematics/flyclap-tssp.svg) | [схема підключення](schematics/flysonar-arduino-wiring.svg) | [схема підключення](schematics/flysonar-esp32-wiring.svg) |

Роз'єми основи й модулів — на окремій схемі: [modules-connectors](schematics/modules-connectors.svg).

Усі картинки — у папці [`schematics/`](schematics/) (SVG і PNG для друку/телефона). Їх генерує `tools/diagrams.py`: правите скрипт → `python3 tools/diagrams.py --png`.

## Збірка

Проєкт зроблено під [PlatformIO](https://platformio.org/) з фреймворком Arduino. Відкрийте теку репозиторію у VS Code з розширенням PlatformIO IDE або користуйтесь командним рядком; плати, тулчейни й бібліотеку `Servo` PlatformIO завантажить сам.

```bash
pio run -e flyclap -t upload
pio run -e flysonar -t upload
pio device monitor -b 115200
```

> Клони Uno на CH340 потребують драйвера CH340 (Windows/macOS), якщо плату не видно як COM/tty-порт.
>
> Прошивки використовують лише ресурси ATmega328P і працюють без змін і на Nano. Для нього є окремі середовища: `pio run -e flyclap_nano -t upload`, `pio run -e flysonar_nano -t upload`.

FlySonar, ESP32-версія: `pio run -e flysonar_esp32 -t upload` (ESP32-CAM) або `flysonar_esp32_s3` (ESP32-S3 CAM) — див. [docs/flysonar-esp32.md](docs/flysonar-esp32.md).

Інші варіанти: `flyclap_tssp` (завіса TSSP4038), `flysonar_turret` (+ `_nano`) разом з `flysonar_esp32_eyes` (ESP32 як «очі» для Arduino), `mybuild` (шаблон своєї збірки). Кожне середовище збирає одну прошивку з `src/`; модулі лежать у `lib/FlyDefense`.

### Тести
```bash
pio test -e native   # на ПК; потрібен g++ у PATH
```
Модулі бібліотеки на макеті Arduino API: основа (ARM, LED, конфлікти пінів), хлопавка, турель, збірка «турель + очі», симуляція сонара (віртуальна кімната + модель пелюстки HC-SR04), зір на синтетичних кадрах.

## Безпека

- **FlyClap**: механізм у корпусі з сіткою ~8 мм, пальці не пролазять. Соленоїд живиться лише короткими імпульсами.
- **FlySonar**: фільтр великих об'єктів не стріляє по руках і тваринах, а режим SAFE (тумблер ARM вимкнено) тільки логує цілі. Електроніку тримайте вище за рівень води й за перегородкою.
- Жодних лазерів і високої напруги: свідоме архітектурне рішення, пояснене в [docs/flyclap.md](docs/flyclap.md).

## Ліцензія

MIT, див. [LICENSE](LICENSE).
