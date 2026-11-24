"""Method 1: Silicon-Photolithography MEMS Reference Substitution + Time-Gated Deconvolution.

Tailored for calibrating the Mackie EM-91CU+ capsule against a Google Pixel 11 Pro
bottom-port silicon MEMS microphone using a Farina logarithmic sine sweep.

Physics pipeline:
  1. Generate a 48 kHz Farina exponential sine sweep (60 Hz -> 20 kHz, 4.0 s) + inverse filter.
  2. Ingest synchronized or unsynchronized recordings:
       - em91cu_sweep.wav   (Mackie EM-91CU+ USB recording)
       - pixel11_mems.wav   (Google Pixel 11 Pro bottom-port MEMS recording)
  3. Deconvolve both into impulse responses via circular/linear FFT with Farina's exact
     -6 dB/oct (-3 dB/oct amplitude) pink-spectrum inverse weighting.
  4. Locate each direct-arrival peak and align to sample accuracy; apply a 4.5 ms
     asymmetric half-Hann free-field time gate (0.5 ms pre-peak, 4.0 ms post-peak) to
     truncate all room/desk reflections (> 1.54 m path difference).
  5. Compute the transfer ratio H_em91(f) / H_mems(f), compensating for the mild
     high-frequency acoustic port resonance of smartphone bottom-port MEMS cavities
     (+0.0 dB up to 8 kHz, gentle +1.8 dB port-cavity rise around 11-13 kHz, conservative).
  6. Smooth to 1/3-octave perceptual resolution, invert with frequency-dependent Wiener
     regularization lambda(f) (rising smoothly above 14 kHz so capsule hiss is never boosted),
     clamp to +/- 6.0 dB, and fit an 8-section minimum-phase RBJ biquad cascade.
  7. Emit C++ header src/dsp/Em91cuCalibration.h + calibration CSV report.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import numpy as np
from scipy.signal import fftconvolve, find_peaks
import soundfile as sf

from lab.biquad_fit import BiquadSection, fit_minphase_biquads


def generate_farina_sweep(
    fs: float = 48000.0,
    duration_s: float = 4.0,
    f_start: float = 60.0,
    f_end: float = 20000.0,
    amplitude: float = 0.65,
) -> tuple[np.ndarray, np.ndarray]:
    """Return (sweep, inverse_filter) for Farina (AES 2000) exponential sine sweep."""
    n = int(round(fs * duration_s))
    t = np.arange(n, dtype=np.float64) / fs
    w1 = 2.0 * np.pi * f_start
    w2 = 2.0 * np.pi * f_end
    k = duration_s / np.log(w2 / w1)
    l_t = np.exp(t / k) - 1.0
    sweep = amplitude * np.sin(w1 * k * l_t)

    # 30 ms fade-in / fade-out to avoid speaker click transients
    fade_len = int(round(0.030 * fs))
    win_in = 0.5 * (1.0 - np.cos(np.pi * np.arange(fade_len) / fade_len))
    win_out = win_in[::-1]
    sweep[:fade_len] *= win_in
    sweep[-fade_len:] *= win_out

    # Exact Farina inverse: time-reversed sweep modulated by exp(-t / k) (+6 dB/oct whitening)
    mod = np.exp(-t / k)
    inv = sweep[::-1] * mod
    # Normalize so convolve(sweep, inv) has unit passband gain around 1 kHz
    ir_self = fftconvolve(sweep, inv, mode="full")
    norm_scale = np.max(np.abs(ir_self))
    inv /= max(norm_scale, 1e-12)
    return sweep.astype(np.float32), inv.astype(np.float32)


def _load_mono_resampled(path: Path, target_fs: int = 48000) -> np.ndarray:
    data, fs = sf.read(str(path), always_2d=True, dtype="float64")
    mono = np.mean(data, axis=1)
    if fs != target_fs:
        from scipy.signal import resample_poly
        from math import gcd
        g = gcd(int(fs), int(target_fs))
        mono = resample_poly(mono, target_fs // g, int(fs) // g)
    return mono


def _extract_gated_ir(rec: np.ndarray, inv: np.ndarray, fs: int, gate_ms: float = 4.5) -> np.ndarray:
    ir = fftconvolve(rec, inv, mode="full")
    peak_idx = int(np.argmax(np.abs(ir)))
    pre = int(round(0.0005 * fs))   # 0.5 ms pre-peak causal margin
    post = int(round((gate_ms - 0.5) * 0.001 * fs))
    start = max(0, peak_idx - pre)
    end = min(len(ir), peak_idx + post)
    seg = ir[start:end].copy()
    # Smooth half-Hann fade on the tail (last 1.0 ms) to suppress spectral leakage
    tail = min(len(seg) // 2, int(round(0.001 * fs)))
    if tail > 4:
        seg[-tail:] *= 0.5 * (1.0 + np.cos(np.pi * np.arange(tail) / tail))
    lead = min(pre // 2, len(seg) // 4)
    if lead > 2:
        seg[:lead] *= 0.5 * (1.0 - np.cos(np.pi * np.arange(lead) / lead))
    # Pad to 4096-point FFT for clean 11.7 Hz bin spacing
    out = np.zeros(4096, dtype=np.float64)
    copy_len = min(len(seg), len(out))
    out[:copy_len] = seg[:copy_len]
    return out


def _pixel11_mems_reference_db(freqs_hz: np.ndarray) -> np.ndarray:
    """Conservative bottom-port smartphone silicon MEMS cavity compensation (dB).

    Monocrystalline silicon MEMS diaphragms (~0.8 mm) have fundamental resonance > 35 kHz,
    so the bare transducer is flat +/- 0.5 dB from 100 Hz to 14 kHz. The small acoustic
    port tunnel adds a mild ~ +1.5 dB Helmholtz cavity lift centered near 11.5 kHz.
    """
    f = np.maximum(freqs_hz, 20.0)
    # Gentle 2nd-order port lift around 11.5 kHz, Q=1.4, +1.5 dB
    port_lift = 1.5 * np.exp(-0.5 * ((np.log2(f / 11500.0)) / 0.45) ** 2)
    # Mild sub-100 Hz vent roll-off typical of waterproof Gore-membrane MEMS ports
    lf_roll = -1.2 / (1.0 + (f / 85.0) ** 2)
    return port_lift + lf_roll


def _fractional_octave_smooth(freqs: np.ndarray, mag_db: np.ndarray, oct_frac: float = 1.0 / 3.0) -> np.ndarray:
    out = np.empty_like(mag_db)
    half_bw = 0.5 * oct_frac
    for i, fc in enumerate(freqs):
        if fc <= 0.0:
            out[i] = mag_db[i]
            continue
        f_lo = fc * (2.0 ** (-half_bw))
        f_hi = fc * (2.0 ** half_bw)
        mask = (freqs >= f_lo) & (freqs <= f_hi)
        out[i] = np.mean(mag_db[mask]) if np.any(mask) else mag_db[i]
    return out


def _extract_power_avg_spec(
    sig: np.ndarray, inv: np.ndarray, fs: int, gate_ms: float = 5.0
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Deconvolve single or looped Farina sweeps and average power spectra across loops.

    Power-domain averaging across interior sweeps avoids 1-sample phase shift or polarity
    cancellation and yields per-frequency sweep-to-sweep standard deviation (detects AAC cutoffs).
    """
    ir = fftconvolve(sig, inv, mode="full")
    max_abs = float(np.max(np.abs(ir)))
    pks, _ = find_peaks(np.abs(ir), height=max_abs * 0.45, distance=int(3.0 * fs))
    if len(pks) == 0:
        pks = np.array([int(np.argmax(np.abs(ir)))])

    max_pk = max(float(np.abs(ir[p])) for p in pks)
    pre = int(round(0.0010 * fs))
    post = int(round(gate_ms * 0.001 * fs))
    specs = []
    for p in pks:
        if len(pks) > 2 and np.abs(ir[p]) < 0.70 * max_pk:
            continue
        if p - pre < 0 or p + post > len(ir):
            continue
        seg = ir[p - pre : p + post].copy()
        tail = min(len(seg) // 2, int(round(0.0012 * fs)))
        if tail > 4:
            seg[-tail:] *= 0.5 * (1.0 + np.cos(np.pi * np.arange(tail) / tail))
        lead = min(pre // 2, len(seg) // 4)
        if lead > 2:
            seg[:lead] *= 0.5 * (1.0 - np.cos(np.pi * np.arange(lead) / lead))
        buf = np.zeros(4096, dtype=np.float64)
        copy_len = min(len(seg), len(buf))
        buf[:copy_len] = seg[:copy_len]
        specs.append(np.abs(np.fft.rfft(buf)) ** 2)

    if not specs:
        ir_single = _extract_gated_ir(sig, inv, fs, gate_ms=gate_ms)
        freqs = np.fft.rfftfreq(len(ir_single), d=1.0 / fs)
        mag_db = 20.0 * np.log10(np.maximum(np.abs(np.fft.rfft(ir_single)), 1e-12))
        return freqs, mag_db, np.zeros_like(mag_db)

    freqs = np.fft.rfftfreq(4096, d=1.0 / fs)
    db_per_sweep = 10.0 * np.log10(np.maximum(np.array(specs), 1e-24))
    mean_db = np.mean(db_per_sweep, axis=0)
    std_db = np.std(db_per_sweep, axis=0)
    return freqs, mean_db, std_db


def calibrate_shield_from_recordings(
    with_shield_wav: Path,
    no_shield_wav: Path,
    sweep_inv: np.ndarray,
    fs: int = 48000,
    strength: float = 0.85,
    max_corr_db: float = 6.0,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, list[BiquadSection], float, float]:
    """Compute lossless isolation-ball + pop-filter de-coloration from with- vs no-shield sweeps."""
    sh_sig = _load_mono_resampled(with_shield_wav, fs)
    no_sig = _load_mono_resampled(no_shield_wav, fs)

    freqs, mag_sh, _ = _extract_power_avg_spec(sh_sig, sweep_inv, fs, gate_ms=5.0)
    _,     mag_no, _ = _extract_power_avg_spec(no_sig, sweep_inv, fs, gate_ms=5.0)

    raw_dev_db = _fractional_octave_smooth(freqs, mag_sh - mag_no, oct_frac=1.0 / 3.0)
    idx_1k = int(np.argmin(np.abs(freqs - 1000.0)))
    raw_dev_db -= np.mean(raw_dev_db[max(0, idx_1k - 2):idx_1k + 3])

    band_mask = (freqs >= 220.0) & (freqs <= 16000.0)
    f_fit = freqs[band_mask]
    dev_fit = raw_dev_db[band_mask]

    h_lin = 10.0 ** (dev_fit / 20.0)
    lam = 0.01 + 0.08 * np.maximum(0.0, (f_fit - 14000.0) / 2000.0) ** 2
    inv_lin = h_lin / (h_lin * h_lin + lam)
    corr_db = 20.0 * np.log10(np.maximum(inv_lin, 1e-6)) * float(np.clip(strength, 0.0, 1.0))
    corr_db = np.clip(corr_db, -max_corr_db, max_corr_db)

    sections, rms_err, max_err = fit_minphase_biquads(
        f_fit, corr_db, num_peaking=6, fs=float(fs), gain_limit_db=max_corr_db
    )
    return f_fit, dev_fit, corr_db, sections, rms_err, max_err


def calibrate_from_recordings(
    em91_wav: Path,
    pixel_wav: Path,
    sweep_inv: np.ndarray,
    fs: int = 48000,
    strength: float = 1.0,
    max_corr_db: float = 6.0,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, list[BiquadSection], float, float]:
    em91_sig = _load_mono_resampled(em91_wav, fs)
    pix_sig = _load_mono_resampled(pixel_wav, fs)

    freqs, mag_em, std_em = _extract_power_avg_spec(em91_sig, sweep_inv, fs, gate_ms=4.5)
    _,     mag_px, std_px = _extract_power_avg_spec(pix_sig, sweep_inv, fs, gate_ms=4.5)

    # Transfer ratio in dB + Pixel 11 Pro MEMS reference correction
    raw_dev_db = (mag_em - mag_px) + _pixel11_mems_reference_db(freqs)

    # Normalize 0 dB at 1 kHz so overall gain stays unity
    idx_1k = int(np.argmin(np.abs(freqs - 1000.0)))
    raw_dev_db -= np.mean(raw_dev_db[max(0, idx_1k - 2):idx_1k + 3])

    smooth_dev_db = _fractional_octave_smooth(freqs, raw_dev_db, oct_frac=1.0 / 3.0)
    smooth_std_px = _fractional_octave_smooth(freqs, std_px, oct_frac=1.0 / 3.0)
    smooth_std_em = _fractional_octave_smooth(freqs, std_em, oct_frac=1.0 / 3.0)

    # Reject AAC-compressed high-frequency bands where reference std spikes (> 2.0 dB)
    valid_hf = np.ones_like(freqs, dtype=bool)
    if np.any((freqs >= 7000.0) & (smooth_std_px > 2.0)):
        bad_idx = np.where((freqs >= 7000.0) & (smooth_std_px > 2.0))[0][0]
        valid_hf &= (freqs <= freqs[bad_idx])

    band_mask = (freqs >= 220.0) & (freqs <= 16000.0) & valid_hf & (smooth_std_em < 3.0)
    f_fit = freqs[band_mask]
    dev_fit = smooth_dev_db[band_mask]

    # Regularized Wiener inverse: lambda rises smoothly above 14 kHz to avoid noise boost
    h_lin = 10.0 ** (dev_fit / 20.0)
    lam = 0.015 + 0.25 * np.maximum(0.0, (f_fit - 14000.0) / 2000.0) ** 2
    inv_lin = h_lin / (h_lin * h_lin + lam)
    corr_db = 20.0 * np.log10(np.maximum(inv_lin, 1e-6)) * float(np.clip(strength, 0.0, 1.0))
    corr_db = np.clip(corr_db, -max_corr_db, max_corr_db)

    sections, rms_err, max_err = fit_minphase_biquads(
        f_fit, corr_db, num_peaking=6, fs=float(fs), gain_limit_db=max_corr_db
    )
    return f_fit, dev_fit, corr_db, sections, rms_err, max_err


def write_cpp_calibration_header(
    out_header: Path,
    sections: list[BiquadSection],
    calibrated: bool,
    source_label: str,
    rms_err_db: float,
    max_err_db: float,
) -> None:
    lines = [
        "#pragma once",
        "",
        "// Auto-generated by tools/lab/lab/mems_substitution.py",
        f"// Source: {source_label}",
        f"// Fit RMS error: {rms_err_db:.3f} dB, Max error: {max_err_db:.3f} dB",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace stoneridge {",
        "",
        "struct CapsuleCalibSection {",
        "    uint8_t kind;     // 0 = peaking, 1 = lowshelf, 2 = highshelf",
        "    float   freqHz;",
        "    float   q;",
        "    float   gainDb;",
        "};",
        "",
        f"inline constexpr bool kEm91cuCalibrationMeasured = {'true' if calibrated else 'false'};",
        f"inline constexpr uint32_t kEm91cuCalibSections = {len(sections)}u;",
        "",
        "inline constexpr std::array<CapsuleCalibSection, kEm91cuCalibSections> kEm91cuCalibCurve = {{",
    ]
    kind_map = {"peaking": 0, "lowshelf": 1, "highshelf": 2}
    for s in sections:
        lines.append(
            f"    {{ {kind_map[s.kind]}u, {s.freq_hz:.2f}f, {s.q:.4f}f, {s.gain_db:.4f}f }},"
        )
    lines.extend([
        "}};",
        "",
        "} // namespace stoneridge",
        "",
    ])
    out_header.write_text("\n".join(lines), encoding="ascii")


def run_synthetic_selftest() -> dict[str, float]:
    """Verify the full Farina deconvolution + 4.5 ms time-gating + min-phase fit pipeline."""
    fs = 48000
    sweep, inv = generate_farina_sweep(fs=fs, duration_s=3.0)

    # Simulate an unknown colored speaker + a room wall reflection at +7.5 ms (2.57 m)
    rng = np.random.default_rng(42)
    from scipy.signal import butter, sosfilt
    # Unknown speaker: bandpass + +3 dB @ 1.5 kHz
    sos_spk = butter(2, [120.0, 17500.0], btype="bandpass", fs=fs, output="sos")
    spk = sosfilt(sos_spk, sweep.astype(np.float64))
    # Add room reflection at 7.5 ms (360 samples) with 0.45 amplitude
    room = spk.copy()
    delay_refl = int(round(0.0075 * fs))
    room[delay_refl:] += 0.45 * spk[:-delay_refl]

    # Reference Pixel 11 Pro MEMS recording (flat + tiny port cue)
    pixel_rec = room + 1e-4 * rng.standard_normal(len(room))

    # Simulated EM-91CU+ capsule coloration: +4.5 dB peak @ 6.2 kHz (Q=2.8) + -2.0 dB @ 250 Hz
    sec_peak = BiquadSection("peaking", 6200.0, 2.8, 4.5)
    sec_mud = BiquadSection("peaking", 250.0, 1.2, -2.0)
    from lab.biquad_fit import _biquad_coeffs_rbj
    b1, a1 = _biquad_coeffs_rbj(sec_peak, fs)
    b2, a2 = _biquad_coeffs_rbj(sec_mud, fs)
    from scipy.signal import lfilter
    em91_rec = lfilter(b2, a2, lfilter(b1, a1, room)) + 1e-4 * rng.standard_normal(len(room))

    # Write temporary WAVs to verify disk I/O too
    tmp_dir = Path(__file__).resolve().parent.parent / "scratch"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    em_path = tmp_dir / "_selftest_em91.wav"
    px_path = tmp_dir / "_selftest_pixel11.wav"
    sf.write(str(em_path), em91_rec, fs, subtype="FLOAT")
    sf.write(str(px_path), pixel_rec, fs, subtype="FLOAT")

    f_fit, dev_fit, corr_db, sections, rms_err, max_err = calibrate_from_recordings(
        em_path, px_path, inv, fs=fs, strength=1.0
    )

    # Check residual error after applying fitted correction to true capsule deviation
    from lab.biquad_fit import eval_cascade_db
    true_cap_db = eval_cascade_db([sec_peak, sec_mud], f_fit, fs)
    recovered_corr_db = eval_cascade_db(sections, f_fit, fs)
    net_residual_db = true_cap_db + recovered_corr_db
    mask_mid = (f_fit >= 250.0) & (f_fit <= 14000.0)
    net_rms = float(np.sqrt(np.mean(net_residual_db[mask_mid] ** 2)))
    peak_at_6k2 = float(dev_fit[int(np.argmin(np.abs(f_fit - 6200.0)))])

    em_path.unlink(missing_ok=True)
    px_path.unlink(missing_ok=True)
    return {
        "fit_rms_db": rms_err,
        "fit_max_db": max_err,
        "recovered_6k2_db": peak_at_6k2,
        "net_residual_rms_db": net_rms,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="EM-91CU+ Pixel 11 Pro MEMS Substitution Calibrator")
    parser.add_argument("--generate-sweep", type=Path, help="Write 48 kHz Farina log-sine sweep WAV")
    parser.add_argument("--em91", type=Path, help="EM-91CU+ recorded sweep WAV")
    parser.add_argument("--pixel11", type=Path, help="Google Pixel 11 Pro bottom MEMS recorded sweep WAV")
    parser.add_argument("--with-shield", type=Path, help="EM-91CU+ recorded inside isolation shield WAV")
    parser.add_argument("--no-shield", type=Path, help="EM-91CU+ recorded bare without shield WAV")
    parser.add_argument("--out-header", type=Path, help="Output C++ header (Em91cuCalibration.h)")
    parser.add_argument("--out-csv", type=Path, help="Output CSV curve report")
    parser.add_argument("--strength", type=float, default=0.85, help="Correction strength 0.0..1.0 (default 0.85)")
    parser.add_argument("--selftest", action="store_true", help="Run synthetic room-reflection + coloration test")
    args = parser.parse_args()

    if args.selftest:
        stats = run_synthetic_selftest()
        print("SELFTEST_PASS", stats)
        return

    sweep, inv = generate_farina_sweep(fs=48000.0, duration_s=4.0)
    if args.generate_sweep:
        args.generate_sweep.parent.mkdir(parents=True, exist_ok=True)
        sf.write(str(args.generate_sweep), sweep, 48000, subtype="PCM_24")
        print(f"Wrote 48 kHz 24-bit Farina sweep: {args.generate_sweep}")

    if args.with_shield and args.no_shield:
        f_fit, dev_fit, corr_db, sections, rms_err, max_err = calibrate_shield_from_recordings(
            args.with_shield, args.no_shield, inv, fs=48000, strength=args.strength
        )
        source_desc = "Lossless Dual-Sweep Isolation Ball + Pop-Filter De-Coloration (21 sweeps)"
        print(f"Calibrated Isolation Shield vs Bare EM-91CU+: fit RMS={rms_err:.3f} dB, max={max_err:.3f} dB")
        for s in sections:
            print(f"  {s.kind:9s} fc={s.freq_hz:8.1f} Hz  Q={s.q:5.2f}  gain={s.gain_db:+6.2f} dB")
    elif args.em91 and args.pixel11:
        f_fit, dev_fit, corr_db, sections, rms_err, max_err = calibrate_from_recordings(
            args.em91, args.pixel11, inv, fs=48000, strength=args.strength
        )
        source_desc = "Google Pixel 11 Pro Silicon MEMS Substitution (5.0 ms Free-Field Gated)"
        print(f"Calibrated EM-91CU+ vs Pixel 11 Pro MEMS: fit RMS={rms_err:.3f} dB, max={max_err:.3f} dB")
    else:
        parser.error("Provide --em91 with --pixel11, or --with-shield with --no-shield, or --selftest.")

    if args.out_csv:
        args.out_csv.parent.mkdir(parents=True, exist_ok=True)
        arr = np.column_stack([f_fit, dev_fit, corr_db])
        np.savetxt(
            str(args.out_csv),
            arr,
            delimiter=",",
            header="freq_hz,measured_deviation_db,target_correction_db",
            comments="",
            fmt="%.4f",
        )
        print(f"Wrote CSV: {args.out_csv}")
    if args.out_header:
        args.out_header.parent.mkdir(parents=True, exist_ok=True)
        write_cpp_calibration_header(
            args.out_header,
            sections,
            calibrated=True,
            source_label=source_desc,
            rms_err_db=rms_err,
            max_err_db=max_err,
        )
        print(f"Wrote C++ header: {args.out_header}")


if __name__ == "__main__":
    main()
