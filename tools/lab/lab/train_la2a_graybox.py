"""Train and validate continuous-time Teletronix LA-2A gray-box models against
SignalTrain LA-2A hardware recordings (Zenodo 3824876).

Fits:
  1. Continuous-time R37 HF emphasis shelf + dual-memory CdS T4B constants
     (La2aT4bConstants + La2aR37Constants).
  2. 8-state GR-domain continuous-time diagonal SSM (La2aGrayBoxWeights /
     La2aGrayBoxResidual) parameterized in lambda_i < 0 s^-1 so alpha_i = exp(lambda_i / fs)
     is sample-rate invariant across 44.1 / 48 / 96 / 192 kHz.
  3. 8-state audio-domain continuous-time diagonal SSM + 12AX7A harmonic
     weights (La2aAudioWeights / La2aAudioStageModel) capturing HA-100X / HS-56
     transformer phase dispersion and bandwidth normalized to 0 dB @ 1 kHz.

Emits src/vocal/La2aCalibration.h and src/vocal/La2aGrayBoxWeights.h.
"""

from __future__ import annotations

import math
from pathlib import Path
import numpy as np
from scipy.optimize import least_squares
from scipy.signal import lfilter
import soundfile as sf


GR_LAMBDAS = np.array([-1.5, -4.0, -10.0, -24.0, -55.0, -120.0, -260.0, -580.0], dtype=np.float64)
AUD_LAMBDAS = np.array([-40.0, -160.0, -600.0, -1800.0, -5000.0, -14000.0, -32000.0, -68000.0], dtype=np.float64)


def _r37_biquad(fs: float, fc: float, gain_db: float) -> tuple[np.ndarray, np.ndarray]:
    w0 = 2.0 * math.pi * fc / fs
    a_lin = 10.0 ** (gain_db / 40.0)
    g = math.tan(w0 * 0.5)
    inv_a = 1.0 / a_lin
    denom = g + inv_a
    b0 = (g + a_lin) / denom
    b1 = (g - a_lin) / denom
    a1 = (g - inv_a) / denom
    return np.array([b0, b1], dtype=np.float64), np.array([1.0, a1], dtype=np.float64)


