#!/usr/bin/env python3
"""
FlySonar (ESP32-версія) — підставка для LED-панелі A4 з захисним оргсклом і жолобом.

Панель стоїть нижнім краєм у пазах двох ніжок з нахилом назад (стійкість),
ззаду її підпирають ребра. Між ніжками — два відрізки жолоба: збирають краплі
після пострілів і тримають приманку.

Запуск з кореня репозиторію:
    pip install cadquery
    python3 cad/flysonar/led_panel_stand.py            # перевірки + STEP/STL
    python3 cad/flysonar/led_panel_stand.py --preview  # + картинки

Координати: X — уздовж панелі (0 — центр), Y — від камери назад (панель
дивиться в −Y, камера за ~300 мм у −Y), Z — вгору (0 — стіл).
"""
import math
import os
import sys

import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from cadlib import B, cylZ, overlap, fits_bed, single_solid  # noqa: E402

BED = (220.0, 220.0, 250.0)
CLR = 0.3

# ============================ ПАРАМЕТРИ ============================
# LED-планшет A4 (звірте свій!): типово 335 × 235 × 5 мм, робоча зона A4
PANEL_W, PANEL_H, PANEL_T = 335.0, 235.0, 5.0
SHIELD_T = 2.0                   # захисне оргскло / плівка на рамці спереду (0 — без нього)
SHIELD_H = PANEL_H - 10.0
TILT = 8.0                       # нахил панелі назад, град (0 — вертикально)

WALL = 3.0                       # стінки паза
SLOT_D = 18.0                    # глибина паза
Z0 = 5.0                         # висота низу панелі над столом (передній край)
FOOT_LEN = 50.0                  # скільки панелі заходить у кожну ніжку
END_T = 3.0                      # бічний упор
FOOT_Y = (-32.0, 82.0)           # глибина основи ніжки (спереду / ззаду)
BASE_T = 3.0
GUSSET_H = 110.0                 # висота ребра-підпори ззаду
GUSSET_T = 5.0

GUTTER_L = 115.0                 # довжина відрізка жолоба (друкується 2 шт.)
TROUGH_W, TROUGH_D = 24.0, 12.0  # ширина / глибина жолоба

STACK = SHIELD_T + PANEL_T       # товщина пакета в пазу


# ============================ ГЕОМЕТРІЯ НАХИЛУ ============================
def tilt_wp(wp):
    """Повернути з "локальних" координат панелі (вертикально) у нахилені (назад, до +Y)."""
    return wp.rotate((0, 0, Z0), (1, 0, Z0), -TILT)


def L(x0, x1, ly0, ly1, lz0, lz1):
    """Брусок у локальних координатах панелі (ly — від передньої грані, lz — від Z0), нахилений."""
    return tilt_wp(B(x0, x1, ly0, ly1, Z0 + lz0, Z0 + lz1))


def tilt_pt(ly, lz):
    """Точка (y, z) з локальних координат панелі; lz відраховується від Z0."""
    t = math.radians(TILT)
    return ly * math.cos(t) + lz * math.sin(t), -ly * math.sin(t) + lz * math.cos(t) + Z0


def slot_block(x0, x1, height=SLOT_D):
    """Паз з нахилом: стінки + нахилене дно (перпендикулярне до панелі)."""
    outer = L(x0, x1, -CLR - WALL, STACK + CLR + WALL, -Z0 - 3, height)
    cavity = L(x0 - 1, x1 + 1, -CLR, STACK + CLR, 0, height + 30)
    return outer, cavity


def clip_above_table(wp):
    return wp.intersect(B(-1000, 1000, -1000, 1000, 0, 1000))


# ============================ ДЕТАЛІ ============================
def foot_right():
    xi, xe = PANEL_W / 2 - FOOT_LEN, PANEL_W / 2 + CLR
    xo = xe + END_T
    base = B(xi, xo, FOOT_Y[0], FOOT_Y[1], 0, BASE_T)
    outer, cavity = slot_block(xi, xo)
    # бічний упор — нижня частина торця (вище — виріз під USB-кабель)
    stop = L(xe, xo, -CLR - WALL, STACK + CLR + WALL, -Z0 - 3, 6)
    f = base.union(outer).cut(cavity).union(stop)
    # ребро-підпора: передня кромка вздовж задньої грані панелі
    xm = (xi + xe) / 2
    a = tilt_pt(STACK + CLR, 0)
    b = tilt_pt(STACK + CLR, GUSSET_H)
    c = (FOOT_Y[1] - 2, BASE_T - 0.5)
    rib = (cq.Workplane("YZ").polyline([a, b, c]).close().extrude(GUSSET_T)
           .translate((xm - GUSSET_T / 2, 0, 0)))
    f = f.union(rib)
    f = clip_above_table(f)
    # дренаж з паза (вода стікає по оргсклу) + 2 отвори під шурупи в стіл
    for x in (xi + 12, xi + 32):
        ly = tilt_pt(STACK / 2, 0)[0]
        f = f.cut(cylZ(x, ly, 1.6, -1, Z0 + 2))
    for y in (FOOT_Y[0] + 9, FOOT_Y[1] - 9):
        f = f.cut(cylZ(xi + 10, y, 2.1, -1, BASE_T + 1))
    return f


