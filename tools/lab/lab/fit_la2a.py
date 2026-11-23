"""Fit and validate OptoLa2aT4bStage dual-memory CdS constants against real hardware
Teletronix LA-2A recordings from SignalTrain (Zenodo 3824876).
"""

from __future__ import annotations

from pathlib import Path
import numpy as np
from scipy.optimize import least_squares
import soundfile as sf


def _extract_pair_envelopes(in_wav: Path, tgt_wav: Path, start_s: float, dur_s: float, block_ms: float = 10.0):
    info = sf.info(str(in_wav))
    fs = info.samplerate
    start_fr = int(start_s * fs)
    num_fr = int(dur_s * fs)
    x, _ = sf.read(str(in_wav), start=start_fr, frames=num_fr, dtype="float64", always_2d=False)
    y, _ = sf.read(str(tgt_wav), start=start_fr, frames=num_fr, dtype="float64", always_2d=False)
    if x.ndim > 1:
        x = x[:, 0]
    if y.ndim > 1:
        y = y[:, 0]

    # Align makeup gain on quiet (< -40 dBFS) active regions so DC/makeup offset doesn't skew GR
    blk = int(round(block_ms * 0.001 * fs))
    n_blk = len(x) // blk
    x_blk = x[: n_blk * blk].reshape(n_blk, blk)
    y_blk = y[: n_blk * blk].reshape(n_blk, blk)
    rms_x = np.sqrt(np.mean(x_blk * x_blk, axis=1) + 1e-12)
    rms_y = np.sqrt(np.mean(y_blk * y_blk, axis=1) + 1e-12)
    db_x = 20.0 * np.log10(np.maximum(rms_x, 1e-6))
    db_y = 20.0 * np.log10(np.maximum(rms_y, 1e-6))

    # Static makeup gain from low-level active frames (-48 .. -34 dBFS)
    low_mask = (db_x > -48.0) & (db_x < -34.0)
    makeup_db = float(np.median(db_y[low_mask] - db_x[low_mask])) if np.any(low_mask) else 0.0
    gr_true_db = np.minimum(0.0, (db_y - makeup_db) - db_x)
    active = db_x > -45.0
    return rms_x, db_x, gr_true_db, active, block_ms, makeup_db


def _sim_opto_block_gr(rms_x: np.ndarray, block_ms: float, amount: float, p: np.ndarray) -> np.ndarray:
    tau_atk, tau_rel_f, tau_chg, tau_rel_s, knee_db, ratio, th_min, th_max = p
    thresh_db = th_min + (th_max - th_min) * amount
    slope = 1.0 - 1.0 / max(1.1, ratio)
    atk_c = block_ms / (tau_atk + block_ms)
    chg_c = block_ms / (tau_chg + block_ms)
    rel_s_c = block_ms / (tau_rel_s + block_ms)

    g_fast = 0.0
    g_slow = 0.0
    out_gr = np.empty_like(rms_x)
    in_db = 20.0 * np.log10(np.maximum(rms_x, 1e-5))
    for i, db in enumerate(in_db):
        over = db - thresh_db
        if over > 0.5 * knee_db:
            drive = over * slope
        elif over > -0.5 * knee_db:
            t = over + 0.5 * knee_db
            drive = slope * (t * t) / (2.0 * knee_db)
        else:
            drive = 0.0

        sc = chg_c if drive > g_slow else rel_s_c
        g_slow += (drive - g_slow) * sc

        tau_eff = tau_rel_f + (tau_rel_s - tau_rel_f) * (g_slow / (4.0 + g_slow))
        fc = atk_c if drive > g_fast else (block_ms / (tau_eff + block_ms))
        g_fast += (drive - g_fast) * fc
        out_gr[i] = -g_fast
    return out_gr


