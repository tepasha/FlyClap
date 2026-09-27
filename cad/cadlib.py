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
