#!/usr/bin/env python3
"""
FlyClap — корпус і механіка для 3D-друку (параметрична модель CadQuery).

Запуск з кореня репозиторію:
    pip install cadquery
    python3 cad/flyclap/flyclap_case.py

Результат:
    cad/flyclap/step/*.step        — кожна деталь окремо (відкривається у FreeCAD: File → Open)
    cad/flyclap/stl/*.stl          — для слайсера
    cad/flyclap/flyclap_assembly.step — складання (пластини відкриті, "взведено")
Скрипт також перевіряє, що деталі не перетинаються (відкрито й закрито)
і що кожна влазить на стіл 220×220×250 мм.

Система координат складання: X — ліво/право (0 — центральний шов),
Y — спереду назад (0 — передня площина), Z — вгору (0 — низ корпусу).
"""
import math
import os
import sys

import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import cadlib  # noqa: E402  (спільна коробка основи з вікнами під роз'єми JST-XH)

# ============================ ПАРАМЕТРИ ============================
# Корпус
W, D, H = 200.0, 170.0, 200.0   # зовнішні розміри
T = 3.0                          # стінки
TF = 3.0                         # дно
BED = (220.0, 220.0, 250.0)      # стіл принтера для перевірки

# Пластини (оргскло, НЕ друкуються)
PLATE = 150.0                    # 150×150 мм
PLATE_T = 2.0
PLATE_Y0 = 10.0                  # пластина займає Y 10..160
OPEN_DEG = 35.0                  # кут розкриття кожної пластини від вертикалі

# Петлі: дві осі (пруток / шпилька Ø3) на відстані 2·AX_X, синхронізовані шестернями
AX_X = 6.25
AX_Z = 35.0
ROD_D = 3.0
BORE = 3.3                       # отвір у петлі (обертається на прутку)
ROD_HOLE = 3.2                   # отвір у стінці (пруток сидить щільно)
TUBE_R = 4.0
GEAR_M, GEAR_Z = 1.25, 10        # модуль і зуби: ділильний радіус = AX_X
GEAR_Y = (3.5, 9.5)
TAB_T = 2.5                      # вусо, до якого клеїться / кріпиться пластина
TAB_TOP = 72.0

# ІЧ-завіса
IR_Z = AX_Z + 90.0               # висота променів
IR_X = (13.0, 40.0)              # ± від центру
LED_BORE, LED_BOSS_R, LED_BOSS_L = 5.2, 4.6, 12.0     # TSAL6200, 5 мм
PT_BORE, PT_BOSS_R, PT_BOSS_L = 3.2, 3.6, 15.0        # TEFT4300, 3 мм

# Серво MG996R (звірте свій екземпляр!)
SERVO_L, SERVO_W, SERVO_H = 40.7, 19.7, 42.9
SERVO_SHAFT_FROM_END = 10.0
SERVO_HOLES_DX, SERVO_HOLES_DZ = 49.5, 10.0
SERVO_SHAFT = (22.0, 18.0)       # (x, z) осі серви на передній стінці
SERVO_TOP_INSIDE = 9.0           # наскільки верх серви заходить усередину

# Засувка й соленоїд JF-0530B (звірте свій екземпляр!)
LATCH_Y, LATCH_Z = 85.0, 165.0
BOLT = 6.0                       # переріз засувки 6×6
BOLT_TIP_X = -85.0               # кінчик засувки у висунутому стані
SOL_L, SOL_W, SOL_H = 30.0, 16.0, 14.0

# Стяжні болти половинок
SEAM_W = 9.0                     # ширина рамки шва (пластина починається з Y=10)
SEAM_GAP = (12.0, 58.0)          # проріз у рамці для шестерень / хвостовиків

CLR = 0.3                        # типовий зазор FDM

# Роз'єми основи FlyClap (docs/modules.md): вікна в зовнішній стінці блока електроніки,
# від переду корпусу до заду — так само, як розташовані модулі.
BASE_PORTS = [
    ("E: випромінювачі завіси (XH-3)", "XH3"),
    ("servo: взведення (3-pin)", "servo"),
    ("P1: соленоїд (XH-2)", "XH2"),
    ("S1: кінцевик (XH-4)", "XH4"),
    ("B: приймачі завіси (XH-6)", "XH6"),
]


