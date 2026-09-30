#!/usr/bin/env python3
"""
FlySonar — турель pan-tilt для 3D-друку (параметрична модель CadQuery).

Одна поворотна основа на дві версії:
  * Arduino-версія: головка з HC-SR04 і соплом під ним (head_sonar);
  * ESP32-версія:   головка лише з соплом на осі нахилу (head_nozzle)
                    + кріплення ESP32-CAM на стійці (camera_post, camera_cradle).
Для обох — коробка основи (electronics_box, electronics_lid) з вікнами під роз'єми
JST-XH, до яких підключаються модулі (docs/modules.md).

Запуск з кореня репозиторію:
    pip install cadquery
    python3 cad/flysonar/flysonar_turret.py            # перевірки + STEP/STL
    python3 cad/flysonar/flysonar_turret.py --preview  # + картинки (numpy, Chromium)

Система координат: Z — вгору (0 — низ основи), вісь PAN — вертикаль X=Y=0,
турель дивиться в +X, вісь TILT — горизонталь уздовж Y на висоті TILT_Z.
"""
import os
import sys

import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import cadlib  # noqa: E402
from cadlib import B, cylX, cylY, cylZ, overlap, fits_bed, single_solid  # noqa: E402

BED = (220.0, 220.0, 250.0)
CLR = 0.3

# ============================ ПАРАМЕТРИ ============================
# Мікросерво MG90S / SG90 / ES08MD (звірте свій екземпляр штангенциркулем!)
SV_L, SV_W = 22.8, 12.2          # корпус: довжина × ширина
SV_TAB_Z = 15.9                  # від низу корпусу до низу вушок
SV_TAB_T = 2.5                   # товщина вушок
SV_CASE_H = 22.7                 # від низу до верху корпусу
SV_SHAFT_H = 26.7                # від низу до торця вала
SV_SHAFT_OFF = 5.9               # вал — на такій відстані від торця корпусу
SV_TAB_SPAN = 32.5               # вушка від краю до краю
SV_HOLE_SPAN = 27.8              # між отворами у вушках
SV_SCREW = 1.6                   # отвір під саморіз 2 мм з комплекту серви
HORN_T, HORN_GAP = 2.0, 0.5      # качалка на валу

# Основа (PAN)
BASE = 64.0
BASE_H = 18.0
FLOOR = 2.0

# Платформа + вилка (обертається серво PAN)
PLATTER_D, PLATTER_T = 72.0, 4.0
TILT_ABOVE_PLATTER = 30.0        # висота осі нахилу над платформою
UP_T = 3.0                       # товщина стійок вилки
HEAD_HALF = 27.5                 # половина ширини головки (зовнішні грані)

# HC-SR04 (плата 45 × 20 × 1,6; випромінювачі Ø16, між центрами 26 мм)
SR_W, SR_H, SR_T = 45.0, 20.0, 1.6
SR_CAN_D, SR_CAN_DY, SR_CAN_L = 16.0, 26.0, 12.0

# Сопло: латунна трубка / голка Ø4 (або силікон 4×6 — тоді NOZ_D = 6.2)
NOZ_D = 4.2
NOZ_Z_SONAR = -17.0              # сопло під сонаром (відносно осі нахилу)

# ESP32-CAM (плата 27 × 40,5; висота з деталями ~5 мм)
CAM_W, CAM_L, CAM_T = 27.0, 40.5, 5.0
CAM_POST_H = 90.0                # висота осі нахилу камери над столом

# Роз'єми основи турелі (docs/modules.md): вікна в стінці коробки електроніки.
# Arduino-версія використовує всі; ESP32-версія — серви й P1 (камера на своїй стійці).
BASE_PORTS = [
    ("L: зв'язок з \"очима\" (XH-3)", "XH3"),
    ("S2: кнопка RECAL (XH-4)", "XH4"),
    ("S1: сонар HC-SR04 (XH-4)", "XH4"),
    ("servo: TILT", "servo"),
    ("servo: PAN", "servo"),
    ("P1: помпа / клапан (XH-2)", "XH2"),
]
BOX_AT = (-150.0, -50.0, 0.0)    # місце коробки на дошці відносно турелі

