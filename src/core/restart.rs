//! When to restart a helper process that exited.

use std::time::Duration;

/// A helper that ran at least this long before exiting starts again from the first delay.
pub const STABLE_RUN: Duration = Duration::from_secs(60);

const FIRST_DELAY: Duration = Duration::from_secs(1);
const MAX_DELAY: Duration = Duration::from_secs(30);

/// The wait before restart number `attempt` (0 for the first): 1 s, doubling, at most 30 s.
pub fn restart_delay(attempt: u32) -> Duration {
    FIRST_DELAY
        .checked_mul(1u32.checked_shl(attempt).unwrap_or(u32::MAX))
        .map_or(MAX_DELAY, |delay| delay.min(MAX_DELAY))
}

/// The attempt number for the restart after a run that lasted `ran`, following attempt `attempt`.
pub fn next_attempt(attempt: u32, ran: Duration) -> u32 {
    if ran >= STABLE_RUN {
        0
    } else {
        attempt.saturating_add(1)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn delays_double_from_one_second() {
        assert_eq!(restart_delay(0), Duration::from_secs(1));
        assert_eq!(restart_delay(1), Duration::from_secs(2));
        assert_eq!(restart_delay(2), Duration::from_secs(4));
        assert_eq!(restart_delay(4), Duration::from_secs(16));
    }

    #[test]
    fn delays_stop_at_thirty_seconds() {
        assert_eq!(restart_delay(5), MAX_DELAY);
        assert_eq!(restart_delay(31), MAX_DELAY);
        assert_eq!(restart_delay(32), MAX_DELAY);
        assert_eq!(restart_delay(u32::MAX), MAX_DELAY);
    }

    #[test]
    fn a_long_run_starts_over() {
        assert_eq!(next_attempt(7, STABLE_RUN), 0);
        assert_eq!(next_attempt(7, Duration::from_secs(3600)), 0);
    }

    #[test]
    fn a_quick_exit_waits_longer() {
        assert_eq!(next_attempt(0, Duration::from_millis(500)), 1);
        assert_eq!(next_attempt(3, Duration::from_secs(59)), 4);
        assert_eq!(next_attempt(u32::MAX, Duration::ZERO), u32::MAX);
    }
}
