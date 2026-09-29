//! FlyClap — автоматична "хлопавка" для мух на Arduino Uno / Nano (ATmega328P).
//!
//! Муха перетинає ІЧ-промінь → переривання PCINT2 одразу подає імпульс на
//! соленоїд-засувку (без очікування головного циклу) → пружини схлопують
//! пластини → серво знову взводить механізм. Машина станів — у
//! [`flycore::flyclap`]; тут лише залізо.
//!
//! Пінаут:
//!   D3      — (лише `tssp`) несуча 38 кГц на ключ ІЧ-світлодіодів (OC2B)
//!   D4..D7  — входи завіси (промінь цілий = LOW, перекритий = HIGH):
//!             LM339 з гістерезисом, або TSSP4038 (feature `tssp`)
//!   D8      — кінцевик "взведено" (pull-up, LOW = засувка зачеплена)
//!   D9      — затвор MOSFET соленоїда (IRLZ44N)
//!   D10     — сервопривід взведення (MG996R), апаратний PWM Timer1 (OC1B)
//!   D11     — п'єзо-бузер: пасивний (тон з Timer2, OC2A); для `tssp` — активний
//!   D13     — статусний світлодіод
//!   A0      — тумблер ARM (pull-up, LOW = озброєно)
//!
//! Таймери: Timer0 — millis(), Timer1 — серво 50 Гц, Timer2 — бузер або несуча ІЧ.

#![no_std]
#![no_main]
#![feature(abi_avr_interrupt)]

use core::cell::Cell;
use core::convert::Infallible;
use core::sync::atomic::{AtomicBool, Ordering};

use arduino_hal::hal::port::{mode, Pin};
use arduino_hal::pac;
use arduino_hal::prelude::*;
use avr_device::interrupt::{self, Mutex};
use flycore::flyclap::{Event, FlyClap, Inputs, Io};

const BEAM_MASK: u8 = 0b1111_0000; // PD4..PD7
const CONFIRM_US: u32 = 30; // повторне читання в ISR проти імпульсних завад
const EEPROM_ADDR_CLAPS: u16 = 0; // u32 LE — той самий формат, що й EEPROM.put() у C++-версії

// ---------------- Спільне з перериваннями ----------------

static MILLIS: Mutex<Cell<u32>> = Mutex::new(Cell::new(0));
/// Дозвіл стріляти прямо з ISR.
static TRIGGER_ARMED: AtomicBool = AtomicBool::new(false);
/// Коли ISR вистрілив (None — ще ні з моменту озброєння).
static FIRED_AT: Mutex<Cell<Option<u32>>> = Mutex::new(Cell::new(None));

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

fn beams() -> u8 {
    // SAFETY: лише читання PIND, без побічних ефектів
    unsafe { &*pac::PORTD::ptr() }.pind().read().bits() & BEAM_MASK
}

// ---------------- ISR: мінімальна латентність спрацювання ----------------

#[avr_device::interrupt(atmega328p)]
fn PCINT2() {
    if !TRIGGER_ARMED.load(Ordering::SeqCst) {
        return;
    }
    if beams() == 0 {
        return; // це був фронт відновлення променя
    }
    arduino_hal::delay_us(CONFIRM_US); // короткий антидребезг проти ЕМ-завад
    if beams() == 0 {
        return;
    }

    // Стріляємо одразу, без очікування головного циклу. PORTB з головного
    // циклу змінюється лише в interrupt::free, тож гонки read-modify-write немає.
    // SAFETY: ISR виконується з вимкненими перериваннями.
    unsafe { &*pac::PORTB::ptr() }.portb().modify(|_, w| w.pb1().set_bit());
    TRIGGER_ARMED.store(false, Ordering::SeqCst);
    interrupt::free(|cs| FIRED_AT.borrow(cs).set(Some(millis())));
}

// Паніка: насамперед знеструмити соленоїд (інакше перегріється) і замовкнути.
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    interrupt::disable();
    // SAFETY: переривання вимкнені, далі нічого не виконується
    let dp = unsafe { pac::Peripherals::steal() };
    dp.PORTB.portb().modify(|_, w| w.pb1().clear_bit().pb3().clear_bit());
    dp.TC2.tccr2a().reset();
    loop {}
}

// ---------------- Залізо ----------------

