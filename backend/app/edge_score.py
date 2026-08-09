"""Heuristic edge-feature scoring (Tier 3 ML stub — phone/cloud feature vectors)."""

from typing import Any


def score_edge_features(
    *,
    band_corr: float | None,
    band_delta_max: float | None,
    bands: list[float] | None,
    edge_crest: float | None,
    edge_zcr_hz: float | None,
    edge_hf_ratio: float | None,
    level: int | None = None,
) -> dict[str, Any]:
    """Return edge_score 0..1 and human-readable flags for raw_json storage."""
    score = 0.0
    flags: list[str] = []

    if band_corr is not None:
        if band_corr < 0.7:
            score += 0.45
            flags.append("band_corr_low")
        elif band_corr < 0.85:
            score += 0.2
            flags.append("band_corr_warn")

    if band_delta_max is not None and band_delta_max > 0.15:
        score += min(0.35, band_delta_max)
        flags.append("band_delta_high")

    if edge_hf_ratio is not None and edge_hf_ratio > 0.55:
        score += 0.15
        flags.append("hf_elevated")

    if edge_crest is not None and edge_crest > 6.0:
        score += 0.1
        flags.append("crest_spike")

    if edge_zcr_hz is not None and edge_zcr_hz > 80.0:
        score += 0.08
        flags.append("zcr_high")

    if bands:
        peak = max(bands)
        mean = sum(bands) / len(bands)
        if mean > 1e-6 and peak / mean > 4.0:
            score += 0.12
            flags.append("band_peaky")

    if level is not None and level >= 2:
        score = max(score, 0.75)
        flags.append("verdict_alert")
    elif level is not None and level >= 1:
        score = max(score, 0.45)
        flags.append("verdict_warn")

    score = min(1.0, round(score, 3))
    risk = "ok"
    if score >= 0.65:
        risk = "high"
    elif score >= 0.35:
        risk = "medium"

    return {
        "edge_score": score,
        "edge_risk": risk,
        "edge_flags": flags,
    }
