"""Battery bench helpers — estimated discharge current from dV/dt."""

from __future__ import annotations

V_FULL = 4.20
V_EMPTY = 3.00


def estimate_discharge_ma(
    voltage_v: float | None,
    prev_voltage_v: float | None,
    dt_ms: int,
    cell_mah: int = 500,
) -> float | None:
    if (
        voltage_v is None
        or prev_voltage_v is None
        or dt_ms <= 0
        or cell_mah <= 0
    ):
        return None
    dv = voltage_v - prev_voltage_v
    dt_h = dt_ms / 3_600_000.0
    if dt_h <= 0:
        return None
    v_span = V_FULL - V_EMPTY
    if v_span <= 0:
        return None
    dsoc = dv / v_span
    return abs(cell_mah * dsoc / dt_h)
