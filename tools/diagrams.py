#!/usr/bin/env python3
"""Генератор схем підключення та ілюстрацій пристроїв (SVG + PNG) для папки schematics/.

Запуск з кореня репозиторію:
    python3 tools/diagrams.py          # лише SVG (стандартна бібліотека Python)
    python3 tools/diagrams.py --png    # + PNG 2x у schematics/png/

PNG рендерить headless Chromium: шлях у змінній CHROME або перший знайдений
з chrome-headless-shell / headless_shell / chromium / google-chrome.
"""
import os
import shutil
import subprocess
import sys
from xml.sax.saxutils import escape

OUT = os.path.join(os.path.dirname(__file__), "..", "schematics")
SAVED = []

# Кольори дротів (однакові на всіх схемах)
C12 = "#d62828"    # +12 В
C5 = "#f08c00"     # +5 В логіка
CSV = "#8e44ad"    # живлення серв
C33 = "#e0a100"    # +3.3 В
CSIG = "#1f6fd1"   # сигнали
CGND = "#222222"   # земля
CWATER = "#2a9df4"
INK = "#1b1b1b"
MUTED = "#5b6470"
CCONN = "#0b7a75"  # мітки роз'ємів JST-XH (docs/modules.md)
FONT = "DejaVu Sans, Arial, Helvetica, sans-serif"


class Svg:
    def __init__(self, w, h, title):
        self.w, self.h, self.items = w, h, []
        self.rect(0, 0, w, h, fill="#ffffff", stroke="none")
        self.text(24, 38, title, size=22, weight="bold")

    # ---------- примітиви ----------
    def add(self, s):
        self.items.append(s)

    def rect(self, x, y, w, h, fill="none", stroke=INK, sw=1.6, rx=0, dash=None, opacity=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        o = f' opacity="{opacity}"' if opacity is not None else ""
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" '
                 f'stroke="{stroke}" stroke-width="{sw}"{d}{o}/>')

    def line(self, x1, y1, x2, y2, color=INK, sw=2, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" '
                 f'stroke-width="{sw}" stroke-linecap="round"{d}/>')

    def wire(self, pts, color=CSIG, sw=2.4, dash=None):
        p = " ".join(f"{x},{y}" for x, y in pts)
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<polyline points="{p}" fill="none" stroke="{color}" stroke-width="{sw}" '
                 f'stroke-linejoin="round" stroke-linecap="round"{d}/>')

    def path(self, d, fill="none", stroke=INK, sw=1.8, extra=""):
        self.add(f'<path d="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" '
                 f'stroke-linejoin="round" stroke-linecap="round" {extra}/>')

    def circle(self, x, y, r, fill="none", stroke=INK, sw=1.8):
        self.add(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>')

    def dot(self, x, y, color=INK):
        self.circle(x, y, 4, fill=color, stroke=color, sw=1)

    def text(self, x, y, s, size=14, anchor="start", weight="normal", color=INK, italic=False):
        st = ' font-style="italic"' if italic else ""
        for i, line in enumerate(str(s).split("\n")):
            self.add(f'<text x="{x}" y="{y + i * (size + 4)}" font-family="{FONT}" font-size="{size}" '
                     f'text-anchor="{anchor}" font-weight="{weight}" fill="{color}"{st}>{escape(line)}</text>')

    def box(self, x, y, w, h, title=None, fill="#f7f9fc", stroke="#b8c2cf"):
        self.rect(x, y, w, h, fill=fill, stroke=stroke, sw=1.4, rx=10)
        if title:
            self.text(x + 14, y + 24, title, size=15, weight="bold")

    # ---------- електричні символи ----------
    def gnd(self, x, y, color=CGND):
        self.line(x, y - 10, x, y, color, 2.2)
        for i, hw in enumerate((12, 8, 4)):
            self.line(x - hw, y + i * 4, x + hw, y + i * 4, color, 2.2)

    def vcc(self, x, y, label, color):
        self.line(x - 10, y, x + 10, y, color, 2.6)
        self.line(x, y, x, y + 10, color, 2.4)
        self.text(x, y - 6, label, size=12, anchor="middle", weight="bold", color=color)

    def res_v(self, x, y1, y2, label, color=INK, side="right"):
        """Резистор вертикально між (x,y1) і (x,y2)."""
        m1, m2 = y1 + (y2 - y1) * 0.2, y2 - (y2 - y1) * 0.2
        self.line(x, y1, x, m1, color, 2.2)
        self.rect(x - 7, m1, 14, m2 - m1, fill="#fff", stroke=INK, sw=1.8)
        self.line(x, m2, x, y2, color, 2.2)
        if side == "right":
            self.text(x + 12, (y1 + y2) / 2 + 5, label, size=12)
        else:
            self.text(x - 12, (y1 + y2) / 2 + 5, label, size=12, anchor="end")

    def res_h(self, x1, x2, y, label, color=INK):
        m1, m2 = x1 + (x2 - x1) * 0.2, x2 - (x2 - x1) * 0.2
        self.line(x1, y, m1, y, color, 2.2)
        self.rect(m1, y - 7, m2 - m1, 14, fill="#fff", stroke=INK, sw=1.8)
        self.line(m2, y, x2, y, color, 2.2)
        self.text((x1 + x2) / 2, y - 12, label, size=12, anchor="middle")

    def pot_v(self, x, y1, y2, wiper_y, label):
        self.res_v(x, y1, y2, "")
        self.path(f"M{x + 26},{wiper_y} L{x + 9},{wiper_y}", sw=2)
        self.path(f"M{x + 9},{wiper_y} l7,-5 M{x + 9},{wiper_y} l7,5", sw=2)
        self.text(x - 12, (y1 + y2) / 2 + 5, label, size=12, anchor="end")

    def diode_v(self, x, y1, y2, label, cathode_top=True, color=INK, led=False, side="right"):
        """Діод вертикально; cathode_top — смужка зверху (струм знизу вгору)."""
        mid = (y1 + y2) / 2
        self.line(x, y1, x, mid - 9, color, 2.2)
        self.line(x, mid + 9, x, y2, color, 2.2)
        if cathode_top:
            self.path(f"M{x - 10},{mid + 9} L{x + 10},{mid + 9} L{x},{mid - 9} Z", fill="#fff")
            self.line(x - 10, mid - 9, x + 10, mid - 9, INK, 2.2)
        else:
            self.path(f"M{x - 10},{mid - 9} L{x + 10},{mid - 9} L{x},{mid + 9} Z", fill="#fff")
            self.line(x - 10, mid + 9, x + 10, mid + 9, INK, 2.2)
        if led:
            for dy in (-6, 3):
                self.path(f"M{x + 12},{mid + dy} l12,-8 m-5,0 l5,0 l0,5", sw=1.6, stroke="#c0392b")
        tx = x + (30 if led else 16) if side == "right" else x - 16
        self.text(tx, mid + 5, label, size=12, anchor="start" if side == "right" else "end")

    def cap_v(self, x, y1, y2, label, polar=True, side="right"):
        mid = (y1 + y2) / 2
        self.line(x, y1, x, mid - 4, INK, 2.2)
        self.line(x, mid + 4, x, y2, INK, 2.2)
        self.line(x - 12, mid - 4, x + 12, mid - 4, INK, 2.6)
        if polar:
            self.path(f"M{x - 12},{mid + 7} Q{x},{mid + 1} {x + 12},{mid + 7}", sw=2.6)
            self.text(x - 16, mid - 6, "+", size=12, anchor="end")
        else:
            self.line(x - 12, mid + 4, x + 12, mid + 4, INK, 2.6)
        if side == "right":
            self.text(x + 18, mid + 5, label, size=12)
        else:
            self.text(x - 18, mid + 5, label, size=12, anchor="end")

    def switch_h(self, x1, x2, y, label=None):
        self.circle(x1, y, 3.5, fill="#fff")
        self.circle(x2, y, 3.5, fill="#fff")
        self.line(x1 + 3, y - 2, x2 - 2, y - 14, INK, 2.2)
        if label:
            self.text((x1 + x2) / 2, y + 20, label, size=11, anchor="middle", color=MUTED)

    def mosfet(self, x, y, name):
        """N-MOSFET: G ліворуч (x, y+30), D зверху (x+30, y), S знизу (x+30, y+60)."""
        self.line(x, y + 30, x + 16, y + 30, INK, 2.2)
        self.line(x + 16, y + 14, x + 16, y + 46, INK, 2.6)
        for yy in (16, 30, 44):
            self.line(x + 22, y + yy - 6, x + 22, y + yy + 6, INK, 2.6)
        self.path(f"M{x + 22},{y + 16} L{x + 30},{y + 16} L{x + 30},{y}", sw=2.2)
        self.path(f"M{x + 22},{y + 44} L{x + 30},{y + 44} L{x + 30},{y + 60}", sw=2.2)
        self.path(f"M{x + 22},{y + 30} L{x + 30},{y + 30} L{x + 30},{y + 44}", sw=2.2)
        self.path(f"M{x + 23},{y + 30} l7,-4 l0,8 Z", fill=INK, sw=1)
        self.text(x + 38, y + 34, name, size=12, weight="bold")
        self.text(x + 2, y + 24, "G", size=10, color=MUTED)
        self.text(x + 34, y + 12, "D", size=10, color=MUTED)
        self.text(x + 34, y + 58, "S", size=10, color=MUTED)
        return (x, y + 30), (x + 30, y), (x + 30, y + 60)

    def npn(self, x, y, name):
        """NPN: база ліворуч (x, y+30), колектор зверху (x+30, y), емітер знизу (x+30, y+60)."""
        self.circle(x + 22, y + 30, 20, fill="#fff")
        self.line(x, y + 30, x + 14, y + 30, INK, 2.2)
        self.line(x + 14, y + 18, x + 14, y + 42, INK, 2.8)
        self.path(f"M{x + 14},{y + 24} L{x + 30},{y + 12} L{x + 30},{y}", sw=2.2)
        self.path(f"M{x + 14},{y + 36} L{x + 30},{y + 48} L{x + 30},{y + 60}", sw=2.2)
        self.path(f"M{x + 30},{y + 48} l-8,-1 l4,-6 Z", fill=INK, sw=1)
        self.text(x + 48, y + 34, name, size=12, weight="bold")
        return (x, y + 30), (x + 30, y), (x + 30, y + 60)

    def coil_v(self, x, y1, y2, label, side="left"):
        self.line(x, y1, x, y1 + 6, INK, 2.2)
        h = y2 - y1 - 12
        n = 4
        d = f"M{x},{y1 + 6}"
        for i in range(n):
            d += f" a8,{h / n / 2} 0 0 1 0,{h / n}"
        self.path(d, sw=2.2)
        self.line(x, y2 - 6, x, y2, INK, 2.2)
        if side == "left":
            self.text(x - 16, (y1 + y2) / 2 + 5, label, size=12, anchor="end")
        else:
            self.text(x + 18, (y1 + y2) / 2 + 5, label, size=12)

    def motor(self, x, y, label):
        self.circle(x, y, 16, fill="#fff")
        self.text(x, y + 5, "M", size=14, anchor="middle", weight="bold")
        self.text(x + 24, y + 5, label, size=12)

    def servo(self, x, y, name, sub="серво"):
        """Серво-коробочка; повертає точки під'єднання (сигнал, V+, GND) ліворуч."""
        self.rect(x, y, 110, 56, fill="#2f3b4a", stroke="#1d2530", rx=6)
        self.circle(x + 84, y + 28, 12, fill="#dfe6ee", stroke="#1d2530")
        self.text(x + 8, y + 22, name, size=12, weight="bold", color="#ffffff")
        self.text(x + 8, y + 42, sub, size=11, color="#cfd8e3")
        return (x, y + 14), (x, y + 28), (x, y + 42)

    def chip(self, x, y, w, name, left, right, pitch=34, top=46, sub=None, fill="#0f5c8c"):
        """Модуль/плата з пінами. left/right — списки назв (None — пропуск). Повертає {pin: (x,y)}."""
        rows = max(len(left), len(right))
        h = top + rows * pitch + 10
        self.rect(x, y, w, h, fill=fill, stroke="#0b3a58", rx=8, sw=2)
        self.text(x + w / 2, y + 26, name, size=15, anchor="middle", weight="bold", color="#ffffff")
        if sub:
            self.text(x + w / 2, y + 42, sub, size=11, anchor="middle", color="#d7e6f3")
        pins = {}
        for side, names in (("L", left), ("R", right)):
            for i, n in enumerate(names):
                if n is None:
                    continue
                py = y + top + 12 + i * pitch
                px = x if side == "L" else x + w
                self.rect(px - 5, py - 5, 10, 10, fill="#e9c46a", stroke="#8a6d1f", sw=1)
                tx = x + 12 if side == "L" else x + w - 12
                self.text(tx, py + 5, n, size=13, anchor="start" if side == "L" else "end",
                          weight="bold", color="#ffffff")
                pins.setdefault(n, (px, py))       # однакові назви (GND) — перша, ліва
        return pins, h

    def tag(self, x, y, label):
        """Мітка роз'єму основи, напр. "B·3" — роз'єм B, контакт 3. Повертає ширину."""
        w = 10 + 7 * len(label)
        self.rect(x, y - 9, w, 18, fill="#e6f4f3", stroke=CCONN, sw=1.2, rx=4)
        self.text(x + w / 2, y + 4, label, size=11, anchor="middle", weight="bold", color=CCONN)
        return w

    def net(self, x, y, label, color=CSIG, anchor="start", size=13, weight="bold"):
        self.text(x, y + 5, label, size=size, anchor=anchor, weight=weight, color=color)

    def legend(self, x, y, entries, notes=(), width=560):
        self.box(x, y, width, 40 + 24 * len(entries) + 20 * len(notes), "Позначення")
        yy = y + 50
        for color, label in entries:
            self.line(x + 18, yy - 4, x + 58, yy - 4, color, 4)
            self.text(x + 70, yy, label, size=13)
            yy += 24
        for n in notes:
            self.text(x + 18, yy, n, size=12, color=MUTED)
            yy += 20

    def save(self, name):
        SAVED.append((name, self.w, self.h))
        os.makedirs(OUT, exist_ok=True)
        body = "\n".join(self.items)
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
               f'viewBox="0 0 {self.w} {self.h}">\n{body}\n</svg>\n')
        with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
            f.write(svg)
        print("wrote", name)


