# EM-91CU+ Calibration via Google Pixel 11 Pro (Method 1: Silicon MEMS Substitution)

A smartphone's bottom-port microphone uses a photolithographically etched monocrystalline silicon MEMS diaphragm (~0.8 mm) whose fundamental mechanical resonance sits above 35 kHz. Using a 4.5 ms free-field time gate on a Farina exponential sine sweep cancels both your speaker coloration and room reflections.

## 2-Minute Recording Protocol (Works With Your Foam Isolation Ball / "Eyeball" Shield)

Because `mems_substitution.py` deconvolves and time-aligns each impulse response independently, the two recordings **do not** have to be simultaneous. **Sequential substitution** at the exact same physical spot lets you keep your EM-91CU+ inside the foam isolation ball—so the calibration removes **both** the capsule's quirks **and** the foam cavity's low-mid "boxiness" and pop-filter HF roll-off:

1. **Sweep File**: Play `data/calibration/farina_sweep_48k.wav` through any studio monitor or speaker at a comfortable conversational volume (do not move the speaker between Take 1 and Take 2).
2. **Take 1 — EM-91CU+ Inside the Isolation Ball**:
   - Leave your EM-91CU+ mounted inside the foam isolation ball, aimed straight at the speaker **45–60 cm** away (at least 80 cm from walls if possible).
   - Record `farina_sweep_48k.wav` in FL Studio / Audacity at **48 kHz / 24-bit** and save as `data/calibration/em91cu_sweep.wav`.
3. **Take 2 — Google Pixel 11 Pro at the Same Spot (Ball Off)**:
   - Without moving the speaker, slide the foam ball off the mic (or place the phone right in front of the mic position with the ball removed) so the **Pixel 11 Pro's bottom-edge primary microphone port** sits at the **exact same distance and axis** where the EM-91CU+ capsule was.
   - Play `farina_sweep_48k.wav` once more, record in Pixel **Recorder** (WAV / lossless), and save as `data/calibration/pixel11_mems.wav`.
   *(Tip: if you also want a bare-capsule-only calibration, just record Take 1 with the foam ball off.)*
4. **Generate Your C++ Calibration Header**:
   Save both files into `data/calibration/` as `em91cu_sweep.wav` and `pixel11_mems.wav`, then run:
   ```powershell
   cd "c:\Users\dell\Documents\Stoneridge DSP\tools\lab"
   uv run -m lab.mems_substitution --em91 ..\..\data\calibration\em91cu_sweep.wav --pixel11 ..\..\data\calibration\pixel11_mems.wav --out-csv ..\..\data\calibration\em91cu_vs_pixel11.csv --out-header ..\..\src\dsp\Em91cuCalibration.h --strength 0.85
   ```
5. Rebuild Release (`cmake --build build --config Release --parallel 2`) so the driver compiles your measured capsule curve into `InputProcessor` and the VST3 library.
