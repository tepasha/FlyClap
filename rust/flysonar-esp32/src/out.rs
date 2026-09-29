//! Вихід зору: або власна турель (самостійна роль), або UART на Arduino ("очі").

/// Куди зір віддає рішення.
pub trait Output {
    /// Навестись (кути в 0.1°); `fire` — можна стріляти.
    fn aim(&mut self, pan10: i32, tilt10: i32, fire: bool);
    /// Просто навестись (калібрування).
    fn move_to(&mut self, pan10: i32, tilt10: i32) {
        self.aim(pan10, tilt10, false);
    }
    /// Тестовий постріл (калібрування).
    fn test_shot(&mut self, ms: u16);
    /// У кадрі великий об'єкт: не стріляти.
    fn big_object(&mut self);
    /// Викликається з головного циклу (лог пострілів).
    fn poll(&mut self) {}
}

#[cfg(feature = "eyes")]
pub use eyes::LinkOut as Out;
#[cfg(not(feature = "eyes"))]
pub use standalone::TurretOut as Out;

#[cfg(feature = "eyes")]
mod eyes {
    use core::fmt::Write;

    use esp_idf_svc::hal::gpio::AnyIOPin;
    use esp_idf_svc::hal::prelude::*;
    use esp_idf_svc::hal::uart::{self, UartTxDriver, UART1};
    use flycore::link::Command;
    use flycore::servo::clamp10;

    use super::Output;
    use crate::board::IoPins;

    /// Команди на Arduino (flysonar з feature `camera`), протокол — flycore::link.
    pub struct LinkOut(UartTxDriver<'static>);

    impl LinkOut {
        pub fn new(uart: UART1, pins: IoPins) -> anyhow::Result<Self> {
            let cfg = uart::config::Config::default().baudrate(Hertz(115_200));
            let tx = UartTxDriver::new(uart, pins.link_tx, None::<AnyIOPin>, None::<AnyIOPin>, &cfg)?;
            Ok(LinkOut(tx))
        }

        fn send(&mut self, cmd: Command) {
            let _ = writeln!(self.0, "{cmd}");
        }
    }

    impl Output for LinkOut {
        fn aim(&mut self, pan10: i32, tilt10: i32, fire: bool) {
            self.send(Command::Aim { pan10: clamp10(pan10), tilt10: clamp10(tilt10), fire });
        }

        fn move_to(&mut self, pan10: i32, tilt10: i32) {
            self.send(Command::Move { pan10: clamp10(pan10), tilt10: clamp10(tilt10) });
        }

        fn test_shot(&mut self, ms: u16) {
            self.send(Command::TestShot { ms: ms.min(i16::MAX as u16) as i16 });
        }

        fn big_object(&mut self) {
            self.send(Command::Big);
        }
    }
}

#[cfg(not(feature = "eyes"))]
mod standalone {
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};

    use esp_idf_svc::hal::cpu::Core;
    use esp_idf_svc::hal::delay::FreeRtos;
    use esp_idf_svc::hal::gpio::{Level, PinDriver, Pull};
    use esp_idf_svc::hal::ledc::{config::TimerConfig, LedcDriver, LedcTimerDriver, Resolution, LEDC};
    use esp_idf_svc::hal::prelude::*;
    use esp_idf_svc::hal::task::thread::ThreadSpawnConfiguration;
    use flycore::servo::deg10_to_us;
    use flycore::turret::{Turret, TurretConfig};

    use super::Output;
    use crate::board::{IoPins, LED_ACTIVE_LOW};
    use crate::millis;

    const SERVO_HZ: u32 = 50;
    const SERVO_BITS: u32 = 14; // 20 мс / 16384 ≈ 1.2 мкс ≈ 0.12°

    fn servo_duty(deg10: i16) -> u32 {
        deg10_to_us(deg10) as u32 * (1 << SERVO_BITS) / (1_000_000 / SERVO_HZ)
    }