# =====================================================================
#                          FlyClap: схема (варіант A)
# =====================================================================
def flyclap_wiring():
    s = Svg(1200, 900, "FlyClap — схема підключення (варіант A: фототранзистори + LM339)")

    # ---- живлення ----
    s.box(24, 60, 300, 470, "Живлення")
    s.rect(44, 100, 130, 64, fill="#3d3d3d", stroke="#222", rx=6)
    s.text(109, 128, "БЖ 12 В", size=14, anchor="middle", weight="bold", color="#fff")
    s.text(109, 148, "2 А", size=12, anchor="middle", color="#ddd")
    s.text(182, 118, "+", size=13, weight="bold", color=C12)
    s.text(182, 160, "−", size=15, weight="bold")
    s.wire([(174, 122), (290, 122)], C12)
    s.dot(240, 122, C12)
    s.wire([(240, 122), (240, 350), (174, 350)], C12)                  # → DC-DC
    s.dot(290, 122, C12)
    s.net(186, 100, "+12V → соленоїд", C12, size=12)
    s.wire([(174, 152), (205, 152), (205, 170)], CGND)
    s.gnd(205, 180)

    s.rect(44, 330, 130, 76, fill="#1f7a4d", stroke="#145236", rx=6)
    s.text(109, 358, "DC-DC", size=14, anchor="middle", weight="bold", color="#fff")
    s.text(109, 376, "5–6 В ≥3 А", size=12, anchor="middle", color="#e6f4ec")
    s.text(109, 394, "XL4015", size=11, anchor="middle", color="#cde9da")
    s.wire([(174, 390), (205, 390), (205, 408)], CGND)
    s.gnd(205, 418)
    s.wire([(109, 406), (109, 470), (200, 470)], CSV)
    s.dot(150, 470, CSV)
    s.cap_v(150, 470, 505, "470 мкФ", side="left")
    s.gnd(150, 515)
    s.net(206, 470, "V_servo → серво", CSV, size=12)

    # ---- Arduino Nano ----
    right = ["D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11", "D13", "A0"]
    left = [None, "VIN", None, "5V", None, "GND"]
    pins, h = s.chip(360, 140, 140, "Arduino Nano", left, right, pitch=33, sub="(або Uno)")
    vx, vy = pins["VIN"]
    s.wire([(290, 122), (290, vy), (vx, vy)], C12)
    x5, y5 = pins["5V"]
    s.wire([(x5, y5), (310, y5)], C5)
    s.net(304, y5, "+5V", C5, anchor="end")
    xg, yg = pins["GND"]
    s.wire([(xg, yg), (336, yg), (336, yg + 16)], CGND)
    s.gnd(336, yg + 26)

    notes = {
        "D4": "← ІЧ к.1 (п.2)",
        "D5": "← ІЧ к.2 (п.1)",
        "D6": "← ІЧ к.3 (п.14)",
        "D7": "← ІЧ к.4 (п.13)",
        "D8": "← кінцевик",
        "D9": "→ затвор MOSFET",
        "D10": "→ сигнал серви",
        "D11": "→ бузер",
        "D13": "вбудований LED",
        "A0": "← тумблер ARM",
    }
    tags = {"D4": "B·3", "D5": "B·4", "D6": "B·5", "D7": "B·6", "D8": "S1·3", "D9": "P1", "D10": "servo"}
    for p, n in notes.items():
        px, py = pins[p]
        s.wire([(px, py), (px + 12, py)], CSIG)
        tx = px + 16
        if p in tags:
            tx += s.tag(px + 14, py, tags[p]) + 2
        s.text(tx, py + 5, n, size=12, color=CSIG if p != "D13" else MUTED)

    # ---- ІЧ-канал ----
    s.box(700, 60, 480, 330, "ІЧ-канал ×4 (показано канал 1) — роз'єми B і E")
    # ІЧ-світлодіод
    s.vcc(740, 110, "+5V", C5)
    s.res_v(740, 120, 170, "150 Ом", side="left")
    s.diode_v(740, 170, 218, "", cathode_top=False, led=True)
    s.gnd(740, 238)
    s.text(740, 262, "ІЧ-LED\n940 нм", size=11, anchor="middle", color=MUTED)
    s.path("M772,196 q8,-8 16,0 t16,0 t16,0", stroke="#c0392b", sw=2)
    s.path("M814,196 l-7,-5 m7,5 l-7,5", stroke="#c0392b", sw=2)
    s.text(792, 184, "промінь", size=11, anchor="middle", color="#c0392b")
    # фототранзистор
    s.vcc(850, 110, "+5V", C5)
    s.res_v(850, 120, 160, "10 кОм", side="left")
    s.dot(850, 170)
    s.line(850, 160, 850, 170, INK, 2.2)
    s.circle(850, 200, 17, fill="#fff")
    s.line(843, 190, 843, 210, INK, 2.6)
    s.path("M843,195 L852,184 L852,170", sw=2.2)
    s.path("M843,205 L852,216 L852,228", sw=2.2)
    s.path("M852,216 l-7,-1 l3,-6 Z", fill=INK, sw=1)
    s.path("M822,190 l10,6 m-4,-6 l4,6 l-6,1", stroke="#c0392b", sw=1.6)
    s.line(850, 228, 850, 238, INK, 2.2)
    s.gnd(850, 248)
    s.text(836, 272, "TEFT4300", size=11, anchor="end", color=MUTED)
    # компаратор
    s.path("M960,140 L960,250 L1040,195 Z", fill="#fff", sw=2.2)
    s.text(968, 170, "+", size=15, weight="bold")
    s.text(968, 232, "−", size=17, weight="bold")
    s.text(1000, 270, "LM339 (¼)", size=12, anchor="middle", weight="bold")
    s.wire([(850, 170), (960, 170)], INK, 2.2)
    s.dot(910, 170)
    # гістерезис 1 МОм
    s.wire([(910, 170), (910, 110), (930, 110)], INK, 2.2)
    s.res_h(930, 1040, 110, "1 МОм (гістерезис)")
    s.wire([(1040, 110), (1070, 110), (1070, 195)], INK, 2.2)
    # підстроювальник
    s.vcc(905, 272, "", C5)
    s.text(885, 272, "+5V", size=12, anchor="end", weight="bold", color=C5)
    s.pot_v(905, 282, 342, 312, "10 кОм")
    s.gnd(905, 352)
    s.wire([(931, 312), (945, 312), (945, 225), (960, 225)], INK, 2.2)
    # вихід + підтяжка
    s.wire([(1040, 195), (1150, 195)], CSIG)
    s.dot(1070, 195)
    s.dot(1110, 195)
    s.vcc(1110, 130, "+5V", C5)
    s.res_v(1110, 140, 195, "10 кОм", side="right")
    s.text(1150, 222, "→ D4", size=14, anchor="end", weight="bold", color=CSIG)
    s.text(1164, 330, "Промінь цілий → LOW,\nперекритий → HIGH", size=11, anchor="end", color=MUTED)

    # ---- соленоїд ----
    s.box(700, 405, 480, 215, "Соленоїд — роз'єм P1")
    s.net(716, 530, "D9", CSIG)
    s.wire([(740, 530), (752, 530)], CSIG)
    s.res_h(752, 812, 530, "100 Ом", CSIG)
    s.dot(830, 530)
    s.wire([(812, 530), (850, 530)], CSIG)
    s.res_v(830, 530, 580, "10 кОм", side="left")
    s.gnd(830, 592)
    g, d, sr = s.mosfet(850, 500, "IRLZ44N")
    s.wire([sr, (sr[0], 592)], CGND)
    s.gnd(sr[0], 600)
    s.wire([d, (d[0], 490), (1000, 490)], INK, 2.2)
    s.vcc(1000, 432, "+12V", C12)
    s.dot(1000, 446)
    s.coil_v(1000, 442, 490, "соленоїд 12 В", side="left")
    s.dot(1000, 490)
    s.wire([(1000, 446), (1060, 446)], INK, 2.2)
    s.diode_v(1060, 446, 490, "1N5819", cathode_top=True)
    s.wire([(1060, 490), (1000, 490)], INK, 2.2)
    s.vcc(1130, 500, "+12V", C12)
    s.cap_v(1130, 510, 560, "2200 мкФ", side="left")
    s.gnd(1130, 572)

    # ---- інше ----
    s.box(700, 635, 480, 250, "Серво (3-pin), кінцевик (S1), бузер і ARM (на основі)")
    sig, vp, gg = s.servo(870, 670, "MG996R")
    s.net(716, 684, "D10", CSIG)
    s.wire([(752, 684), sig], CSIG)
    s.wire([vp, (820, 698)], CSV)
    s.net(816, 698, "V_servo", CSV, anchor="end", size=11)
    s.wire([gg, (850, 712), (850, 718)], CGND)
    s.gnd(850, 726)
    s.text(996, 694, "живлення лише\nвід DC-DC", size=11, color=MUTED)
    rows = [("D8", 770, "кінцевик (LOW = взведено)"),
            ("D11", 808, "бузер (пасивний)"),
            ("A0", 846, "тумблер ARM (LOW = озброєно)")]
    for pin, y, lab in rows:
        s.net(716, y, pin, CSIG)
        s.wire([(752, y), (790, y)], CSIG)
        if pin == "D11":
            s.circle(806, y, 14, fill="#fff")
            s.text(806, y + 4, "♪", size=13, anchor="middle")
            s.wire([(820, y), (850, y)], CGND)
        else:
            s.switch_h(790, 830, y)
            s.wire([(833, y), (850, y)], CGND)
        s.gnd(850, y + 10)
        s.text(880, y + 5, lab, size=12)

    s.legend(24, 560, [(C12, "+12 В (БЖ): соленоїд, VIN Nano, вхід DC-DC"),
                       (C5, "+5 В логіка (пін 5V Nano): LM339, ІЧ-LED, підтяжки"),
                       (CSV, "V_servo 5–6 В (DC-DC): лише серво"),
                       (CSIG, "сигнали"),
                       (CGND, "земля (⏚) — спільна для всього")],
             notes=("Силові землі (соленоїд, серво) — зіркою до мінуса БЖ, не через Nano.",
                    "LM339: пін 3 → +5V, пін 12 → ⏚, 100 нФ між ними.",
                    "Канали (IN+, IN−, OUT): 1 — 5, 4, 2 · 2 — 7, 6, 1 · 3 — 9, 8, 14 · 4 — 11, 10, 13.",
                    "Трубки з термоусадки на фототранзистори — від стороннього світла.",
                    "Зелені мітки — роз'єм основи JST-XH і контакт (B·3 = роз'єм B, контакт 3): modules-connectors.svg."),
             width=660)
    s.save("flyclap-wiring.svg")



