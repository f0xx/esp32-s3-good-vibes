"""Attach IMU proofs to FTA mechanical leaves only.

Electrical and process leaves stay in the template as not-speakable —
the mechanic picks them at END REPAIR; the IMU never claims them.
"""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Any

TEMPLATE_DIR = Path(__file__).resolve().parent / "fta_templates"

# Mechanical leaves the IMU may hint. Confidence is heuristic, not ground truth.
_BEARING_FLAGS = frozenset({"hf_elevated", "crest_spike", "band_peaky", "zcr_high"})
_MISALIGN_FLAGS = frozenset({"band_corr_low", "band_delta_high"})
_CAVITATION_FLAGS = frozenset({"hf_elevated", "band_peaky"})

NEW_REF_LEAVES = frozenset(
    {"bearing", "impeller", "seal", "remount", "misalignment"}
)


@lru_cache(maxsize=8)
def load_template(kind: str = "pump") -> dict[str, Any]:
    name = "pump.json" if kind == "pump" else "generic.json"
    path = TEMPLATE_DIR / name
    if not path.is_file():
        path = TEMPLATE_DIR / "generic.json"
    with path.open(encoding="utf-8") as fh:
        data = json.load(fh)
    return data if isinstance(data, dict) else {}


def default_new_ref_required(fta_leaf: str | None) -> bool:
    if not fta_leaf:
        return True
    return fta_leaf in NEW_REF_LEAVES or fta_leaf == "other"


def map_imu_hints(
    *,
    edge_flags: list[str] | None,
    edge_score: float | None = None,
    kind: str = "pump",
) -> list[dict[str, Any]]:
    """Return speakable IMU-leaf hints. Electrical/process omitted."""
    flags = set(edge_flags or [])
    template = load_template(kind)
    leaves = {str(leaf.get("leaf_id")): leaf for leaf in template.get("leaves", [])}
    hints: list[dict[str, Any]] = []

    bearing_hits = flags & _BEARING_FLAGS
    if bearing_hits and "bearing" in leaves:
        conf = min(0.95, 0.35 + 0.15 * len(bearing_hits) + (0.1 if (edge_score or 0) >= 0.6 else 0))
        hints.append(_hint(leaves["bearing"], conf, sorted(bearing_hits)))

    mis_hits = flags & _MISALIGN_FLAGS
    if "band_corr_low" in flags and mis_hits and "misalignment" in leaves:
        conf = min(0.9, 0.4 + 0.2 * len(mis_hits))
        hints.append(_hint(leaves["misalignment"], conf, sorted(mis_hits)))

    cav_hits = flags & _CAVITATION_FLAGS
    if cav_hits == _CAVITATION_FLAGS and "cavitation" in leaves:
        conf = min(0.85, 0.45 + (0.15 if (edge_score or 0) >= 0.5 else 0))
        hints.append(_hint(leaves["cavitation"], conf, sorted(cav_hits)))

    hints.sort(key=lambda h: float(h.get("confidence") or 0), reverse=True)
    return hints


def _hint(leaf: dict[str, Any], confidence: float, evidence: list[str]) -> dict[str, Any]:
    return {
        "leaf_id": leaf.get("leaf_id"),
        "label": leaf.get("label"),
        "category": leaf.get("category"),
        "confidence": round(confidence, 3),
        "speakable": True,
        "evidence_flags": evidence,
    }


def mechanic_leaves(kind: str = "pump") -> list[dict[str, Any]]:
    """Full pick list for END REPAIR (includes not-speakable electrical/process)."""
    template = load_template(kind)
    out: list[dict[str, Any]] = []
    for leaf in template.get("leaves", []):
        if not isinstance(leaf, dict):
            continue
        out.append(
            {
                "leaf_id": leaf.get("leaf_id"),
                "label": leaf.get("label"),
                "category": leaf.get("category"),
                "speakable": bool(leaf.get("speakable")),
                "new_ref_required": bool(leaf.get("new_ref_required", True)),
            }
        )
    return out