struct Board<W> {
    serial: W,
    tc1: pac::TC1,
    tc2: pac::TC2,
    eeprom: arduino_hal::Eeprom,
    solenoid: Pin<mode::Output>,
    led: Pin<mode::Output>,
    #[cfg(feature = "tssp")]
    buzzer: Pin<mode::Output>,
}

impl<W> Board<W> {
    /// Timer1: Fast PWM, TOP = ICR1, 16 МГц / 8 = 0.5 мкс на тік, 40000 тіків = 20 мс.
    /// Вихід OC1B (D10) підключається лише під час руху серво.
    fn setup_servo_timer(&self) {
        self.tc1.icr1().write(|w| w.set(39_999));
        self.tc1.tccr1a().write(|w| w.wgm1().set(0b10).com1b().disconnected());
        self.tc1.tccr1b().write(|w| w.wgm1().set(0b11).cs1().prescale_8());
    }

    /// Timer2, Fast PWM з TOP = OCR2A: 16 МГц / 8 / (52 + 1) ≈ 37.7 кГц,
    /// шпаруватість ~50 % на OC2B (D3).
    #[cfg(feature = "tssp")]
    fn setup_ir_carrier(&self) {
        self.tc2.ocr2a().write(|w| w.set(52));
        self.tc2.ocr2b().write(|w| w.set(26));
        self.tc2.tccr2a().write(|w| w.wgm2().pwm_fast().com2b().match_clear());
        self.tc2.tccr2b().write(|w| w.wgm22().set_bit().cs2().prescale_8());
    }

    /// Тон на OC2A (D11): CTC з перемиканням виходу, f = 16 МГц / (2·N·(OCR2A+1)).
    #[cfg(not(feature = "tssp"))]
    fn tone(&self, hz: u16) {
        const PRESCALERS: [u32; 6] = [8, 32, 64, 128, 256, 1024];
        let hz = hz.max(31) as u32;
        let (idx, top) = PRESCALERS
            .iter()
            .map(|&n| 16_000_000 / (2 * n * hz))
            .enumerate()
            .find(|&(_, top)| top <= 256)
            .unwrap_or((PRESCALERS.len() - 1, 256));
        self.tc2.ocr2a().write(|w| w.set((top.max(1) - 1) as u8));
        self.tc2.tccr2a().write(|w| w.wgm2().ctc().com2a().match_toggle());
        self.tc2.tccr2b().write(|w| match idx {
            0 => w.cs2().prescale_8(),
            1 => w.cs2().prescale_32(),
            2 => w.cs2().prescale_64(),
            3 => w.cs2().prescale_128(),
            4 => w.cs2().prescale_256(),
            _ => w.cs2().prescale_1024(),
        });
    }

    #[cfg(not(feature = "tssp"))]
    fn tone_off(&self) {
        self.tc2.tccr2b().write(|w| w.cs2().no_clock());
        self.tc2.tccr2a().reset(); // OC2A відключено — D11 повертається до PORTB3 = LOW
    }

    fn load_claps(&self) -> u32 {
        let mut b = [0u8; 4];
        for (i, v) in b.iter_mut().enumerate() {
            *v = self.eeprom.read_byte(EEPROM_ADDR_CLAPS + i as u16);
        }
        match u32::from_le_bytes(b) {
            u32::MAX => 0, // чиста EEPROM
            n => n,
        }
    }
}

impl<W: ufmt::uWrite<Error = Infallible>> Io for Board<W> {
    fn solenoid_off(&mut self) {
        interrupt::free(|_| self.solenoid.set_low());
    }

    fn servo_write(&mut self, deg: u8) {
        let us = (544 + deg.min(180) as u32 * (2400 - 544) / 180) as u16; // як Servo::write()
        self.tc1.ocr1b().write(|w| w.set(us * 2)); // спершу кут, щоб не смикнуло
        self.tc1.tccr1a().modify(|_, w| w.com1b().match_clear());
    }

    fn servo_detach(&mut self) {
        // вихід відключається миттєво, навіть посеред імпульсу: D10 = PORTB2 = LOW
        self.tc1.tccr1a().modify(|_, w| w.com1b().disconnected());
    }

    #[cfg(not(feature = "tssp"))]
    fn buzzer(&mut self, freq_hz: Option<u16>) {
        match freq_hz {
            Some(hz) => self.tone(hz),
            None => self.tone_off(),
        }
    }