def power_block(s, x, y, dcdc_label, cap_label, out_label, out_color):
    """БЖ 12 В + DC-DC. Повертає точку виходу +12V (праворуч) і вихід DC-DC."""
    s.box(x, y, 300, 430, "Живлення")
    s.rect(x + 20, y + 40, 130, 64, fill="#3d3d3d", stroke="#222", rx=6)
    s.text(x + 85, y + 68, "БЖ 12 В", size=14, anchor="middle", weight="bold", color="#fff")
    s.text(x + 85, y + 88, "2 А", size=12, anchor="middle", color="#ddd")
    s.wire([(x + 150, y + 62), (x + 266, y + 62)], C12)
    s.dot(x + 216, y + 62, C12)
    s.wire([(x + 150, y + 92), (x + 181, y + 92), (x + 181, y + 110)], CGND)
    s.gnd(x + 181, y + 120)
    s.rect(x + 20, y + 250, 130, 76, fill="#1f7a4d", stroke="#145236", rx=6)
    s.text(x + 85, y + 278, "DC-DC", size=14, anchor="middle", weight="bold", color="#fff")
    s.text(x + 85, y + 297, dcdc_label, size=12, anchor="middle", color="#e6f4ec")
    s.wire([(x + 216, y + 62), (x + 216, y + 270), (x + 150, y + 270)], C12)
    s.wire([(x + 150, y + 310), (x + 181, y + 310), (x + 181, y + 328)], CGND)
    s.gnd(x + 181, y + 338)
    s.wire([(x + 85, y + 326), (x + 85, y + 380), (x + 200, y + 380)], out_color)
    s.dot(x + 126, y + 380, out_color)
    s.cap_v(x + 126, y + 380, y + 412, cap_label, side="left")
    s.gnd(x + 126, y + 422)
    s.net(x + 206, y + 380, out_label, out_color, size=12)
    return (x + 266, y + 62)


