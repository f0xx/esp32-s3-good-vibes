"""profiles.txt #2: "if the bias monotonically increases from record to
record — this is the prediction for alarm." This module scores whether a
device's recent verdict metric (edge_score by default) is trending upward
over time, as a leading indicator distinct from any single instantaneous
verdict level.

Uses a Mann-Kendall-style statistic: for every pair of points (i < j), count
+1 if value increased, -1 if it decreased, 0 if tied. Normalizing by the
maximum possible sum gives a score in [-1, 1] that is robust to noisy,
non-monotonic-but-generally-rising series (unlike a strict "is every step an
increase" check, which real sensor noise would almost never satisfy).
"""

from typing import Literal, NamedTuple

TrendLabel = Literal["increasing", "decreasing", "none", "insufficient"]

MIN_SAMPLES = 5
TREND_THRESHOLD = 0.35


class TrendResult(NamedTuple):
    score: float
    trend: TrendLabel
    early_warning: bool


def mann_kendall_score(values: list[float]) -> tuple[float, TrendLabel]:
    n = len(values)
    if n < MIN_SAMPLES:
        return 0.0, "insufficient"

    s = 0
    for i in range(n - 1):
        vi = values[i]
        for j in range(i + 1, n):
            diff = values[j] - vi
            if diff > 0:
                s += 1
            elif diff < 0:
                s -= 1

    max_s = n * (n - 1) / 2
    score = (s / max_s) if max_s else 0.0

    if score >= TREND_THRESHOLD:
        trend: TrendLabel = "increasing"
    elif score <= -TREND_THRESHOLD:
        trend = "decreasing"
    else:
        trend = "none"
    return score, trend


def score_trend(values: list[float], *, latest_high: bool = False) -> TrendResult:
    """`latest_high` — caller already knows the most recent value is itself
    elevated (e.g. WARN/ALERT-ish); an increasing trend is only surfaced as
    an actionable early_warning once it's not just noise around a low
    baseline."""
    score, trend = mann_kendall_score(values)
    early_warning = trend == "increasing" and (latest_high or score >= 0.6)
    return TrendResult(score=score, trend=trend, early_warning=early_warning)
