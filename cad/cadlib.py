"""Спільні примітиви CadQuery для моделей у cad/ (координати в мм, осі як у складанні)."""
import cadquery as cq


def B(x0, x1, y0, y1, z0, z1):
    """Прямокутний брусок за межами по осях."""
    x0, x1 = min(x0, x1), max(x0, x1)
    y0, y1 = min(y0, y1), max(y0, y1)
    z0, z1 = min(z0, z1), max(z0, z1)
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def cylX(y, z, r, x0, x1):
    x0, x1 = min(x0, x1), max(x0, x1)
    return cq.Workplane("YZ").center(y, z).circle(r).extrude(x1 - x0).translate((x0, 0, 0))


def cylY(x, z, r, y0, y1):
    y0, y1 = min(y0, y1), max(y0, y1)
    return cq.Workplane("XZ").center(x, z).circle(r).extrude(-(y1 - y0)).translate((0, y0, 0))


def cylZ(x, y, r, z0, z1):
    z0, z1 = min(z0, z1), max(z0, z1)
    return cq.Workplane("XY").center(x, y).circle(r).extrude(z1 - z0).translate((0, 0, z0))


def overlap(a, b):
    """Об'єм перетину двох деталей, мм³ (NaN, якщо ядро не впоралося)."""
    try:
        return a.val().intersect(b.val()).Volume()
    except Exception:
        return float("nan")


def fits_bed(wp, bed):
    """Чи влазить деталь на стіл (x, y, висота) у будь-якій з осьових орієнтацій."""
    bb = wp.val().BoundingBox()
    d = sorted([bb.xlen, bb.ylen, bb.zlen])
    return d[0] <= bed[2] and d[1] <= min(bed[:2]) and d[2] <= max(bed[:2])


def single_solid(wp):
    s = wp.val()
    return len(s.Solids()) == 1 and s.isValid()


# ============================ КОРОБКА ОСНОВИ ============================
# Основа модульної системи (docs/modules.md): плата 5×7 см з контролером, тумблером
# ARM, статус-LED, гніздом живлення і роз'ємами JST-XH, до яких підключаються модулі.
# Плата зсунута до зовнішньої стінки: кутові вилки (S?B-XH-A) на її краю дивляться
# у вікна, штекер вставляється ззовні.

XH_PITCH = 2.5
XH_WINDOW_H = 8.0          # висота вікна: вилка 7 мм + клямка штекера
PORT_GAP = 2.0             # перемичка між вікнами
PORT_CLR = 0.8             # зазор вікна по ширині

BOX_L, BOX_W, BOX_H, BOX_T = 100.0, 70.0, 40.0, 2.4
PCB_W, PCB_L, PCB_T = 50.0, 70.0, 1.6
PCB_STANDOFF = 5.0         # висота стійок під платою
PCB_EDGE_GAP = 0.6         # від краю плати до стінки з вікнами
PANEL_Z = 29.0             # висота отворів DC-гнізда, тумблера ARM і LED (над платою)


def port_width(kind):
    """Ширина колодки штекера, мм: 'XH2'…'XH6' — JST-XH, 'servo' — 3-pin 2,54 мм."""
    if kind == "servo":
        return 7.8
    return XH_PITCH * (int(kind[2:]) - 1) + 4.9


def port_layout(ports, length=BOX_L):
    """Розкласти вікна вздовж стінки по центру. ports: [(назва, тип)].
    Повертає [(назва, тип, y0, y1)]."""
    widths = [port_width(k) + PORT_CLR for _, k in ports]
    y = (length - sum(widths) - PORT_GAP * (len(ports) - 1)) / 2
    out = []
    for (name, kind), w in zip(ports, widths):
        out.append((name, kind, y, y + w))
        y += w + PORT_GAP
    return out


def port_z():
    """Межі вікон по висоті (z0, z1) у координатах коробки."""
    z0 = BOX_T + PCB_STANDOFF + PCB_T - 0.2
    return z0, z0 + XH_WINDOW_H


