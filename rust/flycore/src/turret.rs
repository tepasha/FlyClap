//! Логіка турелі: коли можна стріляти. Спільна для ESP32-версії (самостійна
//! роль) і Arduino-версії в режимі камери (через [`crate::link::CameraLink`]).
//!
//! Постріл, лише якщо:
//!   * ARM увімкнено;
//!   * серво доїхало (пауза залежить від кута повороту);
//!   * дозвіл від зору свіжий (`<= fire_fresh_ms`);
//!   * не було великого об'єкта `big_hold_ms`;
//!   * не вичерпано серію `max_shots` / минув кулдаун.
//!
//! Вимкнення ARM або великий об'єкт зупиняють клапан посеред пострілу.
//! Кути — у десятих градуса (0..=1800). Структура лише рахує стан; застосовує
//! його до заліза (серви, клапан) викликаючий код.

use crate::servo::{clamp10, Settle};
use crate::time::{elapsed, reached};

#[derive(Clone, Copy, Debug)]
pub struct TurretConfig {
    pub shot_ms: u16,        // помпа; для клапана під тиском ~50
    pub shot_gap_ms: u16,    // між пострілами серії
    pub cooldown_ms: u16,    // після серії
    pub max_shots: u8,
    pub big_hold_ms: u16,    // тиша після великого об'єкта
    pub fire_fresh_ms: u16,  // дозвіл старший за це — ціль уже деінде
    pub test_shot_max_ms: u16,
    pub settle: Settle,
}

impl TurretConfig {
    pub const DEFAULT: TurretConfig = TurretConfig {
        shot_ms: 120,
        shot_gap_ms: 350,
        cooldown_ms: 2500,
        max_shots: 3,
        big_hold_ms: 3000,
        fire_fresh_ms: 150,
        test_shot_max_ms: 200,
        settle: Settle::MG90S,
    };
}

impl Default for TurretConfig {
    fn default() -> Self {
        Self::DEFAULT
    }
}

#[derive(Clone, Debug)]
pub struct Turret {
    pub cfg: TurretConfig,
    pub pan10: i16,
    pub tilt10: i16,
    pub valve_on: bool,
    /// Усього пострілів (включно з тестовими).
    pub shots: u32,
    want_fire: bool,
    last_fire_req_at: u32,
    settled_at: u32,
    next_shot_at: u32,
    big_hold_until: u32,
    valve_off_at: u32,
    burst: u8,
}

impl Default for Turret {
    fn default() -> Self {
        Self::new(TurretConfig::DEFAULT)
    }
}

impl Turret {
    pub const fn new(cfg: TurretConfig) -> Self {
        Turret {
            cfg,
            pan10: 900,
            tilt10: 900,
            valve_on: false,
            shots: 0,
            want_fire: false,
            last_fire_req_at: 0,
            settled_at: 0,
            next_shot_at: 0,
            big_hold_until: 0,
            valve_off_at: 0,
            burst: 0,
        }
    }

    /// Навести. true — кут змінився (треба оновити серви).
    pub fn aim(&mut self, pan10: i32, tilt10: i32, now: u32) -> bool {
        let (p, t) = (clamp10(pan10), clamp10(tilt10));
        if p == self.pan10 && t == self.tilt10 {
            return false;
        }
        let dist = p.abs_diff(self.pan10).max(t.abs_diff(self.tilt10));
        self.settled_at = now.wrapping_add(self.cfg.settle.ms(dist) as u32);
        self.pan10 = p;
        self.tilt10 = t;
        true
    }

    /// Дозвіл від зору (надходить з кожним кадром).
    pub fn request(&mut self, fire: bool, now: u32) {
        self.want_fire = fire;
        if fire {
            self.last_fire_req_at = now;
        }
    }

    /// Зір замовк (втрачено зв'язок): дозволу більше немає.
    pub fn cancel_request(&mut self) {
        self.want_fire = false;
    }

    /// Зір бачить великий об'єкт: клапан закривається одразу.
    pub fn big_object(&mut self, now: u32) {
        self.big_hold_until = now.wrapping_add(self.cfg.big_hold_ms as u32);
        self.want_fire = false;
        self.valve_on = false;
    }

    /// Тестовий постріл (калібрування). true — почали.
    pub fn test_shot(&mut self, ms: u16, now: u32, armed: bool) -> bool {
        if !armed || self.valve_on || ms == 0 || !reached(now, self.settled_at) || !reached(now, self.big_hold_until) {
            return false;
        }
        self.start_shot(ms.min(self.cfg.test_shot_max_ms), now);
        true
    }

    /// Викликати часто (~1 мс). true — цього разу почався постріл.
    pub fn update(&mut self, now: u32, armed: bool) -> bool {
        if self.valve_on && (reached(now, self.valve_off_at) || !armed) {
            self.valve_on = false;
        }

        let since_req = elapsed(now, self.last_fire_req_at);
        let fresh = self.want_fire && since_req < self.cfg.fire_fresh_ms as u32;
        if !self.want_fire && since_req > 1000 {
            self.burst = 0; // ціль пропала — нова серія
        }

        if fresh
            && armed
            && !self.valve_on
            && reached(now, self.settled_at)
            && reached(now, self.next_shot_at)
            && reached(now, self.big_hold_until)
        {
            self.start_shot(self.cfg.shot_ms, now);
            self.burst += 1;
            let pause = if self.burst >= self.cfg.max_shots {
                self.burst = 0;
                self.cfg.cooldown_ms
            } else {
                self.cfg.shot_gap_ms
            };
            self.next_shot_at = now.wrapping_add(pause as u32);
            return true;
        }
        false
    }