# =====================================================================
#                   FlyClap: варіант B (TSSP4038)
# =====================================================================
def flyclap_tssp():
    s = Svg(1200, 600, "FlyClap — варіант B: модульована завіса TSSP4038 (BEAM_SENSOR_TSSP 1)")
    s.text(24, 64, "Замість фототранзисторів, LM339 і підстроювальників. Решта схеми (живлення, соленоїд, серво, кінцевик, ARM) — як у варіанті A.",
           size=13, color=MUTED)

    pins, h = s.chip(40, 100, 140, "Arduino Nano", ["5V", None, "GND"],
                     ["D3", "D4", "D5", "D6", "D7", "D11"], pitch=40)
    tags = {"D3": "E·3", "D4": "B·3", "D5": "B·4", "D6": "B·5", "D7": "B·6"}
    for p, lab in (("D3", "→ 38 кГц"), ("D4", "← приймач 1"), ("D5", "← приймач 2"),
                   ("D6", "← приймач 3"), ("D7", "← приймач 4"), ("D11", "→ бузер")):
        px, py = pins[p]
        s.wire([(px, py), (px + 12, py)], CSIG)
        tx = px + 16
        if p in tags:
            tx += s.tag(px + 14, py, tags[p]) + 2
        s.text(tx, py + 5, lab, size=12, color=CSIG)
    x5, y5 = pins["5V"]
    s.wire([(x5, y5), (24, y5)], C5)
    s.net(24, y5 - 16, "+5V", C5, anchor="start", size=12)
    xg, yg = pins["GND"]
    s.wire([(xg, yg), (28, yg), (28, yg + 16)], CGND)
    s.gnd(28, yg + 26)

    # --- передавач: 4 ІЧ-світлодіоди через BC337 ---
    s.box(330, 90, 400, 360, "Передавач: 4 ІЧ-LED — роз'єм E (XH-3)")
    for i in range(4):
        x = 370 + i * 70
        s.vcc(x, 140, "+5V", C5)
        s.res_v(x, 150, 200, "68 Ом" if i == 0 else "", side="right")
        s.diode_v(x, 200, 250, "", cathode_top=False, led=True)
        s.line(x, 250, x, 270, INK, 2.2)
    s.wire([(370, 270), (580, 270)], INK, 2.2)
    for i in range(4):
        s.dot(370 + i * 70, 270)
    s.wire([(580, 270), (620, 270), (620, 300)], INK, 2.2)
    b, c, e = s.npn(590, 300, "BC337")
    s.wire([e, (e[0], 372)], CGND)
    s.gnd(e[0], 382)
    s.net(346, 330, "D3", CSIG)
    s.wire([(372, 330), (420, 330)], CSIG)
    s.res_h(420, 520, 330, "1 кОм", CSIG)
    s.wire([(520, 330), b], CSIG)
    s.text(346, 420, "Струм LED ~50 мА імпульсами. Якщо промінь не\nперекривається сірником — збільште 68 Ом → 220 Ом → 1 кОм.",
           size=11, color=MUTED)

    # --- приймачі ---
    s.box(750, 90, 430, 360, "Приймачі TSSP4038 ×4 — роз'єм B (XH-6)")
    s.rect(900, 150, 90, 110, fill="#2b2b2b", stroke="#111", rx=8)
    s.circle(945, 185, 22, fill="#4a4a4a", stroke="#111")
    s.text(945, 245, "TSSP4038", size=11, anchor="middle", color="#fff")
    for i, (lab, y) in enumerate((("OUT", 285), ("GND", 305), ("VS", 325))):
        x = 920 + i * 25
        s.line(x, 260, x, y, INK, 2.2)
        s.text(x, y + 16, lab, size=10, anchor="middle", color=MUTED)
    # OUT
    s.wire([(920, 285), (820, 285)], CSIG)
    s.text(812, 290, "→ D4", size=14, anchor="end", weight="bold", color=CSIG)
    # GND
    s.wire([(945, 305), (945, 322)], CGND)
    s.gnd(945, 330)
    # VS через 100 Ом + 100 нФ
    s.wire([(970, 325), (1040, 325)], C5)
    s.dot(1040, 325, C5)
    s.res_v(1040, 250, 325, "100 Ом", side="right")
    s.vcc(1040, 240, "+5V", C5)
    s.wire([(1040, 325), (1040, 350)], INK, 2.2)
    s.cap_v(1040, 350, 390, "100 нФ", polar=False, side="right")
    s.gnd(1040, 402)
    s.text(770, 430, "Вивід 1 — OUT, 2 — GND, 3 — VS (дивлячись на лінзу).", size=11, color=MUTED)

    # --- бузер + примітки ---
    s.box(24, 470, 1156, 110, "Важливо")
    s.text(40, 520, "• Бузер на D11 — АКТИВНИЙ (5 В): Timer2 зайнятий несучою, tone() недоступний.", size=13)
    s.text(40, 544, "• Трубки з термоусадки на КОЖЕН світлодіод і КОЖЕН приймач обов'язкові: приймачі бачать усі світлодіоди,", size=13)
    s.text(40, 566, "   і без трубок перекритий промінь \"підсвітять\" сусідні.", size=13)
    s.save("flyclap-tssp.svg")


# =====================================================================
#                   FlySonar — Arduino-версія
# =====================================================================
def flysonar_arduino_wiring():
    s = Svg(1200, 900, "FlySonar — Arduino-версія: схема підключення")
    p12 = power_block(s, 24, 60, "5–6 В ≥2 А", "1000 мкФ", "V_servo → серви", CSV)
    s.net(180, 100, "+12V → помпа", C12, size=12)

    right = ["D0", "D2", "D3", "D4", "D5", "D9", "D10", "D13", "A0"]
    left = [None, "VIN", None, "5V", None, "GND"]
    pins, h = s.chip(360, 140, 140, "Arduino Nano", left, right, pitch=36, sub="(або Uno)")
    vx, vy = pins["VIN"]
    s.wire([p12, (290, vy), (vx, vy)], C12)
    x5, y5 = pins["5V"]
    s.wire([(x5, y5), (310, y5)], C5)
    s.net(304, y5, "+5V", C5, anchor="end")
    xg, yg = pins["GND"]
    s.wire([(xg, yg), (336, yg), (336, yg + 16)], CGND)
    s.gnd(336, yg + 26)
    notes = {"D0": ("← ESP32 IO14 (опц.)", MUTED), "D2": ("→ TRIG", CSIG), "D3": ("← ECHO", CSIG),
             "D4": ("← кнопка RECAL", CSIG), "D5": ("→ затвор MOSFET", CSIG),
             "D9": ("→ серво PAN", CSIG), "D10": ("→ серво TILT", CSIG),
             "D13": ("вбудований LED", MUTED), "A0": ("← тумблер ARM", CSIG)}
    tags = {"D0": "L·3", "D2": "S1·3", "D3": "S1·4", "D4": "S2·3", "D5": "P1", "D9": "servo", "D10": "servo"}
    for p, (n, col) in notes.items():
        px, py = pins[p]
        s.wire([(px, py), (px + 12, py)], col, dash="5,4" if p == "D0" else None)
        tx = px + 16
        if p in tags:
            tx += s.tag(px + 14, py, tags[p]) + 2
        s.text(tx, py + 5, n, size=12, color=col)

    # --- HC-SR04 ---
    s.box(700, 60, 480, 230, "Сонар HC-SR04 — роз'єм S1 (XH-4)")
    s.rect(760, 110, 200, 90, fill="#1b5e9e", stroke="#0e3a63", rx=6)
    for cx in (805, 915):
        s.circle(cx, 150, 30, fill="#c9ced6", stroke="#6b7480", sw=2)
        s.circle(cx, 150, 20, fill="#9aa3ae", stroke="#6b7480")
    s.text(860, 196, "HC-SR04", size=11, anchor="middle", color="#fff", weight="bold")
    labs = ["VCC", "TRIG", "ECHO", "GND"]
    for i, lab in enumerate(labs):
        x = 800 + i * 40
        s.line(x, 200, x, 225, INK, 2.2)
        s.text(x, 240, lab, size=10, anchor="middle", color=MUTED)
    s.wire([(800, 225), (800, 250)], C5)
    s.net(800, 262, "+5V", C5, anchor="middle", size=12)
    s.wire([(840, 225), (840, 250)], CSIG)
    s.net(840, 262, "D2", CSIG, anchor="middle", size=12)
    s.wire([(880, 225), (880, 250)], CSIG)
    s.net(880, 262, "D3", CSIG, anchor="middle", size=12)
    s.wire([(920, 225), (920, 250)], CGND)
    s.gnd(920, 258)
    s.cap_v(1130, 120, 170, "100 нФ", polar=False, side="left")
    s.text(1130, 110, "VCC", size=10, anchor="middle", color=MUTED)
    s.gnd(1130, 182)

    # --- помпа ---
    s.box(700, 305, 480, 230, "Помпа R385 (або клапан 12 В) — роз'єм P1 (XH-2)")
    s.net(716, 470, "D5", CSIG)
    s.wire([(740, 470), (752, 470)], CSIG)
    s.res_h(752, 812, 470, "100 Ом", CSIG)
    s.dot(830, 470)
    s.wire([(812, 470), (850, 470)], CSIG)
    s.res_v(830, 470, 505, "10 кОм", side="left")
    s.gnd(830, 517)
    g, d, sr = s.mosfet(850, 440, "IRLZ44N")
    s.wire([sr, (sr[0], 510)], CGND)
    s.gnd(sr[0], 518)
    s.wire([d, (d[0], 428), (1000, 428)], INK, 2.2)
    s.vcc(1000, 360, "+12V", C12)
    s.dot(1000, 376)
    s.line(1000, 370, 1000, 386, INK, 2.2)
    s.motor(1000, 402, "")
    s.text(972, 407, "помпа", size=12, anchor="end")
    s.line(1000, 418, 1000, 428, INK, 2.2)
    s.dot(1000, 428)
    s.wire([(1000, 376), (1070, 376)], INK, 2.2)
    s.diode_v(1070, 376, 428, "1N5819", cathode_top=True)
    s.wire([(1070, 428), (1000, 428)], INK, 2.2)

    # --- серви, кнопки ---
    s.box(700, 550, 480, 330, "Серви (3-pin), кнопка RECAL (S2), ARM (на основі)")
    for i, (pin, name) in enumerate((("D9", "PAN"), ("D10", "TILT"))):
        y = 590 + i * 90
        sig, vp, gg = s.servo(900, y, name, sub="MG90S")
        s.net(716, y + 14, pin, CSIG)
        s.wire([(756, y + 14), sig], CSIG)
        s.wire([vp, (850, y + 28)], CSV)
        s.net(846, y + 28, "V_servo", CSV, anchor="end", size=11)
        s.wire([gg, (870, y + 42), (870, y + 50)], CGND)
        s.gnd(870, y + 58)
    for pin, y, lab in (("D4", 800, "кнопка RECAL (перекалібрувати фон)"),
                        ("A0", 840, "тумблер ARM (LOW = стріляти дозволено)")):
        s.net(716, y, pin, CSIG)
        s.wire([(752, y), (790, y)], CSIG)
        s.switch_h(790, 830, y)
        s.wire([(833, y), (850, y)], CGND)
        s.gnd(850, y + 10)
        s.text(880, y + 5, lab, size=12)

    s.legend(24, 540, [(C12, "+12 В (БЖ): помпа / клапан, VIN Nano, вхід DC-DC"),
                       (C5, "+5 В логіка (пін 5V Nano): HC-SR04"),
                       (CSV, "V_servo 5–6 В (DC-DC): лише серви"),
                       (CSIG, "сигнали"),
                       (CGND, "земля (⏚) — спільна для всього")],
             notes=("Серви й помпу НЕ живити від піна 5V Nano — лише від DC-DC / БЖ.",
                    "D0 — лише для режиму \"очі\" (ESP32 шле кути). На час прошивки Nano",
                    "від'єднувати. Клапан 12 В підключається замість помпи тим самим ключем.",
                    "Електроніку — вище рівня води й за перегородкою."),
             width=660)
    s.save("flysonar-arduino-wiring.svg")


