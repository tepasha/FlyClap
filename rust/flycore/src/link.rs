//! Протокол ESP32 ("очі") → Arduino (стріляє), 115200 8N1, рядки ASCII:
//!
//! ```text
//! A <pan10> <tilt10> <fire>   навестись (кути в 0.1°), fire=1 — можна стріляти
//! M <pan10> <tilt10>          просто навестись (калібрування)
//! F <ms>                      тестовий постріл (калібрування)
//! B                           у кадрі великий об'єкт: не стріляти
//! ```
//!
//! Тут і формат (ESP32 шле [`Command`] через `Display`), і розбір, і контролер
//! Arduino в режимі камери ([`CameraLink`]): плавно наводить серви, відкриває
//! клапан/помпу, тримає ARM, кулдаун і блокування великих об'єктів.

use core::fmt;

use crate::time::elapsed;
use crate::turret::{Turret, TurretConfig};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Command {
    Aim { pan10: i16, tilt10: i16, fire: bool },
    Move { pan10: i16, tilt10: i16 },
    TestShot { ms: i16 },
    Big,
}

impl Command {
    /// Розібрати рядок без `\n`. Сміття — `None`.
    pub fn parse(line: &[u8]) -> Option<Command> {
        let (&kind, args) = line.split_first()?;
        let mut v = [0i16; 3];
        match kind {
            b'A' if parse_ints(args, &mut v) == 3 => Some(Command::Aim { pan10: v[0], tilt10: v[1], fire: v[2] != 0 }),
            b'M' if parse_ints(args, &mut v[..2]) == 2 => Some(Command::Move { pan10: v[0], tilt10: v[1] }),
            b'F' if parse_ints(args, &mut v[..1]) == 1 => Some(Command::TestShot { ms: v[0] }),
            b'B' => Some(Command::Big),
            _ => None,
        }
    }
}

/// Рядок протоколу без `\n`.
impl fmt::Display for Command {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Command::Aim { pan10, tilt10, fire } => write!(f, "A {} {} {}", pan10, tilt10, fire as u8),
            Command::Move { pan10, tilt10 } => write!(f, "M {} {}", pan10, tilt10),
            Command::TestShot { ms } => write!(f, "F {}", ms),
            Command::Big => f.write_str("B"),
        }
    }
}

/// Розібрати до `out.len()` цілих (як послідовні `strtol`). Повертає, скільки
/// прочитано; решту рядка ігноруємо.
fn parse_ints(mut s: &[u8], out: &mut [i16]) -> usize {
    let mut k = 0;
    while k < out.len() {
        while let [c, rest @ ..] = s {
            if !c.is_ascii_whitespace() {
                break;
            }
            s = rest;
        }
        let (neg, body) = match s {
            [b'-', rest @ ..] => (true, rest),
            [b'+', rest @ ..] => (false, rest),
            _ => (false, s),
        };
        let n = body.iter().take_while(|c| c.is_ascii_digit()).count();
        if n == 0 {
            break;
        }
        let x = body[..n]
            .iter()
            .fold(0i32, |acc, &c| acc.saturating_mul(10).saturating_add((c - b'0') as i32));
        out[k] = (if neg { -x } else { x }) as i16;
        k += 1;
        s = &body[n..];
    }
    k
}

/// Збирає рядки з байтів UART. Задовгий рядок відкидається цілком.
pub struct LineReader {
    buf: [u8; 32],
    len: usize,
    overflow: bool,
}

impl LineReader {
    pub const fn new() -> Self {
        LineReader { buf: [0; 32], len: 0, overflow: false }
    }

    /// Подати байт. Повертає готовий непорожній рядок (без `\r\n`), коли прийшов `\n`.
    pub fn push(&mut self, c: u8) -> Option<&[u8]> {
        match c {
            b'\r' => None,
            b'\n' => {
                let (len, ok) = (self.len, !self.overflow && self.len > 0);
                self.len = 0;
                self.overflow = false;
                ok.then(|| &self.buf[..len])
            }
            _ if self.len < self.buf.len() - 1 => {
                self.buf[self.len] = c;
                self.len += 1;
                None
            }
            _ => {
                self.overflow = true;
                None
            }
        }
    }
}

impl Default for LineReader {
    fn default() -> Self {
        Self::new()
    }
}

pub const LINK_TIMEOUT_MS: u32 = 500; // немає команд довше — зв'язок втрачено

