# 🪰 fly-defense

Два DIY-проєкти на Arduino для боротьби з мухами:

| Проєкт | Принцип | Сенсор | Виконавчий механізм | Реакція |
|---|---|---|---|---|
| [**FlyClap**](docs/flyclap.md) | «Хлопавка»: муха пролітає між пластинами → вони схлопуються | ІЧ-завіса (4 промені) + LM339 | Соленоїд-засувка + пружини, серво взводить | ~25 мс |
| [**FlySonar**](docs/flysonar.md) | Турель: ехолокатор сканує зону, водомет стріляє по цілі | HC-SR04 на pan-tilt | Помпа + сопло, 2 серви | ~3 с на повний скан |

## Структура

```
fly-defense/
├── firmware/
│   ├── flyclap/flyclap.ino     # ІЧ-завіса + хлопавка
│   └── flysonar/flysonar.ino   # сонар-турель з водометом
├── docs/
│   ├── flyclap.md              # механіка, BOM, схема, налаштування
│   └── flysonar.md
├── platformio.ini
└── LICENSE
```

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

## Безпека

- **FlyClap**: механізм у корпусі з сіткою ~8 мм, пальці не пролазять. Соленоїд живиться лише короткими імпульсами.
- **FlySonar**: фільтр великих об'єктів не стріляє по руках і тваринах, а режим SAFE (тумблер ARM вимкнено) тільки логує цілі. Електроніку тримайте вище за рівень води й за перегородкою.
- Жодних лазерів і високої напруги: свідоме архітектурне рішення, пояснене в [docs/flyclap.md](docs/flyclap.md).

## Ліцензія

MIT, див. [LICENSE](LICENSE).
