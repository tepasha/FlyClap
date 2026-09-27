"""Прев'ю CadQuery-моделей у PNG: растеризація з z-буфером на numpy.

Затінення за Ламбертом, контури на межах деталей і перепадах глибини,
згладжування 2×. Потрібні лише cadquery і numpy (PNG пише сам модуль).
Підписи додаються через SVG-обгортку і headless Chromium (див. to_png_with_text),
без Chromium зберігається PNG без підписів.
"""
import base64
import math
import os
import shutil
import struct
import subprocess
import zlib

import numpy as np


def _rot(yaw_deg, pitch_deg):
    """Камера: yaw — поворот навколо Z, pitch — нахил (−90 = вид спереду, 0 = згори)."""
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    rz = np.array([[math.cos(y), -math.sin(y), 0], [math.sin(y), math.cos(y), 0], [0, 0, 1]])
    rx = np.array([[1, 0, 0], [0, math.cos(p), -math.sin(p)], [0, math.sin(p), math.cos(p)]])
    return rx @ rz


def _write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[i].tobytes() for i in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def render_png(items, path, yaw=-35, pitch=-60, size=(1200, 900), margin=(30, 30, 30, 30), tol=0.3, ss=2):
    """items: [(Workplane/Shape, (r,g,b) 0..1)]. margin: (верх, праворуч, низ, ліворуч) у пікселях."""
    R = _rot(yaw, pitch)
    light = R @ (np.array([-0.35, -0.55, 0.75]) / np.linalg.norm([-0.35, -0.55, 0.75]))
    meshes = []
    for pid, (obj, rgb) in enumerate(items):
        shape = obj.val() if hasattr(obj, "val") else obj
        verts, faces = shape.tessellate(tol, 0.25)
        if not faces:
            continue
        v = np.array([(p.x, p.y, p.z) for p in verts]) @ R.T
        meshes.append((pid + 1, v, np.array(faces), np.array(rgb)))
    allv = np.vstack([m[1] for m in meshes])
    mn, mx = allv.min(axis=0), allv.max(axis=0)
    W, H = size[0] * ss, size[1] * ss
    mt, mr, mb, ml = (m * ss for m in margin)
    sc = min((W - ml - mr) / (mx[0] - mn[0]), (H - mt - mb) / (mx[1] - mn[1]))
    ox = ml + ((W - ml - mr) - (mx[0] - mn[0]) * sc) / 2 - mn[0] * sc
    oy = mt + ((H - mt - mb) - (mx[1] - mn[1]) * sc) / 2 + mx[1] * sc

    zbuf = np.full((H, W), -np.inf)
    img = np.ones((H, W, 3))
    pidb = np.zeros((H, W), dtype=np.int32)
    for pid, v, f, rgb in meshes:
        sx = ox + v[:, 0] * sc
        sy = oy - v[:, 1] * sc
        sz = v[:, 2]
        a, b, c = f[:, 0], f[:, 1], f[:, 2]
        n = np.cross(v[b] - v[a], v[c] - v[a])
        ln = np.linalg.norm(n, axis=1)
        ok = ln > 1e-12
        n[ok] /= ln[ok, None]
        shade = 0.35 + 0.65 * np.abs(n @ light)
        for i in np.nonzero(ok)[0]:
            ia, ib, ic = a[i], b[i], c[i]
            x0, x1 = int(max(min(sx[ia], sx[ib], sx[ic]), 0)), int(min(max(sx[ia], sx[ib], sx[ic]) + 1, W - 1))
            y0, y1 = int(max(min(sy[ia], sy[ib], sy[ic]), 0)), int(min(max(sy[ia], sy[ib], sy[ic]) + 1, H - 1))
            if x1 < x0 or y1 < y0:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            d = (sy[ib] - sy[ic]) * (sx[ia] - sx[ic]) + (sx[ic] - sx[ib]) * (sy[ia] - sy[ic])
            if abs(d) < 1e-12:
                continue
            w0 = ((sy[ib] - sy[ic]) * (xs - sx[ic]) + (sx[ic] - sx[ib]) * (ys - sy[ic])) / d
            w1 = ((sy[ic] - sy[ia]) * (xs - sx[ic]) + (sx[ia] - sx[ic]) * (ys - sy[ic])) / d
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            z = w0 * sz[ia] + w1 * sz[ib] + w2 * sz[ic]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            upd = inside & (z > sub)
            sub[upd] = z[upd]
            img[y0:y1 + 1, x0:x1 + 1][upd] = rgb * shade[i]
            pidb[y0:y1 + 1, x0:x1 + 1][upd] = pid

    # контури: межі деталей і різкі перепади глибини
    edge = np.zeros((H, W), dtype=bool)
    for dy, dx in ((0, 1), (1, 0)):
        p2 = np.roll(pidb, (-dy, -dx), axis=(0, 1))
        z2 = np.roll(zbuf, (-dy, -dx), axis=(0, 1))
        with np.errstate(invalid="ignore"):
            e = (pidb != p2) | (np.abs(np.nan_to_num(zbuf - z2, nan=0, posinf=0, neginf=0)) > 2.0)
        edge |= e
    img[edge & ((pidb > 0) | np.roll(pidb > 0, -1, axis=1) | np.roll(pidb > 0, -1, axis=0))] *= 0.35
    # згладжування ss×ss
    img = img.reshape(H // ss, ss, W // ss, ss, 3).mean(axis=(1, 3))
    _write_png(path, (np.clip(img, 0, 1) * 255).astype(np.uint8))

    def project(x, y, z):
        """3D-точка моделі → піксель готової картинки (для виносок)."""
        q = R @ np.array([x, y, z])
        return round((ox + q[0] * sc) / ss), round((oy - q[1] * sc) / ss)
    return project


def _chrome():
    return os.environ.get("CHROME") or next(
        (p for p in ("chrome-headless-shell", "headless_shell", "chromium", "chromium-browser", "google-chrome")
         if shutil.which(p)), None)


def add_text(png_path, size, title=None, notes=(), labels=()):
    """Накласти заголовок, примітки й виноски [(x, y, lx, ly, text)] на PNG через Chromium."""
    chrome = _chrome()
    if not chrome:
        print("підписи пропущено: не знайдено Chromium (змінна CHROME)")
        return png_path
    W, H = size
    data = base64.b64encode(open(png_path, "rb").read()).decode()
    font = 'font-family="DejaVu Sans, Arial, sans-serif"'
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
         f'<image href="data:image/png;base64,{data}" x="0" y="0" width="{W}" height="{H}"/>']
    if title:
        s.append(f'<text x="24" y="38" {font} font-size="22" font-weight="bold" fill="#1b1b1b">{title}</text>')
    for i, n in enumerate(notes):
        s.append(f'<text x="24" y="{H - 16 - 20 * (len(notes) - 1 - i)}" {font} font-size="14" fill="#5b6470">{n}</text>')
    for x, y, lx, ly, t in labels:
        anchor = "end" if lx < x else "start"
        s.append(f'<line x1="{x}" y1="{y}" x2="{lx}" y2="{ly}" stroke="#374151" stroke-width="1.3"/>')
        s.append(f'<circle cx="{x}" cy="{y}" r="3.5" fill="#374151"/>')
        tx = lx - 6 if anchor == "end" else lx + 6
        s.append(f'<text x="{tx}" y="{ly + 5}" {font} font-size="14" text-anchor="{anchor}" fill="#111" '
                 f'stroke="#ffffff" stroke-width="4" paint-order="stroke">{t}</text>')
    s.append("</svg>")
    svg = png_path[:-4] + ".tmp.svg"
    with open(svg, "w", encoding="utf-8") as f:
        f.write("\n".join(s))
    subprocess.run([chrome, "--headless", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                    f"--window-size={W},{H}", f"--screenshot={os.path.abspath(png_path)}",
                    "file://" + os.path.abspath(svg)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.remove(svg)
    return png_path