    // Активний бузер: частота фіксована, важливо лише "пищить / ні"
    #[cfg(feature = "tssp")]
    fn buzzer(&mut self, freq_hz: Option<u16>) {
        interrupt::free(|_| {
            if freq_hz.is_some() {
                self.buzzer.set_high()
            } else {
                self.buzzer.set_low()
            }
        });
    }

    fn led(&mut self, on: bool) {
        interrupt::free(|_| if on { self.led.set_high() } else { self.led.set_low() });
    }

    fn arm_trigger(&mut self) {
        interrupt::free(|cs| {
            FIRED_AT.borrow(cs).set(None);
            TRIGGER_ARMED.store(true, Ordering::SeqCst);
        });
    }

    fn disarm_trigger(&mut self) {
        TRIGGER_ARMED.store(false, Ordering::SeqCst);
    }

    fn save_claps(&mut self, claps: u32) {
        // як EEPROM.put(): пишемо лише змінені байти — ресурс комірок бережемо
        for (i, &v) in claps.to_le_bytes().iter().enumerate() {
            let addr = EEPROM_ADDR_CLAPS + i as u16;
            if self.eeprom.read_byte(addr) != v {
                self.eeprom.write_byte(addr, v);
            }
        }
    }

    fn event(&mut self, ev: Event) {
        ufmt::uwrite!(&mut self.serial, "{}\r\n", ev).unwrap_infallible();
    }
}

#[arduino_hal::entry]
fn main() -> ! {
    let dp = arduino_hal::Peripherals::take().unwrap();
    let pins = arduino_hal::pins!(dp);
    let serial = arduino_hal::default_serial!(dp, pins, 115200);

    // Timer0: CTC, 16 МГц / 64 / 250 = 1 кГц → millis()
    dp.TC0.tccr0a().write(|w| w.wgm0().ctc());
    dp.TC0.ocr0a().write(|w| w.set(249));
    dp.TC0.tccr0b().write(|w| w.cs0().prescale_64());
    dp.TC0.timsk0().write(|w| w.ocie0a().set_bit());

    let cocked = pins.d8.into_pull_up_input();
    let arm = pins.a0.into_pull_up_input();
    pins.d10.into_output(); // серво: при відключеному OC1B пін тримає LOW

    // Входи завіси D4..D7
    #[cfg(feature = "tssp")]
    let _beams = (
        // TSSP має слабку власну підтяжку — додаємо внутрішню
        pins.d4.into_pull_up_input(),
        pins.d5.into_pull_up_input(),
        pins.d6.into_pull_up_input(),
        pins.d7.into_pull_up_input(),
    );
    #[cfg(not(feature = "tssp"))]
    let _beams = (
        // без внутрішніх підтяжок: підтяжки на платі компаратора
        pins.d4.into_floating_input(),
        pins.d5.into_floating_input(),
        pins.d6.into_floating_input(),
        pins.d7.into_floating_input(),
    );

    #[cfg(not(feature = "tssp"))]
    pins.d11.into_output(); // пасивний бузер на OC2A

    let mut board = Board {
        serial,
        tc1: dp.TC1,
        tc2: dp.TC2,
        eeprom: arduino_hal::Eeprom::new(dp.EEPROM),
        solenoid: pins.d9.into_output().downgrade(),
        led: pins.d13.into_output().downgrade(),
        #[cfg(feature = "tssp")]
        buzzer: pins.d11.into_output().downgrade(),
    };
    board.setup_servo_timer();

    // SAFETY: усі спільні з ISR дані — атомарні або під interrupt::Mutex
    unsafe { interrupt::enable() };

    #[cfg(feature = "tssp")]
    {
        pins.d3.into_output();
        board.setup_ir_carrier();
        arduino_hal::delay_ms(50); // АРУ приймачів встановлюється на несучу
    }

    // Переривання на зміну D4..D7 (PCINT20..23, група PCIE2 — порт D)
    dp.EXINT.pcmsk2().write(|w| w.set(BEAM_MASK));
    dp.EXINT.pcicr().write(|w| w.pcie().set(0b100));

    let mut clap = FlyClap::new(board.load_claps());
    clap.begin(millis(), &mut board);

    loop {
        let inputs = Inputs {
            arm_switch: arm.is_low(),
            cocked: cocked.is_low(),
            beams_blocked: beams() != 0,
            fired_at: interrupt::free(|cs| FIRED_AT.borrow(cs).get()), // 32 біти з ISR — атомарно
        };
        clap.step(millis(), inputs, &mut board);
    }
}
