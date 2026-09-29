//! FlySonar — "турель" з водометом для мух (Arduino Uno / Nano).
//!
//! Два джерела цілей (feature `camera`):
//!
//! * без `camera` — СОНАР HC-SR04 на pan-tilt платформі разом із соплом;
//!   алгоритм (калібрування фону, змійка, фільтр ширини, центроїд, "сидить") —
//!   у [`flycore::sonar`].
//! * `camera` — ESP32-CAM з прошивкою `flysonar-esp32` (feature `eyes`) шукає комах
//!   і шле готові кути по UART у D0. Arduino — контролер реального часу: плавно
//!   наводить серви, відкриває клапан/помпу, тримає ARM, кулдаун і блокування
//!   великих об'єктів ([`flycore::link`]).
//!
//! Серви — апаратний PWM Timer1 (0.5 мкс на тік): роздільність краща за 0.1°.
//!
//! Пінаут:
//!   D0  — (камера) RX від ESP32 GPIO14. Від'єднувати на час прошивки Arduino!
//!   D2  — HC-SR04 TRIG           (сонар)
//!   D3  — HC-SR04 ECHO           (сонар)
//!   D4  — кнопка перекалібрування фону (сонар; pull-up, до GND)
//!   D5  — затвор MOSFET помпи або клапана (IRLZ44N)
//!   D9  — серво PAN  (горизонталь), OC1A
//!   D10 — серво TILT (вертикаль),   OC1B
//!   D13 — статус-LED
//!   A0  — тумблер ARM (pull-up, LOW = стріляти дозволено)

#![no_std]
#![no_main]
#![feature(abi_avr_interrupt)]

use core::cell::Cell;

use arduino_hal::pac;
use avr_device::interrupt::{self, Mutex};

static MILLIS: Mutex<Cell<u32>> = Mutex::new(Cell::new(0));

fn millis() -> u32 {
    interrupt::free(|cs| MILLIS.borrow(cs).get())
}

#[avr_device::interrupt(atmega328p)]
fn TIMER0_COMPA() {
    interrupt::free(|cs| {
        let c = MILLIS.borrow(cs);
        c.set(c.get().wrapping_add(1));
    })
}

// Паніка: насамперед закрити помпу/клапан.
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    interrupt::disable();
    // SAFETY: переривання вимкнені, далі нічого не виконується
    let dp = unsafe { pac::Peripherals::steal() };
    dp.PORTD.portd().modify(|_, w| w.pd5().clear_bit());
    loop {}
}

/// Timer0 → millis(); Timer1 → серви: Fast PWM, TOP = ICR1, 16 МГц / 8 = 0.5 мкс
/// на тік, 40000 тіків = 20 мс. Виходи OC1A/OC1B підключаються першим записом кута.
fn setup_timers(tc0: &pac::TC0, tc1: &pac::TC1) {
    tc0.tccr0a().write(|w| w.wgm0().ctc());
    tc0.ocr0a().write(|w| w.set(249)); // 16 МГц / 64 / 250 = 1 кГц
    tc0.tccr0b().write(|w| w.cs0().prescale_64());
    tc0.timsk0().write(|w| w.ocie0a().set_bit());

    tc1.icr1().write(|w| w.set(39_999));
    tc1.tccr1a().write(|w| w.wgm1().set(0b10));
    tc1.tccr1b().write(|w| w.wgm1().set(0b11).cs1().prescale_8());
}

/// Записати імпульси серв (мкс) і підключити виходи — спершу кут, щоб не смикнуло.
fn servos_us(tc1: &pac::TC1, pan_us: u16, tilt_us: u16) {
    tc1.ocr1a().write(|w| w.set(pan_us * 2));
    tc1.ocr1b().write(|w| w.set(tilt_us * 2));
    tc1.tccr1a().modify(|_, w| w.com1a().match_clear().com1b().match_clear());
}

