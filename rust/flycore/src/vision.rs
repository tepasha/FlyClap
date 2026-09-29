//! Машинний зір FlySonar (ESP32-версія).
//!
//! Сцена: яскравий підсвічений фон (LED-панель + розсіювач), комаха на ньому —
//! темна точка. Кадр у відтінках сірого, 1 байт на піксель.
//!
//! 1. Фон — повільне ковзне середнє (фіксована кома 8.8). Під темними пікселями
//!    фон не оновлюється, тож муха, що сіла, не "розчиняється".
//! 2. Маска: піксель на яскравій частині фону (`>= backdrop_min`) і темніший за
//!    фон щонайменше на `dark_delta`.
//! 3. Зв'язні області (4-зв'язність, заливка стеком). Малі — цілі, великі (рука,
//!    кіт, людина) — прапорець `big`, по ньому не стріляємо.
//! 4. Трекер альфа-бета: згладжена позиція + швидкість у пікселях/с, прогноз на
//!    час затримки пострілу.
//! 5. Калібрування "піксель → кути" афінною моделлю методом найменших квадратів.
//!    Точки знімаються тестовими пострілами, тож зміщення сопла й падіння
//!    струменя входять у модель автоматично.

use alloc::vec::Vec;

#[derive(Clone, Copy, Debug)]
pub struct VisionConfig {
    pub dark_delta: u8,     // на скільки піксель темніший за фон, щоб бути "комахою"
    pub backdrop_min: u8,   // фон яскравіший за це = робоча зона (панель)
    pub bg_shift: u8,       // швидкість адаптації фону: 1/32 за кадр
    pub min_pix: u32,       // менше — шум сенсора
    pub max_side_px: usize, // бокс більший за це — вже не комаха
    pub big_total_pix: u32, // стільки темних пікселів у кадрі — великий об'єкт
}

impl Default for VisionConfig {
    fn default() -> Self {
        VisionConfig { dark_delta: 28, backdrop_min: 110, bg_shift: 5, min_pix: 2, max_side_px: 14, big_total_pix: 400 }
    }
}

#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Blob {
    pub x: f32, // центроїд, пікселі
    pub y: f32,
    pub pix: u16, // площа
    pub w: u16,   // габарити
    pub h: u16,
}

/// Результат обробки кадру.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Detection {
    /// Скільки малих цілей записано у вихідний масив.
    pub count: usize,
    /// У кадрі великий об'єкт — не стріляти.
    pub big: bool,
}

pub struct Vision {
    pub cfg: VisionConfig,
    w: usize,
    h: usize,
    bg: Vec<u16>,
    mask: Vec<u8>,
    stack: Vec<u32>,
    have_bg: bool,
}

fn try_vec<T: Clone>(n: usize, fill: T) -> Option<Vec<T>> {
    let mut v = Vec::new();
    v.try_reserve_exact(n).ok()?;
    v.resize(n, fill);
    Some(v)
}

impl Vision {
    /// Виділяє буфери під кадр `width`×`height` (~7 байт/піксель — на ESP32 це
    /// PSRAM). `None` — не вистачило пам'яті.
    pub fn new(width: usize, height: usize) -> Option<Vision> {
        let n = width * height;
        Some(Vision {
            cfg: VisionConfig::default(),
            w: width,
            h: height,
            bg: try_vec(n, 0)?,
            mask: try_vec(n, 0)?,
            stack: try_vec(n, 0)?,
            have_bg: false,
        })
    }

    /// Примусово перевчити фон з наступного кадру.
    pub fn reset_background(&mut self) {
        self.have_bg = false;
    }

    pub fn has_background(&self) -> bool {
        self.have_bg
    }