def foot_left():
    return foot_right().mirror("YZ")


def gutter():
    """Відрізок жолоба: затискає нижній край панелі (той самий паз, що в ніжках),
    спереду — корито для крапель і приманки. Локально X = 0…GUTTER_L."""
    L = GUTTER_L
    outer, cavity = slot_block(0, L, height=10.0)
    front = -CLR - WALL                                   # передня грань стінки паза внизу
    body = B(0, L, front - TROUGH_W - 2, STACK + CLR + WALL, 0, Z0)
    body = body.union(B(0, L, front - TROUGH_W - 2, front - TROUGH_W, 0, TROUGH_D + 2))     # борт
    for x0 in (0, L - 2):
        body = body.union(B(x0, x0 + 2, front - TROUGH_W - 2, front + 0.5, 0, TROUGH_D + 2))  # торці
    g = body.union(outer).cut(cavity)
    g = g.cut(B(2, L - 2, front - TROUGH_W, front - 0.2, 2, TROUGH_D + 10))                 # корито
    return clip_above_table(g)


def panel():
    return L(-PANEL_W / 2, PANEL_W / 2, SHIELD_T, STACK, 0, PANEL_H)


def shield():
    return L(-PANEL_W / 2, PANEL_W / 2, 0, SHIELD_T, 0, SHIELD_H)


PRINTED = {"stand_foot_left": foot_left, "stand_foot_right": foot_right, "stand_gutter": gutter}
GUTTER_X = (-GUTTER_L - 1.5, 1.5)                          # два відрізки, зазор 3 мм посередині


def assembly(parts, with_panel=True):
    items = [("foot_left", parts["stand_foot_left"], (0.3, 0.33, 0.38)),
             ("foot_right", parts["stand_foot_right"], (0.3, 0.33, 0.38))]
    for i, x in enumerate(GUTTER_X):
        items.append((f"gutter_{i + 1}", parts["stand_gutter"].translate((x, 0, 0)), (0.95, 0.55, 0.15)))
    if with_panel:
        items.append(("led_panel", panel(), (1.0, 0.96, 0.72)))
        if SHIELD_T > 0:
            items.append(("shield", shield(), (0.8, 0.9, 0.95)))
    return items


# ============================ ПЕРЕВІРКИ ============================
def checks(parts):
    ok = True
    print("== Деталі: стіл %gx%gx%g, одне тіло ==" % BED)
    for n, w in parts.items():
        bb = w.val().BoundingBox()
        f, one = fits_bed(w, BED), single_solid(w)
        ok &= f and one
        print(f"  {n:18s} {bb.xlen:6.1f} × {bb.ylen:6.1f} × {bb.zlen:6.1f} мм   {'OK' if f else 'НЕ ВЛАЗИТЬ'}"
              f"{'' if one else '   НЕ ОДНЕ ТІЛО!'}")
    it = {n: w for n, w, _ in assembly(parts)}
    printed = [n for n in it if n.startswith(("foot", "gutter"))]
    print("\n== Перетини ==")
    worst = 0.0
    for a in ("led_panel", "shield"):
        if a not in it:
            continue
        for b in printed:
            v = overlap(it[a], it[b])
            worst = max(worst, v)
            if v >= 0.5:
                ok = False
                print(f"  {a} ∩ {b} = {v:.2f} мм³  ПЕРЕТИН!")
    for i, a in enumerate(printed):
        for b in printed[i + 1:]:
            v = overlap(it[a], it[b])
            worst = max(worst, v)
            if v >= 0.5:
                ok = False
                print(f"  {a} ∩ {b} = {v:.2f} мм³  ПЕРЕТИН!")
    print(f"  найбільший перетин: {worst:.2f} мм³  {'OK' if worst < 0.5 else 'ПЕРЕТИН!'}")

    # опора: панель, зсунута на 0.5 мм уздовж себе вниз, упирається в дно пазів ніжок
    t = math.radians(TILT)
    down = panel().translate((0, -0.5 * math.sin(t), -0.5 * math.cos(t)))
    rests = all(overlap(down, it[f]) > 0.01 for f in ("foot_left", "foot_right"))
    back = panel().translate((0, 0.5 * math.cos(t), -0.5 * math.sin(t)))
    leans = all(overlap(back, it[f]) > 0.01 for f in ("foot_left", "foot_right"))
    print(f"\n== Опора: дно пазів тримає панель — {'OK' if rests else 'НІ'}; "
          f"ребра підпирають ззаду — {'OK' if leans else 'НІ'}")
    ok &= rests and leans

    # стійкість: центр мас панелі й оргскла над основою ніжок (з запасом)
    com_y = []
    for w, rho in ((panel(), 1.0), (shield(), 1.2)):
        s = w.val()
        com_y.append((s.Center().y, s.Volume() * rho))
    cy = sum(y * m for y, m in com_y) / sum(m for _, m in com_y)
    front_margin, back_margin = cy - FOOT_Y[0], FOOT_Y[1] - cy
    # кут, на який треба нахилити підставку, щоб вона перекинулась уперед (найгірший випадок)
    top = tilt_pt(STACK / 2, PANEL_H / 2)[1]
    tip_front = math.degrees(math.atan2(front_margin, top))
    stable = front_margin > 15 and back_margin > 15
    print(f"== Стійкість: центр мас y = {cy:.1f} мм (основа {FOOT_Y[0]:.0f}…{FOOT_Y[1]:.0f}), "
          f"запас уперед {front_margin:.0f} мм (перекидання при нахилі {tip_front:.0f}°), "
          f"назад {back_margin:.0f} мм — {'OK' if stable else 'НЕСТІЙКО'}")
    ok &= stable
    return ok


