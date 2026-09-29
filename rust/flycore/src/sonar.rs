//! FlySonar, режим сонара: HC-SR04 на pan-tilt платформі разом із соплом.
//!
//! 1. CALIBRATE: обхід сітки кутів, запам'ятовуємо "фон" (медіана з 5 вимірів).
//! 2. SCAN: змійкою по сітці; ехо значно ближче за фон — кандидат.
//! 3. CONFIRM: медіана з 3 + ширина об'єкта (широке ехо — рука/кіт, НЕ стріляємо).
//! 4. REFINE: дрібний скан ±10° кроком 2° по горизонталі й вертикалі, ціль =
//!    центроїд кутів, де є ехо. Точність ~1–2° замість кроку сітки 5°.
//! 5. STILL: кілька вимірів у центроїді; стріляємо лише по цілі, що сидить
//!    ([`SonarConfig::static_only`]) — по мусі в польоті сонар не влучить.
//! 6. FIRE: поправка на сопло й балістику, постріл, перевірка, до [`MAX_SHOTS`].
//!
//! Код блокуючий (як і оригінал на Arduino): очікування серв і пінгів — це
//! виклики [`SonarHal::delay_ms`]. Кути — у десятих градуса.

use crate::servo::{clamp10, deg10_to_us, Deg10, Settle};
use crate::time::{elapsed, reached};

// ---------------- Геометрія сканування (градуси) ----------------
pub const PAN_MIN: u8 = 30;
pub const PAN_MAX: u8 = 150;
pub const PAN_STEP: u8 = 5; // ~ ширина пелюстки HC-SR04 / 3
pub const PAN_CELLS: usize = ((PAN_MAX - PAN_MIN) / PAN_STEP + 1) as usize; // 25
pub const TILT_LEVELS: [u8; 3] = [80, 95, 110];
pub const TILT_CELLS: usize = TILT_LEVELS.len();

// ---------------- Сонар ----------------
pub const ECHO_TIMEOUT_US: u16 = 6000; // ~1 м — далі нас не цікавить
pub const PING_GAP_MS: u32 = 30; // пауза між пінгами, щоб не ловити старе ехо
pub const ECHO_IDLE_TIMEOUT_MS: u32 = 50; // без ехо HC-SR04 тримає ECHO у HIGH ~38 мс
pub const NO_ECHO: u16 = 999;
pub const MIN_CM: u16 = 8; // ближче — мертва зона HC-SR04 / сам корпус
pub const MAX_CM: u16 = 60; // далі муху вже не видно в шумі
pub const DELTA_CM: u16 = 10; // на скільки ближче за фон має бути ціль
pub const SIMILAR_CM: u16 = 6; // "та сама відстань" для перевірки ширини
// Пелюстка ~16° накриває до 4 клітинок сітки 5°, тож навіть точкова муха дає
// ехо в сусідах. Великий об'єкт (рука, кіт) — суцільна смуга з 5+ клітинок.
pub const WIDE_RANGE: i8 = 6; // далі в кожен бік не дивимось
pub const WIDE_SPAN: u8 = 5; // суцільна смуга стільки клітинок = великий об'єкт
pub const FRESH_MS: u32 = 400; // вимір сусідньої клітинки, свіжіший за це, не повторюємо

// ---------------- Уточнення й "сидить" ----------------
pub const FINE_SPAN10: i16 = 100; // дрібний скан ±10°
pub const FINE_STEP10: i16 = 20; // кроком 2°
pub const FINE_MIN_HITS: u8 = 2; // менше влучань — ехо випадкове
pub const FINE_PASSES: u8 = 3; // макс. пересувань вікна
pub const STILL_SAMPLES: usize = 4; // вимірів у центроїді
pub const STILL_CM: u16 = 2; // розкид відстані, за якого ціль "сидить"

// ---------------- Водомет ----------------
pub const PAN_NOZZLE_OFFSET10: i16 = 0; // сопло збоку від сонара — поправка, 0.1°
pub const TILT_NOZZLE_OFFSET10: i16 = 20; // сопло під сонаром — трохи вгору, 0.1°
pub const TILT_UP_SIGN: i16 = 1; // +1, якщо більший кут серви = вгору
pub const MAX_SHOTS: u8 = 3;
pub const COOLDOWN_MS: u32 = 2500;