    /// Обробити кадр; малі цілі — в `out` (скільки влізе).
    pub fn process(&mut self, img: &[u8], out: &mut [Blob]) -> Detection {
        let (w, h) = (self.w, self.h);
        let n = w * h;
        let img = &img[..n];
        if !self.have_bg {
            for (b, &p) in self.bg.iter_mut().zip(img) {
                *b = (p as u16) << 8;
            }
            self.have_bg = true;
            return Detection::default();
        }

        // 1-2. Маска темних пікселів + адаптація фону там, де темного немає
        let cfg = self.cfg;
        let mut dark: u32 = 0;
        for ((b, m), &p) in self.bg.iter_mut().zip(self.mask.iter_mut()).zip(img) {
            let level = (*b >> 8) as u8;
            *m = (level >= cfg.backdrop_min && (p as u16 + cfg.dark_delta as u16) < level as u16) as u8;
            if *m != 0 {
                dark += 1;
            } else {
                let cur = *b as i32;
                *b = (cur + ((((p as i32) << 8) - cur) >> cfg.bg_shift)) as u16;
            }
        }
        if dark == 0 {
            return Detection::default();
        }
        if dark > cfg.big_total_pix {
            return Detection { count: 0, big: true };
        }

        // 3. Зв'язні області
        let (mask, stack) = (&mut self.mask, &mut self.stack);
        let mut det = Detection::default();
        for start in 0..n {
            if mask[start] == 0 {
                continue;
            }
            let (mut sp, mut pix) = (0usize, 0u32);
            let (mut sx, mut sy) = (0u64, 0u64);
            let (mut min_x, mut max_x, mut min_y, mut max_y) = (w, 0, h, 0);
            stack[sp] = start as u32;
            sp += 1;
            mask[start] = 0;
            while sp > 0 {
                sp -= 1;
                let i = stack[sp] as usize;
                let (x, y) = (i % w, i / w);
                pix += 1;
                sx += x as u64;
                sy += y as u64;
                min_x = min_x.min(x);
                max_x = max_x.max(x);
                min_y = min_y.min(y);
                max_y = max_y.max(y);
                let mut visit = |j: usize| {
                    if mask[j] != 0 {
                        mask[j] = 0;
                        stack[sp] = j as u32;
                        sp += 1;
                    }
                };
                if x > 0 {
                    visit(i - 1);
                }
                if x < w - 1 {
                    visit(i + 1);
                }
                if y > 0 {
                    visit(i - w);
                }
                if y < h - 1 {
                    visit(i + w);
                }
            }
            let (bw, bh) = (max_x - min_x + 1, max_y - min_y + 1);
            if bw > cfg.max_side_px || bh > cfg.max_side_px {
                det.big = true;
                continue;
            }
            if pix < cfg.min_pix {
                continue;
            }
            if let Some(b) = out.get_mut(det.count) {
                *b = Blob {
                    x: sx as f32 / pix as f32,
                    y: sy as f32 / pix as f32,
                    pix: pix as u16,
                    w: bw as u16,
                    h: bh as u16,
                };
                det.count += 1;
            }
        }
        det
    }
}

// ---------------- Трекер ----------------

#[derive(Clone, Copy, Debug)]
pub struct TrackerConfig {
    pub alpha: f32,     // довіра до виміру позиції
    pub beta: f32,      // довіра до виміру швидкості
    pub gate_px: f32,   // далі від прогнозу — інша ціль
    pub max_misses: u8, // кадрів без цілі до скидання
    pub max_speed: f32, // пікс/с — обмеження проти стрибків
}

impl Default for TrackerConfig {
    fn default() -> Self {
        TrackerConfig { alpha: 0.6, beta: 0.2, gate_px: 30.0, max_misses: 4, max_speed: 2000.0 }
    }
}

#[derive(Clone, Copy, Default, Debug)]
pub struct Track {
    pub active: bool,
    pub x: f32,
    pub y: f32,
    pub vx: f32,
    pub vy: f32,
    pub last_ms: u32,
    pub hits: u16, // скільки кадрів поспіль підтверджено
    pub misses: u8,
}

impl Track {
    pub fn speed(&self) -> f32 {
        libm::sqrtf(self.vx * self.vx + self.vy * self.vy)
    }

    pub fn predict_x(&self, dt_ms: f32) -> f32 {
        self.x + self.vx * dt_ms * 0.001
    }

    pub fn predict_y(&self, dt_ms: f32) -> f32 {
        self.y + self.vy * dt_ms * 0.001
    }

