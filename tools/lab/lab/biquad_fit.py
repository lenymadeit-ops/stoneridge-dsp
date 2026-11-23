"""Generic minimum-phase biquad cascade fitter for Stoneridge DSP.

Fits N second-order peaking / shelf sections to a target log-magnitude curve
H_target(f) in dB using bounded nonlinear least squares (scipy.optimize.least_squares).
Each section uses standard RBJ Audio EQ Cookbook minimum-phase coefficients
so the resulting cascade is strictly minimum-phase and stable at any sample rate.
"""

from __future__ import annotations

from dataclasses import dataclass
import numpy as np
from scipy.optimize import least_squares


@dataclass
class BiquadSection:
    kind: str  # "peaking", "lowshelf", "highshelf"
    freq_hz: float
    q: float
    gain_db: float


def _biquad_coeffs_rbj(sec: BiquadSection, fs: float) -> tuple[np.ndarray, np.ndarray]:
    A = 10.0 ** (sec.gain_db / 40.0)
    f = float(np.clip(sec.freq_hz, 20.0, fs * 0.45))
    q = float(np.clip(sec.q, 0.35, 10.0))
    w0 = 2.0 * np.pi * f / fs
    cs = np.cos(w0)
    sn = np.sin(w0)
    alpha = sn / (2.0 * q)

    if sec.kind == "lowshelf":
        two_sqrt_a_alpha = 2.0 * np.sqrt(A) * alpha
        a0 = (A + 1.0) + (A - 1.0) * cs + two_sqrt_a_alpha
        b0 = A * ((A + 1.0) - (A - 1.0) * cs + two_sqrt_a_alpha)
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs)
        b2 = A * ((A + 1.0) - (A - 1.0) * cs - two_sqrt_a_alpha)
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs)
        a2 = (A + 1.0) + (A - 1.0) * cs - two_sqrt_a_alpha
    elif sec.kind == "highshelf":
        two_sqrt_a_alpha = 2.0 * np.sqrt(A) * alpha
        a0 = (A + 1.0) - (A - 1.0) * cs + two_sqrt_a_alpha
        b0 = A * ((A + 1.0) + (A - 1.0) * cs + two_sqrt_a_alpha)
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs)
        b2 = A * ((A + 1.0) + (A - 1.0) * cs - two_sqrt_a_alpha)
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs)
        a2 = (A + 1.0) - (A - 1.0) * cs - two_sqrt_a_alpha
    else:  # peaking
        a0 = 1.0 + alpha / A
        b0 = 1.0 + alpha * A
        b1 = -2.0 * cs
        b2 = 1.0 - alpha * A
        a1 = -2.0 * cs
        a2 = 1.0 - alpha / A

    b = np.array([b0, b1, b2], dtype=np.float64) / a0
    a = np.array([1.0, a1 / a0, a2 / a0], dtype=np.float64)
    return b, a


def eval_cascade_db(sections: list[BiquadSection], freqs_hz: np.ndarray, fs: float = 48000.0) -> np.ndarray:
    w = 2.0 * np.pi * freqs_hz / fs
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    h_total = np.ones_like(w, dtype=np.complex128)
    for sec in sections:
        b, a = _biquad_coeffs_rbj(sec, fs)
        num = b[0] + b[1] * z1 + b[2] * z2
        den = a[0] + a[1] * z1 + a[2] * z2
        h_total *= num / den
    return 20.0 * np.log10(np.maximum(np.abs(h_total), 1e-9))


def fit_minphase_biquads(
    freqs_hz: np.ndarray,
    target_db: np.ndarray,
    num_peaking: int = 6,
    fs: float = 48000.0,
    gain_limit_db: float = 8.0,
) -> tuple[list[BiquadSection], float, float]:
    """Fit [lowshelf + num_peaking + highshelf] to target_db over freqs_hz.

    Returns (sections, rms_error_db, max_error_db).
    """
    f_min = float(np.min(freqs_hz))
    f_max = float(np.max(freqs_hz))
    # Initialize center frequencies log-spaced across [220 Hz .. 12 kHz]
    peak_centers = np.geomspace(max(220.0, f_min * 1.4), min(12500.0, f_max * 0.8), num_peaking)

    # Parameter vector:
    # lowshelf:  [f_ls, q_ls, g_ls]
    # peaking*N: [f_k, q_k, g_k]
    # highshelf: [f_hs, q_hs, g_hs]
    x0 = [max(240.0, f_min * 1.1), 0.707, 0.0]
    lb = [max(220.0, f_min), 0.45, -gain_limit_db]
    ub = [450.0, 1.40, gain_limit_db]

    for fc in peak_centers:
        # Interpolate initial gain from target curve
        g_init = float(np.clip(np.interp(fc, freqs_hz, target_db) * 0.4, -gain_limit_db, gain_limit_db))
        x0.extend([float(fc), 1.6, g_init])
        lb.extend([max(120.0, fc * 0.45), 0.5, -gain_limit_db])
        ub.extend([min(16500.0, fc * 2.2), 6.0, gain_limit_db])

    x0.extend([12000.0, 0.707, 0.0])
    lb.extend([6000.0, 0.45, -gain_limit_db])
    ub.extend([16500.0, 1.40, gain_limit_db])

    def unpack(x: np.ndarray) -> list[BiquadSection]:
        secs = [BiquadSection("lowshelf", x[0], x[1], x[2])]
        idx = 3
        for _ in range(num_peaking):
            secs.append(BiquadSection("peaking", x[idx], x[idx + 1], x[idx + 2]))
            idx += 3
        secs.append(BiquadSection("highshelf", x[idx], x[idx + 1], x[idx + 2]))
        return secs

    # Perceptual log-frequency weighting (uniform per octave)
    weights = np.ones_like(freqs_hz, dtype=np.float64)
    weights[(freqs_hz < 180.0) | (freqs_hz > 14000.0)] = 0.7

    def residuals(x: np.ndarray) -> np.ndarray:
        secs_cur = unpack(x)
        pred = eval_cascade_db(secs_cur, freqs_hz, fs)
        # Tiny L2 ridge on section gains prevents redundant opposing +6 / -6 dB sections
        gains = np.array([s.gain_db for s in secs_cur], dtype=np.float64)
        return np.concatenate([(pred - target_db) * weights, 0.08 * gains])

    res = least_squares(residuals, x0, bounds=(lb, ub), max_nfev=600)
    secs = unpack(res.x)
    pred = eval_cascade_db(secs, freqs_hz, fs)
    err = pred - target_db
    return secs, float(np.sqrt(np.mean(err ** 2))), float(np.max(np.abs(err)))