# ============================ ПРИМІТИВИ ============================
def B(x0, x1, y0, y1, z0, z1):
    x0, x1 = min(x0, x1), max(x0, x1)
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def cylY(x, z, r, y0, y1):
    return cq.Workplane("XZ").center(x, z).circle(r).extrude(-(y1 - y0)).translate((0, y0, 0))


def cylX(y, z, r, x0, x1):
    x0, x1 = min(x0, x1), max(x0, x1)
    return cq.Workplane("YZ").center(y, z).circle(r).extrude(x1 - x0).translate((x0, 0, 0))


def cylZ(x, y, r, z0, z1):
    return cq.Workplane("XY").center(x, y).circle(r).extrude(z1 - z0).translate((0, 0, z0))


def gear_profile(cx, cz, m, z, phase_deg, backlash=0.25, addendum=1.0, n=10):
    """Евольвентний профіль зубчастого колеса в площині XZ (точки (x, z)).
    Бічний зазор 0.25 мм — під FDM (±0.1–0.2 мм)."""
    alpha = math.radians(20)
    rp = m * z / 2
    rb = rp * math.cos(alpha)
    ra = rp + addendum * m
    rf = rp - 1.25 * m
    inv = lambda a: math.tan(a) - a
    s_p = math.pi * m / 2 - backlash
    psi_p = s_p / 2 / rp

    def half(r):
        if r <= rb:
            return psi_p + inv(alpha)
        return psi_p + inv(alpha) - inv(math.acos(rb / r))

    rs = [rb + (ra - rb) * i / n for i in range(n + 1)]
    pts = []
    for k in range(z):
        c = math.radians(phase_deg) + 2 * math.pi * k / z
        # западина → лівий бік зуба (від кореня до вершини) → вершина → правий бік
        flank_up = [(c - half(r), r) for r in rs]
        pts.append((c - half(rb), rf))
        pts += flank_up
        pts += [(c + half(r), r) for r in reversed(rs)]
        pts.append((c + half(rb), rf))
    return [(cx + r * math.cos(a), cz + r * math.sin(a)) for a, r in pts]


def gear(cx, cz, y0, y1, phase_deg):
    pts = gear_profile(cx, cz, GEAR_M, GEAR_Z, phase_deg)
    return cq.Workplane("XZ").polyline(pts).close().extrude(-(y1 - y0)).translate((0, y0, 0))


def mirrorX(wp):
    return wp.mirror("YZ")