    fn start_shot(&mut self, ms: u16, now: u32) {
        self.valve_on = true;
        self.valve_off_at = now.wrapping_add(ms as u32);
        self.shots += 1;
    }
}

#[cfg(test)]
mod tests {
    //! Порт tools/turret_test.cpp: ті самі сценарії безпеки.
    use super::*;

    struct Sim {
        t: Turret,
        now: u32,
        arm: bool,
        starts: u32,
    }

    impl Sim {
        fn new() -> Self {
            Sim { t: Turret::default(), now: 1000, arm: false, starts: 0 }
        }

        /// Крок 1 мс; зір шле кадр кожні 33 мс.
        fn run_at(&mut self, ms: u32, frames: bool, pan: i32, tilt: i32) {
            for i in 0..ms {
                if frames && i % 33 == 0 {
                    self.t.aim(pan, tilt, self.now);
                    self.t.request(true, self.now);
                }
                if self.t.update(self.now, self.arm) {
                    self.starts += 1;
                }
                self.now += 1;
            }
        }

        fn run(&mut self, ms: u32, frames: bool) {
            self.run_at(ms, frames, 900, 900);
        }

        fn measure_shot(&mut self) -> u32 {
            let (mut on, mut i) = (0, 0);
            while i < 4000 && !self.t.valve_on {
                self.run(1, i % 33 == 0);
                i += 1;
            }
            while i < 4000 && self.t.valve_on {
                on += 1;
                self.run(1, i % 33 == 0);
                i += 1;
            }
            on
        }

        fn wait_for_shot(&mut self) {
            let mut i = 0;
            while !self.t.valve_on && i < 500 {
                self.run(1, true);
                i += 1;
            }
            assert!(self.t.valve_on, "no shot to interrupt");
        }
    }

    #[test]
    fn burst_cooldown_and_shot_length() {
        let mut s = Sim::new();
        s.run(1000, true);
        assert_eq!(s.starts, 0, "fired while SAFE");

        s.arm = true;
        s.run(100, true);
        assert_eq!(s.starts, 1, "expected first shot");
        s.run(2000, true);
        assert_eq!(s.starts, 3, "burst should cap at 3");
        s.run(1100, true);
        assert_eq!(s.starts, 3, "fired during cooldown");
        s.run(200, true);
        assert_eq!(s.starts, 4, "after cooldown expected 4th shot");

        let len = s.measure_shot();
        assert!(len + 1 >= 120 && len <= 121, "shot length {len}, expected 120");

        s.t.cfg.shot_ms = 50; // профіль клапана
        s.run(3000, false);
        let len = s.measure_shot();
        assert!((49..=51).contains(&len), "valve shot length {len}, expected 50");
    }

    #[test]
    fn big_object_holds_fire_and_closes_valve() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(3000, false);
        s.t.big_object(s.now);
        let before = s.starts;
        s.run(2900, true);
        assert_eq!(s.starts, before, "fired during big-object hold");
        s.run(400, true);
        assert!(s.starts > before, "did not resume after hold");

        s.run(3000, false);
        s.wait_for_shot();
        s.t.big_object(s.now);
        assert!(!s.t.valve_on, "valve still open after big object");
    }

    #[test]
    fn waits_for_servo_after_a_big_move() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(4000, false);
        let (before, t0) = (s.starts, s.now);
        while s.starts == before && s.now - t0 < 1000 {
            let frame = (s.now - t0).is_multiple_of(33);
            s.run_at(1, frame, 300, 900);
        }
        assert!(s.now - t0 >= 130, "fired {} ms after a 60 deg move", s.now - t0);
        assert_eq!(s.t.pan10, 300);
    }

    #[test]
    fn stale_request_is_ignored() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(3000, false);
        s.t.aim(310, 900, s.now);
        s.t.request(true, s.now);
        s.run(200, false); // один свіжий постріл допустимий
        let before = s.starts;
        s.run(3000, false);
        assert_eq!(s.starts, before, "fired on a stale request");
    }

    #[test]
    fn angles_are_clamped() {
        let mut t = Turret::default();
        t.aim(-50, 5000, 0);
        assert_eq!((t.pan10, t.tilt10), (0, 1800));
    }

    #[test]
    fn disarm_mid_shot_closes_valve() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(3000, false);
        s.wait_for_shot();
        s.arm = false;
        s.run(1, false);
        assert!(!s.t.valve_on, "valve open after disarm");
    }

    #[test]
    fn test_shot_needs_arm_and_is_capped() {
        let mut s = Sim::new();
        s.run(500, false);
        assert!(!s.t.test_shot(100, s.now, false), "test shot while SAFE");
        s.arm = true;
        assert!(s.t.test_shot(5000, s.now, true), "test shot refused");
        let mut on = 0;
        for _ in 0..600 {
            if s.t.valve_on {
                on += 1;
            }
            s.run(1, false);
        }
        assert!((199..=201).contains(&on), "test shot {on} ms, expected 200");
    }
}