#[derive(Clone, Copy, Debug)]
pub struct SonarConfig {
    pub shot_ms: u16,
    /// Падіння струменя, сотих градуса на см відстані.
    pub ballistic_x100: u16,
    /// Стріляти лише по нерухомій цілі.
    pub static_only: bool,
}

impl SonarConfig {
    /// Помпа R385: їй потрібен час, щоб набрати тиск; струмінь провисає.
    pub const PUMP: SonarConfig = SonarConfig { shot_ms: 120, ballistic_x100: 25, static_only: true };
    /// Клапан 12 В + бак під тиском: відкривається за ~5–10 мс, струмінь майже прямий.
    pub const VALVE: SonarConfig = SonarConfig { shot_ms: 50, ballistic_x100: 8, static_only: true };

    /// Поправка на провисання струменя, 0.1° (з округленням, як `(int)(k*d*10 + 0.5)`).
    pub fn ballistic_lift10(&self, cm: u16) -> i16 {
        ((self.ballistic_x100 as u32 * cm as u32 + 5) / 10) as i16
    }
}

/// Залізо для сонарного режиму.
pub trait SonarHal {
    fn millis(&mut self) -> u32;
    fn delay_ms(&mut self, ms: u16);
    /// Рівень на ECHO.
    fn echo_high(&mut self) -> bool;
    /// Імпульс TRIG 10 мкс і тривалість ECHO у мкс (як `pulseIn`); 0 — ехо не
    /// прийшло за `timeout_us`.
    fn ping_us(&mut self, timeout_us: u16) -> u16;
    /// Тумблер ARM: стріляти дозволено.
    fn armed(&mut self) -> bool;
    /// Кнопка перекалібрування фону.
    fn recal_pressed(&mut self) -> bool;
    fn led(&mut self, on: bool);
    fn pump(&mut self, on: bool);
    fn servos_us(&mut self, pan_us: u16, tilt_us: u16);
    fn event(&mut self, ev: SonarEvent);
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SonarEvent {
    Calibrating,
    BackgroundReady,
    BigObject { cm: u16 },
    LostDuringRefine,
    Target { still: bool, pan10: i16, tilt10: i16, cm: u16 },
    Safe,
    Shot { pan10: i16, tilt10: i16, total: u32, cm: u16 },
    TargetGone,
}

#[cfg(feature = "ufmt")]
impl ufmt::uDisplay for SonarEvent {
    fn fmt<W: ufmt::uWrite + ?Sized>(&self, f: &mut ufmt::Formatter<'_, W>) -> Result<(), W::Error> {
        use ufmt::uwrite;
        match *self {
            SonarEvent::Calibrating => f.write_str("Calibrating background... keep the area clear"),
            SonarEvent::BackgroundReady => f.write_str("Background ready"),
            SonarEvent::BigObject { cm } => uwrite!(f, "Big object at {}cm - holding fire", cm),
            SonarEvent::LostDuringRefine => f.write_str("Lost during refine"),
            SonarEvent::Target { still, pan10, tilt10, cm } => uwrite!(
                f,
                "Target ({}) at {}/{} deg, {}cm",
                if still { "still" } else { "moving" },
                Deg10(pan10),
                Deg10(tilt10),
                cm
            ),
            SonarEvent::Safe => f.write_str("[SAFE] not firing"),
            SonarEvent::Shot { pan10, tilt10, total, cm } => uwrite!(
                f,
                "PSSHT! pan={} tilt={} total={}\n  d={}cm",
                Deg10(pan10),
                Deg10(tilt10),
                total,
                cm
            ),
            SonarEvent::TargetGone => f.write_str("Target gone"),
        }
    }
}

pub fn median3(mut a: u16, mut b: u16, mut c: u16) -> u16 {
    if a > b {
        core::mem::swap(&mut a, &mut b);
    }
    if b > c {
        core::mem::swap(&mut b, &mut c);
    }
    if a > b {
        core::mem::swap(&mut a, &mut b);
    }
    b
}

/// Ехо значно ближче за фон.
pub fn is_foreground(d: u16, bg: u16) -> bool {
    if d == NO_ECHO || !(MIN_CM..=MAX_CM).contains(&d) {
        return false;
    }
    if bg == NO_ECHO {
        return true; // раніше там була порожнеча
    }
    d + DELTA_CM < bg
}

/// Ехо від тієї самої цілі (для дрібного скану, де фону по клітинці немає).
pub fn same_target(dn: u16, d: u16) -> bool {
    dn != NO_ECHO && dn >= MIN_CM && dn.abs_diff(d) <= SIMILAR_CM
}

fn pan_of10(i: usize) -> i16 {
    (PAN_MIN as i16 + i as i16 * PAN_STEP as i16) * 10
}

fn tilt_of10(t: usize) -> i16 {
    TILT_LEVELS[t] as i16 * 10
}

pub struct Sonar {
    cfg: SonarConfig,
    settle: Settle,
    background: [[u16; PAN_CELLS]; TILT_CELLS],
    last_scan: [[u16; PAN_CELLS]; TILT_CELLS], // останній вимір у кожній клітинці...
    last_scan_at: [[u32; PAN_CELLS]; TILT_CELLS], // ...і коли його зроблено
    cooldown_until: [u32; TILT_CELLS], // кулдаун по рядах (дешево по RAM)
    cur_pan10: i16,
    cur_tilt10: i16,
    shots_total: u32,
    last_ping: u32,
    // позиція "змійки": без великих перекидів серви
    t: usize,
    p: usize,
    dir: i8,
}

impl Sonar {
    pub const fn new(cfg: SonarConfig) -> Self {
        Sonar {
            cfg,
            settle: Settle::MG90S,
            background: [[NO_ECHO; PAN_CELLS]; TILT_CELLS],
            last_scan: [[NO_ECHO; PAN_CELLS]; TILT_CELLS],
            last_scan_at: [[0; PAN_CELLS]; TILT_CELLS],
            cooldown_until: [0; TILT_CELLS],
            cur_pan10: 900,
            cur_tilt10: 900,
            shots_total: 0,
            last_ping: 0,
            t: 0,
            p: 0,
            dir: 1,
        }
    }