# ============================ КОРПУС ============================
def shell(side):
    """Половина корпусу: side = +1 права, -1 ліва."""
    s = side
    X = lambda a: s * a
    body = B(0, X(W / 2), 0, D, 0, H)
    body = body.cut(B(X(-1), X(W / 2 - T), T, D - T, TF, H + 1))          # порожнина, шов відкритий

    # рамка шва (болти М3 стягують половинки)
    frame = (B(0, X(T), 0, SEAM_W, 0, SEAM_GAP[0])
             .union(B(0, X(T), 0, SEAM_W, SEAM_GAP[1], H))
             .union(B(0, X(T), D - SEAM_W, D, 0, SEAM_GAP[0]))
             .union(B(0, X(T), D - SEAM_W, D, SEAM_GAP[1], H))
             .union(B(0, X(T), 0, 30, 0, 11))
             .union(B(0, X(T), D - 30, D, 0, 11)))
    body = body.union(frame)
    for y, z in ((6, 6), (6, 80), (6, 140), (6, 190), (D - 6, 6), (D - 6, 80), (D - 6, 140), (D - 6, 190),
                 (22, 7), (D - 22, 7)):
        body = body.cut(cylX(y, z, 1.65, -5, 5))

    # стійки під гвинти кришки (в зовнішніх кутах)
    for y0 in (T, D - T - 8):
        post = B(X(W / 2 - T - 8), X(W / 2 - T), y0, y0 + 8, TF, H)
        body = body.union(post)
        body = body.cut(cylZ(X(W / 2 - T - 4), y0 + 4, 1.3, H - 16, H + 1))

    # отвори під осі петель (пруток Ø3 крізь передню і задню стінки)
    body = body.cut(cylY(X(AX_X), AX_Z, ROD_HOLE / 2, -1, D + 1))

    # ІЧ-тунелі: світлодіоди спереду, фототранзистори ззаду (деталь — на зовнішньому кінці)
    for x in IR_X:
        body = body.union(cylY(X(x), IR_Z, LED_BOSS_R, -LED_BOSS_L, T))
        body = body.cut(cylY(X(x), IR_Z, LED_BORE / 2, -LED_BOSS_L - 1, T + 1))
        body = body.union(cylY(X(x), IR_Z, PT_BOSS_R, D - T, D + PT_BOSS_L))
        body = body.cut(cylY(X(x), IR_Z, PT_BORE / 2, D - T - 1, D + PT_BOSS_L + 1))

    # стійки для гумки (закриває пластини)
    post_y = 163.0 if s > 0 else 157.0
    body = body.union(cylZ(X(45), post_y, 3, TF, 15)).union(cylZ(X(45), post_y, 4.5, 13, 15))

    if s > 0:
        # вікно під MG996R у передній стінці (корпус серви зовні, вал усередину)
        sx, sz = SERVO_SHAFT
        x0 = sx - SERVO_SHAFT_FROM_END
        body = body.cut(B(x0 - CLR, x0 + SERVO_L + CLR, -1, T + 1, sz - SERVO_W / 2 - CLR, sz + SERVO_W / 2 + CLR))
        cx = x0 + SERVO_L / 2
        for dx in (-SERVO_HOLES_DX / 2, SERVO_HOLES_DX / 2):
            for dz in (-SERVO_HOLES_DZ / 2, SERVO_HOLES_DZ / 2):
                body = body.cut(cylY(cx + dx, sz + dz, 1.5, -1, T + 1))
        # отвори під кріплення блока електроніки
        for y in (60, 110):
            body = body.cut(cylX(y, 100, 1.65, W / 2 - T - 1, W / 2 + 1))
    else:
        # вікно засувки + пази кріплення соленоїда (регулювання ±3 мм по висоті)
        body = body.cut(B(-W / 2 - 1, -W / 2 + T + 1, LATCH_Y - 7, LATCH_Y + 7, LATCH_Z - 7, LATCH_Z + 7))
        for dy in (-17, 17):
            for dz in (-15, 15):
                y, z = LATCH_Y + dy, LATCH_Z + dz
                body = body.cut(B(-W / 2 - 1, -W / 2 + T + 1, y - 1.7, y + 1.7, z - 3, z + 3))
                body = body.cut(cylX(y, z - 3, 1.7, -W / 2 - 1, -W / 2 + T + 1))
                body = body.cut(cylX(y, z + 3, 1.7, -W / 2 - 1, -W / 2 + T + 1))
    return body


def lid():
    """Кришка-сітка: вічко 8 мм, перемички 2 мм."""
    t = 2.4
    plate = B(-W / 2, W / 2, 0, D, H, H + t)
    nx, ny, pitch = 17, 14, 10.0
    holes = (cq.Workplane("XY").workplane(offset=H - 1)
             .center(0, D / 2).rarray(pitch, pitch, nx, ny).rect(8, 8).extrude(t + 2))
    plate = plate.cut(holes)
    for x in (-(W / 2 - T - 4), W / 2 - T - 4):
        for y in (T + 4, D - T - 4):
            plate = plate.cut(cylZ(x, y, 1.7, H - 1, H + t + 1))
    return plate


# ============================ ПЕТЛІ ============================
def hinge(side):
    """Петля пластини: трубка на прутку + шестерня синхронізації + вусо під пластину
    + хвостовик(и) під гумку / серво. Модель у ЗАКРИТОМУ стані (пластини вертикально)."""
    s = side
    # будуємо праву і дзеркалимо для лівої; фаза шестерні лівої — на півзуба
    ax, az = AX_X, AX_Z
    tube = cylY(ax, az, TUBE_R, GEAR_Y[0], D - T - 0.5)
    g = gear(ax, az, GEAR_Y[0], GEAR_Y[1], 0.0 if s > 0 else 18.0)
    tab = B(PLATE_T, PLATE_T + TAB_T, PLATE_Y0, PLATE_Y0 + PLATE, az - 2, TAB_TOP)
    part = tube.union(g).union(tab)
    # хвостовик під гумку (ззаду); лівий і правий — у різних площинах Y, щоб не зачепились
    ty = (160.5, 166.0) if s > 0 else (154.0, 159.5)
    tail = B(ax - 3, ax + 3, ty[0], ty[1], 17, az).union(cylY(ax, 17, 3, ty[0], ty[1]))
    part = part.union(tail).cut(cylZ(ax, (ty[0] + ty[1]) / 2, 1.2, 12, 22))  # отвір для гумки
    if s > 0:
        # хвостовик, який штовхає важіль серви при взводі (спереду)
        st = B(ax - 3, ax + 3, 11, 17, 15, az).union(cylY(ax, 15, 3, 11, 17))
        part = part.union(st)
    part = part.cut(cylY(ax, az, BORE / 2, -1, D + 1))
    for y in (25, 85, 145):                                               # М2 для пластини
        part = part.cut(cylX(y, 60, 1.1, -1, 10))
    if s < 0:
        part = mirrorX(part)
    return part


