//! Хост-симуляція сонарного режиму: віртуальна кімната зі стіною, ціль і модель
//! пелюстки HC-SR04. Перевіряє, що уточнення центроїдом наводить точніше за крок
//! сітки, і що фільтри не стріляють по руці, по цілі в польоті та в режимі SAFE.
//! (Порт tools/flysonar_sonar_test.cpp.)

use super::*;
use crate::servo::{US_MAX, US_MIN};
use std::println;

const WALL_CM: f32 = 80.0;
const BEAM_HALF_DEG: f32 = 8.0; // ефективна напівширина пелюстки для малої цілі

#[derive(Clone, Copy)]
struct Obj {
    on: bool,
    pan: f32,
    tilt: f32,
    half_deg: f32,
    cm: f32,
    moving: bool,
}

const NONE: Obj = Obj { on: false, pan: 0.0, tilt: 0.0, half_deg: 0.0, cm: 0.0, moving: false };

/// Віртуальне залізо. Час віртуальний; кожен виклик `millis()` додає 1 мс,
/// щоб активні очікування завершувались.
struct Room {
    ms: u32,
    pan_us: u16,
    tilt_us: u16,
    pump: bool,
    arm: bool,
    target: Obj,
    ping_no: u32,
    shots: u32,
    last_shot: (f32, f32),
}

fn deg(us: u16) -> f32 {
    (us as f32 - US_MIN as f32) * 180.0 / (US_MAX - US_MIN) as f32
}

impl Room {
    fn echo_us(&mut self) -> u32 {
        self.ping_no += 1;
        let (p, t) = (deg(self.pan_us), deg(self.tilt_us));
        let o = self.target;
        if o.on {
            let reach = BEAM_HALF_DEG + o.half_deg;
            if (p - o.pan).abs() <= reach && (t - o.tilt).abs() <= reach {
                let cm = if o.moving { o.cm + if self.ping_no % 2 == 1 { 4.0 } else { -4.0 } } else { o.cm };
                return (cm * 58.0) as u32;
            }
        }
        (WALL_CM * 58.0) as u32
    }
}

impl SonarHal for Room {
    fn millis(&mut self) -> u32 {
        self.ms += 1;
        self.ms - 1
    }
    fn delay_ms(&mut self, ms: u16) {
        self.ms += ms as u32;
    }
    fn echo_high(&mut self) -> bool {
        false
    }
    fn ping_us(&mut self, timeout_us: u16) -> u16 {
        let mut us = self.echo_us();
        if us > timeout_us as u32 {
            us = 0;
        }
        self.ms += if us > 0 { us } else { timeout_us as u32 } / 1000;
        us as u16
    }
    fn armed(&mut self) -> bool {
        self.arm
    }
    fn recal_pressed(&mut self) -> bool {
        false
    }
    fn led(&mut self, _: bool) {}
    fn pump(&mut self, on: bool) {
        if on && !self.pump {
            self.shots += 1;
            self.last_shot = (deg(self.pan_us), deg(self.tilt_us));
        }
        self.pump = on;
    }
    fn servos_us(&mut self, pan_us: u16, tilt_us: u16) {
        self.pan_us = pan_us;
        self.tilt_us = tilt_us;
    }
    fn event(&mut self, ev: SonarEvent) {
        if std::env::var_os("SERIAL_ECHO").is_some() {
            println!("{ev:?}");
        }
    }
}

struct Sim {
    sonar: Sonar,
    room: Room,
}

impl Sim {
    fn new() -> Self {
        let mut s = Sim {
            sonar: Sonar::new(SonarConfig::PUMP),
            room: Room {
                ms: 1000,
                pan_us: 1472,
                tilt_us: 1472,
                pump: false,
                arm: true,
                target: NONE,
                ping_no: 0,
                shots: 0,
                last_shot: (0.0, 0.0),
            },
        };
        s.sonar.begin(&mut s.room); // калібрування фону по порожній кімнаті
        s
    }

    /// Прогнати скан до першого пострілу (або `timeout_ms` віртуального часу).
    fn run_until_shot(&mut self, timeout_ms: u32) -> Option<(f32, f32)> {
        let s0 = self.room.shots;
        let t0 = self.room.ms;
        while self.room.shots == s0 && self.room.ms - t0 < timeout_ms {
            self.sonar.step(&mut self.room);
        }
        (self.room.shots != s0).then_some(self.room.last_shot)
    }

