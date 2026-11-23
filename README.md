# Stoneridge DSP — Mackie EM-91CU+ Zero-Latency ASIO Driver & Studio Vocal Chain

Custom high-performance Windows WDM-KS / WASAPI Exclusive ASIO driver (`StoneridgeASIO_x64.dll`), Apple-style WebView2 Control Panel (`StoneridgePanel.exe`), platform-free Studio Vocal DSP library (`src/vocal/`), and offline metrology & fitting toolkit (`tools/lab/`) engineered specifically for the **Mackie EM-91CU+** (`USB\VID_0A73&PID_002E`).

## Highlights
- **Direct WDM-KS & Hardware Control**: Zero-copy WavePci/WaveCyclic streaming aligned to 1.000 ms USB Full-Speed SOF frames (`96 / 192 smp @ 48 kHz`), direct `IOCTL_KS_PROPERTY` control of ADC PGA (`Node[6]`) & Headphone DAC (`Node[2]`) in exact 0.5 dB steps, Consumer HID mute/volume sync (`MI_03`), and PI-locked 6-point Lagrange `AdaptiveDriftResampler`.
- **Mastered DAC Output (`DacConditioner`)**: ITU-R BS.1770-4 4x oversampled True-Peak ISP limiter (`-0.55 dBTP`), 24-bit TPDF dither, and 96 kHz / 16-bit 3rd-order psychoacoustic noise shaper.
- **Flagship Microphone Locker & Proximity (`MicLocker.h`)**: 5 min-phase curves fitted within `<= 0.32 dB` (`50 Hz - 16 kHz`) (`Sony C800G`, `Neumann U87 Ai`, `Telefunken ELA M 251E`, `Neumann U47 Tube`, `Shure SM7B`) + `2x`-oversampled head-amp curvature + exact bilinear `1/r` cardioid spherical-wave proximity (`5 - 35 cm`).
- **4 Legendary Preamps & Circuit Console EQs (`PreampAndConsole.h`)**: `4x`-oversampled Jiles-Atherton ferromagnetic transformer hysteresis (`Exact Langevin + RK2`) across `Neve 1073`, `API 312`, `Telefunken V76 Tube`, and `SSL 9000J`, plus `Pultec EQP-1A`, `Neve 1073 Inductor`, `SSL 4000G`, and `API 550A`.
- **Serial Studio Dynamics (`VocalDynamics.h`)**: `1176` fast feedback FET peak catcher (`80 us` attack) into a coupled dual-memory CdS `LA-2A T4B` optical leveler validated against held-out SignalTrain hardware recordings (`Zenodo 3824876`).
- **Monitor-Only Control Room (`ControlRoomMonitor.h`)**: 4x4 paraunitary Hadamard FDN comfort chamber, 300 us binaural crossfeed, anti-bleed dynamic cue carve, 3-way Mic 2 stem routing, and K-weighted BS.1770 level-matched A/B auditioning.
- **Stoneridge Lab (`tools/lab/`)**: Farina log-sine sweep metrology, lossless isolation-shield de-coloration, SignalTrain LA-2A ODE fitting, and automated C++ coefficient header generation.

## Building & Testing
```powershell
cmake --preset release-x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```