def plate(side):
    p = B(0, PLATE_T, PLATE_Y0, PLATE_Y0 + PLATE, AX_Z, AX_Z + PLATE)
    return p if side > 0 else mirrorX(p)


def rod(side):
    return cylY(side * AX_X, AX_Z, ROD_D / 2, -2, D + 2)


def rotate_open(wp, side, deg):
    """Повернути праву (+deg) / ліву (−deg) рухому частину навколо її осі."""
    a = (side * AX_X, 0, AX_Z)
    return wp.rotate(a, (a[0], 1, a[2]), side * deg)


# ============================ ЗАСУВКА ============================
def latch_mount():
    """Кріплення соленоїда зовні лівої стінки: пластина з 4 отворами, напрямна
    засувки 6×6 і люлька під соленоїд (кріпиться стяжками)."""
    x_wall = -W / 2
    y, z = LATCH_Y, LATCH_Z
    base = B(x_wall - 3, x_wall, y - 22, y + 22, z - 20, z + 20)
    for dy in (-17, 17):
        for dz in (-15, 15):
            base = base.cut(cylX(y + dy, z + dz, 1.7, x_wall - 5, x_wall + 1))
    guide = B(x_wall - 18, x_wall - 3, y - 7, y + 7, z - 7, z + 7)
    part = base.union(guide)
    # люлька: дно + дві стінки
    xs0, xs1 = x_wall - 20 - SOL_L - 2, x_wall - 18
    zb = z - SOL_H / 2
    part = part.union(B(xs0, xs1, y - SOL_W / 2 - 2, y + SOL_W / 2 + 2, zb - 2.5, zb))
    for sg in (-1, 1):
        yy = y + sg * (SOL_W / 2 + 1)
        part = part.union(B(xs0, xs1, yy - 1, yy + 1, zb - 2.5, z + SOL_H / 2 - 3))
    # перемичка люлька ↔ напрямна (з перекриттям, щоб деталь була одним тілом)
    part = part.union(B(x_wall - 22, x_wall - 10, y - 7, y + 7, zb - 2.5, zb + 1))
    # канал засувки і прорізи під стяжки
    part = part.cut(B(x_wall - 30, x_wall + 1, y - BOLT / 2 - CLR, y + BOLT / 2 + CLR, z - BOLT / 2 - CLR, z + BOLT / 2 + CLR))
    for xx in (xs0 + 8, xs1 - 8):
        part = part.cut(B(xx - 2, xx + 2, y - SOL_W / 2 - 3, y + SOL_W / 2 + 3, zb - 3, zb + 0.01))
    return part


def latch_bolt(extended=True):
    """Засувка 6×6 зі скосом зверху: пластина при взводі відсуває її, а закритися не може."""
    tip = BOLT_TIP_X if extended else BOLT_TIP_X - 5
    L = 38.0
    y, z = LATCH_Y, LATCH_Z
    bolt = B(tip - L, tip, y - BOLT / 2, y + BOLT / 2, z - BOLT / 2, z + BOLT / 2)
    wedge = (cq.Workplane("XZ").polyline([(tip - 5, z + BOLT / 2), (tip, z + BOLT / 2), (tip, z - BOLT / 2 + 1.5)])
             .close().extrude(-(BOLT + 2)).translate((0, y - BOLT / 2 - 1, 0)))
    bolt = bolt.cut(wedge)
    bolt = bolt.cut(cylZ(tip - L + 4, y, 1.1, z - 5, z + 5))            # отвір для зв'язку з якорем
    return bolt


