# Changelog

## v1.1

**Breaking:** `param2` semantics changed from `idle_ms` to `window_ms`. Existing configs continue to compile but behave differently — review your value.

- Accumulated counts now decay continuously at a rate of `threshold / window_ms` per millisecond, instead of resetting only after a full idle gap. Sustained low-rate jitter no longer accumulates past the threshold.
- Re-block happens automatically when the accumulator drains to zero; no separate idle timer.
- Accumulator is capped at `2 × threshold` so confident movement can't bank unbounded re-block credit.

## v1.0

Initial release. `param2` was `idle_ms`: accumulator reset to zero only after `idle_ms` of total silence.
