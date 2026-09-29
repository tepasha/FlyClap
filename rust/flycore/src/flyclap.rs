//! FlyClap — автоматична "хлопавка" для мух: машина станів.
//!
//! Між двома пружинними пластинами проходить ІЧ-завіса з 4 променів. Муха
//! перетинає промінь → переривання в прошивці одразу відкриває соленоїд-засувку
//! (латентність — мікросекунди, без очікування головного циклу) → пружини
//! схлопують пластини (~15–25 мс) → сервопривід знову взводить механізм.
//!
//! Сам постріл робить ISR прошивки; ядро отримує лише знімок "вистрілило о
//! `fired_at`" ([`Inputs::fired_at`]) і веде все інше: взведення, очікування
//! чистої завіси, лічильник, аварії, бузер, LED.

use crate::time::{elapsed, reached};

pub const SOLENOID_PULSE_MS: u32 = 40; // час утримання засувки відкритою
pub const CLAP_SETTLE_MS: u32 = 300; // чекаємо, поки пластини схлопнуться і заспокояться
pub const SERVO_REST_DEG: u8 = 10; // важіль відведений, не заважає пластинам
pub const SERVO_COCK_DEG: u8 = 150; // важіль розводить пластини до зачеплення засувки
pub const COCK_TIMEOUT_MS: u32 = 1500; // не дочекались кінцевика → FAULT
pub const SERVO_RETURN_MS: u32 = 400; // час на відведення важеля; потім серво відключаємо
pub const BEAMS_CLEAR_MS: u32 = 500; // завіса має бути чистою стільки часу перед ARM
pub const BEAMS_STUCK_MS: u32 = 3000; // довше перекрито (прилипла муха/сміття) → FAULT

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Disarmed,
    WaitClear,
    Armed,
    Fired,
    Cocking,
    ServoReturn,
    Fault,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Fault {
    BeamBlocked,
    CockingTimeout,
}

impl Fault {
    pub fn message(self) -> &'static str {
        match self {
            Fault::BeamBlocked => "beam blocked too long - clean the gap",
            Fault::CockingTimeout => "cocking timeout - check latch/servo",
        }
    }
}

/// Що варто написати в лог.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Event {
    Ready { claps: u32 },
    Disarmed,
    Armed,
    /// Спрацювання, а не підтверджена муха (пил теж рахується).
    Clap { total: u32 },
    LatchLost,
    Fault(Fault),
}

#[cfg(feature = "ufmt")]
impl ufmt::uDisplay for Event {
    fn fmt<W: ufmt::uWrite + ?Sized>(&self, f: &mut ufmt::Formatter<'_, W>) -> Result<(), W::Error> {
        match *self {
            Event::Ready { claps } => ufmt::uwrite!(f, "FlyClap ready. Total claps: {}", claps),
            Event::Disarmed => f.write_str("Disarmed"),
            Event::Armed => f.write_str("ARMED"),
            Event::Clap { total } => ufmt::uwrite!(f, "CLAP! #{}", total),
            Event::LatchLost => f.write_str("Latch lost, re-cocking"),
            Event::Fault(why) => ufmt::uwrite!(f, "FAULT: {}", why.message()),
        }
    }
}

/// Знімок входів на цей прохід циклу.
#[derive(Clone, Copy, Default, Debug)]
pub struct Inputs {
    /// Тумблер ARM увімкнено.
    pub arm_switch: bool,
    /// Кінцевик: засувка зачеплена.
    pub cocked: bool,
    /// Хоч один промінь завіси перекрито.
    pub beams_blocked: bool,
    /// ISR вистрілив (і коли) з моменту останнього [`Io::arm_trigger`].
    pub fired_at: Option<u32>,
}

/// Виходи, які ядро просить застосувати.
pub trait Io {
    fn solenoid_off(&mut self);
    /// Кут серво; якщо воно відключене — підключити (спершу кут, потім імпульси).
    fn servo_write(&mut self, deg: u8);
    /// Відключити серво: у спокої не тремтить, не гуде, не їсть струм і не
    /// наводить завади на завісу.
    fn servo_detach(&mut self);
    /// `Some(Гц)` — пищати, `None` — тиша. Тривалість веде ядро.
    fn buzzer(&mut self, freq_hz: Option<u16>);
    fn led(&mut self, on: bool);
    /// Атомарно скинути "вистрілило" і дозволити ISR стріляти.
    fn arm_trigger(&mut self);
    /// Заборонити ISR стріляти.
    fn disarm_trigger(&mut self);
    fn save_claps(&mut self, claps: u32);
    fn event(&mut self, ev: Event);
}