    /// Одна плата — зір + серви + клапан + ARM. Серви й клапан обслуговує окрема
    /// задача кожну 1 мс, тож таймінг пострілу не залежить від кадрів камери.
    pub struct TurretOut {
        turret: Arc<Mutex<Turret>>,
        armed: Arc<AtomicBool>,
        logged: u32,
    }

    impl TurretOut {
        pub fn new(ledc: LEDC, pins: IoPins) -> anyhow::Result<Self> {
            let mut cfg = TurretConfig::DEFAULT;
            cfg.shot_ms = if cfg!(feature = "pump") { 120 } else { 50 };
            let turret = Arc::new(Mutex::new(Turret::new(cfg)));
            let armed = Arc::new(AtomicBool::new(false));

            // Серви — на явно заданому LEDC timer 2: timer 0 / channel 0 зайняті
            // XCLK камери, який драйвер камери налаштовує напряму. Таймер живе
            // весь час роботи, тож Box::leak дає обом каналам 'static-посилання.
            let timer: &'static _ = Box::leak(Box::new(LedcTimerDriver::new(
                ledc.timer2,
                &TimerConfig::default().frequency(SERVO_HZ.Hz()).resolution(Resolution::Bits14),
            )?));
            let mut pan = LedcDriver::new(ledc.channel4, timer, pins.pan)?;
            let mut tilt = LedcDriver::new(ledc.channel5, timer, pins.tilt)?;
            pan.set_duty(servo_duty(900))?;
            tilt.set_duty(servo_duty(900))?;
            let mut valve = PinDriver::output(pins.valve)?;
            valve.set_low()?;
            let mut arm = PinDriver::input(pins.arm)?;
            arm.set_pull(Pull::Up)?;
            let mut led = PinDriver::output(pins.led)?;

            // Ядро 0: зір і головний цикл живуть на ядрі 1
            ThreadSpawnConfiguration { name: Some(b"turret\0"), stack_size: 4096, priority: 3, pin_to_core: Some(Core::Core0), ..Default::default() }
                .set()?;
            let (t, a) = (turret.clone(), armed.clone());
            std::thread::spawn(move || {
                let (mut shown_pan, mut shown_tilt) = (900, 900);
                loop {
                    let now = millis();
                    let is_armed = arm.is_low();
                    a.store(is_armed, Ordering::Relaxed);
                    let (valve_on, p, tl) = {
                        let mut tu = t.lock().unwrap();
                        tu.update(now, is_armed);
                        (tu.valve_on, tu.pan10, tu.tilt10)
                    };
                    let _ = valve.set_level(Level::from(valve_on));
                    if p != shown_pan && pan.set_duty(servo_duty(p)).is_ok() {
                        shown_pan = p;
                    }
                    if tl != shown_tilt && tilt.set_duty(servo_duty(tl)).is_ok() {
                        shown_tilt = tl;
                    }
                    // LED: ARM — блимає, SAFE — горить
                    let on = if is_armed { (now / 250) % 2 == 1 } else { true };
                    let _ = led.set_level(Level::from(on != LED_ACTIVE_LOW));
                    FreeRtos::delay_ms(1);
                }
            });
            ThreadSpawnConfiguration::default().set()?;

            Ok(TurretOut { turret, armed, logged: 0 })
        }
    }

    impl Output for TurretOut {
        fn aim(&mut self, pan10: i32, tilt10: i32, fire: bool) {
            let now = millis();
            let mut t = self.turret.lock().unwrap();
            t.aim(pan10, tilt10, now);
            t.request(fire, now);
        }

        fn test_shot(&mut self, ms: u16) {
            let now = millis();
            let armed = self.armed.load(Ordering::Relaxed);
            if !self.turret.lock().unwrap().test_shot(ms, now, armed) {
                println!("Test shot refused: ARM off, servo moving or big object hold");
            }
        }

        fn big_object(&mut self) {
            self.turret.lock().unwrap().big_object(millis());
        }

        // Лог пострілів — з головного циклу, щоб не друкувати з задачі реального часу
        fn poll(&mut self) {
            let shots = self.turret.lock().unwrap().shots;
            if shots != self.logged {
                self.logged = shots;
                println!("PSSHT! total={shots}");
            }
        }
    }
}