/// Що сталося за виклик [`CameraLink::feed`] / [`CameraLink::update`].
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LinkEvent {
    None,
    /// Почався постріл (серія або тестовий).
    Shot { pan10: i16, tilt10: i16, total: u32 },
}

/// Arduino в режимі камери: команди від ESP32 → турель.
pub struct CameraLink {
    pub turret: Turret,
    reader: LineReader,
    last_link_at: u32,
}

impl CameraLink {
    pub const fn new(cfg: TurretConfig) -> Self {
        CameraLink { turret: Turret::new(cfg), reader: LineReader::new(), last_link_at: 0 }
    }

    /// Байт з UART.
    pub fn feed(&mut self, c: u8, now: u32, armed: bool) -> LinkEvent {
        match self.reader.push(c).and_then(Command::parse) {
            Some(cmd) => self.handle(cmd, now, armed),
            None => LinkEvent::None, // сміття — ігноруємо, зв'язок не оновлюємо
        }
    }

    pub fn handle(&mut self, cmd: Command, now: u32, armed: bool) -> LinkEvent {
        let t = &mut self.turret;
        let mut ev = LinkEvent::None;
        match cmd {
            Command::Aim { pan10, tilt10, fire } => {
                t.aim(pan10 as i32, tilt10 as i32, now);
                t.request(fire, now);
            }
            Command::Move { pan10, tilt10 } => {
                t.aim(pan10 as i32, tilt10 as i32, now);
                t.request(false, now);
            }
            Command::TestShot { ms } => {
                if ms > 0 && t.test_shot(ms as u16, now, armed) {
                    ev = self.shot_event();
                }
            }
            Command::Big => t.big_object(now),
        }
        self.last_link_at = now;
        ev
    }

    pub fn link_ok(&self, now: u32) -> bool {
        elapsed(now, self.last_link_at) < LINK_TIMEOUT_MS
    }

    /// Викликати часто (~1 мс).
    pub fn update(&mut self, now: u32, armed: bool) -> LinkEvent {
        if !self.link_ok(now) {
            self.turret.cancel_request();
        }
        if self.turret.update(now, armed) {
            self.shot_event()
        } else {
            LinkEvent::None
        }
    }

    /// LED: немає зв'язку — швидко; ARM — повільно; SAFE — горить.
    pub fn led(&self, now: u32, armed: bool) -> bool {
        if !self.link_ok(now) {
            (now / 100) % 2 == 1
        } else if armed {
            (now / 250) % 2 == 1
        } else {
            true
        }
    }

    fn shot_event(&self) -> LinkEvent {
        let t = &self.turret;
        LinkEvent::Shot { pan10: t.pan10, tilt10: t.tilt10, total: t.shots }
    }
}

#[cfg(test)]
mod tests {
    //! Порт tools/flysonar_camera_test.cpp: протокол з ESP32, серії, кулдаун,
    //! блокування великих об'єктів, втрата зв'язку, ARM.
    use super::*;
    use alloc::format;

    #[test]
    fn parse_and_format_round_trip() {
        for cmd in [
            Command::Aim { pan10: 973, tilt10: -20, fire: true },
            Command::Move { pan10: 0, tilt10: 1800 },
            Command::TestShot { ms: 40 },
            Command::Big,
        ] {
            assert_eq!(Command::parse(format!("{cmd}").as_bytes()), Some(cmd));
        }
        assert_eq!(Command::parse(b"A12 7   0"), Some(Command::Aim { pan10: 12, tilt10: 7, fire: false }));
        assert_eq!(Command::parse(b"A 12"), None);
        assert_eq!(Command::parse(b"XYZ"), None);
        assert_eq!(Command::parse(b"F"), None);
    }

    #[test]
    fn line_reader_drops_overlong_lines() {
        let mut r = LineReader::new();
        let mut lines = alloc::vec::Vec::new();
        for &c in b"A 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18\r\nB\r\n\n" {
            if let Some(l) = r.push(c) {
                lines.push(l.to_vec());
            }
        }
        assert_eq!(lines, [b"B".to_vec()]);
    }

    struct Sim {
        link: CameraLink,
        now: u32,
        arm: bool,
        starts: u32,
        pump: bool,
    }

    impl Sim {
        fn new() -> Self {
            Sim { link: CameraLink::new(TurretConfig::DEFAULT), now: 1000, arm: false, starts: 0, pump: false }
        }

