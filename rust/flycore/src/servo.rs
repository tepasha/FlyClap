//! Серви pan-tilt: кути в десятих градуса (0..=1800), імпульс як у Arduino `Servo`.

use core::fmt;

pub const US_MIN: u16 = 544; // 0°
pub const US_MAX: u16 = 2400; // 180°
pub const DEG10_MAX: i16 = 1800;

/// Кут (0.1°) → ширина імпульсу, мкс.
#[inline]
pub fn deg10_to_us(d10: i16) -> u16 {
    (US_MIN as i32 + d10 as i32 * (US_MAX - US_MIN) as i32 / DEG10_MAX as i32) as u16
}

#[inline]
pub fn clamp10(v: i32) -> i16 {
    v.clamp(0, DEG10_MAX as i32) as i16
}

/// Скільки чекати, поки серво доїде: база + мс на градус, з обмеженням.
#[derive(Clone, Copy, Debug)]
pub struct Settle {
    pub base_ms: u16,
    pub ms_per_deg: u16,
    pub max_ms: u16,
}

impl Settle {
    /// MG90S: ~0.1 с/60° + запас.
    pub const MG90S: Settle = Settle { base_ms: 12, ms_per_deg: 2, max_ms: 300 };

    pub fn ms(&self, dist10: u16) -> u16 {
        let ms = self.base_ms as u32 + dist10 as u32 * self.ms_per_deg as u32 / 10;
        ms.min(self.max_ms as u32) as u16
    }
}

/// Кут 0.1° для друку: `Deg10(973)` → `97.3`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Deg10(pub i16);

impl Deg10 {
    fn parts(self) -> (&'static str, u16, u16) {
        let a = self.0.unsigned_abs();
        (if self.0 < 0 { "-" } else { "" }, a / 10, a % 10)
    }
}

impl fmt::Display for Deg10 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let (sign, int, frac) = self.parts();
        write!(f, "{sign}{int}.{frac}")
    }
}

#[cfg(feature = "ufmt")]
impl ufmt::uDisplay for Deg10 {
    fn fmt<W: ufmt::uWrite + ?Sized>(&self, f: &mut ufmt::Formatter<'_, W>) -> Result<(), W::Error> {
        let (sign, int, frac) = self.parts();
        ufmt::uwrite!(f, "{}{}.{}", sign, int, frac)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::format;

    #[test]
    fn pulse_width_matches_arduino_servo() {
        assert_eq!(deg10_to_us(0), 544);
        assert_eq!(deg10_to_us(900), 1472);
        assert_eq!(deg10_to_us(1800), 2400);
    }

    #[test]
    fn settle_is_capped() {
        assert_eq!(Settle::MG90S.ms(0), 12);
        assert_eq!(Settle::MG90S.ms(600), 132);
        assert_eq!(Settle::MG90S.ms(1800), 300);
    }

    #[test]
    fn deg10_display() {
        assert_eq!(format!("{}", Deg10(973)), "97.3");
        assert_eq!(format!("{}", Deg10(-5)), "-0.5");
        assert_eq!(format!("{}", Deg10(1800)), "180.0");
    }
}
