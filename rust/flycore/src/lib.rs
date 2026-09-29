//! Спільне ядро прошивок FlyClap і FlySonar.
//!
//! Уся логіка — машини станів, сонарний скан, протокол ESP32 → Arduino, турель,
//! машинний зір — живе тут без жодної залежності від заліза. Прошивки лише
//! читають входи, викликають ядро і застосовують результат до пінів:
//!
//! | прошивка          | плата              | модулі ядра                         |
//! |-------------------|--------------------|-------------------------------------|
//! | `flyclap`         | Uno / Nano (AVR)   | [`flyclap`]                         |
//! | `flysonar`        | Uno / Nano (AVR)   | [`sonar`] або [`link`] + [`turret`] |
//! | `flysonar-esp32`  | ESP32(-S3)-CAM     | [`vision`], [`turret`], [`link`]    |
//!
//! Завдяки цьому весь код тестується на ПК: `cargo test` у теці `rust/`.

#![no_std]

#[cfg(any(feature = "alloc", test))]
extern crate alloc;
#[cfg(test)]
extern crate std;

pub mod flyclap;
pub mod link;
pub mod servo;
pub mod sonar;
pub mod time;
pub mod turret;
#[cfg(any(feature = "alloc", test))]
pub mod vision;
