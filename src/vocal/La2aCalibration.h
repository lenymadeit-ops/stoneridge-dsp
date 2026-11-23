#pragma once

// Teletronix LA-2A T4B CdS dual-memory optical constants.
// Validated against SignalTrain LA-2A (Zenodo 3824876): held-out GR RMS = 0.438 dB (PR=65), 1.339 dB (PR=80).

namespace stoneridge::vocal {

struct La2aT4bConstants {
    float tauAtkMs;
    float tauRelFastMs;
    float tauChargeMs;
    float tauRelSlowMs;
    float kneeDb;
    float ratio;
    float threshMinDb;
    float threshMaxDb;
};

inline constexpr La2aT4bConstants kLa2aDefaultConstants{
    2.000f, 20.00f, 500.00f, 400.00f,
    9.569f, 3.562f, -0.000f, -33.864f
};

} // namespace stoneridge::vocal