    pub fn config(&self) -> &SonarConfig {
        &self.cfg
    }

    /// Старт: серви в початкову позицію, пауза, калібрування фону.
    pub fn begin(&mut self, h: &mut impl SonarHal) {
        self.cur_pan10 = 900;
        self.cur_tilt10 = tilt_of10(0);
        h.servos_us(deg10_to_us(self.cur_pan10), deg10_to_us(self.cur_tilt10));
        h.delay_ms(1000);
        self.calibrate(h);
    }

    /// Один крок скану (одна клітинка сітки).
    pub fn step(&mut self, h: &mut impl SonarHal) {
        if h.recal_pressed() {
            self.calibrate(h);
            self.p = 0;
            self.t = 0;
            self.dir = 1;
        }

        let armed = h.armed();
        let blink = (h.millis() / 250) % 2 == 1;
        h.led(armed && blink);

        let (t, p) = (self.t, self.p);
        if reached(h.millis(), self.cooldown_until[t]) {
            let d = self.measure_cell(h, t, p);
            if is_foreground(d, self.background[t][p]) {
                self.engage(h, t, p);
            }
        }

        // наступна клітинка змійкою
        let np = p as i8 + self.dir;
        if np < 0 || np >= PAN_CELLS as i8 {
            self.dir = -self.dir;
            self.t = (t + 1) % TILT_CELLS;
        } else {
            self.p = np as usize;
        }
    }

    // ---------------- Серви ----------------

    /// Навести без очікування. Повертає, скільки мс серво їхатиме.
    fn aim10(&mut self, h: &mut impl SonarHal, pan10: i16, tilt10: i16) -> u16 {
        let pan = clamp10(pan10 as i32);
        let tilt = clamp10(tilt10 as i32);
        let dist = pan.abs_diff(self.cur_pan10).max(tilt.abs_diff(self.cur_tilt10));
        h.servos_us(deg10_to_us(pan), deg10_to_us(tilt));
        self.cur_pan10 = pan;
        self.cur_tilt10 = tilt;
        self.settle.ms(dist)
    }

    /// Навести й дочекатись.
    fn move_to10(&mut self, h: &mut impl SonarHal, pan10: i16, tilt10: i16) {
        let ms = self.aim10(h, pan10, tilt10);
        h.delay_ms(ms);
    }

    // ---------------- Сонар ----------------