TILT_RANGE = (-35, 45)           # діапазон нахилу, що перевіряється (град)
PAN_RANGE = (-90, 90)

# ============================ ПОХІДНІ РОЗМІРИ ============================
SHAFT_TOP_PAN = BASE_H - SV_TAB_Z + SV_SHAFT_H                  # торець вала PAN
PLATTER_Z0 = SHAFT_TOP_PAN + HORN_GAP + HORN_T                  # низ платформи
PLATTER_Z1 = PLATTER_Z0 + PLATTER_T
TILT_Z = PLATTER_Z1 + TILT_ABOVE_PLATTER
# серво TILT: качалка прикручена до правої щоки головки, вушка серви — до зовнішньої грані стійки
SHAFT_TOP_TILT_Y = HEAD_HALF + HORN_T + HORN_GAP                 # торець вала TILT
UP_OUT = SHAFT_TOP_TILT_Y + (SV_SHAFT_H - SV_TAB_Z - SV_TAB_T)   # зовнішня грань правої стійки
UP_IN = UP_OUT - UP_T
CAM_FRAME_H = CAM_L + 2 * 1.6 + 2 * CLR
CAM_ZC = CAM_POST_H + 10 + CAM_FRAME_H / 2                         # центр рамки: низ на 10 мм над віссю (вище вушок вилки)


# ============================ ДЕТАЛІ ============================
def base():
    """Основа: кишеня під серво PAN (вал угору, на осі), вушка лягають на верх,
    проріз для кабелю, 4 отвори для кріплення до дошки."""
    b = (cq.Workplane("XY").rect(BASE, BASE).extrude(BASE_H).edges("|Z").fillet(6))
    x0, x1 = -SV_SHAFT_OFF, SV_L - SV_SHAFT_OFF
    b = b.cut(B(x0 - CLR, x1 + CLR, -SV_W / 2 - CLR, SV_W / 2 + CLR, FLOOR, BASE_H + 1))
    cx = (x0 + x1) / 2
    for dx in (-SV_HOLE_SPAN / 2, SV_HOLE_SPAN / 2):
        b = b.cut(cylZ(cx + dx, 0, SV_SCREW / 2, BASE_H - 8, BASE_H + 1))
    b = b.cut(B(x1, BASE / 2 + 1, -3, 3, FLOOR, FLOOR + 7))                  # кабель
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * (BASE / 2 - 6), sy * (BASE / 2 - 6)
            b = b.cut(cylZ(x, y, 1.75, -1, BASE_H + 1)).cut(cylZ(x, y, 3.4, 4, BASE_H + 1))
    return b


def yoke():
    """Платформа (кріпиться до качалки PAN) з вилкою: права стійка — вікно під серво TILT,
    ліва — вісь-гвинт М3. Затискач шланга ззаду."""
    p = cylZ(0, 0, PLATTER_D / 2, PLATTER_Z0, PLATTER_Z1)
    p = p.cut(cylZ(0, 0, 3.2, PLATTER_Z0 - 1, PLATTER_Z1 + 1))              # доступ до гвинта качалки
    xs = (-26.0, 12.0)
    top = TILT_Z + 16
    for s in (-1, 1):
        y0, y1 = s * UP_IN, s * UP_OUT
        up = B(xs[0], xs[1], y0, y1, PLATTER_Z1 - 1, top)
        up = up.union(B(xs[0], xs[1], y0 - s * 3, y1, PLATTER_Z1 - 1, PLATTER_Z1 + 3))  # ребро
        p = p.union(up)
    # серво TILT у правій стійці: вал на осі нахилу, корпус назад (−X)
    bx0, bx1 = -(SV_L - SV_SHAFT_OFF), SV_SHAFT_OFF
    p = p.cut(B(bx0 - CLR, bx1 + CLR, UP_IN - 1, UP_OUT + 1, TILT_Z - SV_W / 2 - CLR, TILT_Z + SV_W / 2 + CLR))
    cx = (bx0 + bx1) / 2
    for dx in (-SV_HOLE_SPAN / 2, SV_HOLE_SPAN / 2):
        p = p.cut(cylY(cx + dx, TILT_Z, SV_SCREW / 2, UP_IN - 1, UP_OUT + 1))
    # ліва стійка: отвір під саморіз М3 (вісь), головка обертається на гвинті
    p = p.cut(cylY(0, TILT_Z, 1.4, -UP_OUT - 1, -UP_IN + 1))
    # затискач шланга ззаду (силікон 4×6)
    p = p.union(B(-PLATTER_D / 2 - 6, -PLATTER_D / 2 + 4, -6, 6, PLATTER_Z0, PLATTER_Z1 + 10))
    p = p.cut(cylY(-PLATTER_D / 2 - 1, PLATTER_Z1 + 5, 3.2, -7, 7))
    p = p.cut(B(-PLATTER_D / 2 - 7, -PLATTER_D / 2 - 1, -7, 7, PLATTER_Z1 + 7.5, PLATTER_Z1 + 11))
    return p