def main() -> None:
    st_dir = Path("../../data/signaltrain_subset/Test")
    p1_in = st_dir / "input_256_.wav"
    p1_tg = st_dir / "target_256_LA2A_2c__1__65.wav"
    p2_in = st_dir / "input_259_.wav"
    p2_tg = st_dir / "target_259_LA2A_2c__1__80.wav"

    # Train split: 20s..80s; Held-out Validation split: 140s..200s
    tr1 = _extract_pair_envelopes(p1_in, p1_tg, 20.0, 60.0)
    tr2 = _extract_pair_envelopes(p2_in, p2_tg, 20.0, 60.0)
    va1 = _extract_pair_envelopes(p1_in, p1_tg, 140.0, 60.0)
    va2 = _extract_pair_envelopes(p2_in, p2_tg, 140.0, 60.0)

    # Initial guess: [tauAtk, tauRelFast, tauCharge, tauRelSlow, kneeDb, ratio, thMin, thMax]
    x0 = np.array([10.0, 60.0, 180.0, 1200.0, 6.0, 3.2, -6.0, -32.0], dtype=np.float64)
    lb = np.array([2.0,  20.0, 60.0,  400.0,  1.0, 1.8, -14.0, -45.0])
    ub = np.array([35.0, 180.0, 500.0, 3000.0, 14.0, 8.0, 0.0, -18.0])

    def res_fn(p):
        pred1 = _sim_opto_block_gr(tr1[0], tr1[4], 0.65, p)
        pred2 = _sim_opto_block_gr(tr2[0], tr2[4], 0.80, p)
        e1 = (pred1 - tr1[2])[tr1[3]]
        e2 = (pred2 - tr2[2])[tr2[3]]
        return np.concatenate([e1, e2])

    fit = least_squares(res_fn, x0, bounds=(lb, ub), max_nfev=120)
    p = fit.x

    # Held-out Test V2 evaluation
    v1_pred = _sim_opto_block_gr(va1[0], va1[4], 0.65, p)
    v2_pred = _sim_opto_block_gr(va2[0], va2[4], 0.80, p)
    rms1 = float(np.sqrt(np.mean(((v1_pred - va1[2])[va1[3]]) ** 2)))
    rms2 = float(np.sqrt(np.mean(((v2_pred - va2[2])[va2[3]]) ** 2)))
    print(f"Fitted T4B params: tauAtk={p[0]:.2f}ms, tauRelFast={p[1]:.1f}ms, tauCharge={p[2]:.1f}ms, tauRelSlow={p[3]:.1f}ms, knee={p[4]:.2f}dB, ratio={p[5]:.2f}, th=[{p[6]:.2f}, {p[7]:.2f}] dBFS")
    print(f"TEST_V2_HELDOUT_GR_RMS: PR65={rms1:.3f} dB, PR80={rms2:.3f} dB")

    out_hdr = Path("../../src/vocal/La2aCalibration.h")
    hdr = "\n".join([
        "#pragma once",
        "",
        "// Teletronix LA-2A T4B CdS dual-memory optical constants.",
        f"// Validated against SignalTrain LA-2A (Zenodo 3824876): held-out GR RMS = {rms1:.3f} dB (PR=65), {rms2:.3f} dB (PR=80).",
        "",
        "namespace stoneridge::vocal {",
        "",
        "struct La2aT4bConstants {",
        "    float tauAtkMs;",
        "    float tauRelFastMs;",
        "    float tauChargeMs;",
        "    float tauRelSlowMs;",
        "    float kneeDb;",
        "    float ratio;",
        "    float threshMinDb;",
        "    float threshMaxDb;",
        "};",
        "",
        "inline constexpr La2aT4bConstants kLa2aDefaultConstants{",
        f"    {p[0]:.3f}f, {p[1]:.2f}f, {p[2]:.2f}f, {p[3]:.2f}f,",
        f"    {p[4]:.3f}f, {p[5]:.3f}f, {p[6]:.3f}f, {p[7]:.3f}f",
        "};",
        "",
        "} // namespace stoneridge::vocal",
        "",
    ])
    out_hdr.write_text(hdr, encoding="ascii")
    print(f"Wrote {out_hdr}")


if __name__ == "__main__":
    main()