    pub fn update(&mut self, blobs: &[Blob], now_ms: u32, c: &TrackerConfig) {
        if !self.active {
            // нова ціль: найбільша пляма (перша з найбільших)
            let Some(b) = blobs.iter().reduce(|best, b| if b.pix > best.pix { b } else { best }) else {
                return;
            };
            *self = Track { active: true, x: b.x, y: b.y, vx: 0.0, vy: 0.0, last_ms: now_ms, hits: 1, misses: 0 };
            return;
        }
        let mut dt = now_ms.wrapping_sub(self.last_ms) as f32 * 0.001;
        if dt <= 0.0 {
            dt = 0.001;
        }
        let (px, py) = (self.x + self.vx * dt, self.y + self.vy * dt);

        let mut best = None;
        let mut best_d2 = c.gate_px * c.gate_px;
        for b in blobs {
            let (dx, dy) = (b.x - px, b.y - py);
            let d2 = dx * dx + dy * dy;
            if d2 <= best_d2 {
                best_d2 = d2;
                best = Some(b);
            }
        }
        let Some(b) = best else {
            self.misses += 1;
            if self.misses > c.max_misses {
                self.active = false;
                self.hits = 0;
            }
            return; // позицію/час не чіпаємо — наступний кадр порахує dt від останнього виміру
        };
        let (rx, ry) = (b.x - px, b.y - py);
        self.x = px + c.alpha * rx;
        self.y = py + c.alpha * ry;
        self.vx += c.beta * rx / dt;
        self.vy += c.beta * ry / dt;
        let s = self.speed();
        if s > c.max_speed {
            self.vx *= c.max_speed / s;
            self.vy *= c.max_speed / s;
        }
        self.last_ms = now_ms;
        self.misses = 0;
        self.hits = self.hits.saturating_add(1);
    }
}

// ---------------- Калібрування піксель → кути ----------------

/// `pan = a0*x + a1*y + a2`, `tilt = b0*x + b1*y + b2` (кути в десятих градуса).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Affine {
    pub a: [f32; 3],
    pub b: [f32; 3],
    pub valid: bool,
}

impl Default for Affine {
    fn default() -> Self {
        Affine { a: [0.0, 0.0, 900.0], b: [0.0, 0.0, 900.0], valid: false }
    }
}

impl Affine {
    /// Розмір у NVS — той самий, що й `sizeof(fv::Affine)` у C++-версії, тож
    /// калібрування, збережене старою прошивкою, читається й новою.
    pub const BYTES: usize = 28;

    pub fn apply(&self, x: f32, y: f32) -> (f32, f32) {
        (self.a[0] * x + self.a[1] * y + self.a[2], self.b[0] * x + self.b[1] * y + self.b[2])
    }

    pub fn to_bytes(&self) -> [u8; Self::BYTES] {
        let mut out = [0u8; Self::BYTES];
        for (i, v) in self.a.iter().chain(&self.b).enumerate() {
            out[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        }
        out[24] = self.valid as u8;
        out
    }

    pub fn from_bytes(bytes: &[u8]) -> Option<Affine> {
        if bytes.len() != Self::BYTES {
            return None;
        }
        let f = |i: usize| f32::from_le_bytes([bytes[i * 4], bytes[i * 4 + 1], bytes[i * 4 + 2], bytes[i * 4 + 3]]);
        Some(Affine { a: [f(0), f(1), f(2)], b: [f(3), f(4), f(5)], valid: bytes[24] != 0 })
    }
}

#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct CalPoint {
    pub x: f32,
    pub y: f32,
    pub pan10: f32,
    pub tilt10: f32,
}

fn det3(m: &[[f64; 3]; 3]) -> f64 {
    m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
        + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])
}