def _head_frame(z_lo, z_hi, x_lo=-12.0, x_hi=6.0):
    """Бічні щоки головки: права — під качалку TILT, ліва — втулка на вісь М3."""
    h = B(x_lo, x_hi, HEAD_HALF - 2.5, HEAD_HALF, z_lo, z_hi)
    h = h.union(B(x_lo, x_hi, -HEAD_HALF, -HEAD_HALF + 2.5, z_lo, z_hi))
    h = h.union(cylY(0, 0, 5, -UP_IN + 0.5, -HEAD_HALF + 1))                 # втулка до лівої стійки
    h = h.cut(cylY(0, 0, 1.7, -UP_IN, -HEAD_HALF + 3))                        # М3 крізь втулку
    h = h.cut(cylY(0, 0, 3.0, HEAD_HALF - 3, HEAD_HALF + 1))                  # доступ до гвинта качалки
    return h


def head_sonar():
    """Головка Arduino-версії: HC-SR04 (штирями ВГОРУ) + сопло під ним.
    Локальні координати: вісь нахилу — вісь Y, (0,0,0) — центр обертання."""
    fx0, fx1 = 2.0, 4.5                                                       # передня пластина
    h = _head_frame(-23, 17, x_hi=fx1)
    h = h.union(B(fx0, fx1, -HEAD_HALF, HEAD_HALF, -13, 13))
    for s in (-1, 1):
        # U-паз, відкритий зверху: плата опускається згори, випромінювачі лягають на півкола
        h = h.cut(cylX(s * SR_CAN_DY / 2, 0, SR_CAN_D / 2 + 0.3, fx0 - 1, fx1 + 1))
        h = h.cut(B(fx0 - 1, fx1 + 1, s * SR_CAN_DY / 2 - SR_CAN_D / 2 - 0.3, s * SR_CAN_DY / 2 + SR_CAN_D / 2 + 0.3, 0, 20))
    # паз під плату: між передньою пластиною і задніми упорами, полиця знизу
    bx0 = fx0 - SR_T - 0.1
    for s in (-1, 1):
        h = h.union(B(bx0 - 1.8, bx0, s * (SR_W / 2 - 4), s * HEAD_HALF, -SR_H / 2 - 2, SR_H / 2))
    h = h.union(B(bx0 - 1.8, fx0, -SR_W / 2 - 0.5, SR_W / 2 + 0.5, -SR_H / 2 - 2.2, -SR_H / 2 - 0.2))
    # низ + блок сопла
    h = h.union(B(-12, fx1, -HEAD_HALF, HEAD_HALF, -23, -20.5))
    h = h.union(B(-12, fx1 + 6, -6, 6, -23, -11.5))
    h = h.cut(cylX(0, NOZ_Z_SONAR, NOZ_D / 2, -13, fx1 + 7))
    h = h.cut(B(-6, -2, -7, 7, -24, -21))                                    # проріз під стяжку
    return h


def head_nozzle():
    """Головка ESP32-версії: лише сопло, точно на осі нахилу (немає паралакса по висоті)."""
    h = _head_frame(-14, 12, x_hi=6.0)
    h = h.union(B(-12, 6, -HEAD_HALF, HEAD_HALF, -14, -11.5))
    h = h.union(B(-12, 14, -6, 6, -11.5, 6))
    h = h.cut(cylX(0, 0, NOZ_D / 2, -13, 15))
    h = h.cut(B(-6, -2, -7, 7, -12, -7))                                     # стяжка
    return h