    fn cooldown_all(&mut self, extra_ms: u32) {
        for t in 0..TILT_CELLS {
            self.sonar.cooldown_until[t] = self.room.ms + extra_ms;
        }
    }

    fn set_target(&mut self, pan: f32, tilt: f32, half_deg: f32, cm: f32, moving: bool) {
        self.room.target = Obj { on: true, pan, tilt, half_deg, cm, moving };
    }
}

fn expected_lift() -> f32 {
    (SonarConfig::PUMP.ballistic_lift10(30) + TILT_NOZZLE_OFFSET10) as f32 / 10.0
}

#[test]
fn still_fly_between_grid_nodes_is_hit_precisely() {
    let mut s = Sim::new();
    s.set_target(97.3, 101.0, 0.5, 30.0, false);
    let (sp, st) = s.run_until_shot(120_000).expect("no shot at a still fly");
    let (e_pan, e_tilt) = ((sp - 97.3).abs(), (st - expected_lift() - 101.0).abs());
    println!("still fly: aim err pan={e_pan:.2} tilt={e_tilt:.2} deg (grid-only would be 2.3 / 6.0)");
    assert!(e_pan <= 1.0, "pan error {e_pan:.2} deg");
    assert!(e_tilt <= 1.5, "tilt error {e_tilt:.2} deg");
}

#[test]
fn aim_error_is_small_across_the_field_of_view() {
    let mut s = Sim::new();
    let positions = [(45.5, 82.0), (62.0, 90.0), (88.8, 104.0), (120.2, 86.0), (141.0, 111.0), (33.0, 96.0), (74.4, 108.7)];
    let (mut worst_pan, mut worst_tilt) = (0f32, 0f32);
    for (pan, tilt) in positions {
        s.cooldown_all(3000); // добити попередню серію
        s.room.ms += 3000;
        s.set_target(pan, tilt, 0.5, 30.0, false);
        let (sp, st) = s.run_until_shot(120_000).unwrap_or_else(|| panic!("no shot at {pan}/{tilt}"));
        worst_pan = worst_pan.max((sp - pan).abs());
        worst_tilt = worst_tilt.max((st - expected_lift() - tilt).abs());
    }
    println!("7 positions: worst aim err pan={worst_pan:.2} tilt={worst_tilt:.2} deg");
    assert!(worst_pan <= 1.0 && worst_tilt <= 1.5, "worst error pan={worst_pan:.2} tilt={worst_tilt:.2}");
}

#[test]
fn does_not_fire_at_a_hand() {
    let mut s = Sim::new();
    s.set_target(90.0, 95.0, 20.0, 30.0, false);
    assert!(s.run_until_shot(30_000).is_none(), "fired at a hand");

    // рука далі (50 см, вужча в кутах)
    let mut s = Sim::new();
    s.set_target(70.0, 95.0, 6.0, 50.0, false);
    assert!(s.run_until_shot(30_000).is_none(), "fired at a hand at 50 cm");
}

#[test]
fn does_not_fire_at_a_moving_target_when_static_only() {
    let mut s = Sim::new();
    s.set_target(110.0, 95.0, 0.5, 30.0, true);
    assert!(s.run_until_shot(30_000).is_none(), "fired at a moving target with static_only");
}

#[test]
fn does_not_fire_while_safe() {
    let mut s = Sim::new();
    s.room.arm = false;
    s.set_target(97.3, 101.0, 0.5, 30.0, false);
    assert!(s.run_until_shot(30_000).is_none(), "fired while SAFE");
}

#[test]
fn median_and_filters() {
    assert_eq!(median3(3, 1, 2), 2);
    assert_eq!(median3(NO_ECHO, 30, 31), 31);
    assert!(is_foreground(30, 80));
    assert!(is_foreground(30, NO_ECHO));
    assert!(!is_foreground(75, 80));
    assert!(!is_foreground(5, 80), "dead zone");
    assert!(!is_foreground(NO_ECHO, 80));
    assert!(same_target(33, 30) && !same_target(40, 30) && !same_target(NO_ECHO, 30));
}
