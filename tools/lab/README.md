# Stoneridge Lab (`tools/lab`)

Offline acoustic metrology, reference-curve fitting, and C++ header generation for Stoneridge DSP.

## 1. Generate the 48 kHz Farina Log-Sine Sweep
```powershell
cd tools\lab
uv run -m lab.mems_substitution --generate-sweep ..\..\data\calibration\farina_sweep_48k.wav
```

## 2. Run Pixel 11 Pro MEMS Substitution Calibration
After recording `em91cu_sweep.wav` and `pixel11_mems.wav` side-by-side (`1 cm` apart, `50-60 cm` from a speaker):
```powershell
cd tools\lab
uv run -m lab.mems_substitution `
  --em91 ..\..\data\calibration\em91cu_sweep.wav `
  --pixel11 ..\..\data\calibration\pixel11_mems.wav `
  --out-csv ..\..\data\calibration\em91cu_vs_pixel11.csv `
  --out-header ..\..\src\dsp\Em91cuCalibration.h `
  --strength 0.85
```
Then rebuild `StoneridgeASIO` and `StoneridgePanel`.

## 3. Self-Test (Synthetic Speaker + 7.5 ms Room Wall Reflection)
```powershell
cd tools\lab
uv run -m lab.mems_substitution --selftest
```