#[arduino_hal::entry]
fn main() -> ! {
    let dp = arduino_hal::Peripherals::take().unwrap();
    let pins = arduino_hal::pins!(dp);
    let serial = arduino_hal::default_serial!(dp, pins, 115200);
    setup_timers(&dp.TC0, &dp.TC1);
    pins.d9.into_output();
    pins.d10.into_output();

    let io = Common {
        serial,
        tc1: dp.TC1,
        pump: pins.d5.into_output().downgrade(),
        led: pins.d13.into_output().downgrade(),
        arm: pins.a0.into_pull_up_input().downgrade(),
    };

    #[cfg(not(feature = "camera"))]
    sonar_mode::run(io, pins.d2.into_output().downgrade(), pins.d3.into_floating_input().downgrade(), pins.d4.into_pull_up_input().downgrade());
    #[cfg(feature = "camera")]
    camera_mode::run(io);
}

use arduino_hal::hal::port::{mode, Pin};

/// Залізо, спільне для обох режимів.
struct Common<W> {
    serial: W,
    tc1: pac::TC1,
    pump: Pin<mode::Output>,
    led: Pin<mode::Output>,
    arm: Pin<mode::Input<mode::PullUp>>,
}

impl<W> Common<W> {
    fn set(pin: &mut Pin<mode::Output>, on: bool) {
        if on {
            pin.set_high()
        } else {
            pin.set_low()
        }
    }
}

#[cfg(not(feature = "camera"))]
mod sonar_mode {
    use super::*;
    use arduino_hal::prelude::*;
    use core::convert::Infallible;
    use flycore::sonar::{Sonar, SonarConfig, SonarEvent, SonarHal};

    const SERVO_TICKS: u32 = 40_000; // період Timer1, 0.5 мкс на тік

    struct Hal<W> {
        c: Common<W>,
        trig: Pin<mode::Output>,
        echo: Pin<mode::Input<mode::Floating>>,
        recal: Pin<mode::Input<mode::PullUp>>,
    }

    impl<W> Hal<W> {
        fn ticks(&self) -> u16 {
            self.c.tc1.tcnt1().read().bits()
        }

        /// Півмікросекунд від `t0` за лічильником Timer1 (одне переповнення — ок:
        /// усі вимірювання коротші за 20 мс).
        fn since(&self, t0: u16) -> u32 {
            let now = self.ticks() as u32;
            let t0 = t0 as u32;
            if now >= t0 {
                now - t0
            } else {
                now + SERVO_TICKS - t0
            }
        }
    }

    impl<W: ufmt::uWrite<Error = Infallible>> SonarHal for Hal<W> {
        fn millis(&mut self) -> u32 {
            millis()
        }

        fn delay_ms(&mut self, ms: u16) {
            arduino_hal::delay_ms(ms as u32);
        }

        fn echo_high(&mut self) -> bool {
            self.echo.is_high()
        }

        // Як pulseIn(ECHO, HIGH, timeout), але час міряє Timer1, а не підрахунок тактів.
        fn ping_us(&mut self, timeout_us: u16) -> u16 {
            self.trig.set_low();
            arduino_hal::delay_us(2);
            self.trig.set_high();
            arduino_hal::delay_us(10);
            self.trig.set_low();

            let limit = timeout_us as u32 * 2;
            let t0 = self.ticks();
            while self.echo.is_high() {
                if self.since(t0) > limit {
                    return 0;
                }
            }
            while self.echo.is_low() {
                if self.since(t0) > limit {
                    return 0;
                }
            }
            let start = self.ticks();
            while self.echo.is_high() {
                if self.since(t0) > limit {
                    return 0;
                }
            }
            (self.since(start) / 2) as u16
        }

        fn armed(&mut self) -> bool {
            self.c.arm.is_low()
        }

        fn recal_pressed(&mut self) -> bool {
            self.recal.is_low()
        }

        fn led(&mut self, on: bool) {
            Common::<W>::set(&mut self.c.led, on);
        }

        fn pump(&mut self, on: bool) {
            Common::<W>::set(&mut self.c.pump, on);
        }

        fn servos_us(&mut self, pan_us: u16, tilt_us: u16) {
            servos_us(&self.c.tc1, pan_us, tilt_us);
        }

        fn event(&mut self, ev: SonarEvent) {
            ufmt::uwrite!(&mut self.c.serial, "{}\r\n", ev).unwrap_infallible();
        }
    }

    const CONFIG: SonarConfig = {
        let mut c = if cfg!(feature = "valve") { SonarConfig::VALVE } else { SonarConfig::PUMP };
        c.static_only = !cfg!(feature = "moving-targets");
        c
    };