# =====================================================================
#                   FlySonar — ESP32-версія
# =====================================================================
def flysonar_esp32_wiring():
    s = Svg(1200, 940, "FlySonar — ESP32-версія (ESP32-CAM AI-Thinker): схема підключення")
    power_block(s, 24, 60, "5 В ≥3 А", "1000 мкФ", "+5V → ESP32, серви", C5)
    s.net(180, 100, "+12V → клапан", C12, size=12)

    left = ["5V", "GND", "IO12", "IO13", "IO15", "IO14", "IO2", "IO4"]
    right = ["3V3", "IO16", "IO0", "GND", "VCC", "U0R", "U0T", "GND"]
    pins, h = s.chip(390, 80, 170, "ESP32-CAM", left, right, pitch=36, top=96, sub="AI-Thinker, OV2640", fill="#1c1c1c")
    s.circle(475, 144, 16, fill="#333", stroke="#666")
    s.circle(475, 144, 8, fill="#223a5e", stroke="#8aa")
    # живлення
    x5, y5 = pins["5V"]
    s.wire([(x5, y5), (350, y5)], C5)
    s.net(344, y5, "+5V", C5, anchor="end")
    xg, yg = pins["GND"]
    s.wire([(xg, yg), (300, yg), (300, yg + 14)], CGND)
    s.gnd(300, yg + 24)
    # сигнали ліворуч
    for p, lab in (("IO12", "ARM"), ("IO13", "клапан"), ("IO15", "TILT"), ("IO14", "PAN")):
        px, py = pins[p]
        s.wire([(px, py), (px - 20, py)], CSIG)
        s.text(px - 24, py + 5, lab, size=12, anchor="end", color=CSIG, weight="bold")
    # праворуч — програматор
    for p in ("U0R", "U0T", "IO0"):
        px, py = pins[p]
        s.wire([(px, py), (px + 14, py)], MUTED, dash="4,3")
    s.text(pins["IO0"][0] + 20, pins["IO0"][1] + 5, "програматор", size=11, color=MUTED)
    s.text(pins["U0R"][0] + 20, pins["U0R"][1] + 5, "ESP32-CAM-MB", size=11, color=MUTED)
    s.text(pins["U0T"][0] + 20, pins["U0T"][1] + 5, "(вставляється)", size=11, color=MUTED)
    s.text(pins["IO16"][0] + 20, pins["IO16"][1] + 5, "PSRAM — не чіпати", size=11, color=MUTED)
    s.text(pins["3V3"][0] + 20, pins["3V3"][1] + 5, "не підключати", size=11, color=MUTED)

    # --- клапан ---
    s.box(700, 60, 480, 250, "Клапан 12 В (NC, прямої дії) — роз'єм P1 (XH-2)")
    s.net(716, 225, "IO13", CSIG)
    s.wire([(756, 225), (762, 225)], CSIG)
    s.res_h(762, 822, 225, "100 Ом", CSIG)
    s.dot(840, 225)
    s.wire([(822, 225), (860, 225)], CSIG)
    s.res_v(840, 225, 262, "10 кОм", side="left")
    s.gnd(840, 274)
    g, d, sr = s.mosfet(860, 195, "AO3400")
    s.text(898, 270, "(логіка 3,3 В)", size=10, color=MUTED)
    s.wire([sr, (sr[0], 272)], CGND)
    s.gnd(sr[0], 280)
    s.wire([d, (d[0], 182), (1010, 182)], INK, 2.2)
    s.vcc(1010, 118, "+12V", C12)
    s.dot(1010, 134)
    s.coil_v(1010, 128, 182, "клапан", side="left")
    s.dot(1010, 182)
    s.wire([(1010, 134), (1070, 134)], INK, 2.2)
    s.diode_v(1070, 134, 182, "1N5819", cathode_top=True)
    s.wire([(1070, 182), (1010, 182)], INK, 2.2)

    # --- серви ---
    s.box(700, 325, 480, 220, "Серви (живлення від DC-DC 5 В) — servo 3-pin")
    for i, (pin, name) in enumerate((("IO14", "PAN"), ("IO15", "TILT"))):
        y = 365 + i * 85
        sig, vp, gg = s.servo(930, y, name)
        s.net(716, y + 14, pin, CSIG)
        s.wire([(760, y + 14), sig], CSIG)
        s.wire([vp, (880, y + 28)], C5)
        s.net(876, y + 28, "+5V", C5, anchor="end", size=11)
        s.wire([gg, (900, y + 42), (900, y + 50)], CGND)
        s.gnd(900, y + 58)
    s.text(716, 530, "Цифрові мікросерво (ES08MD II) або MG90S.", size=11, color=MUTED)

    # --- ARM ---
    s.box(700, 560, 480, 120, "Тумблер ARM і статус")
    s.net(716, 606, "IO12", CSIG)
    s.wire([(756, 606), (790, 606)], CSIG)
    s.switch_h(790, 830, 606)
    s.wire([(833, 606), (850, 606)], CGND)
    s.gnd(850, 616)
    s.text(880, 604, "лише на GND, БЕЗ зовнішньої", size=12)
    s.text(880, 622, "підтяжки (strapping-пін)", size=12)
    s.text(716, 660, "Статус-LED (GPIO33): горить = ARM, коротко мигає = SAFE.", size=12, color=MUTED)

    # --- панель + Freenove ---
    s.box(700, 695, 480, 225, "Інше")
    s.rect(716, 735, 90, 60, fill="#fffbe6", stroke="#c9b458", rx=4)
    s.text(761, 770, "LED A4", size=11, anchor="middle", weight="bold")
    s.wire([(806, 765), (850, 765)], C5)
    s.text(858, 770, "USB 5 В (від того ж DC-DC або зарядки)", size=12)
    s.text(716, 820, "Freenove ESP32-S3 CAM: PAN → GPIO1, TILT → GPIO14,", size=12)
    s.text(716, 840, "клапан → GPIO21, ARM → GPIO47, LED → GPIO2 (вбудований).", size=12)
    s.text(716, 868, "Роль \"очі\" для Arduino: GPIO14 → D0 Arduino, GND ↔ GND.", size=12, color=MUTED)
    s.text(716, 888, "(тоді серви й клапан підключаються до Arduino)", size=12, color=MUTED)

    s.legend(24, 560, [(C12, "+12 В (БЖ): лише клапан і вхід DC-DC"),
                       (C5, "+5 В (DC-DC ≥3 А): ESP32-CAM, серви, LED-панель"),
                       (CSIG, "сигнали 3,3 В"),
                       (CGND, "земля (⏚) — спільна для всього")],
             notes=("ESP32-CAM перезавантажується від просадок, коли рушають серви:",
                    "DC-DC ≥3 А і 1000 мкФ біля серв — обов'язково.",
                    "GPIO13/14/15 — лінії SD: картку не вставляти.",
                    "IRLZ44N від 3,3 В відкривається не повністю — беріть AO3400",
                    "або готовий модуль-ключ «3.3V logic».",
                    "Електроніку — вище рівня води й за перегородкою."),
             width=660)
    s.save("flysonar-esp32-wiring.svg")



# =====================================================================
#                   Ілюстрації: як виглядають пристрої
# =====================================================================
def callout(s, tx, ty, lx, ly, text, anchor="start"):
    """Виноска: крапка на деталі (tx,ty) → лінія → підпис біля (lx,ly)."""
    s.line(tx, ty, lx, ly, "#6b7480", 1.3)
    s.circle(tx, ty, 3.5, fill="#6b7480", stroke="#6b7480", sw=1)
    s.text(lx + (6 if anchor == "start" else -6), ly + 5, text, size=13, anchor=anchor)


def fly(s, x, y, k=1.0, angle=0):
    """Муха: тіло, голова, крила."""
    g = f'<g transform="translate({x},{y}) rotate({angle}) scale({k})">'
    g += '<ellipse cx="-8" cy="-6" rx="9" ry="5" fill="#cfe3f5" opacity="0.85" stroke="#7f98b0" stroke-width="0.8" transform="rotate(-25 -8 -6)"/>'
    g += '<ellipse cx="8" cy="-6" rx="9" ry="5" fill="#cfe3f5" opacity="0.85" stroke="#7f98b0" stroke-width="0.8" transform="rotate(25 8 -6)"/>'
    g += '<ellipse cx="0" cy="0" rx="5" ry="8" fill="#2b2b2b"/>'
    g += '<circle cx="0" cy="-9" r="3.6" fill="#3a2020"/>'
    g += '<circle cx="-1.6" cy="-10" r="1.3" fill="#b03030"/><circle cx="1.6" cy="-10" r="1.3" fill="#b03030"/>'
    g += '</g>'
    s.add(g)


def midge(s, x, y):
    s.add(f'<g transform="translate({x},{y})"><ellipse cx="-3" cy="-2" rx="3.5" ry="1.8" fill="#bcd3ea" opacity="0.8"/>'
          f'<ellipse cx="3" cy="-2" rx="3.5" ry="1.8" fill="#bcd3ea" opacity="0.8"/>'
          f'<ellipse cx="0" cy="0" rx="1.6" ry="3" fill="#1b1b1b"/></g>')


def mesh(s, x, y, w, h, step=10):
    s.rect(x, y, w, h, fill="#d9dde2", stroke="#8a929b", sw=1.4)
    for xx in range(x + step, x + w, step):
        s.line(xx, y, xx, y + h, "#8a929b", 1)
    for yy in range(y + step, y + h, step):
        s.line(x, yy, x + w, yy, "#8a929b", 1)