/// Найменші квадрати для 3 і більше точок (не на одній прямій). `None` — вироджено.
pub fn solve_affine(pts: &[CalPoint]) -> Option<Affine> {
    if pts.len() < 3 {
        return None;
    }
    let mut m = [[0f64; 3]; 3];
    let (mut rp, mut rt) = ([0f64; 3], [0f64; 3]);
    for p in pts {
        let v = [p.x as f64, p.y as f64, 1.0];
        for r in 0..3 {
            for c in 0..3 {
                m[r][c] += v[r] * v[c];
            }
            rp[r] += v[r] * p.pan10 as f64;
            rt[r] += v[r] * p.tilt10 as f64;
        }
    }
    let det = det3(&m);
    if libm::fabs(det) < 1e-6 {
        return None;
    }
    // Крамер для двох правих частин
    let cramer = |r: &[f64; 3]| {
        let mut o = [0f32; 3];
        for (col, out) in o.iter_mut().enumerate() {
            let mut t = m;
            for row in 0..3 {
                t[row][col] = r[row];
            }
            *out = (det3(&t) / det) as f32;
        }
        o
    };
    Some(Affine { a: cramer(&rp), b: cramer(&rt), valid: true })
}

#[cfg(test)]
mod tests {
    //! Порт tools/vision_test.cpp: синтетичні кадри.
    use super::*;
    use alloc::vec;
    use std::println;

    const W: usize = 320;
    const H: usize = 240;

    struct Frame(Vec<u8>);

    impl Frame {
        fn new(level: u8) -> Self {
            Frame(vec![level; W * H])
        }
        fn noise(mut self, seed: &mut u32, amp: i32) -> Self {
            for p in self.0.iter_mut() {
                *seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
                let v = *p as i32 + ((*seed >> 16) % (2 * amp as u32 + 1)) as i32 - amp;
                *p = v.clamp(0, 255) as u8;
            }
            self
        }
        /// Темна пляма size×size з лівим верхнім кутом (x, y).
        fn spot(mut self, x: usize, y: usize, size: usize, level: u8) -> Self {
            for j in 0..size {
                for i in 0..size {
                    if x + i < W && y + j < H {
                        self.0[(y + j) * W + x + i] = level;
                    }
                }
            }
            self
        }
    }

    fn vision() -> Vision {
        Vision::new(W, H).unwrap()
    }

    #[test]
    fn clean_backdrop_has_no_false_positives() {
        let (mut v, mut seed, mut blobs) = (vision(), 1u32, [Blob::default(); 8]);
        let mut false_pos = 0;
        for _ in 0..100 {
            let d = v.process(&Frame::new(200).noise(&mut seed, 6).0, &mut blobs);
            false_pos += d.count + d.big as usize;
        }
        assert_eq!(false_pos, 0);
    }

    #[test]
    fn flying_midge_is_tracked_with_velocity() {
        let (mut v, mut seed, mut blobs) = (vision(), 1u32, [Blob::default(); 8]);
        let (mut t, tc) = (Track::default(), TrackerConfig::default());
        v.process(&Frame::new(200).noise(&mut seed, 4).0, &mut blobs);
        let (mut detected, mut max_err) = (0, 0f32);
        for f in 0..30 {
            let (x, y) = (100 + 3 * f, 120);
            let d = v.process(&Frame::new(200).noise(&mut seed, 4).spot(x, y, 2, 80).0, &mut blobs);
            assert!(!d.big, "small midge flagged as big at frame {f}");
            if d.count == 1 {
                detected += 1;
                let err = (blobs[0].x - (x as f32 + 0.5)).abs() + (blobs[0].y - (y as f32 + 0.5)).abs();
                max_err = max_err.max(err);
            }
            t.update(&blobs[..d.count], 1000 + f as u32 * 33, &tc);
        }
        assert_eq!(detected, 30);
        assert!(max_err < 0.01, "centroid error {max_err:.3} px");
        assert!(t.active && t.hits >= 25, "track not locked (hits={})", t.hits);
        assert!((t.vx - 90.9).abs() < 10.0 && t.vy.abs() < 5.0, "velocity {:.1},{:.1} px/s", t.vx, t.vy);
        let lead = t.predict_x(60.0) - t.x;
        assert!((lead - 5.45).abs() < 1.0, "60 ms lead {lead:.2} px");
        println!("midge: {detected}/30 frames, v=({:.1}, {:.1}) px/s, 60ms lead={lead:.2} px", t.vx, t.vy);
    }