pub struct FlyClap {
    state: State,
    state_since: u32,
    clear_since: u32,
    blocked_since: Option<u32>,
    claps: u32,
    servo_attached: bool,
    servo_target: u8,
    servo_moved_at: u32,
    buzzer_off_at: Option<u32>,
}

impl FlyClap {
    /// `claps` — лічильник з EEPROM.
    pub const fn new(claps: u32) -> Self {
        FlyClap {
            state: State::Disarmed,
            state_since: 0,
            clear_since: 0,
            blocked_since: None,
            claps,
            servo_attached: false,
            servo_target: SERVO_REST_DEG,
            servo_moved_at: 0,
            buzzer_off_at: None,
        }
    }

    pub fn state(&self) -> State {
        self.state
    }

    pub fn claps(&self) -> u32 {
        self.claps
    }

    pub fn begin(&mut self, now: u32, io: &mut impl Io) {
        self.servo_to(SERVO_REST_DEG, now, io); // відведе важіль і саме відключиться
        io.event(Event::Ready { claps: self.claps });
        self.enter(State::Disarmed, now);
    }

    /// Один прохід головного циклу.
    pub fn step(&mut self, now: u32, inp: Inputs, io: &mut impl Io) {
        // Тумблер ARM вимкнено — все зупиняємо в будь-якому стані (FAULT скидається так само)
        if !inp.arm_switch && self.state != State::Disarmed {
            io.disarm_trigger();
            io.solenoid_off();
            self.servo_to(SERVO_REST_DEG, now, io);
            io.event(Event::Disarmed);
            self.enter(State::Disarmed, now);
        }

        // Страховка: соленоїд ніколи не тримаємо довше імпульсу (захист від перегріву)
        if let Some(t) = inp.fired_at {
            if elapsed(now, t) >= SOLENOID_PULSE_MS {
                io.solenoid_off();
            }
        }

        self.servo_idle_detach(now, io);
        if let Some(t) = self.buzzer_off_at {
            if reached(now, t) {
                io.buzzer(None);
                self.buzzer_off_at = None;
            }
        }

        match self.state {
            State::Disarmed => {
                if inp.arm_switch {
                    self.beep(2000, 80, now, io);
                    self.clear_since = now;
                    if inp.cocked {
                        self.enter(State::WaitClear, now);
                    } else {
                        self.servo_to(SERVO_COCK_DEG, now, io);
                        self.enter(State::Cocking, now);
                    }
                }
            }

            // Озброюємося лише коли завіса стабільно чиста
            State::WaitClear => {
                if inp.beams_blocked {
                    self.clear_since = now;
                    let since = *self.blocked_since.get_or_insert(now);
                    if elapsed(now, since) > BEAMS_STUCK_MS {
                        self.blocked_since = None;
                        self.fault(Fault::BeamBlocked, now, io);
                    }
                } else {
                    self.blocked_since = None;
                    if elapsed(now, self.clear_since) >= BEAMS_CLEAR_MS {
                        io.arm_trigger();
                        self.beep(3000, 30, now, io);
                        io.event(Event::Armed);
                        self.enter(State::Armed, now);
                    }
                }
            }

            State::Armed => {
                if inp.fired_at.is_some() {
                    self.claps += 1;
                    io.save_claps(self.claps);
                    io.event(Event::Clap { total: self.claps });
                    self.enter(State::Fired, now);
                } else if !inp.cocked {
                    // засувка зірвалась сама (вібрація) — перевзводимо
                    io.disarm_trigger();
                    io.event(Event::LatchLost);
                    self.servo_to(SERVO_COCK_DEG, now, io);
                    self.enter(State::Cocking, now);
                }
            }

            State::Fired => {
                if elapsed(now, self.state_since) >= CLAP_SETTLE_MS {
                    io.solenoid_off();
                    self.servo_to(SERVO_COCK_DEG, now, io);
                    self.enter(State::Cocking, now);
                }
            }

            State::Cocking => {
                if inp.cocked {
                    self.servo_to(SERVO_REST_DEG, now, io);
                    self.enter(State::ServoReturn, now);
                } else if elapsed(now, self.state_since) > COCK_TIMEOUT_MS {
                    self.fault(Fault::CockingTimeout, now, io);
                }
            }

            State::ServoReturn => {
                if elapsed(now, self.state_since) >= SERVO_RETURN_MS {
                    self.clear_since = now;
                    self.blocked_since = None;
                    self.enter(State::WaitClear, now);
                }
            }

            // вихід тільки через вимкнення тумблера ARM (оброблено вище)
            State::Fault => {}
        }

        io.led(self.led_on(now));
    }

