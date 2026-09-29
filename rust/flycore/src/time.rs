//! Час у мілісекундах (u32, як `millis()`), стійкий до переповнення через ~49.7 доби.

/// Момент `t` настав: `(int32_t)(now - t) >= 0`.
#[inline]
pub fn reached(now: u32, t: u32) -> bool {
    now.wrapping_sub(t) as i32 >= 0
}

/// Скільки мс минуло від `since` до `now`.
#[inline]
pub fn elapsed(now: u32, since: u32) -> u32 {
    now.wrapping_sub(since)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn survives_millis_overflow() {
        assert!(reached(5, u32::MAX - 5));
        assert!(!reached(u32::MAX - 5, 5));
        assert_eq!(elapsed(5, u32::MAX - 5), 11);
    }
}