    fn ping_cm(&mut self, h: &mut impl SonarHal) -> u16 {
        while elapsed(h.millis(), self.last_ping) < PING_GAP_MS {
            // чекаємо затухання відлуння
        }

        // Якщо попередній пінг не отримав ехо, сенсор ще тримає ECHO у HIGH і
        // проігнорує новий тригер. Дочекаємось LOW, інакше кожен другий вимір губиться.
        let wait_start = h.millis();
        while h.echo_high() {
            if elapsed(h.millis(), wait_start) > ECHO_IDLE_TIMEOUT_MS {
                // сенсор завис/відключений
                self.last_ping = h.millis();
                return NO_ECHO;
            }
        }
        self.last_ping = h.millis();

        match h.ping_us(ECHO_TIMEOUT_US) {
            0 => NO_ECHO,
            us => us / 58,
        }
    }

    fn ping_median3(&mut self, h: &mut impl SonarHal) -> u16 {
        let a = self.ping_cm(h);
        let b = self.ping_cm(h);
        let c = self.ping_cm(h);
        median3(a, b, c)
    }

    /// Навести сонар на клітинку, виміряти й запам'ятати результат.
    fn measure_cell(&mut self, h: &mut impl SonarHal, t: usize, p: usize) -> u16 {
        self.move_to10(h, pan_of10(p), tilt_of10(t));
        let d = self.ping_cm(h);
        self.last_scan[t][p] = d;
        self.last_scan_at[t][p] = h.millis();
        d
    }

    pub fn calibrate(&mut self, h: &mut impl SonarHal) {
        h.event(SonarEvent::Calibrating);
        h.led(true);
        for t in 0..TILT_CELLS {
            for p in 0..PAN_CELLS {
                self.move_to10(h, pan_of10(p), tilt_of10(t));
                let mut s = [0u16; 5];
                for v in s.iter_mut() {
                    *v = self.ping_cm(h);
                }
                s.sort_unstable();
                self.background[t][p] = s[2];
                self.last_scan[t][p] = s[2];
                self.last_scan_at[t][p] = h.millis().wrapping_sub(FRESH_MS); // одразу "несвіжий"
            }
            self.cooldown_until[t] = h.millis();
        }
        h.led(false);
        h.event(SonarEvent::BackgroundReady);
    }

    /// Ширина об'єкта: скільки клітинок поспіль (у обидва боки від p) бачать його
    /// на тій самій відстані. Рахуємо суцільну смугу, бо скан часто натрапляє на
    /// великий об'єкт з краю — симетрична перевірка сусідів бачить лише половину.
    /// Клітинки, які змійка щойно пройшла, беремо з `last_scan` — серво не
    /// мотаються туди-сюди. Перевимірюємо лише застарілі (зазвичай ті, що попереду).
    fn object_span(&mut self, h: &mut impl SonarHal, t: usize, p: usize, d: u16) -> u8 {
        let mut span = 1;
        for dir in [-1i8, 1] {
            for k in 1..=WIDE_RANGE {
                let q = p as i8 + dir * k;
                if q < 0 || q >= PAN_CELLS as i8 {
                    break;
                }
                let q = q as usize;
                let dn = if elapsed(h.millis(), self.last_scan_at[t][q]) < FRESH_MS {
                    self.last_scan[t][q]
                } else {
                    self.measure_cell(h, t, q)
                };
                if !is_foreground(dn, self.background[t][q]) || dn.abs_diff(d) > SIMILAR_CM {
                    break;
                }
                span += 1;
            }
        }
        span
    }

    /// Дрібний скан однієї осі навколо `center10`. Повертає зсув центроїду (0.1°)
    /// або `None`, якщо влучань замало. Пелюстка симетрична, тому середнє кутів з
    /// ехо — це напрям на ціль. Якщо ехо є на краю вікна, вікно обрізало пелюстку
    /// з одного боку і середнє зміщене — пересуваємо вікно й повторюємо.
    fn fine_axis(&mut self, h: &mut impl SonarHal, pan_axis: bool, center10: i16, other10: i16, d: u16) -> Option<i16> {
        let mut shift: i16 = 0;
        for pass in 0..FINE_PASSES {
            let mut sum: i32 = 0;
            let mut hits: u8 = 0;
            let mut edge = false;
            let mut off = -FINE_SPAN10;
            while off <= FINE_SPAN10 {
                let a = center10 + shift + off;
                if pan_axis {
                    self.move_to10(h, a, other10);
                } else {
                    self.move_to10(h, other10, a);
                }
                if same_target(self.ping_cm(h), d) {
                    sum += off as i32;
                    hits += 1;
                    if off == -FINE_SPAN10 || off + FINE_STEP10 > FINE_SPAN10 {
                        edge = true;
                    }
                }
                off += FINE_STEP10;
            }
            if hits < FINE_MIN_HITS {
                return if pass > 0 { Some(shift) } else { None };
            }
            let delta = (sum / hits as i32) as i16;
            shift += delta;
            if !edge || delta.abs() < FINE_STEP10 / 2 {
                break; // вікно вже накриває пелюстку
            }
        }
        Some(shift)
    }