# ============================ ЕЛЕКТРОНІКА, ПРИМАНКА ============================
def elec_box():
    """Коробка основи під плату 5×7 см (Nano, MOSFET, компаратор) з вікнами під
    роз'єми модулів. Вухо з двома отворами кріпиться до правої стінки корпусу.
    Локальні координати: x — від стінки назовні."""
    return cadlib.base_box(BASE_PORTS, ear=True)


def elec_lid():
    return cadlib.base_lid()


def bait_cup():
    return cylZ(0, 0, 25, 0, 20).cut(cylZ(0, 0, 23.4, 1.6, 21))


# ============================ МУЛЯЖІ (не друкуються) ============================
def servo_dummy():
    sx, sz = SERVO_SHAFT
    x0 = sx - SERVO_SHAFT_FROM_END
    top_y = T + SERVO_TOP_INSIDE
    body = B(x0, x0 + SERVO_L, top_y - SERVO_H, top_y, sz - SERVO_W / 2, sz + SERVO_W / 2)
    fl = B(x0 - 7, x0 + SERVO_L + 7, -2.5, 0, sz - SERVO_W / 2, sz + SERVO_W / 2)
    return body.union(fl).union(cylY(sx, sz, 3, top_y, top_y + 4))


def solenoid_dummy():
    xs1 = -W / 2 - 20
    return B(xs1 - SOL_L, xs1, LATCH_Y - SOL_W / 2, LATCH_Y + SOL_W / 2,
             LATCH_Z - SOL_H / 2, LATCH_Z + SOL_H / 2)


# ============================ ЗБІРКА, ПЕРЕВІРКИ, ЕКСПОРТ ============================
PRINTED = {
    "shell_right": lambda: shell(+1),
    "shell_left": lambda: shell(-1),
    "lid_mesh": lid,
    "hinge_right": lambda: hinge(+1),
    "hinge_left": lambda: hinge(-1),
    "latch_mount": latch_mount,
    "latch_bolt": latch_bolt,
    "electronics_box": elec_box,
    "electronics_lid": elec_lid,
    "bait_cup": bait_cup,
}


def assembly_parts(parts, opened=True, deg=None):
    if deg is None:
        deg = OPEN_DEG if opened else 0.0
    ebox_place = lambda wp: wp.translate((W / 2, 35, 48))
    items = [
        ("shell_right", parts["shell_right"], (0.93, 0.93, 0.95)),
        ("shell_left", parts["shell_left"], (0.93, 0.93, 0.95)),
        ("lid_mesh", parts["lid_mesh"], (0.55, 0.58, 0.62)),
        ("hinge_right", rotate_open(parts["hinge_right"], +1, deg), (0.95, 0.55, 0.15)),
        ("hinge_left", rotate_open(parts["hinge_left"], -1, deg), (0.95, 0.55, 0.15)),
        ("plate_right", rotate_open(plate(+1), +1, deg), (0.6, 0.8, 1.0)),
        ("plate_left", rotate_open(plate(-1), -1, deg), (0.6, 0.8, 1.0)),
        ("rod_right", rod(+1), (0.4, 0.4, 0.4)),
        ("rod_left", rod(-1), (0.4, 0.4, 0.4)),
        ("latch_mount", parts["latch_mount"], (0.2, 0.45, 0.8)),
        ("latch_bolt", latch_bolt(extended=True), (0.85, 0.2, 0.2)),
        ("electronics_box", ebox_place(parts["electronics_box"]), (0.25, 0.28, 0.33)),
        ("electronics_lid", ebox_place(parts["electronics_lid"].translate((0, 0, 40))), (0.25, 0.28, 0.33)),
        ("base_pcb", ebox_place(cadlib.pcb_dummy()), (0.1, 0.45, 0.25)),
        ("base_plugs", ebox_place(cadlib.plug_dummies(BASE_PORTS)), (0.92, 0.9, 0.82)),
        ("bait_cup", parts["bait_cup"].translate((0, 85, TF)), (0.95, 0.75, 0.35)),
        ("servo_MG996R", servo_dummy(), (0.18, 0.2, 0.25)),
        ("solenoid_JF0530B", solenoid_dummy(), (0.45, 0.45, 0.45)),
    ]
    return items


def overlap(a, b):
    try:
        v = a.val().intersect(b.val()).Volume()
    except Exception:
        v = float("nan")
    return v