def camera_post():
    """Стійка камери: основа з двома пазами під М4 + колона + вилка з віссю М3."""
    p = B(-30, 30, -20, 20, 0, 4)
    for sx in (-1, 1):
        p = p.cut(B(sx * 22 - 2.2, sx * 22 + 2.2, -10, 10, -1, 5))
    p = p.union(B(-7, 7, -7, 7, 0, CAM_POST_H - 6))
    for sy in (-1, 1):
        p = p.union(B(-7, 7, sy * 5.5 - 1.5, sy * 5.5 + 1.5, CAM_POST_H - 8, CAM_POST_H + 7))
    return p.cut(cylY(0, CAM_POST_H, 1.7, -10, 10))


def camera_cradle():
    """Рамка під ESP32-CAM: плата вкладається ззаду, лицьова рамка з великим вікном
    (лінза може бути будь-де), дві прорізі під стяжку. Язичок з віссю М3 — у вилку стійки."""
    t = 1.6
    ox, oy = CAM_W + 2 * t + 2 * CLR, CAM_L + 2 * t + 2 * CLR
    zc = CAM_ZC
    c = B(-3, CAM_T + 2, -ox / 2, ox / 2, zc - oy / 2, zc + oy / 2)
    c = c.cut(B(0, CAM_T + 3, -CAM_W / 2 - CLR, CAM_W / 2 + CLR, zc - CAM_L / 2 - CLR, zc + CAM_L / 2 + CLR))
    c = c.cut(B(-4, 1, -CAM_W / 2 + 3, CAM_W / 2 - 3, zc - CAM_L / 2 + 3, zc + CAM_L / 2 - 3))   # вікно
    for z in (zc - 10, zc + 10):
        c = c.cut(B(CAM_T - 1, CAM_T + 3, -ox / 2 - 1, ox / 2 + 1, z - 2, z + 2))            # стяжки
    tab = B(-3, 5, -3.8, 3.8, CAM_POST_H, zc - oy / 2 + 1)
    c = c.union(tab).union(cylY(1, CAM_POST_H, 5, -3.8, 3.8))
    return c.cut(cylY(1, CAM_POST_H, 1.7, -5, 5))


def electronics_box():
    """Коробка основи: плата 5×7 см, вікна під роз'єми модулів, лапки під шурупи."""
    return cadlib.base_box(BASE_PORTS, feet=True)


def electronics_lid():
    return cadlib.base_lid()


# ============================ МУЛЯЖІ ============================
def servo_dummy():
    """Мікросерво: вал на початку координат, угору (+Z); низ корпусу z = −SV_SHAFT_H."""
    z0 = -SV_SHAFT_H
    x0 = -SV_SHAFT_OFF
    s = B(x0, x0 + SV_L, -SV_W / 2, SV_W / 2, z0, z0 + SV_CASE_H)
    cx = x0 + SV_L / 2
    s = s.union(B(cx - SV_TAB_SPAN / 2, cx + SV_TAB_SPAN / 2, -SV_W / 2, SV_W / 2, z0 + SV_TAB_Z, z0 + SV_TAB_Z + SV_TAB_T))
    s = s.union(cylZ(0, 0, 3, z0 + SV_CASE_H, 0))
    return s


def horn_dummy():
    return cylZ(0, 0, 3.5, HORN_GAP - 0.01, HORN_GAP + HORN_T).union(
        B(-16, 16, -3, 3, HORN_GAP, HORN_GAP + HORN_T))


def hcsr04_dummy():
    fx0 = 2.0
    d = B(fx0 - SR_T, fx0, -SR_W / 2, SR_W / 2, -SR_H / 2, SR_H / 2)
    for s in (-1, 1):
        d = d.union(cylX(s * SR_CAN_DY / 2, 0, SR_CAN_D / 2, fx0, fx0 + SR_CAN_L + 2.5))
    d = d.union(B(fx0 - SR_T - 3, fx0 - SR_T, -8, 8, -6, 6))                 # мікросхеми ззаду
    d = d.union(B(fx0 - SR_T - 2.5, fx0 - SR_T, -5.1, 5.1, SR_H / 2, SR_H / 2 + 8))   # штирі вгору
    return d