    /// Уточнити напрям на ціль. `None` — ціль зникла під час скану.
    fn refine_centroid(&mut self, h: &mut impl SonarHal, t: usize, p: usize, d: u16) -> Option<(i16, i16)> {
        let mut pan10 = pan_of10(p);
        let mut tilt10 = tilt_of10(t);
        pan10 += self.fine_axis(h, true, pan10, tilt10, d)?;
        // по вертикалі може не вистачити влучань — лишаємо рядок
        if let Some(dt) = self.fine_axis(h, false, tilt10, pan10, d) {
            tilt10 += dt;
        }
        Some((pan10, tilt10))
    }

    /// Ціль "сидить": усі виміри є і розкид відстані малий. `d` оновлюється медіаною.
    fn is_still(&mut self, h: &mut impl SonarHal, pan10: i16, tilt10: i16, d: &mut u16) -> bool {
        self.move_to10(h, pan10, tilt10);
        let mut s = [0u16; STILL_SAMPLES];
        let (mut lo, mut hi) = (u16::MAX, 0);
        for v in s.iter_mut() {
            *v = self.ping_cm(h);
            if !same_target(*v, *d) {
                return false;
            }
            lo = lo.min(*v);
            hi = hi.max(*v);
        }
        *d = median3(s[0], s[1], s[2]);
        hi - lo <= STILL_CM
    }

    fn aim_and_fire(&mut self, h: &mut impl SonarHal, pan10: i16, tilt10: i16, d: u16) {
        let tilt_comp10 = self.cfg.ballistic_lift10(d) + TILT_NOZZLE_OFFSET10;
        let aim_pan10 = clamp10(pan10 as i32 + PAN_NOZZLE_OFFSET10 as i32);
        let aim_tilt10 = clamp10(tilt10 as i32 + (TILT_UP_SIGN * tilt_comp10) as i32);

        self.move_to10(h, aim_pan10, aim_tilt10);
        h.pump(true);
        h.delay_ms(self.cfg.shot_ms);
        h.pump(false);
        self.shots_total += 1;
        h.event(SonarEvent::Shot { pan10: aim_pan10, tilt10: aim_tilt10, total: self.shots_total, cm: d });
    }

    /// Відпрацювати кандидата в клітинці. true — ціль відпрацьована (скан
    /// продовжується з нової позиції).
    fn engage(&mut self, h: &mut impl SonarHal, t: usize, p: usize) -> bool {
        // 1. Підтвердження: медіана з 3 має теж бути "переднім планом"
        let mut d = self.ping_median3(h);
        if !is_foreground(d, self.background[t][p]) {
            return false;
        }

        // 2. Фільтр великих об'єктів — рука/кіт/людина
        if self.object_span(h, t, p, d) >= WIDE_SPAN {
            h.event(SonarEvent::BigObject { cm: d });
            self.cooldown_until[t] = h.millis().wrapping_add(COOLDOWN_MS);
            return true;
        }

        // 3. Точний напрям
        let Some((pan10, tilt10)) = self.refine_centroid(h, t, p, d) else {
            h.event(SonarEvent::LostDuringRefine);
            return true;
        };

        // 4. Сидить чи летить
        let still = self.is_still(h, pan10, tilt10, &mut d);
        h.event(SonarEvent::Target { still, pan10, tilt10, cm: d });
        if self.cfg.static_only && !still {
            return true;
        }

        if !h.armed() {
            h.event(SonarEvent::Safe);
            return true;
        }

        // 5. Постріли з повторною перевіркою в точці центроїду
        for _ in 0..MAX_SHOTS {
            self.aim_and_fire(h, pan10, tilt10, d);
            self.move_to10(h, pan10, tilt10);
            h.delay_ms(150); // бризки осіли, серво заспокоїлось
            let dn = self.ping_median3(h);
            if !same_target(dn, d) {
                h.event(SonarEvent::TargetGone);
                break;
            }
            d = dn;
        }
        self.cooldown_until[t] = h.millis().wrapping_add(COOLDOWN_MS);
        true
    }
}

#[cfg(test)]
mod tests;
