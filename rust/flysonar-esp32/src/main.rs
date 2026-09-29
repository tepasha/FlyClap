//! FlySonar — ESP32-версія: турель з водометом, що бачить мошку (ESP32-CAM / ESP32-S3-CAM).
//!
//! Камера дивиться на яскравий підсвічений фон. Комаха на ньому — темна точка,
//! яку видно навіть коли вона займає 2–4 пікселі (мошка 1–3 мм). ESP32 шукає
//! точки, веде ціль, прогнозує її положення на час пострілу, перераховує пікселі
//! в кути серв і стріляє. Алгоритми — у [`flycore::vision`] і [`flycore::turret`].
//!
//! Дві ролі (feature `eyes`):
//! * самостійна (за замовчуванням): одна плата — зір + серви + клапан + ARM;
//! * "очі" для Arduino-версії (`flysonar` з feature `camera`): ESP32 лише бачить
//!   і шле команди по UART ([`flycore::link`]), стріляє Arduino.
//!
//! Керування й калібрування — через USB-консоль (115200), див. docs/flysonar-esp32.md.

mod board;
mod camera;
mod out;

use std::io::Read;
use std::sync::mpsc;
use std::time::Duration;

use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use esp_idf_svc::sys;
use flycore::servo::Deg10;
use flycore::time::reached;
use flycore::vision::{solve_affine, Affine, Blob, CalPoint, Track, TrackerConfig, Vision};

use out::{Out, Output};

// ---------------- Налаштування зору ----------------
const LATENCY_MS: f32 = 70.0; // кадр + обробка + доворот серв + політ струменя
const LOCK_HITS: u16 = 5; // стільки кадрів поспіль бачимо ціль, перш ніж дозволити постріл
const STILL_PX_S: f32 = 15.0; // повільніше — ціль "сидить"
const BIG_HOLD_MS: u32 = 3000; // після великого об'єкта мовчимо стільки
const EXPOSURE_LOCK_MS: u32 = 2500; // дати автоекспозиції встановитись, потім заморозити
const JOG_FINE10: i16 = 5; // 0.5°
const JOG_COARSE10: i16 = 50; // 5°
const TEST_SHOT_MS: u16 = 40;
const MAX_CAL_POINTS: usize = 9;
const NVS_NAMESPACE: &str = "flyvision";
const NVS_KEY_CAL: &str = "cal";

/// Мілісекунди від старту (як millis()).
pub fn millis() -> u32 {
    // SAFETY: esp_timer — потокобезпечний лічильник
    (unsafe { sys::esp_timer_get_time() } / 1000) as u32
}

const HELP: &str = "Keys:
  m        calibration mode on/off
  a d w s  jog 0.5 deg (A D W S = 5 deg)   [cal mode]
  f        test shot (ARM must be on)      [cal mode]
  c        capture point: marker on the wet spot, exactly one dark dot in view
  u        undo last point,  v  solve & save (>=3 points)
  b        re-learn background (clear backdrop first)
  t        toggle firing at moving targets (lead aim)
  p        status";

struct App {
    out: Out,
    vision: Vision,
    track: Track,
    track_cfg: TrackerConfig,
    nvs: EspNvs<NvsDefault>,
    cal: Affine,
    cal_pts: Vec<CalPoint>,
    cal_mode: bool,
    fire_moving: bool, // стріляти й по цілях у польоті (з упередженням)
    jog_pan10: i16,
    jog_tilt10: i16,
    big_until: u32,
    exposure_locked: bool,
    boot_ms: u32,
    frames: u32,
    fps_mark: u32,
    fps: f32,
    blobs: [Blob; 8],
    blob_count: usize,
}

impl App {
    fn load_cal(&mut self) {
        let mut buf = [0u8; Affine::BYTES];
        if let Ok(Some(bytes)) = self.nvs.get_raw(NVS_KEY_CAL, &mut buf) {
            self.cal = Affine::from_bytes(bytes).unwrap_or_default();
        }
        println!("{}", if self.cal.valid { "Calibration loaded" } else { "NOT CALIBRATED - press 'm' to calibrate" });
    }

    fn save_cal(&mut self) {
        if let Err(e) = self.nvs.set_raw(NVS_KEY_CAL, &self.cal.to_bytes()) {
            println!("Saving calibration FAILED: {e}");
        }
    }