    fn enter(&mut self, s: State, now: u32) {
        self.state = s;
        self.state_since = now;
    }

    fn beep(&mut self, freq_hz: u16, ms: u32, now: u32, io: &mut impl Io) {
        io.buzzer(Some(freq_hz));
        self.buzzer_off_at = Some(now.wrapping_add(ms));
    }

    // Серво живе лише під час руху; у спокої відключене.
    fn servo_to(&mut self, deg: u8, now: u32, io: &mut impl Io) {
        io.servo_write(deg);
        self.servo_attached = true;
        self.servo_target = deg;
        self.servo_moved_at = now;
    }

    fn servo_idle_detach(&mut self, now: u32, io: &mut impl Io) {
        if self.servo_attached
            && self.servo_target == SERVO_REST_DEG
            && elapsed(now, self.servo_moved_at) >= SERVO_RETURN_MS
        {
            io.servo_detach();
            self.servo_attached = false;
        }
    }

    fn fault(&mut self, why: Fault, now: u32, io: &mut impl Io) {
        io.disarm_trigger();
        io.solenoid_off();
        self.servo_to(SERVO_REST_DEG, now, io);
        io.event(Event::Fault(why));
        self.beep(400, 600, now, io);
        self.enter(State::Fault, now);
    }

    fn led_on(&self, now: u32) -> bool {
        match self.state {
            State::Armed => true,
            State::Fault => (now / 100) % 2 == 1, // швидке блимання
            State::Disarmed => false,
            _ => (now / 400) % 2 == 1, // повільне — зайнятий
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    /// Модель заліза: засувка, соленоїд, ISR, серво.
    #[derive(Default)]
    struct Rig {
        solenoid: bool,
        servo_deg: Option<u8>, // None — відключене
        buzzer: Option<u16>,
        trigger_armed: bool,
        fired_at: Option<u32>,
        saved: Option<u32>,
        events: Vec<Event>,
    }

    impl Io for Rig {
        fn solenoid_off(&mut self) {
            self.solenoid = false;
        }
        fn servo_write(&mut self, deg: u8) {
            self.servo_deg = Some(deg);
        }
        fn servo_detach(&mut self) {
            self.servo_deg = None;
        }
        fn buzzer(&mut self, f: Option<u16>) {
            self.buzzer = f;
        }
        fn led(&mut self, _: bool) {}
        fn arm_trigger(&mut self) {
            self.fired_at = None;
            self.trigger_armed = true;
        }
        fn disarm_trigger(&mut self) {
            self.trigger_armed = false;
        }
        fn save_claps(&mut self, c: u32) {
            self.saved = Some(c);
        }
        fn event(&mut self, ev: Event) {
            self.events.push(ev);
        }
    }

    impl Rig {
        /// Те, що робить ISR прошивки при перекритті променя.
        fn beam_cut(&mut self, now: u32) {
            if self.trigger_armed {
                self.solenoid = true;
                self.trigger_armed = false;
                self.fired_at = Some(now);
            }
        }
    }

    struct Sim {
        fc: FlyClap,
        rig: Rig,
        now: u32,
        arm: bool,
        cocked: bool,
        blocked: bool,
        latch_works: bool,
    }

    impl Sim {
        fn new() -> Self {
            let mut s = Sim {
                fc: FlyClap::new(7),
                rig: Rig::default(),
                now: 1000,
                arm: false,
                cocked: false,
                blocked: false,
                latch_works: true,
            };
            s.fc.begin(s.now, &mut s.rig);
            s
        }

        fn run(&mut self, ms: u32) {
            for _ in 0..ms {
                // механіка: серво на куті взведення зачіплює засувку, соленоїд її зриває
                if self.latch_works && self.rig.servo_deg == Some(SERVO_COCK_DEG) {
                    self.cocked = true;
                }
                if self.rig.solenoid {
                    self.cocked = false;
                }
                let inp = Inputs {
                    arm_switch: self.arm,
                    cocked: self.cocked,
                    beams_blocked: self.blocked,
                    fired_at: self.rig.fired_at,
                };
                self.fc.step(self.now, inp, &mut self.rig);
                self.now += 1;
            }
        }
    }

    #[test]
    fn full_cycle_arm_clap_recock() {
        let mut s = Sim::new();
        s.run(1000);
        assert_eq!(s.fc.state(), State::Disarmed);
        assert_eq!(s.rig.servo_deg, None, "servo must detach at rest");

        s.arm = true;
        s.run(1);
        assert_eq!(s.fc.state(), State::Cocking);
        s.run(1);
        assert_eq!(s.fc.state(), State::ServoReturn);
        s.run(SERVO_RETURN_MS + BEAMS_CLEAR_MS + 5);
        assert_eq!(s.fc.state(), State::Armed);
        assert!(s.rig.trigger_armed);
        assert_eq!(s.rig.servo_deg, None, "servo must detach once armed");

        s.rig.beam_cut(s.now);
        assert!(s.rig.solenoid);
        s.run(1);
        assert_eq!(s.fc.state(), State::Fired);
        assert_eq!(s.fc.claps(), 8);
        assert_eq!(s.rig.saved, Some(8));
        s.run(SOLENOID_PULSE_MS);
        assert!(!s.rig.solenoid, "solenoid held longer than the pulse");

        s.run(CLAP_SETTLE_MS);
        assert_eq!(s.fc.state(), State::ServoReturn);
        s.run(SERVO_RETURN_MS + BEAMS_CLEAR_MS + 5);
        assert_eq!(s.fc.state(), State::Armed);
        assert!(s.rig.events.contains(&Event::Clap { total: 8 }));
    }

    #[test]
    fn does_not_arm_while_beam_is_blocked_and_faults_when_stuck() {
        let mut s = Sim::new();
        s.blocked = true;
        s.arm = true;
        s.run(BEAMS_STUCK_MS);
        assert_eq!(s.fc.state(), State::WaitClear);
        s.run(SERVO_RETURN_MS + 10);
        assert_eq!(s.fc.state(), State::Fault);
        assert!(s.rig.events.contains(&Event::Fault(Fault::BeamBlocked)));
        assert_eq!(s.rig.buzzer, Some(400));
        s.run(600);
        assert_eq!(s.rig.buzzer, None, "buzzer must stop after the beep");

        // FAULT тримається, доки не вимкнуть ARM
        s.blocked = false;
        s.run(2000);
        assert_eq!(s.fc.state(), State::Fault);
        s.arm = false;
        s.run(1);
        assert_eq!(s.fc.state(), State::Disarmed);
    }

    #[test]
    fn cocking_timeout_faults() {
        let mut s = Sim::new();
        s.latch_works = false;
        s.arm = true;
        s.run(COCK_TIMEOUT_MS + 5);
        assert_eq!(s.fc.state(), State::Fault);
        assert_eq!(s.rig.servo_deg, Some(SERVO_REST_DEG));
        assert!(!s.rig.trigger_armed);
    }

    #[test]
    fn lost_latch_is_recocked_without_counting_a_clap() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(1000);
        assert_eq!(s.fc.state(), State::Armed);
        s.cocked = false; // вібрація зірвала засувку
        s.latch_works = false;
        s.run(1);
        assert_eq!(s.fc.state(), State::Cocking);
        assert!(!s.rig.trigger_armed);
        assert_eq!(s.fc.claps(), 7);
        assert!(s.rig.events.contains(&Event::LatchLost));
    }

    #[test]
    fn disarm_stops_everything() {
        let mut s = Sim::new();
        s.arm = true;
        s.run(1000);
        s.rig.beam_cut(s.now);
        s.arm = false;
        s.run(1);
        assert_eq!(s.fc.state(), State::Disarmed);
        assert!(!s.rig.solenoid);
        assert!(!s.rig.trigger_armed);
    }
}
