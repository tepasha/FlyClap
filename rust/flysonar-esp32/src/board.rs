//! Плата: піни камери (з camera_pins.h прикладу CameraWebServer) і вільні піни
//! під серви, клапан, ARM, LED або UART на Arduino.

use esp_idf_svc::hal::gpio::{AnyIOPin, AnyOutputPin, IOPin, OutputPin, Pins};

#[cfg(all(feature = "ai-thinker", feature = "s3-eye"))]
compile_error!("Select exactly one board: feature `ai-thinker` or `s3-eye`");
#[cfg(not(any(feature = "ai-thinker", feature = "s3-eye")))]
compile_error!("Select a board: feature `ai-thinker` or `s3-eye`");

pub struct CameraPins {
    pub pwdn: i32,
    pub reset: i32,
    pub xclk: i32,
    pub sda: i32,
    pub scl: i32,
    /// D0..D7 (Y2..Y9)
    pub data: [i32; 8],
    pub vsync: i32,
    pub href: i32,
    pub pclk: i32,
}

/// Піни, що лишились вільними від камери.
pub struct IoPins {
    #[cfg(not(feature = "eyes"))]
    pub pan: AnyOutputPin,
    #[cfg(not(feature = "eyes"))]
    pub tilt: AnyOutputPin,
    #[cfg(not(feature = "eyes"))]
    pub valve: AnyOutputPin,
    #[cfg(not(feature = "eyes"))]
    pub arm: AnyIOPin,
    #[cfg(not(feature = "eyes"))]
    pub led: AnyOutputPin,
    #[cfg(feature = "eyes")]
    pub link_tx: AnyOutputPin,
}

// ESP32-CAM (AI-Thinker), найдешевша. Вільні піни — без SD-картки. GPIO12 —
// strapping-пін: на старті не має бути HIGH, тому тумблер ARM лише на GND і
// БЕЗ зовнішньої підтяжки.
#[cfg(feature = "ai-thinker")]
pub const CAMERA: CameraPins = CameraPins {
    pwdn: 32,
    reset: -1,
    xclk: 0,
    sda: 26,
    scl: 27,
    data: [5, 18, 19, 21, 36, 39, 34, 35],
    vsync: 25,
    href: 23,
    pclk: 22,
};
#[cfg(feature = "ai-thinker")]
pub const LED_ACTIVE_LOW: bool = true; // червоний LED на платі

#[cfg(feature = "ai-thinker")]
pub fn io_pins(p: Pins) -> IoPins {
    #[cfg(not(feature = "eyes"))]
    return IoPins {
        pan: p.gpio14.downgrade_output(),
        tilt: p.gpio15.downgrade_output(),
        valve: p.gpio13.downgrade_output(),
        arm: p.gpio12.downgrade(),
        led: p.gpio33.downgrade_output(),
    };
    #[cfg(feature = "eyes")]
    return IoPins { link_tx: p.gpio14.downgrade_output() };
}

// ESP32-S3-EYE / Freenove ESP32-S3-WROOM CAM. Розкладка під Freenove; на інших
// платах перевірте, що піни вільні.
#[cfg(feature = "s3-eye")]
pub const CAMERA: CameraPins = CameraPins {
    pwdn: -1,
    reset: -1,
    xclk: 15,
    sda: 4,
    scl: 5,
    data: [11, 9, 8, 10, 12, 18, 17, 16],
    vsync: 6,
    href: 7,
    pclk: 13,
};
#[cfg(feature = "s3-eye")]
pub const LED_ACTIVE_LOW: bool = false;

#[cfg(feature = "s3-eye")]
pub fn io_pins(p: Pins) -> IoPins {
    #[cfg(not(feature = "eyes"))]
    return IoPins {
        pan: p.gpio1.downgrade_output(),
        tilt: p.gpio14.downgrade_output(),
        valve: p.gpio21.downgrade_output(),
        arm: p.gpio47.downgrade(),
        led: p.gpio2.downgrade_output(),
    };
    #[cfg(feature = "eyes")]
    return IoPins { link_tx: p.gpio14.downgrade_output() };
}