def export(parts):
    for d in ("step", "stl"):
        os.makedirs(os.path.join(HERE, d), exist_ok=True)
    for n, w in parts.items():
        cq.exporters.export(w, os.path.join(HERE, "step", n + ".step"))
        cq.exporters.export(w, os.path.join(HERE, "stl", n + ".stl"), tolerance=0.05, angularTolerance=0.15)
    asm = cq.Assembly(name="FlySonar_LED_stand")
    for n, w, rgb in assembly(parts):
        asm.add(w, name=n, color=cq.Color(*rgb))
    asm.save(os.path.join(HERE, "led_stand_assembly.step"))
    print("\nЕкспорт: step/stand_*.step, stl/stand_*.stl, led_stand_assembly.step")


def previews(parts):
    import render
    out = os.path.join(HERE, "preview")
    os.makedirs(out, exist_ok=True)
    size = (1200, 900)

    items = [(w, c) for n, w, c in assembly(parts)]
    pth = os.path.join(out, "5_led_stand.png")
    pj = render.render_png(items, pth, yaw=-28, pitch=-66, size=size, margin=(70, 250, 60, 40))
    xr = PANEL_W / 2
    lab = []
    for (x, y, z), (dx, dy), t in (
            ((xr - 22, 60, 40), (90, -150), "ребро-підпора ззаду"),
            ((xr - 25, -3, 14), (90, 50), "ніжка з пазом"),
            ((-60, -20, 10), (-30, 90), "жолоб: краплі + приманка"),
            ((-60, 10, 150), (-110, -150), "LED-панель A4 + оргскло"),
            ((xr + 2, 3, 12), (90, -40), "виріз під USB-кабель")):
        px, py = pj(x, y, z)
        lab.append((px, py, px + dx, py + dy, t))
    render.add_text(pth, size, "Підставка LED-панелі (ESP32-версія FlySonar)", labels=lab,
                    notes=(f"Панель {PANEL_W:.0f} × {PANEL_H:.0f} × {PANEL_T:.0f} мм, нахил назад {TILT:.0f}°. "
                           "Друкуються 4 деталі: 2 ніжки й 2 відрізки жолоба.",))

    keep = B(60, 1000, -1000, 1000, -1000, 1000)          # розріз по X = 60 мм: жолоб, паз, ніжка позаду
    items = [(w.intersect(keep), c) for n, w, c in assembly(parts)]
    pth = os.path.join(out, "6_led_stand_side.png")
    render.render_png(items, pth, yaw=90, pitch=-90, size=size, margin=(70, 80, 60, 80))
    render.add_text(pth, size, "Збоку: паз із нахиленим дном, ребро-підпора, жолоб спереду",
                    notes=("Камера й турель — справа, за ~30 см. Вода стікає по оргсклу в жолоб і дренажні отвори паза.",))

    layout = [("stand_foot_left", (0, 0, 0)), ("stand_foot_right", (80, 0, 0)), ("stand_gutter", (170, 20, 0)),
              ("stand_gutter", (170, 80, 0))]
    items = []
    for n, off in layout:
        w = parts[n]
        bb = w.val().BoundingBox()
        items.append((w.translate((-bb.xmin, -bb.ymin, -bb.zmin)).translate(off),
                      (0.95, 0.55, 0.15) if "gutter" in n else (0.3, 0.33, 0.38)))
    pth = os.path.join(out, "7_led_stand_parts.png")
    render.render_png(items, pth, yaw=-30, pitch=-60, size=size, margin=(70, 40, 60, 40))
    render.add_text(pth, size, "Деталі підставки: 2 ніжки (ліва/права) + 2 однакові відрізки жолоба",
                    notes=("Друкувати плиском на основі, підтримки не потрібні.",))
    print("Прев'ю: cad/flysonar/preview/5…7")


if __name__ == "__main__":
    parts = {n: f() for n, f in PRINTED.items()}
    ok = checks(parts)
    if "--no-export" not in sys.argv:
        export(parts)
    if "--preview" in sys.argv:
        previews(parts)
    print("\nПЕРЕВІРКИ:", "усе гаразд" if ok else "Є ПРОБЛЕМИ")
    sys.exit(0 if ok else 1)