    fn jog(&mut self, dp: i16, dt: i16) {
        self.jog_pan10 = (self.jog_pan10 + dp).clamp(0, 1800);
        self.jog_tilt10 = (self.jog_tilt10 + dt).clamp(0, 1800);
        self.out.move_to(self.jog_pan10 as i32, self.jog_tilt10 as i32);
        println!("aim pan={} tilt={}", Deg10(self.jog_pan10), Deg10(self.jog_tilt10));
    }

    fn print_status(&self) {
        println!(
            "fps={:.1} cal={} points={} mode={} fireMoving={} blobs={} track={} hits={} v={:.0}px/s",
            self.fps,
            if self.cal.valid { "ok" } else { "NONE" },
            self.cal_pts.len(),
            if self.cal_mode { "CAL" } else { "RUN" },
            self.fire_moving as u8,
            self.blob_count,
            self.track.active as u8,
            self.track.hits,
            self.track.speed()
        );
        if self.cal.valid {
            let (a, b) = (self.cal.a, self.cal.b);
            println!(
                "pan10 = {:.3}*x + {:.3}*y + {:.1} ; tilt10 = {:.3}*x + {:.3}*y + {:.1}",
                a[0], a[1], a[2], b[0], b[1], b[2]
            );
        }
    }

    fn handle_key(&mut self, k: u8) {
        let cal = self.cal_mode;
        match k {
            b'm' => {
                self.cal_mode = !self.cal_mode;
                if self.cal_mode {
                    println!("CAL mode: shoot (f), put a dark marker on the wet spot, capture (c)");
                    self.out.move_to(self.jog_pan10 as i32, self.jog_tilt10 as i32);
                } else {
                    println!("RUN mode");
                }
            }
            b'a' if cal => self.jog(-JOG_FINE10, 0),
            b'd' if cal => self.jog(JOG_FINE10, 0),
            b'w' if cal => self.jog(0, JOG_FINE10),
            b's' if cal => self.jog(0, -JOG_FINE10),
            b'A' if cal => self.jog(-JOG_COARSE10, 0),
            b'D' if cal => self.jog(JOG_COARSE10, 0),
            b'W' if cal => self.jog(0, JOG_COARSE10),
            b'S' if cal => self.jog(0, -JOG_COARSE10),
            b'f' if cal => self.out.test_shot(TEST_SHOT_MS),
            b'c' if cal => {
                if self.blob_count != 1 {
                    println!("Need exactly one dark dot in view, see {}", self.blob_count);
                } else if self.cal_pts.len() >= MAX_CAL_POINTS {
                    println!("Too many points, solve with 'v'");
                } else {
                    let b = self.blobs[0];
                    self.cal_pts.push(CalPoint { x: b.x, y: b.y, pan10: self.jog_pan10 as f32, tilt10: self.jog_tilt10 as f32 });
                    println!(
                        "Point {}: px=({:.1}, {:.1}) -> pan={} tilt={}",
                        self.cal_pts.len(),
                        b.x,
                        b.y,
                        Deg10(self.jog_pan10),
                        Deg10(self.jog_tilt10)
                    );
                }
            }
            b'u' => {
                if self.cal_pts.pop().is_some() {
                    println!("Points: {}", self.cal_pts.len());
                }
            }
            b'v' => match solve_affine(&self.cal_pts) {
                None => println!("Need >=3 points not on one line"),
                Some(a) => {
                    self.cal = a;
                    self.save_cal();
                    // залишкова похибка по точках
                    let worst = self.cal_pts.iter().fold(0f32, |w, p| {
                        let (pan, tilt) = a.apply(p.x, p.y);
                        w.max((pan - p.pan10).abs()).max((tilt - p.tilt10).abs())
                    });
                    println!("Saved. Worst residual {:.2} deg. Remove markers and press 'b'.", worst / 10.0);
                    self.cal_pts.clear();
                }
            },
            b'b' => {
                self.vision.reset_background();
                println!("Background reset");
            }
            b't' => {
                self.fire_moving = !self.fire_moving;
                println!("fireMoving={}", self.fire_moving as u8);
            }
            b'p' => self.print_status(),
            b'h' | b'?' => println!("{HELP}"),
            _ => {}
        }
    }