def esp32cam_dummy():
    zc = CAM_ZC
    return B(0, CAM_T, -CAM_W / 2, CAM_W / 2, zc - CAM_L / 2, zc + CAM_L / 2)


# ============================ ПОЗИЦІЇ ============================
def at_pan_servo(wp):
    """Серво PAN: вал на осі, торець вала на висоті SHAFT_TOP_PAN."""
    return wp.translate((0, 0, SHAFT_TOP_PAN))


def at_tilt_servo(wp):
    """Серво TILT: вал уздовж −Y (всередину), корпус назад (−X), торець вала на SHAFT_TOP_TILT_Y."""
    # корпус назад: поворот на 180° навколо Z; далі локальна +Z → глобальна −Y (90° навколо X)
    w = wp.rotate((0, 0, 0), (0, 0, 1), 180).rotate((0, 0, 0), (1, 0, 0), 90)
    return w.translate((0, SHAFT_TOP_TILT_Y, TILT_Z))


def tilt(wp, deg):
    """Нахил головки навколо осі TILT (+ — ціль вище)."""
    return wp.rotate((0, 0, 0), (0, 1, 0), -deg).translate((0, 0, TILT_Z))


def pan(wp, deg):
    return wp.rotate((0, 0, 0), (0, 0, 1), deg)


PRINTED = {
    "base": base,
    "yoke": yoke,
    "head_sonar": head_sonar,
    "head_nozzle": head_nozzle,
    "camera_post": camera_post,
    "camera_cradle": camera_cradle,
    "electronics_box": electronics_box,
    "electronics_lid": electronics_lid,
}


def assembly(parts, version="sonar", pan_deg=0.0, tilt_deg=0.0):
    """Список (назва, деталь, колір) у складанні."""
    head = parts["head_sonar"] if version == "sonar" else parts["head_nozzle"]
    moving = [
        ("yoke", parts["yoke"], (0.93, 0.93, 0.95)),
        ("servo_tilt", at_tilt_servo(servo_dummy()), (0.18, 0.2, 0.25)),
        ("horn_tilt", tilt(horn_dummy().rotate((0, 0, 0), (1, 0, 0), 90).translate((0, SHAFT_TOP_TILT_Y, 0)), tilt_deg),
         (0.95, 0.95, 0.95)),
        ("head", tilt(head, tilt_deg), (0.95, 0.55, 0.15)),
    ]
    if version == "sonar":
        moving.append(("hcsr04", tilt(hcsr04_dummy(), tilt_deg), (0.12, 0.4, 0.7)))
    items = [("base", parts["base"], (0.3, 0.33, 0.38)),
             ("servo_pan", at_pan_servo(servo_dummy()), (0.18, 0.2, 0.25)),
             ("horn_pan", at_pan_servo(horn_dummy()), (0.95, 0.95, 0.95))]
    items += [(n, pan(w, pan_deg), c) for n, w, c in moving]
    at_box = lambda w: w.translate(BOX_AT)
    items += [("electronics_box", at_box(parts["electronics_box"]), (0.25, 0.28, 0.33)),
              ("electronics_lid", at_box(parts["electronics_lid"].translate((0, 0, cadlib.BOX_H))), (0.25, 0.28, 0.33)),
              ("base_pcb", at_box(cadlib.pcb_dummy()), (0.1, 0.45, 0.25)),
              ("base_plugs", at_box(cadlib.plug_dummies(BASE_PORTS)), (0.92, 0.9, 0.82))]
    if version == "nozzle":
        cam = lambda w: w.translate((-40, -90, 0))
        items += [("camera_post", cam(parts["camera_post"]), (0.3, 0.33, 0.38)),
                  ("camera_cradle", cam(parts["camera_cradle"]), (0.95, 0.55, 0.15)),
                  ("esp32cam", cam(esp32cam_dummy()), (0.1, 0.1, 0.1))]
    return items