def servo_side(s, x, y, w=70, h=40, label=None):
    s.rect(x, y, w, h, fill="#2f3b4a", stroke="#1d2530", rx=5)
    s.rect(x - 8, y + 8, w + 16, 6, fill="#2f3b4a", stroke="#1d2530", rx=2)
    s.circle(x + w - 16, y - 4, 7, fill="#dfe6ee", stroke="#1d2530")
    if label:
        s.text(x + w / 2, y + h - 10, label, size=10, anchor="middle", color="#dfe6ee")


def flyclap_device():
    s = Svg(1200, 1060, "FlyClap — як виглядає пристрій")
    s.text(24, 64, "Розріз спереду (передня стінка знята). Пропорції умовні; пластини 15×15 см.", size=13, color=MUTED)
    X = 150  # зсув головного малюнка

    s.rect(X + 10, 680, 780, 14, fill="#b08958", stroke="#8a6a42", rx=3)          # стіл
    s.rect(X + 110, 190, 540, 490, fill="#e7d0a6", stroke="#9b7440", sw=2, rx=6)  # корпус
    s.rect(X + 132, 212, 496, 446, fill="#fbf7ef", stroke="#c9ab7a", sw=1.2)
    mesh(s, X + 110, 170, 540, 22)
    # приманка
    s.path(f"M{X + 340},648 L{X + 420},648 L{X + 412},664 L{X + 348},664 Z", fill="#f2c57c", stroke="#b0843d")
    s.add(f'<ellipse cx="{X + 380}" cy="648" rx="40" ry="6" fill="#d98c2b" opacity="0.85"/>')
    # пластини (взведено — розведені V) на спільній петлі
    hx, hy = X + 380, 610
    for sg in (-1, 1):
        s.add(f'<polygon points="{hx + 6 * sg},{hy} {hx + 222 * sg},{hy - 262} {hx + 208 * sg},{hy - 272} {hx - 4 * sg},{hy - 8}" '
              f'fill="#bfe0ff" fill-opacity="0.55" stroke="#4d8fcc" stroke-width="2"/>')
    s.circle(hx, hy, 10, fill="#9aa3ae", stroke="#555")
    s.path(f"M{hx - 16},{hy + 14} q16,14 32,0", stroke="#777", sw=3)
    # ІЧ-завіса (промені йдуть від передньої стінки до задньої — видно як точки)
    for bx in (305, 350, 410, 455):
        s.add(f'<circle cx="{X + bx}" cy="430" r="20" fill="#ff4d4d" opacity="0.18"/>')
        s.circle(X + bx, 430, 11, fill="#ff4d4d", stroke="none")
    s.line(X + 270, 430, X + 490, 430, "#ff4d4d", 1.2, dash="3,5")
    # засувка + соленоїд
    s.rect(X + 46, 262, 64, 34, fill="#6d6d6d", stroke="#333", rx=4)
    s.text(X + 78, 284, "SOL", size=11, anchor="middle", color="#fff", weight="bold")
    s.rect(X + 110, 274, 48, 8, fill="#8a8a8a", stroke="#333")
    s.path(f"M{X + 158},272 l10,0 l0,14 l-10,0", stroke="#333", sw=3)
    s.path(f"M{X + 168},278 Q{X + 380},236 {X + 590},340", stroke="#7a5a2a", sw=1.6, extra='stroke-dasharray="6,4"')
    s.rect(X + 140, 300, 22, 14, fill="#1f1f1f", stroke="#000", rx=2)          # кінцевик
    # серво-важіль
    servo_side(s, X + 548, 560, 62, 36, "серво")
    s.line(X + 585, 556, X + 540, 470, "#dfe6ee", 6)
    s.line(X + 585, 556, X + 540, 470, "#1d2530", 1.5)
    # електроніка збоку
    ex = X + 662
    s.rect(ex, 420, 106, 150, fill="#3a4250", stroke="#222", rx=8)
    s.text(ex + 53, 446, "Nano", size=12, anchor="middle", color="#fff", weight="bold")
    s.text(ex + 53, 462, "MOSFET", size=10, anchor="middle", color="#cfd8e3")
    s.circle(ex + 28, 492, 6, fill="#7CFC00", stroke="#333")
    s.rect(ex + 54, 484, 30, 16, fill="#bbb", stroke="#333", rx=3)
    s.line(ex + 69, 492, ex + 78, 478, "#333", 3)
    s.circle(ex + 53, 530, 10, fill="#222", stroke="#555")
    s.path(f"M{ex + 106},560 q30,40 0,110", stroke="#333", sw=3)
    s.text(ex + 120, 712, "до БЖ 12 В", size=12, color=MUTED)
    # мухи
    fly(s, X + 480, 118, 1.3, 15)
    s.path(f"M{X + 476},138 C{X + 470},220 {X + 420},320 {X + 400},410", stroke="#555", sw=1.4, extra='stroke-dasharray="4,5"')
    fly(s, X + 362, 632, 0.8, 20)

    L, R = 200, 940   # колонки підписів
    callout(s, X + 78, 262, L, 214, "соленоїд вибиває", "end")
    s.text(L - 6, 236, "засувку", size=13, anchor="end")
    callout(s, X + 151, 306, L, 330, "кінцевик", "end")
    s.text(L - 6, 352, "«взведено»", size=13, anchor="end")
    callout(s, X + 250, 450, L, 460, "пластини", "end")
    s.text(L - 6, 482, "(оргскло 2 мм)", size=13, anchor="end")
    callout(s, X + 380, 656, L, 630, "приманка", "end")
    s.text(L - 6, 652, "(сироп / оцет)", size=13, anchor="end")
    callout(s, X + 600, 181, R, 150, "сітка ~8 мм: мухи")
    s.text(R + 6, 177, "пролазять, пальці — ні", size=13)
    callout(s, X + 380, 257, R, 230, "тяга до правої пластини")
    callout(s, X + 455, 430, R, 330, "ІЧ-завіса: 4 промені")
    s.text(R + 6, 357, "(від передньої стінки до задньої)", size=13)
    callout(s, ex + 28, 492, R, 440, "LED статусу")
    callout(s, ex + 72, 488, R, 480, "тумблер ARM")
    callout(s, ex + 53, 530, R, 520, "бузер")
    callout(s, X + 579, 590, R, 600, "серво взводить")
    callout(s, hx + 8, hy + 6, R, 650, "петля + пружина стягує пластини")

    # --- знизу: спрацювало + вид зверху ---
    s.box(40, 730, 540, 310, "Після спрацювання (~25 мс)")
    s.rect(170, 772, 280, 230, fill="#fbf7ef", stroke="#c9ab7a", rx=4)
    s.add('<polygon points="302,982 300,792 312,790 314,982" fill="#bfe0ff" fill-opacity="0.7" stroke="#4d8fcc" stroke-width="2"/>')
    s.add('<polygon points="314,982 316,792 326,792 326,982" fill="#bfe0ff" fill-opacity="0.7" stroke="#4d8fcc" stroke-width="2"/>')
    s.circle(312, 986, 8, fill="#9aa3ae", stroke="#555")
    s.add('<ellipse cx="313" cy="880" rx="3" ry="7" fill="#2b2b2b"/>')
    s.text(470, 860, "пластини\nсхлопнулись,\nмуха між ними", size=12, color=MUTED)
    s.text(470, 940, "далі серво\nзнову взводить", size=12, color=MUTED)

    s.box(620, 730, 540, 310, "Вид зверху: ІЧ-завіса")
    s.rect(700, 790, 380, 210, fill="#fbf7ef", stroke="#c9ab7a", rx=4)
    s.text(890, 784, "передня стінка", size=11, anchor="middle", color=MUTED)
    s.text(890, 1018, "задня стінка", size=11, anchor="middle", color=MUTED)
    for bx in (820, 865, 915, 960):
        s.rect(bx - 6, 792, 12, 10, fill="#c0392b", stroke="#7a1f16")
        s.rect(bx - 6, 988, 12, 10, fill="#222", stroke="#000")
        s.line(bx, 802, bx, 988, "#ff4d4d", 2, dash="6,4")
    s.line(740, 810, 740, 980, "#4d8fcc", 5)
    s.line(1040, 810, 1040, 980, "#4d8fcc", 5)
    s.text(690, 812, "ІЧ-LED ↓", size=11, anchor="end", color="#c0392b")
    s.text(690, 996, "фото-\nтранзистори ↑", size=11, anchor="end", color=INK)
    s.text(1050, 900, "пластина", size=11, color="#4d8fcc")
    s.text(730, 900, "пластина", size=11, anchor="end", color="#4d8fcc")
    s.save("flyclap-device.svg")


def turret(s, x, y, head="sonar"):
    """Турель збоку, дивиться праворуч. (x,y) — центр основи на столі.
    Повертає точку сопла і точку "очей"."""
    s.rect(x - 70, y - 40, 140, 40, fill="#4a5563", stroke="#2c333d", rx=6)            # основа
    servo_side(s, x - 30, y - 80, 60, 36, "PAN")
    s.rect(x - 6, y - 100, 12, 20, fill="#9aa3ae", stroke="#555")                        # вісь
    s.path(f"M{x - 40},{y - 100} L{x + 40},{y - 100} L{x + 40},{y - 170} L{x + 30},{y - 170} "
           f"L{x + 30},{y - 110} L{x - 30},{y - 110} L{x - 30},{y - 170} L{x - 40},{y - 170} Z",
           fill="#c0c7cf", stroke="#6b7480")                                           # U-кронштейн
    servo_side(s, x - 26, y - 190, 52, 30, "TILT")
    # голова (нахилена на ціль)
    hx, hy = x + 10, y - 205
    if head == "sonar":
        s.rect(hx - 10, hy - 40, 12, 64, fill="#1b5e9e", stroke="#0e3a63", rx=2)       # плата HC-SR04
        s.rect(hx + 2, hy - 34, 26, 22, fill="#c9ced6", stroke="#6b7480", rx=3)         # "око"
        s.circle(hx + 28, hy - 23, 3, fill="#6b7480", stroke="#6b7480")
        eye = (hx + 30, hy - 23)
    else:
        eye = None
    s.rect(hx - 10, hy + 26, 46, 8, fill="#8a8a8a", stroke="#444", rx=2)               # сопло-трубка
    s.path(f"M{hx + 36},{hy + 27} l14,3 l-14,3 Z", fill="#b08d57", stroke="#6b5530")   # латунне сопло
    return (hx + 50, hy + 30), eye


