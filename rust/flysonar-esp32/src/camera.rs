//! Камера через компонент esp32-camera (FFI-біндинги esp_idf_svc::sys::camera).

use core::ptr;

use esp_idf_svc::sys::{camera, esp, EspError};

use crate::board::CameraPins;

pub const WIDTH: usize = 320; // QVGA: ~1 мм/піксель на полі 30 см
pub const HEIGHT: usize = 240;

pub fn init(p: &CameraPins) -> Result<(), EspError> {
    // SAFETY: camera_config_t — POD-структура C; нулі — коректні значення за замовчуванням
    let mut c: camera::camera_config_t = unsafe { core::mem::zeroed() };
    c.pin_pwdn = p.pwdn;
    c.pin_reset = p.reset;
    c.pin_xclk = p.xclk;
    c.__bindgen_anon_1.pin_sccb_sda = p.sda;
    c.__bindgen_anon_2.pin_sccb_scl = p.scl;
    c.pin_d0 = p.data[0];
    c.pin_d1 = p.data[1];
    c.pin_d2 = p.data[2];
    c.pin_d3 = p.data[3];
    c.pin_d4 = p.data[4];
    c.pin_d5 = p.data[5];
    c.pin_d6 = p.data[6];
    c.pin_d7 = p.data[7];
    c.pin_vsync = p.vsync;
    c.pin_href = p.href;
    c.pin_pclk = p.pclk;
    c.xclk_freq_hz = 20_000_000;
    // Камера займає LEDC timer 0 / channel 0; серви — timer 2 / channels 4, 5
    c.ledc_timer = camera::ledc_timer_t_LEDC_TIMER_0;
    c.ledc_channel = camera::ledc_channel_t_LEDC_CHANNEL_0;
    c.pixel_format = camera::pixformat_t_PIXFORMAT_GRAYSCALE; // 1 байт/піксель, без JPEG — одразу в алгоритм
    c.frame_size = camera::framesize_t_FRAMESIZE_QVGA;
    c.jpeg_quality = 12;
    c.fb_count = 2;
    c.fb_location = camera::camera_fb_location_t_CAMERA_FB_IN_PSRAM;
    c.grab_mode = camera::camera_grab_mode_t_CAMERA_GRAB_LATEST; // завжди найсвіжіший кадр — менша затримка
    // SAFETY: конфіг повністю заповнений, драйвер копіює його
    esp!(unsafe { camera::esp_camera_init(&c) })
}

/// Кадр з драйвера; повертається драйверу при drop.
pub struct Frame(ptr::NonNull<camera::camera_fb_t>);

impl Frame {
    /// Чекає наступний кадр. `None` — драйвер не віддав кадр (таймаут).
    pub fn get() -> Option<Frame> {
        // SAFETY: драйвер ініціалізовано в init()
        ptr::NonNull::new(unsafe { camera::esp_camera_fb_get() }).map(Frame)
    }

    pub fn pixels(&self) -> &[u8] {
        // SAFETY: буфер валідний, доки кадр не повернуто (drop)
        unsafe {
            let fb = self.0.as_ref();
            core::slice::from_raw_parts(fb.buf, fb.len)
        }
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        // SAFETY: кадр отримано з esp_camera_fb_get і повертається рівно раз
        unsafe { camera::esp_camera_fb_return(self.0.as_ptr()) }
    }
}

/// Після встановлення експозиції фіксуємо її: автоекспозиція, що "дихає",
/// ламає модель фону й дає хибні цілі. false — сенсор недоступний.
pub fn lock_exposure() -> bool {
    // SAFETY: вказівник від драйвера; функції сенсора приймають його ж
    unsafe {
        let s = camera::esp_camera_sensor_get();
        let Some(sensor) = s.as_ref() else { return false };
        for set in [sensor.set_exposure_ctrl, sensor.set_gain_ctrl, sensor.set_whitebal].into_iter().flatten() {
            set(s, 0);
        }
    }
    true
}