# ============================ ПЕРЕВІРКИ ============================
def checks(parts):
    ok = True
    print("== Деталі: стіл %gx%gx%g, одне тіло ==" % BED)
    for n, w in parts.items():
        bb = w.val().BoundingBox()
        f, one = fits_bed(w, BED), single_solid(w)
        ok &= f and one
        print(f"  {n:14s} {bb.xlen:6.1f} × {bb.ylen:6.1f} × {bb.zlen:6.1f} мм   {'OK' if f else 'НЕ ВЛАЗИТЬ'}"
              f"{'' if one else '   НЕ ОДНЕ ТІЛО!'}")
    print(f"\n  вісь PAN: вал на висоті {SHAFT_TOP_PAN:.1f} мм, платформа {PLATTER_Z0:.1f}…{PLATTER_Z1:.1f} мм,"
          f" вісь TILT на {TILT_Z:.1f} мм; стійки вилки |y| = {UP_IN:.1f}…{UP_OUT:.1f} мм")

    pairs_static = [("base", "servo_pan"), ("yoke", "servo_pan"), ("yoke", "horn_pan"), ("base", "horn_pan"),
                    ("yoke", "servo_tilt"), ("head", "servo_tilt"), ("head", "yoke"), ("horn_tilt", "yoke"),
                    ("horn_tilt", "servo_tilt"), ("hcsr04", "head"), ("hcsr04", "yoke"), ("hcsr04", "servo_tilt"),
                    ("head", "base"), ("hcsr04", "base"), ("servo_tilt", "base"), ("yoke", "base")]
    worst = 0.0
    for version in ("sonar", "nozzle"):
        for t in range(TILT_RANGE[0], TILT_RANGE[1] + 1, 10 if TILT_RANGE[1] - TILT_RANGE[0] > 40 else 5):
            for p in (PAN_RANGE[0], 0, PAN_RANGE[1]):
                it = {n: w for n, w, _ in assembly(parts, version, p, t)}
                for a, b in pairs_static:
                    if a not in it or b not in it:
                        continue
                    v = overlap(it[a], it[b])
                    worst = max(worst, v)
                    if v >= 0.5:
                        ok = False
                        print(f"  {version}: pan {p}°, tilt {t}°: {a} ∩ {b} = {v:.2f} мм³  ПЕРЕТИН!")
        if version == "nozzle":
            it = {n: w for n, w, _ in assembly(parts, version)}
            for a, b in (("camera_cradle", "camera_post"), ("esp32cam", "camera_cradle"), ("esp32cam", "camera_post")):
                v = overlap(it[a], it[b])
                worst = max(worst, v)
                if v >= 0.5:
                    ok = False
                    print(f"  {a} ∩ {b} = {v:.2f} мм³  ПЕРЕТИН!")
    # коробка основи: плата і штекери у вікнах, сама коробка не заважає турелі й стійці камери
    it = {n: w for n, w, _ in assembly(parts, "nozzle")}
    wbox = 0.0
    for a, b in (("base_pcb", "electronics_box"), ("base_plugs", "electronics_box"), ("base_pcb", "electronics_lid"),
                 ("electronics_box", "base"), ("electronics_box", "yoke"), ("electronics_box", "camera_post"),
                 ("base_plugs", "base"), ("base_plugs", "yoke")):
        wbox = max(wbox, overlap(it[a], it[b]))
    print(f"\n== Коробка основи: плата, штекери у вікнах, сусідні деталі: найбільший перетин {wbox:.2f} мм³  "
          f"{'OK' if wbox < 0.5 else 'ПЕРЕТИН!'}")
    ok &= wbox < 0.5
    # складання HC-SR04: плата опускається згори в паз — на всьому шляху не чіпляє головку
    head = parts["head_sonar"]
    wmax = 0.0
    for dz in range(0, 41, 4):
        wmax = max(wmax, overlap(head, hcsr04_dummy().translate((0, 0, dz))))
    print(f"\n== Встановлення HC-SR04 згори (0…40 мм шляху): найбільший перетин {wmax:.2f} мм³  "
          f"{'OK' if wmax < 0.5 else 'НЕ ВСТАВЛЯЄТЬСЯ!'}")
    ok &= wmax < 0.5
    # камера: нахил рамки ±25° на гвинті М3
    wcam = 0.0
    for d in range(-25, 26, 5):
        c = parts["camera_cradle"].rotate((0, 0, CAM_POST_H), (0, 1, CAM_POST_H), d)
        wcam = max(wcam, overlap(c, parts["camera_post"]))
    print(f"== Нахил камери ±25°: найбільший перетин {wcam:.2f} мм³  {'OK' if wcam < 0.5 else 'ПЕРЕТИН!'}")
    ok &= wcam < 0.5
    print(f"\n== Рух: tilt {TILT_RANGE[0]}…{TILT_RANGE[1]}°, pan {PAN_RANGE[0]}…{PAN_RANGE[1]}°, обидві головки: "
          f"найбільший перетин {worst:.2f} мм³  {'OK' if worst < 0.5 else 'ПЕРЕТИН!'}")
    return ok