def flysonar_arduino_device():
    s = Svg(1200, 760, "FlySonar — Arduino-версія: як виглядає пристрій")
    s.text(24, 64, "Вигляд збоку. Турель сторожить зону на 10–40 см: миску з фруктами, приманку, підвіконня.", size=13, color=MUTED)
    s.rect(20, 640, 1160, 14, fill="#b08958", stroke="#8a6a42", rx=3)                    # стіл

    # резервуар з помпою (ліворуч, нижче електроніки)
    s.rect(60, 500, 110, 140, fill="#e6f3ff", stroke="#6f9cc4", sw=2, rx=10)
    s.rect(62, 540, 106, 98, fill="#9fd0ff", stroke="none", rx=8)
    s.rect(95, 585, 40, 40, fill="#333", stroke="#111", rx=6)
    s.text(115, 610, "M", size=13, anchor="middle", color="#fff", weight="bold")
    # електроніка на полиці вище води
    s.rect(40, 330, 200, 16, fill="#b08958", stroke="#8a6a42")
    s.line(60, 346, 60, 640, "#8a6a42", 6)
    s.line(220, 346, 220, 640, "#8a6a42", 6)
    s.rect(60, 250, 160, 80, fill="#3a4250", stroke="#222", rx=8)
    s.text(140, 276, "Arduino Nano", size=12, anchor="middle", color="#fff", weight="bold")
    s.text(140, 292, "DC-DC, MOSFET", size=10, anchor="middle", color="#cfd8e3")
    s.circle(86, 312, 6, fill="#7CFC00", stroke="#333")
    s.rect(110, 304, 30, 16, fill="#bbb", stroke="#333", rx=3)
    s.line(125, 312, 134, 298, "#333", 3)
    s.circle(170, 312, 7, fill="#d33", stroke="#333")
    s.rect(236, 200, 8, 150, fill="#9fb3c8", stroke="#6b7480")                       # перегородка
    # турель
    nozzle, eye = turret(s, 420, 640)
    # шланг: помпа → петля → сопло
    s.path(f"M115,500 C115,430 300,470 330,560 C350,620 300,600 320,520 C340,440 372,470 {nozzle[0] - 40},{nozzle[1] + 2}",
           stroke="#7fb3e0", sw=5)
    # дроти до серв/сенсора
    s.path("M220,300 C300,300 360,380 402,452", stroke="#333", sw=2, extra='stroke-dasharray="7,5"')
    # миска з фруктами і муха
    s.path("M840,560 Q930,660 1020,560 Z", fill="#e9e1d3", stroke="#8a7a60", sw=2)
    for cx, cy, r, col in ((880, 548, 26, "#e74c3c"), (930, 540, 30, "#f1c40f"), (982, 550, 24, "#27ae60")):
        s.circle(cx, cy, r, fill=col, stroke="#7a5a2a", sw=1.2)
    fly(s, 930, 505, 1.2, 10)
    # ультразвук: конус + дуги
    ex, ey = eye
    tx, ty = 930, 505
    s.add(f'<polygon points="{ex},{ey} {tx},{ty - 60} {tx},{ty + 60}" fill="#9b59b6" opacity="0.08"/>')
    for k in range(1, 7):
        px = ex + (tx - ex) * k / 7
        py = ey + (ty - ey) * k / 7
        rr = 8 + 60 * k / 7
        s.path(f"M{px},{py - rr} Q{px + rr * 0.35},{py} {px},{py + rr}", stroke="#9b59b6", sw=1.6)
    # струмінь
    nx, ny = nozzle
    s.path(f"M{nx},{ny} Q{(nx + tx) / 2},{ny - 120} {tx - 6},{ty + 4}", stroke=CWATER, sw=3, extra='stroke-dasharray="10,6"')
    for dx, dy in ((0, 0), (10, -8), (-8, -10), (6, 10)):
        s.circle(tx - 6 + dx, ty + 4 + dy, 3, fill=CWATER, stroke="none")

    L, R = 40, 1040
    callout(s, ex - 10, ey, 470, 150, "сонар HC-SR04 (\"очі\")")
    callout(s, nx - 6, ny, 560, 200, "сопло під сонаром (голка ∅1 мм)")
    callout(s, 405, 450, 600, 260, "серво TILT на U-кронштейні")
    callout(s, 425, 580, 600, 330, "серво PAN (обертає турель)")
    callout(s, 322, 540, 330, 700, "шланг 4 мм з петлею (не тягне серви)")
    callout(s, 115, 605, 40, 700, "резервуар + помпа R385")
    callout(s, 140, 262, 40, 120, "електроніка (Nano, DC-DC, MOSFET) — вище рівня води")
    callout(s, 240, 230, 280, 200, "перегородка від бризок")
    callout(s, 125, 306, 40, 170, "тумблер ARM, LED, кнопка RECAL")
    callout(s, 700, 470, 560, 600, "ультразвук 40 кГц (пелюстка ~15°)")
    callout(s, 780, 470, 820, 380, "струмінь води")
    callout(s, 930, 500, 1000, 440, "муха сидить → постріл")
    s.save("flysonar-arduino-device.svg")


def flysonar_esp32_device():
    s = Svg(1200, 800, "FlySonar — ESP32-версія: як виглядає пристрій")
    s.text(24, 64, "Вигляд збоку. Камера дивиться на підсвічену панель; комахи на ній — темні точки. Одна плата ESP32-CAM керує всім.",
           size=13, color=MUTED)
    s.add('<defs><linearGradient id="glow" x1="0" x2="1"><stop offset="0" stop-color="#fffef2"/>'
          '<stop offset="1" stop-color="#fff3b0"/></linearGradient></defs>')
    s.rect(20, 660, 1160, 14, fill="#b08958", stroke="#8a6a42", rx=3)                    # стіл

    # LED-панель A4 (у перспективі) на підставці
    s.add('<polygon points="860,190 1080,150 1080,600 860,620" fill="url(#glow)" stroke="#c9b458" stroke-width="3"/>')
    s.add('<polygon points="852,196 866,193 866,626 852,628" fill="#d6d1b5" stroke="#9c9670"/>')
    s.add('<polygon points="870,200 1072,164 1072,590 870,608" fill="#ffffff" opacity="0.35" stroke="#b9c7d6" stroke-dasharray="6,4"/>')
    s.path("M900,630 L930,660 M1050,610 L1020,660", stroke="#8a7a60", sw=6)
    for mx, my in ((930, 300), (1000, 270), (1030, 380), (910, 510)):
        midge(s, mx, my)
    fly(s, 1010, 490, 0.9, -10)
    # приманка біля панелі
    s.path("M900,640 L960,640 L954,656 L906,656 Z", fill="#f2c57c", stroke="#b0843d")
    # бак-обприскувач під тиском
    s.rect(60, 430, 120, 230, fill="#eaf4ea", stroke="#5b8f5b", sw=2, rx=26)
    s.rect(64, 500, 112, 156, fill="#9fd0ff", stroke="none", rx=22)
    s.rect(110, 390, 20, 44, fill="#5b8f5b", stroke="#3d6b3d")
    s.rect(90, 380, 60, 12, fill="#3d6b3d", stroke="#2a4a2a", rx=4)
    s.text(120, 470, "1,5–2 л", size=11, anchor="middle", color="#2a4a2a")
    s.text(120, 486, "до 3 бар", size=11, anchor="middle", color="#2a4a2a")
    # клапан біля основи турелі
    s.rect(250, 590, 50, 40, fill="#b0b8c2", stroke="#555", rx=4)
    s.rect(262, 570, 26, 22, fill="#333", stroke="#111", rx=3)
    s.path("M180,620 L250,610", stroke="#7fb3e0", sw=5)
    # турель
    nozzle, _ = turret(s, 440, 660, head="none")
    s.path(f"M300,606 C360,600 330,520 380,500 C420,480 360,470 {nozzle[0] - 44},{nozzle[1] + 2}", stroke="#7fb3e0", sw=5)
    # камера на кронштейні
    s.line(560, 660, 560, 380, "#6b7480", 6)
    s.line(560, 380, 600, 380, "#6b7480", 6)
    s.rect(600, 350, 40, 56, fill="#1c1c1c", stroke="#000", rx=4)
    s.circle(640, 378, 10, fill="#333", stroke="#777")
    s.circle(643, 378, 5, fill="#223a5e", stroke="#8aa")
    # поле зору камери
    s.add('<polygon points="646,378 866,196 866,622" fill="#1f6fd1" opacity="0.06"/>')
    s.line(646, 378, 866, 196, CSIG, 1.2, dash="5,5")
    s.line(646, 378, 866, 622, CSIG, 1.2, dash="5,5")
    # кабель до ESP32 від блока живлення
    s.rect(230, 250, 170, 76, fill="#3a4250", stroke="#222", rx=8)
    s.text(315, 276, "DC-DC 5 В, ключ", size=12, anchor="middle", color="#fff", weight="bold")
    s.text(315, 292, "клапана, ARM", size=11, anchor="middle", color="#cfd8e3")
    s.rect(290, 302, 30, 16, fill="#bbb", stroke="#333", rx=3)
    s.line(305, 310, 314, 296, "#333", 3)
    s.path("M400,300 C480,300 520,340 600,370", stroke="#333", sw=2, extra='stroke-dasharray="7,5"')
    s.path("M315,326 C315,400 360,430 424,462", stroke="#333", sw=2, extra='stroke-dasharray="7,5"')
    s.path("M280,326 C280,450 275,540 275,570", stroke="#333", sw=2, extra='stroke-dasharray="7,5"')
    # розпил на мошку
    nx, ny = nozzle
    tx, ty = 960, 420
    s.add(f'<polygon points="{nx},{ny} {tx - 4},{ty - 26} {tx + 4},{ty + 26}" fill="{CWATER}" opacity="0.22"/>')
    s.path(f"M{nx},{ny} Q{(nx + tx) / 2},{ny - 60} {tx},{ty}", stroke=CWATER, sw=2, extra='stroke-dasharray="8,6"')
    midge(s, tx, ty)
    # відстань
    s.line(646, 700, 860, 700, INK, 1.5)
    s.path("M646,700 l10,-5 l0,10 Z M860,700 l-10,-5 l0,10 Z", fill=INK, sw=1)
    s.text(753, 720, "~30 см", size=13, anchor="middle", weight="bold")

    callout(s, 620, 360, 600, 150, "ESP32-CAM: камера + мозок")
    callout(s, 760, 290, 720, 200, "поле зору камери")
    callout(s, 1076, 250, 1090, 110, "LED-панель A4 + плівка", "end")
    callout(s, 960, 420, 1100, 700, "мошка 1–3 мм = темна точка", "end")
    callout(s, 930, 648, 800, 750, "приманка біля панелі", "end")
    callout(s, 440, 600, 440, 745, "серво PAN / TILT (цифрові)")
    callout(s, nx - 6, ny, 470, 225, "латунне сопло-розпилювач")
    callout(s, 275, 590, 210, 780, "клапан 12 В (прямої дії)")
    callout(s, 120, 560, 40, 745, "бак-обприскувач під тиском")
    callout(s, 305, 300, 40, 180, "живлення, ключ клапана, тумблер ARM")
    s.save("flysonar-esp32-device.svg")


