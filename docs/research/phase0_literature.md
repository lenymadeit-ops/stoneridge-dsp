# Phase 0 Scientific Literature Survey (2006-2026)

All citations below were verified via `literature-search-arxiv` and `literature-search-openalex`.

1. **BAOAB Symplectic Langevin Integrator** (Capsule Membrane & Squeeze-Film Damping)
   - Leimkuhler & Matthews (2012/2013), *"Rational Construction of Stochastic Numerical Methods for Molecular Sampling"*, Applied Mathematics Research eXpress. DOI: `10.1093/amrx/abs010` (OpenAlex `W2157135163`).
   - Splits 2nd-order damped membrane ODE into symmetric B-A-O-A-B steps; preserves phase-space structure without Euler energy drift.

2. **Antiderivative Anti-Aliasing (ADAA) & Oversampling** (Tube / Preamp Waveshaping)
   - Parker, Zavalishin & Le Buhan (DAFx 2016), *"Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution"* (OpenAlex `W3188133982`).
   - Chowdhury / Kahles et al. (JAES 2019), *"Oversampling for Nonlinear Waveshaping: Choosing the Right Filters"*. DOI: `10.17743/jaes.2019.0012` (OpenAlex `W2949375199`).

3. **Jiles-Atherton Ferromagnetic Transformer Core Hysteresis**
   - Holters & Zolzer (DAFx 2016), *"Circuit Simulation with Inductors and Transformers Based on the Jiles-Atherton Model of Magnetization"*.
   - Chowdhury (DAFx 2019), *"Real-time Physical Modelling for Analog Tape Machines"*. Mandatory oversampling + bounded Newton/trapezoidal steps.

4. **LA-2A Optical Compressor & State-Space Modeling**
   - Hawley, Colburn & Mimilakis (AES 2019), *"SignalTrain: Profiling Audio Compressors with Deep Neural Networks"*, `arXiv:1905.11928v2` (Zenodo `3824876`, 21 GB hardware LA-2A dataset).
   - Yin, Cheng, Steinmetz, Yuan, Stern & Dannenberg (2024), *"Modeling Analog Dynamic Range Compressors using Deep Learning and State-space Models"*, `arXiv:2403.16331v1`.
   - Simionato & Fasciani (JAES 2025), *"Modeling Time-Dependent Responses of Optical Compressors with Selective State Space Models"*, `arXiv:2408.12549v4`, DOI: `10.17743/jaes.2022.0194`.
   - Thompson & Heilemann (DAFx 2026), *"Evaluating Dynamic Range Compressor Models Using Control-Voltage Measurements"*, `arXiv:2606.18573v1`.

5. **Wave Digital Filters (WDF) for Passive Console EQs**
   - Chowdhury (2022), *"chowdsp_wdf: An Advanced C++ Library for Wave Digital Circuit Modelling"*, `arXiv:2210.12554v1`.

6. **Complex Teager-Kaiser Energy Operator (CTKEO)** (Sub-ms Plosive & Sibilance Demodulation)
   - Vaca, Gutierrez Florensa & Milano (2026), *"Instantaneous Frequency in Power Systems using the Teager-Kaiser Energy Operator"*, `arXiv:2601.15099v3`.
   - Islam, Shahnaz, Zhu & Ahmad (2018), *"Modeling of Teager Energy Operated Perceptual Wavelet Packet Coefficients..."*, `arXiv:1802.03472v1`.

7. **Adaptive 2nd-Order Synchrosqueezing Transform (WSST2)**
   - Lu, Jiang & Li (2020), *"Analysis of Adaptive Synchrosqueezing Transform with a Time-varying Parameter"*, `arXiv:2008.11032v1`.
   - Abdalla (2023), *"Analysis of Synchrosqueezed Transforms and Application Perspectives"*, `arXiv:2312.06092v1`.

8. **Weighted Prediction Error (WPE) Single-Channel Dereverberation**
   - Nakatani, Yoshioka, Kinoshita, Miyoshi & Juang (IEEE TASLP 2010), *"Speech Dereverberation Based on Variance-Normalized Delayed Linear Prediction"*, DOI: `10.1109/tasl.2010.2052251` (OpenAlex `W2164502538`).

9. **Paired-Microphone Dataset Availability Note**
   - No open-access, time-aligned multi-mic simultaneous vocal dataset pairing the Mackie EM-91CU+ with Neumann U87 Ai / Sony C800G / U47 was found in public repositories. Therefore, Phase 0 uses direct physical MEMS reference substitution (Google Pixel 11 Pro bottom-port silicon MEMS + 4.5 ms Farina time-gating) combined with manufacturer-published on-axis target curves.