def export(parts):
    for d in ("step", "stl"):
        os.makedirs(os.path.join(HERE, d), exist_ok=True)
    for n, w in parts.items():
        cq.exporters.export(w, os.path.join(HERE, "step", n + ".step"))
        cq.exporters.export(w, os.path.join(HERE, "stl", n + ".stl"), tolerance=0.03, angularTolerance=0.1)
    for version, fname in (("sonar", "turret_arduino_assembly.step"), ("nozzle", "turret_esp32_assembly.step")):
        asm = cq.Assembly(name="FlySonar_" + version)
        for n, w, rgb in assembly(parts, version, 0, 15):
            asm.add(w, name=n, color=cq.Color(*rgb))
        asm.save(os.path.join(HERE, fname))
    print("\nЕкспорт: step/, stl/, turret_arduino_assembly.step, turret_esp32_assembly.step")


def previews(parts):
    import render
    out = os.path.join(HERE, "preview")
    os.makedirs(out, exist_ok=True)
    size = (1200, 900)

    box_parts = ("electronics_box", "electronics_lid", "base_pcb", "base_plugs")
    items = [(w, c) for n, w, c in assembly(parts, "sonar", 20, 15) if n not in box_parts]
    pth = os.path.join(out, "1_arduino.png")
    pj = render.render_png(items, pth, yaw=-120, pitch=-65, size=size, margin=(70, 250, 60, 250))
    lab = []
    for (x, y, z), (dx, dy), t in (
            ((0, 0, BASE_H - 4), (-230, 40), "основа + серво PAN"),
            ((-6, UP_OUT + 12, TILT_Z), (170, -30), "серво TILT"),
            ((-20, -30, PLATTER_Z1), (-120, 40), "платформа з вилкою"),
            ((14, -13, TILT_Z + 4), (-190, -120), "HC-SR04 (штирі вгору)"),
            ((10, 0, TILT_Z - 17), (220, 70), "сопло Ø4 під сонаром"),
            ((0, -UP_OUT, TILT_Z), (-160, -40), "вісь-гвинт М3")):
        import math
        a = math.radians(20)
        px, py = pj(x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a), z)
        lab.append((px, py, px + dx, py + dy, t))
    render.add_text(pth, size, "FlySonar, Arduino-версія: турель pan-tilt з HC-SR04", labels=lab,
                    notes=("Осі PAN і TILT перетинаються — приціл не зсувається при повороті.",))

    items = [(w, c) for n, w, c in assembly(parts, "nozzle", -25, 10) if n not in box_parts]
    pth = os.path.join(out, "2_esp32.png")
    render.render_png(items, pth, yaw=-30, pitch=-65, size=size, margin=(70, 60, 60, 60))
    render.add_text(pth, size, "FlySonar, ESP32-версія: турель із соплом + кріплення ESP32-CAM",
                    notes=("Сопло точно на осі нахилу. Камера на окремій стійці, нахил — на гвинті М3.",))

    # нахил: три положення головки збоку
    # нахил: розріз по центру (Y = 0), три положення поруч
    keep = B(-500, 500, 0, 500, -500, 500)
    items = []
    for i, t in enumerate((-35, 0, 45)):
        for n, w, c in assembly(parts, "sonar", 0, t):
            if n in ("head", "hcsr04", "yoke", "horn_tilt"):
                items.append((w.intersect(keep).translate((i * 110, 0, 0)), c))
    pth = os.path.join(out, "3_tilt_range.png")
    pj = render.render_png(items, pth, yaw=0, pitch=-90, size=(1200, 600), margin=(80, 40, 70, 40))
    lab = [(pj(i * 110, 0, PLATTER_Z0 - 4)[0], pj(0, 0, PLATTER_Z0 - 4)[1], pj(i * 110, 0, PLATTER_Z0 - 4)[0],
            pj(0, 0, PLATTER_Z0 - 4)[1] + 1, t) for i, t in enumerate(("−35°", "0°", "+45°"))]
    render.add_text(pth, (1200, 600), "Розріз по центру: нахил головки −35° … +45° (перевірено на зіткнення)",
                    labels=lab,
                    notes=("Сопло — на 17 мм нижче осі нахилу (у ESP32-версії — точно на осі).",))

    layout = [("base", (0, 0, 0)), ("yoke", (95, 0, 0)), ("head_sonar", (190, -10, 0)),
              ("head_nozzle", (255, -10, 0)), ("camera_post", (330, 0, 0)), ("camera_cradle", (400, 0, 0)),
              ("electronics_box", (60, -150, 0)), ("electronics_lid", (170, -150, 0))]
    colors = {"base": (0.3, 0.33, 0.38), "yoke": (0.93, 0.93, 0.95), "head_sonar": (0.95, 0.55, 0.15),
              "head_nozzle": (0.95, 0.55, 0.15), "camera_post": (0.3, 0.33, 0.38), "camera_cradle": (0.95, 0.55, 0.15),
              "electronics_box": (0.25, 0.28, 0.33), "electronics_lid": (0.25, 0.28, 0.33)}
    items = []
    for n, off in layout:
        w = parts[n]
        bb = w.val().BoundingBox()
        items.append((w.translate((-bb.xmin, -bb.ymin, -bb.zmin)).translate(off), colors[n]))
    pth = os.path.join(out, "4_parts.png")
    render.render_png(items, pth, yaw=-25, pitch=-60, size=size, margin=(70, 40, 60, 40))
    render.add_text(pth, size, "Деталі для друку",
                    notes=("Головку друкувати передньою пластиною донизу, основу й вилку — плиском на дні.",))
    # коробка основи: вікна під роз'єми модулів (стінка з вікнами — до глядача)
    turn = lambda w: w.rotate((0, 0, 0), (0, 0, 1), -90)
    items = [(turn(parts["electronics_box"]), (0.25, 0.28, 0.33)),
             (turn(cadlib.pcb_dummy()), (0.1, 0.45, 0.25)),
             (turn(cadlib.plug_dummies(BASE_PORTS)), (0.92, 0.9, 0.82))]
    pth = os.path.join(out, "5_base_box.png")
    pj = render.render_png(items, pth, yaw=-18, pitch=-72, size=size, margin=(190, 120, 170, 120))
    z0, z1 = cadlib.port_z()
    lab = []
    k = len(BASE_PORTS)
    for i, (name, kind, y0, y1) in enumerate(cadlib.port_layout(BASE_PORTS)):
        px, py = pj((y0 + y1) / 2, -cadlib.BOX_W - 6, (z0 + z1) / 2)
        lab.append((px, py, 260 + i * (size[0] - 520) // (k - 1), size[1] - 110 + (i % 2) * 28, name))
    for (y, t) in ((20, "гніздо живлення 12 В"), (50, "тумблер ARM"), (80, "статус-LED")):
        px, py = pj(y, -cadlib.BOX_W, cadlib.PANEL_Z)
        lab.append((px, py, px - 40 + (y - 50) * 3, 120, t))
    render.add_text(pth, size, "Коробка основи турелі: вікна під роз'єми модулів", labels=lab,
                    notes=("Кутові вилки JST-XH стоять на краю плати 5 × 7 см; штекери вставляються ззовні. Контакт 1 — GND.",))
    print("Прев'ю: cad/flysonar/preview/")


if __name__ == "__main__":
    parts = {n: f() for n, f in PRINTED.items()}
    ok = checks(parts)
    if "--no-export" not in sys.argv:
        export(parts)
    if "--preview" in sys.argv:
        previews(parts)
    print("\nПЕРЕВІРКИ:", "усе гаразд" if ok else "Є ПРОБЛЕМИ")
    sys.exit(0 if ok else 1)