# =====================================================================
#                  Основа і модулі: роз'єми JST-XH
# =====================================================================
def connector(s, x, y, pins, pitch=64):
    """Роз'єм, вигляд з боку штекера: контакти з номерами й призначенням.
    pins: [(назва, колір)]. Контакт 1 — ліворуч, позначений трикутником."""
    n = len(pins)
    w = pitch * n + 12
    s.rect(x, y, w, 46, fill="#fbfaf5", stroke="#8c8672", sw=1.8, rx=5)
    s.rect(x + 8, y - 6, w - 16, 6, fill="#fbfaf5", stroke="#8c8672", sw=1.4)   # ключ колодки
    s.path(f"M{x + 6 + pitch / 2 - 6},{y + 56} l12,0 l-6,-8 Z", fill=INK, sw=1)  # контакт 1
    for i, (name, color) in enumerate(pins):
        cx = x + 6 + pitch / 2 + i * pitch
        s.rect(cx - 8, y + 15, 16, 16, fill=color, stroke="#555", sw=1)
        s.text(cx, y + 11, str(i + 1), size=10, anchor="middle", color=MUTED)
        s.text(cx, y + 74, name, size=12, anchor="middle", weight="bold", color=color if color != INK else INK)
    return w


def modules_connectors():
    s = Svg(1200, 1146, "Основа і модулі: роз'єми JST-XH (крок 2,5 мм)")
    s.text(24, 64, "Основа: плата, DC-DC 12 → 5 В, тумблер ARM, статус-LED, бузер, силові ключі. Модулі підключаються до неї "
                   "цими роз'ємами.", size=13, color=MUTED)
    s.text(24, 84, "Різна кількість контактів не дає вставити модуль не туди. Земля — завжди контакт 1 (▲), крім "
                   "силового виходу P.", size=13, color=MUTED)

    rows = [
        ("P", "силовий вихід", "XH-2", [("+12 В", C12), ("OUT", INK)],
         "Соленоїд, помпа, клапан — просто два дроти.\nMOSFET і діод 1N5819 стоять на основі.\nСоленоїд > 2 А — гвинтова клема замість XH-2."),
        ("L", "зв'язок між платами", "XH-3", [("GND", CGND), ("TX", CSIG), ("RX", CSIG)],
         "UART 115200: «очі» (ESP32) → турель (Arduino).\nКабель перехресний: TX ↔ RX.\nЖивлення не передає — спільна лише земля."),
        ("E", "випромінювачі завіси", "XH-3", [("GND", CGND), ("+5 В", C5), ("CARRIER", CSIG)],
         "ІЧ-світлодіоди завіси.\nCARRIER — несуча 38 кГц (D3), лише для TSSP4038;\nу варіанті з LM339 не використовується."),
        ("S", "сенсор / кнопка", "XH-4", [("GND", CGND), ("+5 В", C5), ("SIG1", CSIG), ("SIG2", CSIG)],
         "HC-SR04: SIG1 = TRIG, SIG2 = ECHO.\nКінцевик, кнопка RECAL: SIG1 на GND.\nНа ESP32 ECHO (5 В) — через дільник 1 кОм / 2 кОм."),
        ("B", "приймачі завіси", "XH-6",
         [("GND", CGND), ("+5 В", C5), ("B1", CSIG), ("B2", CSIG), ("B3", CSIG), ("B4", CSIG)],
         "Виходи LM339 або TSSP4038, чотири промені.\nПромінь цілий = LOW, перекритий = HIGH."),
        ("servo", "серво", "3-pin 2,54 мм", [("GND", CGND), ("+5 В", CSV), ("сигнал", CSIG)],
         "Стандартний роз'єм серви (коричневий, червоний,\nпомаранчевий). Живлення — від DC-DC 5–6 В ≥3 А,\nне від плати; поруч електроліт 470–1000 мкФ."),
    ]
    y = 112
    for code, name, kind, pins, desc in rows:
        s.box(24, y, 1152, 108)
        s.tag(42, y + 30, code)
        s.text(42, y + 62, name, size=14, weight="bold")
        s.text(42, y + 82, kind, size=12, color=MUTED)
        connector(s, 250, y + 18, pins)
        s.text(680, y + 38, desc, size=13)
        y += 118

    # ---- роз'єми типових збірок ----
    s.box(24, y + 4, 1152, 314, "Роз'єми типових збірок: що до якого піна")
    cols = [(42, "Роз'єм основи"), (190, "FlyClap"), (440, "FlySonar"), (690, "FlySonar Turret"),
            (920, "FlySonar ESP32 (AI-Thinker)")]
    ty = y + 62
    for cx, title in cols:
        s.text(cx, ty, title, size=13, weight="bold")
    s.line(42, ty + 10, 1158, ty + 10, "#b8c2cf", 1.4)
    table = [
        ("B", "D4–D7 ← промені", "", "", ""),
        ("E", "+5 В · D3 (TSSP)", "", "", ""),
        ("S1", "D8 ← кінцевик", "D2 → TRIG · D3 ← ECHO", "", ""),
        ("S2", "", "D4 ← кнопка RECAL", "", ""),
        ("servo", "D10 → взведення", "D9 → PAN · D10 → TILT", "D9 → PAN · D10 → TILT", "IO14 → PAN · IO15 → TILT"),
        ("P1", "D9 → соленоїд", "D5 → помпа / клапан", "D5 → помпа / клапан", "IO13 → клапан"),
        ("L", "", "", "D0 (RX) ← «очі»", "IO14 (TX) → Arduino*"),
        ("на основі", "ARM A0 · LED D13 · бузер D11", "ARM A0 · LED D13", "ARM A0 · LED D13", "ARM IO12 · LED IO33"),
    ]
    ry = ty + 34
    for row in table:
        if row[0] != "на основі":
            s.tag(42, ry - 4, row[0])
        else:
            s.text(42, ry, row[0], size=12, color=MUTED)
        for (cx, _), cell in zip(cols[1:], row[1:]):
            s.text(cx, ry, cell, size=12, color=CSIG if cell else MUTED)
        ry += 25
    s.text(42, ry + 6, "* у ролі «очі» (прошивка flysonar_eyes): серви й клапан тоді підключаються до Arduino.",
           size=12, color=MUTED)
    s.save("modules-connectors.svg")


def export_png():
    chrome = os.environ.get("CHROME") or next(
        (p for p in ("chrome-headless-shell", "headless_shell", "chromium", "chromium-browser", "google-chrome")
         if shutil.which(p)), None)
    if not chrome:
        sys.exit("PNG: не знайдено Chromium — вкажіть шлях у змінній CHROME")
    png_dir = os.path.join(OUT, "png")
    os.makedirs(png_dir, exist_ok=True)
    for name, w, h in SAVED:
        src = os.path.abspath(os.path.join(OUT, name))
        dst = os.path.abspath(os.path.join(png_dir, name.replace(".svg", ".png")))
        subprocess.run([chrome, "--headless", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                        "--force-device-scale-factor=2", f"--window-size={w},{h}",
                        f"--screenshot={dst}", "file://" + src],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("wrote png/" + os.path.basename(dst))


if __name__ == "__main__":
    flyclap_wiring()
    flyclap_tssp()
    flysonar_arduino_wiring()
    flysonar_esp32_wiring()
    flyclap_device()
    flysonar_arduino_device()
    flysonar_esp32_device()
    modules_connectors()
    if "--png" in sys.argv:
        export_png()