def checks(parts):
    ok = True
    print("\n== Розміри деталей (стіл %gx%gx%g) ==" % BED)
    for name, wp in parts.items():
        bb = wp.val().BoundingBox()
        dims = sorted([bb.xlen, bb.ylen])
        fits = dims[0] <= BED[0] and dims[1] <= BED[1] and bb.zlen <= BED[2]
        # деталі, що друкуються лежачи, влазять і так
        if not fits:
            d3 = sorted([bb.xlen, bb.ylen, bb.zlen])
            fits = d3[0] <= BED[2] and d3[1] <= BED[0] and d3[2] <= BED[1]
        one = len(wp.val().Solids()) == 1 and wp.val().isValid()   # друкується одним шматком
        ok &= fits and one
        print(f"  {name:18s} {bb.xlen:6.1f} × {bb.ylen:6.1f} × {bb.zlen:6.1f} мм   {'OK' if fits else 'НЕ ВЛАЗИТЬ'}"
              f"{'' if one else '   НЕ ОДНЕ ТІЛО!'}")

    pairs = [("hinge_right", "shell_right"), ("hinge_left", "shell_left"), ("hinge_right", "hinge_left"),
             ("hinge_right", "shell_left"), ("hinge_left", "shell_right"),
             ("plate_right", "shell_right"), ("plate_left", "shell_left"), ("plate_right", "plate_left"),
             ("plate_right", "hinge_left"), ("plate_left", "hinge_right"),
             ("bait_cup", "hinge_right"), ("bait_cup", "hinge_left"), ("bait_cup", "shell_right"),
             ("latch_bolt", "plate_left"), ("latch_bolt", "hinge_left"),
             ("servo_MG996R", "shell_right"), ("servo_MG996R", "hinge_right"),
             ("lid_mesh", "shell_right"), ("lid_mesh", "shell_left"),
             ("electronics_box", "shell_right"), ("latch_mount", "shell_left"),
             ("base_pcb", "electronics_box"), ("base_plugs", "electronics_box"), ("base_pcb", "electronics_lid"),
             ("rod_right", "hinge_right"), ("rod_left", "hinge_left")]
    for opened in (True, False):
        items = {n: w for n, w, _ in assembly_parts(parts, opened)}
        print(f"\n== Перетини деталей: пластини {'ВІДКРИТІ (взведено)' if opened else 'ЗАКРИТІ'} ==")
        for a, b in pairs:
            if not opened and a == "latch_bolt":
                continue                                   # у закритому стані засувка не заважає пластині — вона вище
            v = overlap(items[a], items[b])
            good = v < 0.5                                 # мм³: дотик/похибка тесселяції
            ok &= good
            print(f"  {a:16s} ∩ {b:16s} = {v:8.2f} мм³  {'OK' if good else 'ПЕРЕТИН!'}")
    # рух: шестерні, хвостовики й пластини на проміжних кутах
    print("\n== Хід пластин 0…%g° (крок 5°): рухомі деталі ∩ корпус / одна одну ==" % OPEN_DEG)
    moving = [("hinge_right", "hinge_left"), ("hinge_right", "shell_right"), ("hinge_left", "shell_left"),
              ("hinge_right", "shell_left"), ("hinge_left", "shell_right"),
              ("plate_right", "shell_right"), ("plate_left", "shell_left"), ("plate_right", "plate_left"),
              ("hinge_right", "bait_cup"), ("hinge_left", "bait_cup")]
    worst = 0.0
    for deg in range(0, int(OPEN_DEG) + 1, 5):
        items = {n: w for n, w, _ in assembly_parts(parts, deg=float(deg))}
        for a, b in moving:
            v = overlap(items[a], items[b])
            worst = max(worst, v)
            if v >= 0.5:
                ok = False
                print(f"  {deg:2d}°: {a} ∩ {b} = {v:.2f} мм³  ПЕРЕТИН!")
    print(f"  найбільший перетин на всьому ході: {worst:.2f} мм³  {'OK' if worst < 0.5 else 'ПЕРЕТИН!'}")
    # засувка справді тримає пластину: у відкритому стані вона нависає над верхнім торцем
    items = {n: w for n, w, _ in assembly_parts(parts, True)}
    pl = items["plate_left"].val().BoundingBox()
    bolt = items["latch_bolt"].val().BoundingBox()
    holds = bolt.xmax > pl.xmin + 2 and bolt.zmin >= pl.zmax - 0.01
    print(f"\n== Засувка над торцем лівої пластини: кінчик x={bolt.xmax:.1f}, край пластини x={pl.xmin:.1f}, "
          f"низ засувки z={bolt.zmin:.1f}, верх пластини z={pl.zmax:.1f}  {'OK' if holds else 'НЕ ТРИМАЄ'}")
    ok &= holds
    return ok