def base_box(ports, ear=False, feet=False):
    """Коробка основи під плату 5×7 см з вікнами під роз'єми модулів.
    Локальні координати: x — від стінки кріплення назовні (вікна на x = BOX_W),
    y — уздовж коробки, z — вгору.
    ear  — вухо з двома отворами М3 над задньою стінкою (кріплення до корпусу);
    feet — лапки з отворами під шурупи (кріплення до дошки)."""
    L, Wd, Hh, t = BOX_L, BOX_W, BOX_H, BOX_T
    box = B(0, Wd, 0, L, 0, Hh).cut(B(t, Wd - t, t, L - t, t, Hh + 1))
    if ear:
        box = box.union(B(0, t, 0, L, Hh, Hh + 22))
        for y in (25, 75):
            box = box.cut(cylX(y, Hh + 12, 1.7, -1, t + 1))
    if feet:
        for y0, y1 in ((-12, 0), (L, L + 12)):
            box = box.union(B(10, Wd - 10, y0, y1, 0, t + 0.6))
            for x in (20, Wd - 20):
                box = box.cut(cylZ(x, (y0 + y1) / 2, 2.0, -1, t + 2))
    # стійки під гвинти кришки
    for (x, y) in ((t + 4, t + 4), (Wd - t - 4, t + 4), (t + 4, L - t - 4), (Wd - t - 4, L - t - 4)):
        box = box.union(cylZ(x, y, 3.5, t, Hh)).cut(cylZ(x, y, 1.3, Hh - 12, Hh + 1))
    # стійки плати: край плати — за PCB_EDGE_GAP від стінки з вікнами
    px1 = Wd - t - PCB_EDGE_GAP
    px0, py0 = px1 - PCB_W, (L - PCB_L) / 2
    for (x, y) in ((px0 + 2.5, py0 + 2.5), (px1 - 2.5, py0 + 2.5), (px0 + 2.5, py0 + PCB_L - 2.5),
                   (px1 - 2.5, py0 + PCB_L - 2.5)):
        box = box.union(cylZ(x, y, 2.8, t, t + PCB_STANDOFF)).cut(cylZ(x, y, 0.9, t, t + PCB_STANDOFF + 1))
    # вікна під роз'єми модулів
    z0, z1 = port_z()
    for _, _, y0, y1 in port_layout(ports, L):
        box = box.cut(B(Wd - t - 1, Wd + 1, y0, y1, z0, z1))
    # панель: DC-гніздо, тумблер ARM, статус-LED
    for y, d in ((20, 8.0), (50, 6.3), (80, 5.2)):
        box = box.cut(cylX(y, PANEL_Z, d / 2, Wd - t - 1, Wd + 1))
    # торці: силові дроти 12 В, шланг, запас
    box = box.cut(cylY(Wd / 2 - 12, 24, 5, -13, t + 1)).cut(cylY(Wd / 2 - 12, 24, 5, L - t - 1, L + 13))
    return box


def base_lid():
    """Кришка коробки основи: 4 гвинти, отвір під бузер Ø12."""
    L, Wd, t = BOX_L, BOX_W, BOX_T
    lid = B(0, Wd, 0, L, 0, t)
    for (x, y) in ((t + 4, t + 4), (Wd - t - 4, t + 4), (t + 4, L - t - 4), (Wd - t - 4, L - t - 4)):
        lid = lid.cut(cylZ(x, y, 1.7, -1, t + 1))
    return lid.cut(cylZ(Wd / 2, L / 2, 6, -1, t + 1))


def pcb_dummy():
    """Плата 5×7 см на стійках (муляж для перевірок і картинок)."""
    px1 = BOX_W - BOX_T - PCB_EDGE_GAP
    py0 = (BOX_L - PCB_L) / 2
    z = BOX_T + PCB_STANDOFF
    return B(px1 - PCB_W, px1, py0, py0 + PCB_L, z, z + PCB_T)


def plug_dummies(ports):
    """Кутові вилки на краю плати зі вставленими штекерами (муляж): перевіряє,
    що кожен штекер проходить у своє вікно."""
    z = BOX_T + PCB_STANDOFF + PCB_T
    px1 = BOX_W - BOX_T - PCB_EDGE_GAP
    out = None
    for _, kind, y0, y1 in port_layout(ports):
        w = port_width(kind)
        yc = (y0 + y1) / 2
        h = 2.6 if kind == "servo" else 5.75
        plug = B(px1 - 7, BOX_W + 6, yc - w / 2, yc + w / 2, z + 0.6, z + 0.6 + h)
        out = plug if out is None else out.union(plug)
    return out