    #[test]
    fn single_hot_pixel_is_noise() {
        let (mut v, mut blobs) = (vision(), [Blob::default(); 8]);
        v.process(&Frame::new(200).0, &mut blobs);
        let d = v.process(&Frame::new(200).spot(50, 50, 1, 20).0, &mut blobs);
        assert_eq!(d, Detection { count: 0, big: false });
    }

    #[test]
    fn hand_and_finger_are_big() {
        let (mut v, mut blobs) = (vision(), [Blob::default(); 8]);
        v.process(&Frame::new(200).0, &mut blobs);
        let d = v.process(&Frame::new(200).spot(100, 60, 40, 60).0, &mut blobs);
        assert_eq!(d, Detection { count: 0, big: true }, "hand not flagged");

        // пляма середнього розміру (18×3: палець) — теж big, хоча загалом пікселів мало
        let mut v = vision();
        v.process(&Frame::new(200).0, &mut blobs);
        let finger = (0..6).fold(Frame::new(200), |f, k| f.spot(10 + 3 * k, 10, 3, 60));
        let d = v.process(&finger.0, &mut blobs);
        assert_eq!(d, Detection { count: 0, big: true }, "finger-like strip not flagged");
    }

    #[test]
    fn dark_area_outside_backdrop_is_ignored() {
        let (mut v, mut blobs) = (vision(), [Blob::default(); 8]);
        let mut fr = Frame::new(200);
        for y in 0..H {
            for x in 0..60 {
                fr.0[y * W + x] = 50; // рамка/стіна
            }
        }
        v.process(&fr.0, &mut blobs);
        let d = v.process(&fr.spot(20, 100, 3, 5).0, &mut blobs);
        assert_eq!(d, Detection { count: 0, big: false }, "target outside backdrop detected");
    }

    #[test]
    fn landed_fly_is_not_absorbed_into_background() {
        let (mut v, mut seed, mut blobs) = (vision(), 1u32, [Blob::default(); 8]);
        v.process(&Frame::new(200).0, &mut blobs);
        let detected = (0..300)
            .filter(|_| v.process(&Frame::new(200).noise(&mut seed, 4).spot(200, 150, 4, 70).0, &mut blobs).count == 1)
            .count();
        assert_eq!(detected, 300);
    }

    #[test]
    fn slow_brightness_drift_is_tolerated() {
        let (mut v, mut seed, mut blobs) = (vision(), 1u32, [Blob::default(); 8]);
        let mut false_pos = 0;
        for f in 0..600 {
            let d = v.process(&Frame::new((200 - f / 20) as u8).noise(&mut seed, 4).0, &mut blobs);
            false_pos += d.count + d.big as usize;
        }
        assert_eq!(false_pos, 0);
    }

    #[test]
    fn affine_calibration() {
        let (xs, ys) = ([40.0, 280.0, 160.0, 60.0], [30.0, 40.0, 210.0, 190.0]);
        let pts: Vec<CalPoint> = (0..4)
            .map(|i| CalPoint {
                x: xs[i],
                y: ys[i],
                pan10: -2.5 * xs[i] + 0.1 * ys[i] + 1300.0,
                tilt10: 0.05 * xs[i] + 2.2 * ys[i] + 600.0,
            })
            .collect();
        let a = solve_affine(&pts).expect("solve_affine failed");
        let (pan, tilt) = a.apply(123.0, 77.0);
        let (ep, et) = (-2.5 * 123.0 + 0.1 * 77.0 + 1300.0, 0.05 * 123.0 + 2.2 * 77.0 + 600.0);
        assert!((pan - ep).abs() < 0.5 && (tilt - et).abs() < 0.5, "affine {pan:.1}/{tilt:.1} vs {ep:.1}/{et:.1}");
        assert_eq!(Affine::from_bytes(&a.to_bytes()), Some(a));

        let line = [
            CalPoint { x: 0.0, y: 0.0, pan10: 900.0, tilt10: 900.0 },
            CalPoint { x: 10.0, y: 10.0, pan10: 950.0, tilt10: 950.0 },
            CalPoint { x: 20.0, y: 20.0, pan10: 1000.0, tilt10: 1000.0 },
        ];
        assert!(solve_affine(&line).is_none(), "collinear points accepted");
    }
}