        fn send(&mut self, s: &str) {
            for &c in s.as_bytes() {
                if let LinkEvent::Shot { .. } = self.link.feed(c, self.now, self.arm) {
                    self.starts += 1;
                }
            }
        }

        /// Один прохід loop() Arduino.
        fn tick(&mut self) {
            if let LinkEvent::Shot { .. } = self.link.update(self.now, self.arm) {
                self.starts += 1;
            }
            self.pump = self.link.turret.valve_on;
        }

        /// Крутимо loop() з кроком 1 мс; ESP шле `line` кожні 33 мс (~30 кадр/с).
        fn run(&mut self, ms: u32, line: Option<&str>) {
            for i in 0..ms {
                if let Some(l) = line {
                    if i % 33 == 0 {
                        self.send(l);
                    }
                }
                self.tick();
                self.now += 1;
            }
        }
    }

    const FIRE: Option<&str> = Some("A 900 900 1\n");

    #[test]
    fn bursts_cooldown_and_safe() {
        let mut s = Sim::new();
        s.run(1000, FIRE);
        assert_eq!(s.starts, 0, "fired while SAFE");

        s.arm = true;
        s.run(100, FIRE);
        assert_eq!(s.starts, 1, "expected first shot");
        s.run(2000, FIRE);
        assert_eq!(s.starts, 3, "burst should cap at 3");
        s.run(1100, FIRE); // серія ~0.7 с + кулдаун 2.5 с ще не минули
        assert_eq!(s.starts, 3, "fired during cooldown");
        s.run(200, FIRE);
        assert_eq!(s.starts, 4, "after cooldown expected 4th shot");
    }

    #[test]
    fn big_object_hold() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(3000, None);
        s.send("B\n");
        let before = s.starts;
        s.run(2900, FIRE);
        assert_eq!(s.starts, before, "fired during big-object hold");
        s.run(400, FIRE);
        assert!(s.starts > before, "did not resume after hold");
    }

    #[test]
    fn fires_only_after_servo_settles() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(3000, None);
        let before = s.starts;
        let t0 = s.now;
        while s.starts == before && s.now - t0 < 1000 {
            if (s.now - t0).is_multiple_of(33) {
                s.send("A 300 900 1\n");
            }
            s.tick();
            s.now += 1;
        }
        assert!(s.now - t0 >= 130, "fired {} ms after a 60 deg move", s.now - t0);
        assert_eq!(s.link.turret.pan10, 300);
    }

    #[test]
    fn stale_request_and_lost_link() {
        let mut s = Sim::new();
        s.arm = true;
        s.send("M 300 900\n"); // серво вже на місці: без паузи на доворот
        s.run(3000, None);
        s.send("A 300 900 1\n");
        s.run(10, None);
        let after = s.starts;
        s.run(3000, None);
        assert_eq!(s.starts, after, "fired on a stale request");
        assert!(!s.link.link_ok(s.now));
    }

    #[test]
    fn garbage_does_not_move_servo() {
        let mut s = Sim::new();
        s.send("M 1200 900\n");
        let before = s.link.turret.pan10;
        s.send("A 12\nXYZ\nA 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18\n");
        s.run(50, None);
        assert_eq!(s.link.turret.pan10, before, "garbage moved servo");
        s.send("M 600 900\n");
        assert_eq!(s.link.turret.pan10, 600, "M not applied");
    }

    #[test]
    fn disarm_mid_shot_and_test_shot() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(3000, None);
        let t0 = s.now;
        while !s.pump && s.now - t0 < 500 {
            if (s.now - t0).is_multiple_of(33) {
                s.send("A 1200 900 1\n");
            }
            s.tick();
            s.now += 1;
        }
        assert!(s.pump, "no shot to interrupt");
        s.arm = false;
        s.tick();
        assert!(!s.pump, "pump still on after disarm");

        // тестовий постріл: лише з ARM і не довше test_shot_max_ms
        let before = s.starts;
        s.send("F 150\n");
        s.run(300, None);
        assert_eq!(s.starts, before, "test shot while SAFE");
        s.arm = true;
        s.send("F 5000\n");
        let mut on = 0;
        for _ in 0..600 {
            s.tick();
            if s.pump {
                on += 1;
            }
            s.now += 1;
        }
        assert!((198..=202).contains(&on), "test shot {on} ms, expected 200");
    }
}