def _stream_block_envelopes(
    in_wav: Path,
    tgt_wav: Path,
    start_s: float,
    dur_s: float,
    block_ms: float = 10.0,
    fixed_mu_db: float | None = None,
    r37_fc: float = 2200.0,
    r37_gain_db: float = 2.5,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray, float, float]:
    info = sf.info(str(in_wav))
    fs = float(info.samplerate)
    blk = int(round(block_ms * 0.001 * fs))
    start_fr = int(round(start_s * fs))
    total_fr = (int(round(dur_s * fs)) // blk) * blk

    # Stream in 15-second chunks (< 15 MB peak RAM) to compute 10 ms RMS & R37 sidechain levels.
    chunk_blks = 1500
    chunk_fr = chunk_blks * blk
    b_r37, a_r37 = _r37_biquad(fs, r37_fc, r37_gain_db)
    zi_r37 = np.zeros(1, dtype=np.float64)

    dx_list: list[np.ndarray] = []
    dxhf_list: list[np.ndarray] = []
    dy_list: list[np.ndarray] = []

    offset = 0
    with sf.SoundFile(str(in_wav)) as fx, sf.SoundFile(str(tgt_wav)) as fy:
        fx.seek(start_fr)
        fy.seek(start_fr)
        while offset < total_fr:
            to_read = min(chunk_fr, total_fr - offset)
            x = fx.read(to_read, dtype="float64", always_2d=False)
            y = fy.read(to_read, dtype="float64", always_2d=False)
            if x.ndim > 1:
                x = x[:, 0]
            if y.ndim > 1:
                y = y[:, 0]
            xhf, zi_r37 = lfilter(b_r37, a_r37, x, zi=zi_r37)

            nb = len(x) // blk
            if nb <= 0:
                break
            xb = x[: nb * blk].reshape(nb, blk)
            xhfb = xhf[: nb * blk].reshape(nb, blk)
            yb = y[: nb * blk].reshape(nb, blk)

            rx = np.sqrt(np.mean(xb * xb, axis=1) + 1e-12)
            rxhf = np.sqrt(np.mean(xhfb * xhfb, axis=1) + 1e-12)
            ry = np.sqrt(np.mean(yb * yb, axis=1) + 1e-12)
            dx_list.append(20.0 * np.log10(np.maximum(rx, 1e-6)))
            dxhf_list.append(20.0 * np.log10(np.maximum(rxhf, 1e-6)))
            dy_list.append(20.0 * np.log10(np.maximum(ry, 1e-6)))
            offset += to_read

    dx = np.concatenate(dx_list)
    dx_hf = np.concatenate(dxhf_list)
    dy = np.concatenate(dy_list)

    low_mask = (dx > -48.0) & (dx < -34.0)
    local_mu = float(np.median(dy[low_mask] - dx[low_mask])) if np.any(low_mask) else 0.0
    mu = fixed_mu_db if fixed_mu_db is not None else local_mu
    gr_true = np.minimum(0.0, (dy - mu) - dx)
    active = dx > -45.0
    return dx, dx_hf, gr_true, active, dy, mu, local_mu


def _sim_opto_physical(dx: np.ndarray, dx_hf: np.ndarray, amount: float, p: np.ndarray, block_ms: float = 10.0) -> np.ndarray:
    tau_atk, tau_rf, tau_chg, tau_rs, knee, ratio, th0, th1, r37_mix = p
    th = th0 + (th1 - th0) * amount
    slope = 1.0 - 1.0 / max(1.1, ratio)
    ac = 1.0 - math.exp(-block_ms / max(0.5, tau_atk))
    cc = 1.0 - math.exp(-block_ms / max(5.0, tau_chg))
    rsc = 1.0 - math.exp(-block_ms / max(20.0, tau_rs))

    det_db = dx + r37_mix * (dx_hf - dx)
    gf = 0.0
    gs = 0.0
    out = np.empty_like(det_db)
    for i, db in enumerate(det_db):
        over = db - th
        if over > 0.5 * knee:
            drive = over * slope
        elif over > -0.5 * knee:
            t = over + 0.5 * knee
            drive = slope * (t * t) / (2.0 * knee)
        else:
            drive = 0.0

        sc = cc if drive > gs else rsc
        gs += (drive - gs) * sc
        tau_eff = tau_rf + (tau_rs - tau_rf) * (gs / (4.0 + gs))
        rfc = 1.0 - math.exp(-block_ms / max(1.0, tau_eff))
        fc = ac if drive > gf else rfc
        gf += (drive - gf) * fc
        out[i] = gf
    return out


def _apply_gr_ssm(gf: np.ndarray, ssm_weights: np.ndarray, dt_s: float) -> np.ndarray:
    # ssm_weights[0..7] = c_k (zero-DC pole diffs), ssm_weights[8] = c_d (proportional)
    corr = ssm_weights[8] * gf
    for k, lam in enumerate(GR_LAMBDAS):
        a = math.exp(lam * dt_s)
        hk = lfilter([1.0 - a], [1.0, -a], gf)
        corr += ssm_weights[k] * (hk - gf)
    return np.maximum(0.0, gf + corr)


def _fit_audio_stage(in_wav: Path, tgt_wav: Path, mu_db: float) -> tuple[np.ndarray, float, float, float]:
    info = sf.info(str(in_wav))
    fs = float(info.samplerate)
    start_fr = int(round(20.0 * fs))
    dur_fr = int(round(100.0 * fs))
    x, _ = sf.read(str(in_wav), start=start_fr, frames=dur_fr, dtype="float64", always_2d=False)
    y, _ = sf.read(str(tgt_wav), start=start_fr, frames=dur_fr, dtype="float64", always_2d=False)
    if x.ndim > 1:
        x = x[:, 0]
    if y.ndim > 1:
        y = y[:, 0]

    # Select low-to-moderate compression blocks (-46..-24 dBFS) for clean HA-100X/HS-56 + 12AX7A ID.
    blk = 441
    nb = len(x) // blk
    xb = x[: nb * blk].reshape(nb, blk)
    dx = 20.0 * np.log10(np.maximum(np.sqrt(np.mean(xb * xb, axis=1) + 1e-12), 1e-6))
    mask = np.repeat((dx > -46.0) & (dx < -24.0), blk)

    mu_lin = 10.0 ** (mu_db / 20.0)
    xs = x[: nb * blk] * mu_lin
    ys = y[: nb * blk]

    n_samp = len(xs)
    phi = np.empty((n_samp, 11), dtype=np.float64)
    for k, lam in enumerate(AUD_LAMBDAS):
        a = math.exp(lam / fs)
        phi[:, k] = lfilter([1.0 - a], [1.0, -a], xs)
    phi[:, 8] = xs

    dc_a = math.exp(-2.0 * math.pi * 10.0 / fs)
    x2 = xs * xs
    x2 = x2 - lfilter([1.0 - dc_a], [1.0, -dc_a], x2)
    phi[:, 9] = x2
    phi[:, 10] = xs * xs * xs

    phi_m = phi[mask]
    ym = ys[mask]
    reg = np.diag([2.5] * 8 + [0.5] + [5.0, 20.0])
    w = np.linalg.solve(phi_m.T @ phi_m + reg, phi_m.T @ ym)

    # Normalize linear part to exact 1.0 (0 dB) magnitude @ 1 kHz so AudioStageModel is unity-gain!
    h_1k = w[8] + sum(w[k] * (-AUD_LAMBDAS[k]) / (2j * math.pi * 1000.0 - AUD_LAMBDAS[k]) for k in range(8))
    g_1k = abs(h_1k)
    w[:9] /= g_1k
    # Bound 12AX7A cubic/quadratic coefficients so full-scale 0 dBFS vocals saturate cleanly without foldback.
    c2 = float(np.clip(w[9] / g_1k, -0.045, 0.045))
    c3 = float(np.clip(w[10] / g_1k, -0.090, 0.020))
    return w[:9], c2, c3, g_1k


def _sim_full_waveform_esr(
    in_wav: Path,
    tgt_wav: Path,
    start_s: float,
    dur_s: float,
    amount: float,
    mu_db: float,
    phys_p: np.ndarray,
    ssm_weights: np.ndarray,
    aud_w: np.ndarray,
    c2: float,
    c3: float,
) -> tuple[float, float]:
    info = sf.info(str(in_wav))
    fs = float(info.samplerate)
    start_fr = int(round(start_s * fs))
    num_fr = int(round(dur_s * fs))
    x, _ = sf.read(str(in_wav), start=start_fr, frames=num_fr, dtype="float64", always_2d=False)
    y, _ = sf.read(str(tgt_wav), start=start_fr, frames=num_fr, dtype="float64", always_2d=False)
    if x.ndim > 1:
        x = x[:, 0]
    if y.ndim > 1:
        y = y[:, 0]

    # Compute sample-accurate R37 + smoothed EL RMS envelope + sample-by-sample ODE + GR SSM
    tau_atk, tau_rf, tau_chg, tau_rs, knee, ratio, th0, th1, r37_mix = phys_p
    th = th0 + (th1 - th0) * amount
    slope = 1.0 - 1.0 / max(1.1, ratio)

    b_r37, a_r37 = _r37_biquad(fs, 2200.0, 2.5)
    xhf = lfilter(b_r37, a_r37, x)

    # 5 ms EL-panel thermal RMS follower (in C speed via lfilter!)
    a_el = math.exp(-1.0 / (0.005 * fs))
    rms_bb = np.sqrt(np.maximum(lfilter([1.0 - a_el], [1.0, -a_el], x * x), 1e-12))
    rms_hf = np.sqrt(np.maximum(lfilter([1.0 - a_el], [1.0, -a_el], xhf * xhf), 1e-12))
    det_db = 20.0 * np.log10(np.maximum(rms_bb, 1e-6)) + r37_mix * (
        20.0 * np.log10(np.maximum(rms_hf, 1e-6)) - 20.0 * np.log10(np.maximum(rms_bb, 1e-6))
    )

    # Downsampled 1 ms optical integration + linear upsample (equivalent to sample-accurate CdS >2ms)
    step = 44
    n_sub = len(det_db) // step
    det_sub = det_db[: n_sub * step].reshape(n_sub, step).mean(axis=1)
    dt_ms = (step / fs) * 1000.0
    gf_sub = _sim_opto_physical(det_sub, det_sub, amount, np.array([tau_atk, tau_rf, tau_chg, tau_rs, knee, ratio, th0, th1, 0.0]), block_ms=dt_ms)
    gc_sub = _apply_gr_ssm(gf_sub, ssm_weights, dt_ms * 0.001)
    t_sub = np.arange(n_sub) * step + step * 0.5
    gc_samp = np.interp(np.arange(len(x)), t_sub, gc_sub)

    gain_lin = 10.0 ** (-gc_samp * 0.05)
    gained = x * gain_lin * (10.0 ** (mu_db / 20.0))

    # Apply 8-state audio continuous-time transformer SSM + 12AX7A harmonics
    yhat = aud_w[8] * gained
    for k, lam in enumerate(AUD_LAMBDAS):
        a = math.exp(lam / fs)
        yhat += aud_w[k] * lfilter([1.0 - a], [1.0, -a], gained)
    dc_a = math.exp(-2.0 * math.pi * 10.0 / fs)
    g2 = gained * gained
    g2 = g2 - lfilter([1.0 - dc_a], [1.0, -dc_a], g2)
    yhat += c2 * g2 + c3 * (gained * gained * gained)

    # Compute overall waveform ESR + envelope GR RMSE on 10 ms blocks (skipping 0.5 s startup)
    s0 = int(0.5 * fs)
    esr = 10.0 * math.log10(float(np.mean((y[s0:] - yhat[s0:]) ** 2) / (np.mean(y[s0:] ** 2) + 1e-12)) + 1e-12)
    blk = 441
    nb = len(x) // blk
    gr_pred = -gc_samp[: nb * blk].reshape(nb, blk).mean(axis=1)
    rx = np.sqrt(np.mean(x[: nb * blk].reshape(nb, blk) ** 2, axis=1) + 1e-12)
    ry = np.sqrt(np.mean(y[: nb * blk].reshape(nb, blk) ** 2, axis=1) + 1e-12)
    dx_b = 20.0 * np.log10(np.maximum(rx, 1e-6))
    dy_b = 20.0 * np.log10(np.maximum(ry, 1e-6))
    gr_true = np.minimum(0.0, (dy_b - mu_db) - dx_b)
    act = dx_b > -45.0
    gr_rmse = float(np.sqrt(np.mean((gr_pred[act] - gr_true[act]) ** 2)))
    return gr_rmse, esr


def main() -> None:
    repo_root = Path(__file__).resolve().parents[3]
    st_dir = repo_root / "data" / "signaltrain_subset" / "Test"
    p65_in = st_dir / "input_256_.wav"
    p65_tg = st_dir / "target_256_LA2A_2c__1__65.wav"
    p80_in = st_dir / "input_259_.wav"
    p80_tg = st_dir / "target_259_LA2A_2c__1__80.wav"

    # Estimate consistent hardware session makeup gain on uncompressed low-level frames in 20..120s
    tr65_probe = _stream_block_envelopes(p65_in, p65_tg, 20.0, 100.0)
    tr80_probe = _stream_block_envelopes(p80_in, p80_tg, 20.0, 100.0)
    mu65 = tr65_probe[5]
    mu80 = tr80_probe[5]
    print(f"Hardware makeup gains (20..120s quiet baseline): PR65={mu65:.3f} dB, PR80={mu80:.3f} dB")

    # Training windows: 20..130s (disjoint from held-out 140..200s!)
    tr65 = _stream_block_envelopes(p65_in, p65_tg, 20.0, 110.0, fixed_mu_db=mu65)
    tr80 = _stream_block_envelopes(p80_in, p80_tg, 20.0, 110.0, fixed_mu_db=mu80)
    va65 = _stream_block_envelopes(p65_in, p65_tg, 140.0, 60.0, fixed_mu_db=mu65)
    va80 = _stream_block_envelopes(p80_in, p80_tg, 140.0, 60.0, fixed_mu_db=mu80)

    # Joint nonlinear optimization of physical T4B + R37 mix + zero-DC GR CT-SSM
    # Parameters: [tauAtk, tauRelFast, tauCharge, tauRelSlow, knee, ratio, thMin, thMax, r37Mix, c_0..c_7, c_d]
    x0 = np.array([10.0, 42.0, 480.0, 350.0, 10.0, 3.6, 0.0, -34.5, 0.12] + [0.0] * 9, dtype=np.float64)
    lb = np.array([1.2, 8.0, 40.0, 120.0, 1.5, 2.0, -10.0, -45.0, 0.0] + [-0.80] * 8 + [-0.25], dtype=np.float64)
    ub = np.array([35.0, 160.0, 1200.0, 3500.0, 16.0, 12.0, 0.0, -20.0, 0.45] + [0.80] * 8 + [0.25], dtype=np.float64)

    def joint_res(theta: np.ndarray) -> np.ndarray:
        phys = theta[:9]
        ssm_w = theta[9:]
        gf65 = _sim_opto_physical(tr65[0], tr65[1], 0.65, phys)
        gf80 = _sim_opto_physical(tr80[0], tr80[1], 0.80, phys)
        gc65 = _apply_gr_ssm(gf65, ssm_w, 0.01)
        gc80 = _apply_gr_ssm(gf80, ssm_w, 0.01)
        e65 = (-gc65 - tr65[2])[tr65[3]]
        e80 = (-gc80 - tr80[2])[tr80[3]]
        reg = 1.5 * ssm_w
        return np.concatenate([e65, e80, reg])

    fit = least_squares(joint_res, x0, bounds=(lb, ub), max_nfev=120)
    phys = fit.x[:9]
    ssm_w = fit.x[9:]

    # Also fit 8-state audio CT-SSM + 12AX7A stage
    aud_w, c2, c3, g_1k = _fit_audio_stage(p65_in, p65_tg, mu65)

    # Evaluate block 10ms GR RMSE
    for tag, seg, amt in [("Train PR65 (20..130s)", tr65, 0.65), ("Train PR80 (20..130s)", tr80, 0.80),
                          ("Held-Out Val PR65 (140..200s)", va65, 0.65), ("Held-Out Val PR80 (140..200s)", va80, 0.80)]:
        gf = _sim_opto_physical(seg[0], seg[1], amt, phys)
        gc = _apply_gr_ssm(gf, ssm_w, 0.01)
        act = seg[3]
        rms_trmu = float(np.sqrt(np.mean(((-gc) - seg[2])[act] ** 2)))
        gr_loc = np.minimum(0.0, (seg[4] - seg[6]) - seg[0])
        rms_locmu = float(np.sqrt(np.mean(((-gc) - gr_loc)[act] ** 2)))
        print(f"10ms GR {tag:30s}: RMSE={rms_trmu:.4f} dB (loc_mu={rms_locmu:.4f} dB)")

    # Evaluate sample-by-sample continuous-time waveform ESR + GR RMSE on 44.1 kHz WAVs
    tr65_gr, tr65_esr = _sim_full_waveform_esr(p65_in, p65_tg, 20.0, 60.0, 0.65, mu65, phys, ssm_w, aud_w, c2, c3)
    tr80_gr, tr80_esr = _sim_full_waveform_esr(p80_in, p80_tg, 20.0, 60.0, 0.80, mu80, phys, ssm_w, aud_w, c2, c3)
    va65_gr, va65_esr = _sim_full_waveform_esr(p65_in, p65_tg, 140.0, 60.0, 0.65, mu65, phys, ssm_w, aud_w, c2, c3)
    va80_gr, va80_esr = _sim_full_waveform_esr(p80_in, p80_tg, 140.0, 60.0, 0.80, mu80, phys, ssm_w, aud_w, c2, c3)
    print(f"Sample-level Waveform PR65: Train ESR={tr65_esr:.2f} dB (GR={tr65_gr:.4f} dB) | Held-Out Val ESR={va65_esr:.2f} dB (GR={va65_gr:.4f} dB)")
    print(f"Sample-level Waveform PR80: Train ESR={tr80_esr:.2f} dB (GR={tr80_gr:.4f} dB) | Held-Out Val ESR={va80_esr:.2f} dB (GR={va80_gr:.4f} dB)")

    # Convert zero-DC SSM weights into C++ La2aGrayBoxResidual weights:
    # C++ m_beta[i] = (beta[i]/lambda[i]) * (alpha[i]-1) => steady state is (-beta[i]/lambda[i])*u.
    # Choosing beta[i] = -lambda[i] sets steady state = 1.0*u (unity DC), so gamma[i] = c_k and delta = c_d - sum(c_k).
    gr_beta = -GR_LAMBDAS
    gr_gamma = ssm_w[:8]
    gr_delta = float(ssm_w[8] - np.sum(gr_gamma))

    aud_beta = -AUD_LAMBDAS
    aud_gamma = aud_w[:8]
    aud_delta = float(aud_w[8])

    # Emit src/vocal/La2aCalibration.h (exact 8-field La2aT4bConstants preserved)
    cal_path = repo_root / "src" / "vocal" / "La2aCalibration.h"
    cal_hdr = "\n".join([
        "#pragma once",
        "",
        "// Teletronix LA-2A T4B CdS dual-memory optical constants.",
        "// Auto-generated by tools/lab/lab/train_la2a_graybox.py from SignalTrain LA-2A (Zenodo 3824876).",
        f"// Held-out (140..200s): PR65 GR RMS = {va65_gr:.4f} dB (ESR {va65_esr:.2f} dB), "
        f"PR80 GR RMS = {va80_gr:.4f} dB (ESR {va80_esr:.2f} dB).",
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
        f"    {phys[0]:.3f}f, {phys[1]:.3f}f, {phys[2]:.3f}f, {phys[3]:.3f}f,",
        f"    {phys[4]:.3f}f, {phys[5]:.3f}f, {phys[6]:.3f}f, {phys[7]:.3f}f",
        "};",
        "",
        "struct La2aR37Constants {",
        "    float shelfFreqHz;",
        "    float shelfGainDb;",
        "    float sidechainBlend;",
        "};",
        "",
        "inline constexpr La2aR37Constants kLa2aR37Constants{",
        f"    2200.0f, 2.5f, {phys[8]:.4f}f",
        "};",
        "",
        "} // namespace stoneridge::vocal",
        "",
    ])
    cal_path.write_text(cal_hdr, encoding="ascii")

    # Emit src/vocal/La2aGrayBoxWeights.h
    def fmt_arr(arr: np.ndarray) -> str:
        return ", ".join(f"{float(v):.6f}f" for v in arr)

    gb_path = repo_root / "src" / "vocal" / "La2aGrayBoxWeights.h"
    gb_hdr = "\n".join([
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <cmath>",
        "",
        "// Auto-generated by tools/lab/lab/train_la2a_graybox.py from SignalTrain LA-2A (Zenodo 3824876).",
        "// Continuous-time diagonal SSM residuals (alpha_i = exp(lambda_i / fs)) in both GR and audio domains.",
        "",
        "namespace stoneridge::vocal {",
        "",
        "struct La2aGrayBoxWeights {",
        "    float lambda[8];",
        "    float beta[8];",
        "    float gamma[8];",
        "    float delta;",
        "};",
        "",
        "inline constexpr La2aGrayBoxWeights kLa2aGrayBoxWeights{",
        f"    {{ {fmt_arr(GR_LAMBDAS)} }},",
        f"    {{ {fmt_arr(gr_beta)} }},",
        f"    {{ {fmt_arr(gr_gamma)} }},",
        f"    {gr_delta:.6f}f",
        "};",
        "",
        "struct La2aAudioWeights {",
        "    float lambda[8];",
        "    float beta[8];",
        "    float gamma[8];",
        "    float delta;",
        "    float tubeC2;",
        "    float tubeC3;",
        "};",
        "",
        "inline constexpr La2aAudioWeights kLa2aAudioWeights{",
        f"    {{ {fmt_arr(AUD_LAMBDAS)} }},",
        f"    {{ {fmt_arr(aud_beta)} }},",
        f"    {{ {fmt_arr(aud_gamma)} }},",
        f"    {aud_delta:.6f}f,",
        f"    {c2:.6f}f, {c3:.6f}f",
        "};",
        "",
        "class La2aGrayBoxResidual {",
        "public:",
        "    void Prepare(double sampleRate) noexcept {",
        "        const float fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);",
        "        for (int i = 0; i < 8; ++i) {",
        "            const float lambda = kLa2aGrayBoxWeights.lambda[i];",
        "            m_alpha[i] = std::exp(lambda / fs);",
        "            m_beta[i] = (kLa2aGrayBoxWeights.beta[i] / lambda) * (m_alpha[i] - 1.0f);",
        "        }",
        "        Reset();",
        "    }",
        "",
        "    void Reset() noexcept {",
        "        for (float& h : m_h) h = 0.0f;",
        "    }",
        "",
        "    inline float Process(float u) noexcept {",
        "        float y = kLa2aGrayBoxWeights.delta * u;",
        "        for (int i = 0; i < 8; ++i) {",
        "            m_h[i] = m_alpha[i] * m_h[i] + m_beta[i] * u;",
        "            y += kLa2aGrayBoxWeights.gamma[i] * m_h[i];",
        "        }",
        "        return y;",
        "    }",
        "",
        "private:",
        "    float m_alpha[8]{};",
        "    float m_beta[8]{};",
        "    float m_h[8]{};",
        "};",
        "",
        "// Continuous-time HA-100X / HS-56 transformer dispersion + 12AX7A mild saturation stage.",
        "class La2aAudioStageModel {",
        "public:",
        "    void Prepare(double sampleRate) noexcept {",
        "        const float fs = static_cast<float>((sampleRate > 1000.0) ? sampleRate : 48000.0);",
        "        for (int i = 0; i < 8; ++i) {",
        "            const float lambda = kLa2aAudioWeights.lambda[i];",
        "            m_alpha[i] = std::exp(lambda / fs);",
        "            m_beta[i] = (kLa2aAudioWeights.beta[i] / lambda) * (m_alpha[i] - 1.0f);",
        "        }",
        "        m_dcAlpha = std::exp(-2.0f * 3.14159265358979323846f * 10.0f / fs);",
        "        Reset();",
        "    }",
        "",
        "    void Reset() noexcept {",
        "        for (float& h : m_h) h = 0.0f;",
        "        m_dcState = 0.0f;",
        "    }",
        "",
        "    inline float Process(float x) noexcept {",
        "        float y = kLa2aAudioWeights.delta * x;",
        "        for (int i = 0; i < 8; ++i) {",
        "            m_h[i] = m_alpha[i] * m_h[i] + m_beta[i] * x;",
        "            y += kLa2aAudioWeights.gamma[i] * m_h[i];",
        "        }",
        "        const float x2 = x * x;",
        "        m_dcState += (x2 - m_dcState) * (1.0f - m_dcAlpha);",
        "        const float x2Ac = x2 - m_dcState;",
        "        return y + kLa2aAudioWeights.tubeC2 * x2Ac + kLa2aAudioWeights.tubeC3 * (x2 * x);",
        "    }",
        "",
        "private:",
        "    float m_alpha[8]{};",
        "    float m_beta[8]{};",
        "    float m_h[8]{};",
        "    float m_dcAlpha = 0.998f;",
        "    float m_dcState = 0.0f;",
        "};",
        "",
        "} // namespace stoneridge::vocal",
        "",
    ])
    gb_path.write_text(gb_hdr, encoding="ascii")
    print(f"Wrote {cal_path} and {gb_path}")


if __name__ == "__main__":
    main()