    pub fn run<W: ufmt::uWrite<Error = Infallible>>(
        c: Common<W>,
        trig: Pin<mode::Output>,
        echo: Pin<mode::Input<mode::Floating>>,
        recal: Pin<mode::Input<mode::PullUp>>,
    ) -> ! {
        let mut hal = Hal { c, trig, echo, recal };
        // SAFETY: спільний з ISR лише лічильник millis під interrupt::Mutex
        unsafe { interrupt::enable() };

        let mut sonar = Sonar::new(CONFIG);
        sonar.begin(&mut hal);
        loop {
            sonar.step(&mut hal);
        }
    }
}

#[cfg(feature = "camera")]
mod camera_mode {
    use super::*;
    use arduino_hal::prelude::*;
    use core::cell::RefCell;
    use core::convert::Infallible;
    use flycore::link::{CameraLink, LinkEvent};
    use flycore::servo::{deg10_to_us, Deg10};
    use flycore::turret::TurretConfig;

    /// Прийом з ESP32 через переривання: поки лог пострілу (~3.5 мс) блокує TX,
    /// команди не губляться (як 64-байтний буфер Serial в Arduino).
    struct Ring {
        buf: [u8; 64],
        head: u8,
        tail: u8,
    }

    static RX: Mutex<RefCell<Ring>> = Mutex::new(RefCell::new(Ring { buf: [0; 64], head: 0, tail: 0 }));

    #[avr_device::interrupt(atmega328p)]
    fn USART_RX() {
        // SAFETY: читання UDR0 у власному перериванні приймача
        let b = unsafe { &*pac::USART0::ptr() }.udr0().read().bits();
        interrupt::free(|cs| {
            let mut r = RX.borrow(cs).borrow_mut();
            let next = (r.head + 1) % 64;
            if next != r.tail {
                let h = r.head as usize;
                r.buf[h] = b;
                r.head = next;
            } // переповнення — байт губиться, а задовгий рядок відкине LineReader
        });
    }

    fn rx_pop() -> Option<u8> {
        interrupt::free(|cs| {
            let mut r = RX.borrow(cs).borrow_mut();
            if r.head == r.tail {
                return None;
            }
            let b = r.buf[r.tail as usize];
            r.tail = (r.tail + 1) % 64;
            Some(b)
        })
    }

    const CONFIG: TurretConfig = {
        let mut c = TurretConfig::DEFAULT;
        c.shot_ms = if cfg!(feature = "valve") { 50 } else { 120 };
        c
    };

    pub fn run<W: ufmt::uWrite<Error = Infallible>>(mut c: Common<W>) -> ! {
        let mut link = CameraLink::new(CONFIG);
        let (mut shown_pan, mut shown_tilt) = (link.turret.pan10, link.turret.tilt10);
        servos_us(&c.tc1, deg10_to_us(shown_pan), deg10_to_us(shown_tilt));

        // SAFETY: спільні з ISR дані — під interrupt::Mutex
        unsafe {
            (*pac::USART0::ptr()).ucsr0b().modify(|_, w| w.rxcie0().set_bit());
            interrupt::enable();
        }
        ufmt::uwrite!(&mut c.serial, "FlySonar camera mode, waiting for ESP32 eyes...\r\n").unwrap_infallible();

        loop {
            let armed = c.arm.is_low();
            let mut ev = LinkEvent::None;
            while let Some(b) = rx_pop() {
                if let e @ LinkEvent::Shot { .. } = link.feed(b, millis(), armed) {
                    ev = e;
                }
            }
            let now = millis();
            if let e @ LinkEvent::Shot { .. } = link.update(now, armed) {
                ev = e;
            }

            let t = &link.turret;
            Common::<W>::set(&mut c.pump, t.valve_on);
            if (t.pan10, t.tilt10) != (shown_pan, shown_tilt) {
                (shown_pan, shown_tilt) = (t.pan10, t.tilt10);
                servos_us(&c.tc1, deg10_to_us(shown_pan), deg10_to_us(shown_tilt));
            }
            Common::<W>::set(&mut c.led, link.led(now, armed));

            if let LinkEvent::Shot { pan10, tilt10, total } = ev {
                ufmt::uwrite!(&mut c.serial, "PSSHT! pan={} tilt={} total={}\r\n", Deg10(pan10), Deg10(tilt10), total)
                    .unwrap_infallible();
            }
        }
    }
}