def export(parts):
    os.makedirs(os.path.join(HERE, "step"), exist_ok=True)
    os.makedirs(os.path.join(HERE, "stl"), exist_ok=True)
    for name, wp in parts.items():
        cq.exporters.export(wp, os.path.join(HERE, "step", name + ".step"))
        cq.exporters.export(wp, os.path.join(HERE, "stl", name + ".stl"), tolerance=0.05, angularTolerance=0.15)
    asm = cq.Assembly(name="FlyClap")
    for name, wp, rgb in assembly_parts(parts, True):
        asm.add(wp, name=name, color=cq.Color(*rgb))
    asm.save(os.path.join(HERE, "flyclap_assembly.step"))
    print("\nЕкспорт: step/, stl/, flyclap_assembly.step")


def previews(parts):
    """Картинки-прев'ю у cad/flyclap/preview/ (потрібні numpy і, для підписів, Chromium)."""
    sys.path.insert(0, os.path.dirname(HERE))
    import render
    out = os.path.join(HERE, "preview")
    os.makedirs(out, exist_ok=True)
    size = (1200, 900)

    # 1. загальний вигляд, закрито кришкою
    items = [(w, c) for n, w, c in assembly_parts(parts, opened=True)]
    pth = os.path.join(out, "1_assembly.png")
    render.render_png(items, pth, yaw=-32, pitch=-62, size=size, margin=(70, 40, 60, 40))
    render.add_text(pth, size, "FlyClap — корпус у зборі (200 × 170 × 200 мм)",
                    notes=("Дві половини стягуються болтами М3 по шву; кришка — сітка з вічком 8 мм.",))

    # 2. вигляд усередину: без кришки й передньої частини (розріз по Y = 12 мм)
    keep = B(-500, 500, 12, 500, -500, 500)
    items = [(w.intersect(keep), c) for n, w, c in assembly_parts(parts, opened=True)
             if n not in ("lid_mesh", "servo_MG996R")]
    pth = os.path.join(out, "2_inside.png")
    render.render_png(items, pth, yaw=-18, pitch=-68, size=size, margin=(70, 40, 60, 40))
    render.add_text(pth, size, "Усередині: пластини відкриті (взведено), передню стінку зрізано",
                    notes=("Пластини з оргскла 150 × 150 × 2 мм кріпляться до петель гвинтами М2 або клеєм.",))

    # 3. розріз спереду по Y = 6 мм: шестерні, петлі, хвостовик серви, засувка
    keep = B(-500, 500, 6, 500, -500, 500)
    items = [(w.intersect(keep), c) for n, w, c in assembly_parts(parts, opened=True)
             if n not in ("lid_mesh", "electronics_lid", "base_pcb", "base_plugs")]
    pth = os.path.join(out, "3_section_front.png")
    pj = render.render_png(items, pth, yaw=0, pitch=-90, size=size, margin=(70, 250, 60, 250))
    lab = []
    for (x, y, z), (dx, dy), t in (
            ((0, 6, AX_Z), (-230, 30), "шестерні синхронізації (m 1,25, z 10)"),
            ((AX_X + 3, 6, 16), (190, 95), "хвостовик: серво взводить"),
            ((-37, 40, 90), (-150, -20), "пластина (оргскло)"),
            ((BOLT_TIP_X - 2, 85, LATCH_Z + 1), (-80, -110), "засувка над торцем пластини"),
            ((-W / 2 - 35, 85, LATCH_Z), (-30, -60), "соленоїд JF-0530B"),
            ((0, 85, TF + 15), (190, 60), "приманка"),
            ((AX_X, 6, AX_Z), (215, 40), "вісь: пруток Ø3 мм"),
            ((W / 2 + 35, 60, 70), (40, -80), "блок електроніки")):
        px, py = pj(x, y, z)
        lab.append((px, py, px + dx, py + dy, t))
    render.add_text(pth, size, "Розріз спереду: механізм у взведеному стані", labels=lab,
                    notes=("Шестерні змушують обидві пластини рухатись синхронно: одна засувка й одне серво на обидві.",))

    # 4. деталі для друку
    order = ["shell_left", "shell_right", "lid_mesh", "hinge_left", "hinge_right", "latch_mount",
             "latch_bolt", "electronics_box", "electronics_lid", "bait_cup"]
    pos = {"shell_left": (-230, 0, 0), "shell_right": (-10, 0, 0), "lid_mesh": (330, 0, 0),
           "hinge_left": (140, -120, 0), "hinge_right": (190, -120, 0), "latch_mount": (300, -120, 0),
           "latch_bolt": (380, -100, 0), "electronics_box": (430, -140, 0), "electronics_lid": (530, -140, 0),
           "bait_cup": (620, -80, 0)}
    colors = {"shell_left": (0.93, 0.93, 0.95), "shell_right": (0.93, 0.93, 0.95), "lid_mesh": (0.55, 0.58, 0.62),
              "hinge_left": (0.95, 0.55, 0.15), "hinge_right": (0.95, 0.55, 0.15), "latch_mount": (0.2, 0.45, 0.8),
              "latch_bolt": (0.85, 0.2, 0.2), "electronics_box": (0.25, 0.28, 0.33),
              "electronics_lid": (0.25, 0.28, 0.33), "bait_cup": (0.95, 0.75, 0.35)}
    items = []
    for n in order:
        w = parts[n]
        bb = w.val().BoundingBox()
        w = w.translate((-bb.xmin, -bb.ymin, -bb.zmin)).translate(pos[n])
        items.append((w, colors[n]))
    pth = os.path.join(out, "4_parts.png")
    render.render_png(items, pth, yaw=-25, pitch=-58, size=size, margin=(70, 40, 60, 40))
    render.add_text(pth, size, "Деталі для друку (10 шт., кожна влазить на стіл 220 × 220 мм)",
                    notes=("Половини корпусу й коробку друкувати стоячи на дні; петлі — лежачи на вусі; кришку — плиском.",))
    # 5. блок електроніки: вікна під роз'єми модулів (стінка з вікнами — до глядача)
    turn = lambda w: w.rotate((0, 0, 0), (0, 0, 1), -90)
    items = [(turn(parts["electronics_box"]), (0.25, 0.28, 0.33)),
             (turn(cadlib.pcb_dummy()), (0.1, 0.45, 0.25)),
             (turn(cadlib.plug_dummies(BASE_PORTS)), (0.92, 0.9, 0.82))]
    pth = os.path.join(out, "5_connectors.png")
    pj = render.render_png(items, pth, yaw=-18, pitch=-72, size=size, margin=(190, 120, 170, 120))
    z0, z1 = cadlib.port_z()
    lab = []
    n = len(BASE_PORTS)
    for i, (name, kind, y0, y1) in enumerate(cadlib.port_layout(BASE_PORTS)):
        px, py = pj((y0 + y1) / 2, -cadlib.BOX_W - 6, (z0 + z1) / 2)
        lab.append((px, py, 260 + i * (size[0] - 520) // (n - 1), size[1] - 110 + (i % 2) * 28, name))
    for (y, t) in ((20, "гніздо живлення 12 В"), (50, "тумблер ARM"), (80, "статус-LED")):
        px, py = pj(y, -cadlib.BOX_W, cadlib.PANEL_Z)
        lab.append((px, py, px - 40 + (y - 50) * 3, 120, t))
    render.add_text(pth, size, "Блок електроніки (основа): вікна під роз'єми модулів", labels=lab,
                    notes=("Кутові вилки JST-XH стоять на краю плати 5 × 7 см; штекери вставляються ззовні. Контакт 1 — GND.",))
    print("Прев'ю: cad/flyclap/preview/")


if __name__ == "__main__":
    parts = {n: f() for n, f in PRINTED.items()}
    ok = checks(parts)
    if "--no-export" not in sys.argv:
        export(parts)
    if "--preview" in sys.argv:
        previews(parts)
    print("\nПЕРЕВІРКИ:", "усе гаразд" if ok else "Є ПРОБЛЕМИ")
    sys.exit(0 if ok else 1)