    /// Обробити один кадр.
    fn frame(&mut self) {
        let Some(frame) = camera::Frame::get() else { return };
        let now = millis();
        let det = self.vision.process(frame.pixels(), &mut self.blobs);
        drop(frame); // кадр повертаємо драйверу якнайшвидше
        self.blob_count = det.count;

        if !self.exposure_locked && now.wrapping_sub(self.boot_ms) > EXPOSURE_LOCK_MS {
            camera::lock_exposure();
            self.exposure_locked = true;
            self.vision.reset_background();
            println!("Exposure locked, background re-learned");
        }

        self.frames += 1;
        let since = now.wrapping_sub(self.fps_mark);
        if since >= 1000 {
            self.fps = self.frames as f32 * 1000.0 / since as f32;
            self.frames = 0;
            self.fps_mark = now;
        }

        if det.big {
            if reached(now, self.big_until) {
                println!("Big object - holding fire");
            }
            self.big_until = now.wrapping_add(BIG_HOLD_MS);
            self.out.big_object();
        }

        if self.cal_mode || !self.exposure_locked {
            return; // у калібруванні приціл веде людина
        }

        self.track.update(&self.blobs[..det.count], now, &self.track_cfg);
        if !self.track.active || !self.cal.valid {
            return;
        }

        let t = self.track;
        let still = t.speed() < STILL_PX_S;
        // По нерухомій цілі цілимось у поточну точку, по рухомій — з упередженням
        let (x, y) = if still { (t.x, t.y) } else { (t.predict_x(LATENCY_MS), t.predict_y(LATENCY_MS)) };
        let (pan10, tilt10) = self.cal.apply(x, y);

        let fire = t.hits >= LOCK_HITS && t.misses == 0 && reached(now, self.big_until) && (still || self.fire_moving);
        self.out.aim(pan10.round() as i32, tilt10.round() as i32, fire);
    }
}

/// Клавіші з USB-консолі: окремий потік, щоб читання не блокувало зір.
fn spawn_key_reader() -> mpsc::Receiver<u8> {
    let (tx, rx) = mpsc::channel();
    std::thread::Builder::new()
        .stack_size(3072)
        .spawn(move || {
            let mut stdin = std::io::stdin();
            let mut b = [0u8; 1];
            loop {
                match stdin.read(&mut b) {
                    Ok(1) => {
                        if tx.send(b[0]).is_err() {
                            return;
                        }
                    }
                    _ => std::thread::sleep(Duration::from_millis(10)),
                }
            }
        })
        .expect("key reader thread");
    rx
}

fn main() -> anyhow::Result<()> {
    sys::link_patches();
    let p = Peripherals::take()?;
    let pins = board::io_pins(p.pins);

    #[cfg(feature = "eyes")]
    let out = Out::new(p.uart1, pins)?;
    #[cfg(not(feature = "eyes"))]
    let out = Out::new(p.ledc, pins)?;

    std::thread::sleep(Duration::from_millis(300));
    println!(
        "\n{}",
        if cfg!(feature = "eyes") { "FlySonar ESP32 - eyes for Arduino" } else { "FlySonar ESP32 - standalone" }
    );

    let vision = match camera::init(&board::CAMERA).ok().and_then(|_| Vision::new(camera::WIDTH, camera::HEIGHT)) {
        Some(v) => v,
        None => {
            println!("Camera init FAILED - check board feature and PSRAM");
            loop {
                std::thread::sleep(Duration::from_secs(1));
            }
        }
    };

    let nvs = EspNvs::new(EspDefaultNvsPartition::take()?, NVS_NAMESPACE, true)?;
    let now = millis();
    let mut app = App {
        out,
        vision,
        track: Track::default(),
        track_cfg: TrackerConfig::default(),
        nvs,
        cal: Affine::default(),
        cal_pts: Vec::with_capacity(MAX_CAL_POINTS),
        cal_mode: false,
        fire_moving: false,
        jog_pan10: 900,
        jog_tilt10: 900,
        big_until: 0,
        exposure_locked: false,
        boot_ms: now,
        frames: 0,
        fps_mark: now,
        fps: 0.0,
        blobs: [Blob::default(); 8],
        blob_count: 0,
    };
    app.load_cal();
    println!("{HELP}");

    let keys = spawn_key_reader();
    loop {
        while let Ok(k) = keys.try_recv() {
            app.handle_key(k);
        }
        app.out.poll();
        app.frame();
    }
}
