const postMsg = (payload) => {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(payload);
  }
};

const el = (id) => document.getElementById(id);

const INIT_PATCH = {
  inputGainDb: 0.0,
  softClipEnabled: true,
  audioHpfEnabled: false,
  micModel: 0,
  virtualDistanceCm: 15,
  preampModel: 0,
  preampDriveDb: 0.0,
  transformerIronAmount: 0.5,
  consoleEqModel: 0,
  consoleLowDb: 0.0,
  consoleMidDb: 0.0,
  consoleAirDb: 0.0,
  fetPeakCatchAmount: 0.0,
  fetRatioMode: 0,
  vocalLevelerAmount: 0.0,
  beautyAmount: 0.0,
  contourAmt: 1.0,
  harmonicSculptAmt: 1.0,
  deHarshAmt: 1.0,
  parallelGlueAmt: 0.15,
  deEsserAmount: 0.0,
  plosiveGuardEnabled: false,
  capsuleEqEnabled: false,
  molecularClip: true,
  wdfEqEnabled: true,
};

const PRESET_LABELS = {
  modernPop: "Modern Pop Gloss",
  rnb: "Warm R&B Velvet",
  rap: "Punchy Rap Authority",
  acoustic: "Intimate Acoustic",
  podcast: "Podcast Broadcast",
  raw: "Raw Reference",
};

const PRESETS = {
  modernPop: {
    ...INIT_PATCH, inputGainDb: 2.0, micModel: 1, preampModel: 1, preampDriveDb: 4.0,
    consoleEqModel: 1, consoleLowDb: 2.0, consoleAirDb: 3.0, fetPeakCatchAmount: 0.35,
    vocalLevelerAmount: 0.45, beautyAmount: 0.55, deEsserAmount: 0.4, audioHpfEnabled: true, capsuleEqEnabled: true,
  },
  rnb: {
    ...INIT_PATCH, inputGainDb: 1.5, micModel: 3, preampModel: 3, preampDriveDb: 5.0,
    transformerIronAmount: 0.65, consoleEqModel: 2, consoleLowDb: 2.5, consoleMidDb: 1.0,
    consoleAirDb: 1.0, vocalLevelerAmount: 0.6, fetPeakCatchAmount: 0.15, beautyAmount: 0.4,
    deEsserAmount: 0.25, audioHpfEnabled: true,
  },
  rap: {
    ...INIT_PATCH, inputGainDb: 3.0, micModel: 5, preampModel: 2, preampDriveDb: 5.0,
    consoleEqModel: 4, consoleLowDb: 1.0, consoleMidDb: 2.0, consoleAirDb: 0.5,
    fetPeakCatchAmount: 0.7, fetRatioMode: 2, vocalLevelerAmount: 0.2, beautyAmount: 0.25,
    deEsserAmount: 0.5, plosiveGuardEnabled: true, audioHpfEnabled: true,
  },
  acoustic: {
    ...INIT_PATCH, inputGainDb: 1.0, micModel: 4, preampModel: 3, preampDriveDb: 2.0,
    consoleEqModel: 1, consoleLowDb: 0.5, consoleAirDb: 1.5, fetPeakCatchAmount: 0.1,
    vocalLevelerAmount: 0.3, beautyAmount: 0.2, deEsserAmount: 0.15,
  },
  podcast: {
    ...INIT_PATCH, inputGainDb: 4.0, micModel: 2, preampModel: 4, preampDriveDb: 1.0,
    consoleEqModel: 3, consoleLowDb: -1.0, consoleMidDb: 1.0, consoleAirDb: 0.5,
    fetPeakCatchAmount: 0.2, vocalLevelerAmount: 0.5, beautyAmount: 0.15,
    deEsserAmount: 0.45, plosiveGuardEnabled: true, audioHpfEnabled: true, capsuleEqEnabled: true,
  },
  raw: { ...INIT_PATCH, softClipEnabled: true, molecularClip: false, wdfEqEnabled: false },
};

// Exact fitted MicLocker biquad definitions from MicLockerCurves.h for analytic curve plot
const MIC_CURVES = [
  [],
  [ [1, 345.07, 0.6607, -2.8042], [0, 136.91, 0.5, 2.2134], [0, 710.62, 4.3838, -0.0549], [0, 1993.7, 2.06, -0.5122], [0, 1506.48, 1.9653, -0.3581], [0, 5036.46, 1.1352, 1.374], [0, 11954.88, 0.8282, 2.2547], [2, 6031.1, 0.9241, 2.2214] ],
  [ [1, 220.0, 0.45, -1.6393], [0, 261.9, 0.635, -2.1702], [0, 222.09, 0.5, 2.9309], [0, 1997.72, 1.3739, -0.7921], [0, 3181.37, 1.6951, -0.3068], [0, 5682.58, 1.0506, 0.8446], [0, 15948.19, 1.4569, -0.9037], [2, 8855.53, 1.4, -0.2771] ],
  [ [1, 250.21, 0.5243, -1.8028], [0, 120.0, 0.5718, 1.9369], [0, 321.18, 4.9173, -0.0532], [0, 1530.58, 0.7565, -0.3277], [0, 4154.55, 0.5, 1.1317], [0, 10632.24, 0.5, 1.2303], [0, 12275.26, 1.1994, 0.793], [2, 7360.49, 0.814, 1.294] ],
  [ [1, 450.0, 0.45, -1.4677], [0, 128.85, 0.6086, 2.5579], [0, 267.27, 0.896, 0.4656], [0, 2024.46, 1.5067, 1.5218], [0, 2988.1, 2.3105, 0.871], [0, 6786.45, 0.8765, 2.8338], [0, 11988.08, 2.2304, -1.281], [2, 13556.45, 1.1688, -4.0772] ],
  [ [1, 220.0, 0.45, -6.7142], [0, 120.0, 0.718, 2.3461], [0, 264.88, 0.5937, 1.7727], [0, 1490.31, 0.7788, -0.513], [0, 3889.94, 0.6636, 3.2517], [0, 7130.21, 1.0472, 2.0206], [0, 14916.84, 0.5, -5.4862], [2, 14745.34, 1.0267, -2.8311] ],
];

let latestParams = { ...INIT_PATCH, themeMode: 0, ecoMode: false };
let latestTelemetry = {
  deviceConnected: false, captureActive: false, streamRunning: false,
  activeEngineMode: 0, activeIrpCount: 3, activeBufferSize: 96, activeSampleRate: 48000, activeBitDepth: 24,
  inputLatencyMs: 2.0, outputLatencyMs: 6.0, roundTripLatencyMs: 8.0,
  inputPeakDb: -96.0, inputRmsDb: -96.0, outputTruePeakDb: -96.0,
  gateGainReductionDb: 0.0, gateIsOpen: true, deEsserReductionDb: 0.0, plosiveReductionDb: 0.0,
  fetReductionDb: 0.0, levelerReductionDb: 0.0, abMatchGainDb: 0.0,
  supportedSampleRatesMask: 0x1F, windowsDark: true, jitterHistoryUs: [],
};
let activePreset = "";
let inspectedStage = "shield";
let undoStack = [];
let redoStack = [];
let slotA = null;
let slotB = null;
let lastCurveSig = "";
let ecoToastTimer = null;

function postParam(key, value) {
  latestParams[key] = value;
  postMsg({ action: "setParam", key, value });
  refreshUiFromLocal();
}

function lockedKeys() {
  const keys = [];
  if (el("lockPga") && el("lockPga").checked) keys.push("hardwarePgaGainDb");
  if (el("lockGate") && el("lockGate").checked) keys.push("gateEnabled", "gateThresholdDb", "gateHysteresisDb", "humNotchFreqHz");
  return keys;
}

function pushUndo() {
  undoStack.push({ ...latestParams });
  if (undoStack.length > 40) undoStack.shift();
  redoStack = [];
  if (el("undoBtn")) el("undoBtn").disabled = undoStack.length === 0;
}

function applyPatchBatch(patch, skipLocked) {
  const skip = skipLocked ? lockedKeys() : [];
  const cleanPatch = {};
  Object.entries(patch).forEach(([k, v]) => {
    if (skip.indexOf(k) >= 0) return;
    cleanPatch[k] = v;
    latestParams[k] = v;
  });
  postMsg({ action: "applyPatch", patch: cleanPatch });
  refreshUiFromLocal();
}

function dbToPercent(db, min = -60, max = 0) {
  return Math.max(0, Math.min(100, ((db - min) / (max - min)) * 100));
}

function applyTheme(mode, windowsDark) {
  let theme = "dark";
  if (mode === 2) theme = "light";
  else if (mode === 0) theme = windowsDark ? "dark" : "light";
  document.documentElement.setAttribute("data-theme", theme);
  document.querySelectorAll("#themeSwitcher button").forEach((btn) => {
    btn.classList.toggle("active", Number(btn.dataset.themeMode) === (mode || 0));
  });
}

// Analytic biquad / ZDF response magnitude in dB at frequency f (sample rate fs = 48000)
function secMagSq(kind, fc, q, gDb, w, fs = 48000.0) {
  if (Math.abs(gDb) < 0.02) return 1.0;
  const A = Math.pow(10.0, gDb / 40.0);
  const w0 = (2.0 * Math.PI * Math.min(fc, fs * 0.45)) / fs;
  const cs = Math.cos(w0);
  const sn = Math.sin(w0);
  const alpha = sn / (2.0 * Math.max(q, 0.35));
  let b0, b1, b2, a0, a1, a2;
  if (kind === 1) {
    const tsa = 2.0 * Math.sqrt(A) * alpha;
    a0 = (A + 1) + (A - 1) * cs + tsa;
    b0 = A * ((A + 1) - (A - 1) * cs + tsa);
    b1 = 2 * A * ((A - 1) - (A + 1) * cs);
    b2 = A * ((A + 1) - (A - 1) * cs - tsa);
    a1 = -2 * ((A - 1) + (A + 1) * cs);
    a2 = (A + 1) + (A - 1) * cs - tsa;
  } else if (kind === 2) {
    const tsa = 2.0 * Math.sqrt(A) * alpha;
    a0 = (A + 1) - (A - 1) * cs + tsa;
    b0 = A * ((A + 1) + (A - 1) * cs + tsa);
    b1 = -2 * A * ((A - 1) + (A + 1) * cs);
    b2 = A * ((A + 1) + (A - 1) * cs - tsa);
    a1 = 2 * ((A - 1) - (A + 1) * cs);
    a2 = (A + 1) - (A - 1) * cs - tsa;
  } else {
    a0 = 1 + alpha / A;
    b0 = 1 + alpha * A;
    b1 = -2 * cs;
    b2 = 1 - alpha * A;
    a1 = -2 * cs;
    a2 = 1 - alpha / A;
  }
  const cosW = Math.cos(w);
  const cos2W = Math.cos(2 * w);
  const sinW = Math.sin(w);
  const sin2W = Math.sin(2 * w);
  const numRe = (b0 + b1 * cosW + b2 * cos2W) / a0;
  const numIm = (-b1 * sinW - b2 * sin2W) / a0;
  const denRe = 1.0 + (a1 * cosW + a2 * cos2W) / a0;
  const denIm = (-a1 * sinW - a2 * sin2W) / a0;
  return (numRe * numRe + numIm * numIm) / Math.max(1e-12, denRe * denRe + denIm * denIm);
}

function evalAnalyticChainDb(freq, p, grTotal) {
  const fs = 48000.0;
  const w = (2.0 * Math.PI * freq) / fs;
  let magSq = 1.0;

  // 1. 80 Hz 2nd-order Butterworth Audio HPF
  if (p.audioHpfEnabled) {
    const r = freq / 80.0;
    const r4 = r * r * r * r;
    magSq *= r4 / (1.0 + r4);
  }

  // 2. Spherical-Wave Proximity H(jw, r_tgt) / H(jw, 15 cm)
  const dist = Math.max(5.0, Math.min(35.0, p.virtualDistanceCm ?? 15));
  if (Math.abs(dist - 15.0) >= 0.25) {
    const om = 2.0 * Math.PI * freq;
    const wz = (0.14 * 343.0) / (dist * 0.01);
    const wp = (0.14 * 343.0) / 0.15;
    magSq *= (om * om + wz * wz) / (om * om + wp * wp);
  }

  // 3. Flagship Mic Locker Curve
  const micIdx = p.micModel || 0;
  if (micIdx >= 1 && micIdx <= 5) {
    for (const [kind, fc, q, g] of MIC_CURVES[micIdx]) {
      magSq *= secMagSq(kind, fc, q, g, w, fs);
    }
  }

  // 4. Console EQ (Pultec WDF/dip, 1073, SSL, API)
  const eqIdx = p.consoleEqModel || 0;
  if (eqIdx > 0) {
    const low = Math.max(-8, Math.min(8, p.consoleLowDb || 0));
    const mid = Math.max(-8, Math.min(8, p.consoleMidDb || 0));
    const air = Math.max(-6, Math.min(10, p.consoleAirDb || 0));
    if (eqIdx === 1) {
      magSq *= secMagSq(1, 88, 0.65, low * 1.12, w, fs);
      if (low > 0.02) magSq *= secMagSq(0, 225, 0.9, -low * 0.45, w, fs);
      magSq *= secMagSq(0, 3200, 0.85, mid, w, fs);
      magSq *= secMagSq(0, 12000, 0.72, air, w, fs);
    } else if (eqIdx === 2) {
      magSq *= secMagSq(1, 110, 0.707, low, w, fs);
      magSq *= secMagSq(0, 1600, 0.9 + 0.14 * Math.abs(mid), mid, w, fs);
      magSq *= secMagSq(2, 12000, 0.707, air, w, fs);
    } else if (eqIdx === 3) {
      magSq *= secMagSq(1, 100, 0.75, low, w, fs);
      magSq *= secMagSq(0, 3500, 1.0 + 0.18 * Math.abs(mid), mid, w, fs);
      magSq *= secMagSq(2, 10000, 0.707, air, w, fs);
    } else {
      magSq *= secMagSq(1, 100, 0.85, low, w, fs);
      magSq *= secMagSq(0, 2400, 0.75 + 0.25 * Math.abs(mid), mid, w, fs);
      magSq *= secMagSq(2, 12500, 0.75, air, w, fs);
    }
  }

  // 5. Capsule Shield Lift & Beauty EQ contour
  if (p.capsuleCorrectionEnabled) {
    const s = p.capsuleCorrectionStrength ?? 0.85;
    magSq *= secMagSq(0, 6200, 0.9, 1.8 * s, w, fs);
  }
  if ((p.beautyAmount || 0) > 0.01) {
    const b = p.beautyAmount * (p.contourAmt ?? 1.0);
    magSq *= secMagSq(1, 95, 0.707, 1.4 * b, w, fs);
    magSq *= secMagSq(2, 11500, 0.707, 1.0 * b, w, fs);
  }

  let db = 10.0 * Math.log10(Math.max(1e-6, magSq));
  if (grTotal > 0.05) {
    const dipFreqWeight = Math.exp(-Math.pow(Math.log10(freq / 1200), 2) / 1.8);
    db -= grTotal * 0.22 * dipFreqWeight;
  }
  return Math.max(-16, Math.min(16, db));
}

function drawCurve(params, telemetry) {
  const canvas = el("curveCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  const style = getComputedStyle(document.documentElement);
  const accent = style.getPropertyValue("--accent").trim() || "#0a84ff";
  const border = style.getPropertyValue("--border").trim() || "rgba(150,150,160,0.2)";
  const muted = style.getPropertyValue("--text-secondary").trim() || "#888";

  ctx.clearRect(0, 0, w, h);

  const mapX = (f) => ((Math.log10(f) - Math.log10(20)) / (Math.log10(20000) - Math.log10(20))) * (w - 48) + 34;
  const mapY = (db) => h * 0.5 - (db / 14.0) * (h * 0.42);

  // Grid: +/-12 dB ticks & log frequency lines
  ctx.lineWidth = 1;
  ctx.font = "10px Consolas, monospace";
  [-12, -6, 0, 6, 12].forEach((db) => {
    const y = Math.round(mapY(db)) + 0.5;
    ctx.strokeStyle = db === 0 ? "rgba(150,150,160,0.38)" : border;
    ctx.beginPath();
    ctx.moveTo(34, y);
    ctx.lineTo(w - 14, y);
    ctx.stroke();
    ctx.fillStyle = muted;
    ctx.fillText(`${db > 0 ? "+" : ""}${db}`, 4, y + 3);
  });

  [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000].forEach((f) => {
    const x = Math.round(mapX(f)) + 0.5;
    ctx.strokeStyle = border;
    ctx.beginPath();
    ctx.moveTo(x, 8);
    ctx.lineTo(x, h - 18);
    ctx.stroke();
    ctx.fillStyle = muted;
    const lbl = f >= 1000 ? `${f / 1000}k` : `${f}`;
    ctx.fillText(lbl, x - 8, h - 5);
  });

  const grTotal = Math.abs((telemetry && telemetry.fetReductionDb) || 0) + Math.abs((telemetry && telemetry.levelerReductionDb) || 0);
  ctx.strokeStyle = accent;
  ctx.lineWidth = 2.0;
  ctx.beginPath();
  const steps = 160;
  for (let i = 0; i <= steps; i++) {
    const frac = i / steps;
    const freq = 20.0 * Math.pow(1000.0, frac);
    const db = evalAnalyticChainDb(freq, params, grTotal);
    const x = mapX(freq);
    const y = mapY(db);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

function drawJitterSparkline(history) {
  const canvas = el("jitterCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  if (!history || history.length < 2) return;
  const maxVal = Math.max(100, ...history);
  ctx.beginPath();
  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#0a84ff";
  ctx.lineWidth = 1.75;
  ctx.lineJoin = "round";
  history.forEach((val, idx) => {
    const x = (idx / (history.length - 1)) * (w - 8) + 4;
    const y = h - 6 - (Math.min(val, maxVal) / maxVal) * (h - 12);
    if (idx === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
}

function drawGateVisualizer(peakDb, threshDb, hystDb, gateEnabled, gateIsOpen) {
  const canvas = el("gateCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  const style = getComputedStyle(document.documentElement);
  const accent = style.getPropertyValue("--accent").trim() || "#0a84ff";
  const green = style.getPropertyValue("--green").trim() || "#30d158";
  const orange = style.getPropertyValue("--orange").trim() || "#ff9f0a";
  const muted = style.getPropertyValue("--text-secondary").trim() || "#888";

  const mapX = (db) => Math.max(0, Math.min(w, ((db + 80) / 80) * w));
  const openX = mapX(threshDb);
  const closeX = mapX(Math.max(-80, threshDb - hystDb));
  if (gateEnabled) {
    ctx.fillStyle = "rgba(255, 159, 10, 0.14)";
    ctx.fillRect(closeX, 0, Math.max(2, openX - closeX), h);
  }
  const sigX = mapX(peakDb);
  ctx.fillStyle = !gateEnabled ? muted : (gateIsOpen ? green : orange);
  ctx.globalAlpha = 0.28;
  ctx.fillRect(0, 14, sigX, h - 28);
  ctx.globalAlpha = 1.0;
  ctx.fillRect(sigX - 1.5, 10, 3, h - 20);

  ctx.strokeStyle = accent;
  ctx.lineWidth = 1.5;
  ctx.setLineDash([4, 3]);
  ctx.beginPath();
  ctx.moveTo(openX, 4);
  ctx.lineTo(openX, h - 4);
  ctx.stroke();
  ctx.setLineDash([]);

  ctx.fillStyle = muted;
  ctx.font = "10px Consolas, monospace";
  ctx.fillText("-80 dB", 6, h - 6);
  ctx.fillText("0 dB", w - 30, h - 6);
  ctx.fillStyle = accent;
  ctx.fillText(`Open ${Math.round(threshDb)} dB`, Math.min(w - 78, openX + 5), 13);
}

function setActivePresetUI(key) {
  activePreset = key || "";
  document.querySelectorAll("#presetPills button").forEach((b) => {
    b.classList.toggle("active", b.dataset.preset === activePreset);
  });
  const ro = el("presetReadout");
  if (ro) ro.textContent = activePreset ? (PRESET_LABELS[activePreset] || activePreset) : "INIT | Default Patch";
}

function updateFlowBadges(p, tel) {
  const micNames = ["Raw EM-91", "C800G", "U87 Ai", "ELA 251E", "U47", "SM7B"];
  const preNames = ["Bypass", "Neve 1073", "API 312", "V76 Tube", "SSL 9000J"];
  const eqNames = ["Bypass", "Pultec EQP", "1073 EQ", "SSL 4000G", "API 550A"];

  const setLed = (id, subId, on, txt) => {
    const led = el(id);
    const sub = el(subId);
    if (led) led.classList.toggle("on", !!on);
    if (sub) sub.textContent = txt;
  };
  setLed("ledShield", "subShield", !!p.capsuleCorrectionEnabled, p.capsuleCorrectionEnabled ? `${Math.round((p.capsuleCorrectionStrength ?? 0.85) * 100)}%` : "Off");
  setLed("ledMic", "subMic", (p.micModel || 0) > 0, micNames[p.micModel || 0] || "Raw");
  setLed("ledPre", "subPre", (p.preampModel || 0) > 0, preNames[p.preampModel || 0] || "Bypass");
  setLed("ledGate", "subGate", !!p.gateEnabled || (p.deEsserAmount || 0) > 0.01, p.gateEnabled ? (tel.gateIsOpen ? "Open" : "Closed") : ((p.deEsserAmount || 0) > 0.01 ? "De-Ess" : "Bypass"));
  setLed("ledFet", "subFet", (p.fetPeakCatchAmount || 0) > 0.01, `${Math.round((p.fetPeakCatchAmount || 0) * 100)}%`);
  setLed("ledLa2a", "subLa2a", (p.vocalLevelerAmount || 0) > 0.01, `${Math.round((p.vocalLevelerAmount || 0) * 100)}%`);
  setLed("ledEq", "subEq", (p.consoleEqModel || 0) > 0, eqNames[p.consoleEqModel || 0] || "Bypass");
  setLed("ledBeauty", "subBeauty", (p.beautyAmount || 0) > 0.01, `${Math.round((p.beautyAmount || 0) * 100)}%`);
}

function refreshUiFromLocal() {
  applyStateMessage({ type: "state", params: latestParams, telemetry: latestTelemetry });
}

const fmtBal = (v) => (Math.abs(v) < 0.25 ? "Center" : (v < 0 ? `L ${Math.abs(v).toFixed(1)} dB` : `R ${v.toFixed(1)} dB`));

const bindSlider = (id, labelId, key, fmt, scale = 1.0) => {
  const node = el(id);
  if (!node) return;
  node.addEventListener("input", (e) => {
    const raw = parseFloat(e.target.value);
    if (el(labelId)) el(labelId).textContent = fmt(raw);
    postParam(key, raw * scale);
  });
};

bindSlider("hwPgaGain", "hwPgaVal", "hardwarePgaGainDb", (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
bindSlider("hwDacGain", "hwDacVal", "hardwareDacGainDb", (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
bindSlider("hwDacBal", "hwDacBalVal", "hardwareDacBalanceDb", fmtBal);
bindSlider("inputGain", "inputGainVal", "inputGainDb", (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
bindSlider("directMonitor", "directMonitorVal", "directMonitorMix", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("backingHeadroom", "backingHeadroomVal", "backingHeadroomDb", (v) => `${v.toFixed(1)} dB`);
bindSlider("monitorReverb", "monitorReverbVal", "monitorReverbAmount", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("deEsserSlider", "deEsserVal", "deEsserAmount", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("fetPeakCatchSlider", "fetPeakCatchVal", "fetPeakCatchAmount", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("vocalLevelerSlider", "vocalLevelerVal", "vocalLevelerAmount", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("gateThreshold", "gateThresholdVal", "gateThresholdDb", (v) => `${Math.round(v)} dB`);
bindSlider("gateHysteresis", "gateHysteresisVal", "gateHysteresisDb", (v) => `${v.toFixed(1)} dB`);
bindSlider("gateRange", "gateRangeVal", "gateRangeDb", (v) => `${Math.round(v)} dB`);
bindSlider("gateAttack", "gateAttackVal", "gateAttackMs", (v) => `${v.toFixed(1)} ms`);
bindSlider("gateHold", "gateHoldVal", "gateHoldMs", (v) => `${Math.round(v)} ms`);
bindSlider("gateRelease", "gateReleaseVal", "gateReleaseMs", (v) => `${Math.round(v)} ms`);
bindSlider("gateHpf", "gateHpfVal", "gateSidechainHpfHz", (v) => (v === 0 ? "Off" : `${Math.round(v)} Hz`));
bindSlider("shieldStrength", "shieldStrengthVal", "capsuleCorrectionStrength", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("virtualDistanceSlider", "virtualDistanceVal", "virtualDistanceCm", (v) => `${Math.round(v)} cm`);
bindSlider("preampDriveSlider", "preampDriveVal", "preampDriveDb", (v) => `${v.toFixed(1)} dB`);
bindSlider("transformerIronSlider", "transformerIronVal", "transformerIronAmount", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("consoleLowSlider", "consoleLowVal", "consoleLowDb", (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
bindSlider("consoleMidSlider", "consoleMidVal", "consoleMidDb", (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
bindSlider("consoleAirSlider", "consoleAirVal", "consoleAirDb", (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
bindSlider("beautyAmount", "beautyAmountVal", "beautyAmount", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("trackBeautySlider", "trackBeautyVal", "beautyAmount", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("contourAmtSlider", "contourAmtVal", "contourAmt", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("harmonicSculptSlider", "harmonicSculptVal", "harmonicSculptAmt", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("deHarshSlider", "deHarshVal", "deHarshAmt", (v) => `${Math.round(v)}%`, 0.01);
bindSlider("parallelGlueSlider", "parallelGlueVal", "parallelGlueAmt", (v) => `${Math.round(v)}%`, 0.01);

el("muteBtn").addEventListener("click", () => postParam("inputMute", !latestParams.inputMute));
el("adcClipPill").addEventListener("click", () => postMsg({ action: "clearAdcClip" }));
el("learnNoiseBtn").addEventListener("click", () => postMsg({ action: "startLearnNoise" }));
el("autoPreampBtn").addEventListener("click", () => postMsg({ action: "startPreampAutoLevel" }));
el("optimalPresetBtn").addEventListener("click", () => postMsg({ action: "applyOptimalPreset" }));
el("xrunChip").addEventListener("click", () => postMsg({ action: "resetXRuns" }));

const bindToggle = (id, key) => {
  const n = el(id);
  if (n) n.addEventListener("change", (e) => postParam(key, e.target.checked));
};
bindToggle("audioHpfToggle", "audioHpfEnabled");
bindToggle("softClipToggle", "softClipEnabled");
bindToggle("autoPadToggle", "autoPadOnClipEnabled");
bindToggle("gateEnableToggle", "gateEnabled");
bindToggle("gateLookAheadToggle", "gateLookAheadEnabled");
bindToggle("capsuleEqToggle", "capsuleEqEnabled");
bindToggle("capsuleCorrectionToggle", "capsuleCorrectionEnabled");
bindToggle("plosiveGuardToggle", "plosiveGuardEnabled");
bindToggle("crossfeedToggle", "crossfeedEnabled");
bindToggle("cueCarveToggle", "cueCarveEnabled");
bindToggle("wdfEqToggle", "wdfEqEnabled");
bindToggle("molecularClipToggle", "molecularClip");
bindToggle("dacIspGuardToggle", "dacIspGuardEnabled");
bindToggle("dacDitherShaperToggle", "dacDitherShaperEnabled");
bindToggle("ultrasonicLpfToggle", "ultrasonicLpfEnabled");
bindToggle("driftAsrcToggle", "driftAsrcEnabled");
bindToggle("autopilotToggle", "autopilotEnabled");
bindToggle("pCoreToggle", "pCorePriority");
bindToggle("xrunGuardToggle", "adaptiveXRunGuard");

document.querySelectorAll('input[type="range"]').forEach((slider) => {
  slider.addEventListener("dblclick", () => {
    const def = slider.dataset.default !== undefined ? slider.dataset.default : slider.defaultValue;
    slider.value = def;
    slider.dispatchEvent(new Event("input"));
  });
});

document.querySelectorAll("#tabBar button").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.body.dataset.page = btn.dataset.tab;
    document.querySelectorAll("#tabBar button").forEach((b) => b.classList.toggle("active", b === btn));
    if (btn.dataset.tab === "chain") {
      lastCurveSig = "";
      drawCurve(latestParams, latestTelemetry);
    }
  });
});

document.querySelectorAll("#themeSwitcher button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("themeMode", Number(btn.dataset.themeMode)));
});
document.querySelectorAll("#fetRatioSelector button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("fetRatioMode", Number(btn.dataset.ratio)));
});
document.querySelectorAll("#humNotchSelector button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("humNotchFreqHz", Number(btn.dataset.hum)));
});
document.querySelectorAll("#mic2RouteSelector button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const r = Number(btn.dataset.route);
    applyPatchBatch({ mic2RoutingMode: r, dualTrackSafetyMode: r === 2 }, false);
  });
});
document.querySelectorAll("#micModelSelector button").forEach((btn) => {
  btn.addEventListener("click", () => { pushUndo(); postParam("micModel", Number(btn.dataset.mic)); });
});
document.querySelectorAll("#preampModelSelector button").forEach((btn) => {
  btn.addEventListener("click", () => { pushUndo(); postParam("preampModel", Number(btn.dataset.preamp)); });
});
document.querySelectorAll("#consoleEqSelector button").forEach((btn) => {
  btn.addEventListener("click", () => { pushUndo(); postParam("consoleEqModel", Number(btn.dataset.eq)); });
});
document.querySelectorAll("#engineSelector button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("engineMode", Number(btn.dataset.engine)));
});
document.querySelectorAll("#irpSelector button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("ksIrpCount", Number(btn.dataset.irp)));
});
document.querySelectorAll("#bufferSelector button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("requestedBufferSize", Number(btn.dataset.buf)));
});
document.querySelectorAll("#rateSelector button").forEach((btn) => {
  btn.addEventListener("click", () => { if (!btn.disabled) postParam("requestedSampleRate", Number(btn.dataset.rate)); });
});

// Presets with Click-to-Deselect -> INIT + 'x' Clear honoring Lock Pins
document.querySelectorAll("#presetPills button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const name = btn.dataset.preset;
    const turningOff = activePreset === name;
    pushUndo();
    setActivePresetUI(turningOff ? "" : name);
    applyPatchBatch(turningOff ? INIT_PATCH : PRESETS[name], true);
  });
});
el("presetClearBtn").addEventListener("click", () => {
  pushUndo();
  setActivePresetUI("");
  applyPatchBatch(INIT_PATCH, true);
});

// Single Monitor-Safe A/B toggle
el("abBypassBtn").addEventListener("click", () => {
  pushUndo();
  applyPatchBatch({
    abLevelMatchedBypass: !latestParams.abLevelMatchedBypass,
    abAuditionMonitorOnly: true,
  }, false);
});

// ECO Toggle with visible LED + badge + toast
function toggleEco(on) {
  postParam("ecoMode", !!on);
  if (on) {
    const toast = el("ecoToast");
    if (toast) {
      toast.classList.add("show");
      if (ecoToastTimer) clearTimeout(ecoToastTimer);
      ecoToastTimer = setTimeout(() => toast.classList.remove("show"), 2000);
    }
  }
}
el("ecoBtn").addEventListener("click", () => toggleEco(!latestParams.ecoMode));

// Slot A / B: Click recalls (or saves on first click), Save btn or Shift+Click overwrites anytime
function saveSlot(which) {
  if (which === "A") {
    slotA = { ...latestParams };
    el("slotABtn").textContent = "Slot A *";
  } else {
    slotB = { ...latestParams };
    el("slotBBtn").textContent = "Slot B *";
  }
}
el("slotABtn").addEventListener("click", (e) => {
  if (!slotA || e.shiftKey) saveSlot("A");
  else { pushUndo(); applyPatchBatch(slotA, false); }
});
el("slotASaveBtn").addEventListener("click", () => saveSlot("A"));
el("slotBBtn").addEventListener("click", (e) => {
  if (!slotB || e.shiftKey) saveSlot("B");
  else { pushUndo(); applyPatchBatch(slotB, false); }
});
el("slotBSaveBtn").addEventListener("click", () => saveSlot("B"));

function performUndo() {
  if (!undoStack.length) return;
  redoStack.push({ ...latestParams });
  applyPatchBatch(undoStack.pop(), false);
  el("undoBtn").disabled = undoStack.length === 0;
}
el("undoBtn").addEventListener("click", performUndo);
document.addEventListener("keydown", (e) => {
  if (!(e.ctrlKey || e.metaKey)) return;
  if (e.key.toLowerCase() === "z" && undoStack.length) { performUndo(); e.preventDefault(); }
  else if (e.key.toLowerCase() === "y" && redoStack.length) {
    undoStack.push({ ...latestParams });
    applyPatchBatch(redoStack.pop(), false);
    el("undoBtn").disabled = undoStack.length === 0;
    e.preventDefault();
  }
});

// Collapsible Setup Guide, Voice Loop Audition, Tracking/Mixing Profiles
el("setupCollapseBtn").addEventListener("click", () => {
  const strip = el("setupStrip");
  const collapsed = strip.classList.toggle("collapsed");
  el("setupCollapseBtn").textContent = collapsed ? "Show Guide" : "Hide Guide";
});
el("voiceRecBtn").addEventListener("click", () => postParam("voiceLoopState", 1));
el("voicePlayBtn").addEventListener("click", () => postParam("voiceLoopState", 2));
el("voiceStopBtn").addEventListener("click", () => postParam("voiceLoopState", 0));
el("profileTrack").addEventListener("click", () => applyPatchBatch({ requestedBufferSize: 96, requestedSampleRate: 48000 }, false));
el("profileMix").addEventListener("click", () => postParam("requestedBufferSize", 512));
el("syncPluginBtn").addEventListener("click", () => {
  postMsg({ action: "syncToPlugin" });
  const btn = el("syncPluginBtn");
  btn.textContent = "Synced!";
  setTimeout(() => { btn.textContent = "Sync to Plugin"; }, 1400);
});

// Interactive Click-to-Inspect 8-stage Flow
const flowCopy = {
  shield: "Shield: Pixel 11 Pro min-phase MEMS capsule correction before mic modeling.",
  mic: "Mic Locker: Fitted V1 curves, 1/r proximity, kHz BAOAB membrane & Koren/ADAA2 head-amps.",
  pre: "Preamp: Series Jiles-Atherton ferromagnetic core + EF804S/12AX7 Koren tube.",
  gate: "Gate / De-Ess: Hysteresis noise gate + 6.5 kHz dynamic sibilance guard.",
  fet: "1176 JFET: 80 us Shichman-Hodges JFET peak catcher + All-Buttons British mode.",
  la2a: "LA-2A T4B: SignalTrain R37 + CdS dual-memory + 8-state continuous-time gray box.",
  eq: "Console EQ: Passive WDF EQP-1A bridge & cached Cytomic SVF (1073 / SSL / API) @ ~192 kHz.",
  beauty: "Beauty: LUFS-driven ISO 226 contour, Hilbert 2f chest, CTKEO de-harsh & NY glue.",
};
document.querySelectorAll("#flowStrip button").forEach((btn) => {
  btn.addEventListener("click", () => {
    inspectedStage = btn.dataset.inspect || "shield";
    document.querySelectorAll("#flowStrip button").forEach((b) => b.classList.toggle("active", b === btn));
    el("flowDetail").textContent = flowCopy[inspectedStage] || "";
  });
});
el("stageQuickBypass").addEventListener("click", () => {
  pushUndo();
  if (inspectedStage === "shield") postParam("capsuleCorrectionEnabled", !latestParams.capsuleCorrectionEnabled);
  else if (inspectedStage === "mic") postParam("micModel", (latestParams.micModel || 0) > 0 ? 0 : 1);
  else if (inspectedStage === "pre") postParam("preampModel", (latestParams.preampModel || 0) > 0 ? 0 : 1);
  else if (inspectedStage === "gate") postParam("gateEnabled", !latestParams.gateEnabled);
  else if (inspectedStage === "fet") postParam("fetPeakCatchAmount", (latestParams.fetPeakCatchAmount || 0) > 0.01 ? 0 : 0.35);
  else if (inspectedStage === "la2a") postParam("vocalLevelerAmount", (latestParams.vocalLevelerAmount || 0) > 0.01 ? 0 : 0.45);
  else if (inspectedStage === "eq") postParam("consoleEqModel", (latestParams.consoleEqModel || 0) > 0 ? 0 : 1);
  else if (inspectedStage === "beauty") postParam("beautyAmount", (latestParams.beautyAmount || 0) > 0.01 ? 0 : 0.45);
});

// 10-Trial Blind ABX
let abx = { trial: 0, correct: 0, hidden: 0, running: false };
function binomP(k) {
  let p = 0;
  const choose = (n, r) => {
    let c = 1;
    for (let i = 0; i < r; i++) c = (c * (n - i)) / (i + 1);
    return c;
  };
  for (let i = k; i <= 10; i++) p += choose(10, i);
  return p / 1024;
}
function abxHear(which) {
  const raw = which === "B" || (which === "X" && abx.hidden === 1);
  applyPatchBatch({ abLevelMatchedBypass: raw, abAuditionMonitorOnly: true, freezeAbxGain: true }, false);
}
el("abxStart").addEventListener("click", () => {
  abx = { trial: 1, correct: 0, hidden: Math.random() < 0.5 ? 0 : 1, running: true };
  applyPatchBatch({ voiceLoopState: 2, freezeAbxGain: true }, false);
  el("abxStatus").textContent = "Trial 1 of 10: Listen to A, B, and X cleanly.";
  el("abxScore").textContent = "Score 0/10";
});
el("abxHearA").addEventListener("click", () => abxHear("A"));
el("abxHearB").addEventListener("click", () => abxHear("B"));
el("abxHearX").addEventListener("click", () => abxHear("X"));
function abxAnswer(isA) {
  if (!abx.running) return;
  if ((isA ? 0 : 1) === abx.hidden) abx.correct += 1;
  if (abx.trial >= 10) {
    abx.running = false;
    el("abxScore").textContent = `Score ${abx.correct}/10  p=${binomP(abx.correct).toFixed(4)}`;
    el("abxStatus").textContent = "Finished! ABX loudness match unlocked.";
    applyPatchBatch({ freezeAbxGain: false, abLevelMatchedBypass: false }, false);
    return;
  }
  abx.trial += 1;
  abx.hidden = Math.random() < 0.5 ? 0 : 1;
  el("abxStatus").textContent = `Trial ${abx.trial} of 10: Listen to A, B, and X cleanly.`;
  el("abxScore").textContent = `Score ${abx.correct}/10`;
}
el("abxPickA").addEventListener("click", () => abxAnswer(true));
el("abxPickB").addEventListener("click", () => abxAnswer(false));

function applyStateMessage(msg) {
  if (!msg || msg.type !== "state") return;
  const { params, telemetry } = msg;
  latestParams = { ...params };
  latestTelemetry = { ...telemetry };

  applyTheme(params.themeMode, telemetry.windowsDark);
  const stream = el("streamPill");
  if (stream) {
    stream.textContent = telemetry.streamRunning ? "DAW Stream Active" : "Standby";
    stream.classList.toggle("pill-ok", !!telemetry.streamRunning);
  }
  const safe = el("recordingSafe");
  if (safe) safe.style.display = params.abAuditionMonitorOnly !== false ? "inline-flex" : "none";

  const abBtn = el("abBypassBtn");
  const abState = !!params.abLevelMatchedBypass;
  const match = telemetry.abMatchGainDb || 0;
  abBtn.classList.toggle("learning", abState);
  abBtn.textContent = abState ? `B: Raw (${match >= 0 ? "+" : ""}${match.toFixed(1)} dB)` : "A: Studio Chain";

  document.body.classList.toggle("eco-on", !!params.ecoMode);
  el("ecoBtn").textContent = params.ecoMode ? "ECO: ON" : "ECO: OFF";
  el("ecoDot").classList.toggle("lit", !!params.ecoMode);

  const live = !!telemetry.streamRunning;
  el("autoPreampBtn").disabled = !live;
  el("learnNoiseBtn").disabled = !live;
  el("setupHint").style.display = live ? "none" : "block";
  ["stepConnect", "stepLevel", "stepNoise"].forEach((id, i) => {
    const node = el(id);
    if (!node) return;
    const done = i === 0 ? live : (i === 1 ? telemetry.learnState === 3 && Math.abs(telemetry.recommendedTrimDb || 0) + Math.abs(telemetry.recommendedPgaDb || 0) > 0.05 : telemetry.measuredNoiseFloorDb > -90);
    node.classList.toggle("done", !!done);
  });

  const vl = params.voiceLoopState || 0;
  el("voiceLoopPill").textContent = vl === 1 ? "Recording 5s..." : (vl === 2 ? "Looping Phrase" : "Standby");
  el("voiceLoopPill").classList.toggle("pill-ok", vl === 2);

  const syncBadge = el("rateSyncBadge");
  if (syncBadge) {
    const req = Math.round(params.requestedSampleRate || 48000);
    const act = Math.round(telemetry.activeSampleRate || 48000);
    const reqB = params.requestedBufferSize || 96;
    const actB = telemetry.activeBufferSize || 96;
    syncBadge.textContent = (req === act && reqB === actB) ? "Requested matches Active" : `Requested ${reqB}/${req} -> Active ${actB}/${act}`;
  }

  const syncSlider = (id, labelId, val, fmt) => {
    const node = el(id);
    if (node && document.activeElement !== node) {
      node.value = val;
      if (el(labelId)) el(labelId).textContent = fmt(Number(val));
    }
  };

  if (el("hwPgaGain")) {
    el("hwPgaGain").min = telemetry.hardwarePgaMinDb ?? -63.5;
    el("hwPgaGain").max = telemetry.hardwarePgaMaxDb ?? 0;
    el("hwPgaGain").disabled = telemetry.hasHardwarePga === false;
  }
  if (el("hwDacGain")) {
    el("hwDacGain").min = telemetry.hardwareDacMinDb ?? -63.5;
    el("hwDacGain").max = telemetry.hardwareDacMaxDb ?? 0;
    el("hwDacGain").disabled = telemetry.hasHardwareDac === false;
  }
  if (el("hwDacBal")) el("hwDacBal").disabled = telemetry.hasHardwareDac === false;

  syncSlider("hwPgaGain", "hwPgaVal", params.hardwarePgaGainDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("hwDacGain", "hwDacVal", params.hardwareDacGainDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("hwDacBal", "hwDacBalVal", params.hardwareDacBalanceDb || 0, fmtBal);
  syncSlider("inputGain", "inputGainVal", params.inputGainDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("directMonitor", "directMonitorVal", Math.round((params.directMonitorMix || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("backingHeadroom", "backingHeadroomVal", params.backingHeadroomDb || 0, (v) => `${v.toFixed(1)} dB`);
  syncSlider("monitorReverb", "monitorReverbVal", Math.round((params.monitorReverbAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("deEsserSlider", "deEsserVal", Math.round((params.deEsserAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("fetPeakCatchSlider", "fetPeakCatchVal", Math.round((params.fetPeakCatchAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("vocalLevelerSlider", "vocalLevelerVal", Math.round((params.vocalLevelerAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("gateThreshold", "gateThresholdVal", params.gateThresholdDb ?? -42, (v) => `${Math.round(v)} dB`);
  syncSlider("gateHysteresis", "gateHysteresisVal", params.gateHysteresisDb ?? 4, (v) => `${v.toFixed(1)} dB`);
  syncSlider("gateRange", "gateRangeVal", params.gateRangeDb ?? -60, (v) => `${Math.round(v)} dB`);
  syncSlider("gateAttack", "gateAttackVal", params.gateAttackMs ?? 1.5, (v) => `${v.toFixed(1)} ms`);
  syncSlider("gateHold", "gateHoldVal", params.gateHoldMs ?? 60, (v) => `${Math.round(v)} ms`);
  syncSlider("gateRelease", "gateReleaseVal", params.gateReleaseMs ?? 140, (v) => `${Math.round(v)} ms`);
  syncSlider("gateHpf", "gateHpfVal", params.gateSidechainHpfHz ?? 90, (v) => (v === 0 ? "Off" : `${Math.round(v)} Hz`));
  syncSlider("shieldStrength", "shieldStrengthVal", Math.round((params.capsuleCorrectionStrength ?? 0.85) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("virtualDistanceSlider", "virtualDistanceVal", params.virtualDistanceCm ?? 15, (v) => `${Math.round(v)} cm`);
  syncSlider("preampDriveSlider", "preampDriveVal", params.preampDriveDb || 0, (v) => `${v.toFixed(1)} dB`);
  syncSlider("transformerIronSlider", "transformerIronVal", Math.round((params.transformerIronAmount ?? 0.5) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("consoleLowSlider", "consoleLowVal", params.consoleLowDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("consoleMidSlider", "consoleMidVal", params.consoleMidDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("consoleAirSlider", "consoleAirVal", params.consoleAirDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);

  const beautyPct = Math.round((params.beautyAmount || 0) * 100);
  syncSlider("beautyAmount", "beautyAmountVal", beautyPct, (v) => `${Math.round(v)}%`);
  syncSlider("trackBeautySlider", "trackBeautyVal", beautyPct, (v) => `${Math.round(v)}%`);
  syncSlider("contourAmtSlider", "contourAmtVal", Math.round((params.contourAmt ?? 1.0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("harmonicSculptSlider", "harmonicSculptVal", Math.round((params.harmonicSculptAmt ?? 1.0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("deHarshSlider", "deHarshVal", Math.round((params.deHarshAmt ?? 1.0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("parallelGlueSlider", "parallelGlueVal", Math.round((params.parallelGlueAmt ?? 0.15) * 100), (v) => `${Math.round(v)}%`);

  el("muteBtn").classList.toggle("muted", !!params.inputMute);
  el("muteBtn").textContent = params.inputMute ? "Muted" : "Mute";
  el("hwMuteBadge").style.display = telemetry.hardwareMuted ? "inline-flex" : "none";
  el("adcClipPill").style.display = telemetry.adcClipDetected ? "inline-flex" : "none";
  el("monoBadge").style.display = telemetry.captureMonoActive ? "inline-flex" : "none";

  el("audioHpfToggle").checked = !!params.audioHpfEnabled;
  el("softClipToggle").checked = params.softClipEnabled !== false;
  el("autoPadToggle").checked = !!params.autoPadOnClipEnabled;
  el("gateEnableToggle").checked = !!params.gateEnabled;
  el("gateLookAheadToggle").checked = !!params.gateLookAheadEnabled;
  el("capsuleEqToggle").checked = !!params.capsuleEqEnabled;
  el("capsuleCorrectionToggle").checked = !!params.capsuleCorrectionEnabled;
  el("capsuleCorrectionToggle").disabled = telemetry.capsuleCalibrated === false;
  el("capsuleCalibBadge").textContent = telemetry.capsuleCalibrated ? "Shield Calibrated" : "Needs Sweep";
  el("plosiveGuardToggle").checked = !!params.plosiveGuardEnabled;
  el("crossfeedToggle").checked = !!params.crossfeedEnabled;
  el("cueCarveToggle").checked = !!params.cueCarveEnabled;
  el("wdfEqToggle").checked = params.wdfEqEnabled !== false;
  el("molecularClipToggle").checked = params.molecularClip !== false;

  document.querySelectorAll("#fetRatioSelector button").forEach((b) => b.classList.toggle("active", Number(b.dataset.ratio) === (params.fetRatioMode || 0)));
  document.querySelectorAll("#mic2RouteSelector button").forEach((b) => {
    const rm = params.mic2RoutingMode !== undefined ? params.mic2RoutingMode : (params.dualTrackSafetyMode ? 2 : 0);
    b.classList.toggle("active", Number(b.dataset.route) === rm);
  });

  const micNames = ["Raw EM-91", "Sony C800G", "Neumann U87 Ai", "ELA M 251E", "Neumann U47", "Shure SM7B"];
  const preNames = ["Clean Bypass", "Neve 1073", "API 312", "V76 Tube", "SSL 9000J"];
  const eqNames = ["Bypass", "Pultec EQP-1A", "1073 Inductor", "SSL 4000G", "API 550A"];

  const micIdx = params.micModel || 0;
  document.querySelectorAll("#micModelSelector button").forEach((b) => b.classList.toggle("active", Number(b.dataset.mic) === micIdx));
  el("micModelBadge").textContent = `Mic: ${micNames[micIdx] || "Raw EM-91"}`;

  const preIdx = params.preampModel || 0;
  document.querySelectorAll("#preampModelSelector button").forEach((b) => b.classList.toggle("active", Number(b.dataset.preamp) === preIdx));
  el("preampModeBadge").textContent = preNames[preIdx] || "Clean Bypass";

  const eqIdx = params.consoleEqModel || 0;
  document.querySelectorAll("#consoleEqSelector button").forEach((b) => b.classList.toggle("active", Number(b.dataset.eq) === eqIdx));
  el("consoleEqBadge").textContent = `EQ: ${eqNames[eqIdx] || "Bypass"}`;

  el("dacIspGuardToggle").checked = params.dacIspGuardEnabled !== false;
  el("dacDitherShaperToggle").checked = params.dacDitherShaperEnabled !== false;
  el("ultrasonicLpfToggle").checked = params.ultrasonicLpfEnabled !== false;
  el("driftAsrcToggle").checked = params.driftAsrcEnabled !== false;
  el("autopilotToggle").checked = params.autopilotEnabled !== false;
  el("pCoreToggle").checked = params.pCorePriority !== false;
  el("xrunGuardToggle").checked = params.adaptiveXRunGuard !== false;

  document.querySelectorAll("#humNotchSelector button").forEach((b) => b.classList.toggle("active", Math.abs(Number(b.dataset.hum) - (params.humNotchFreqHz || 0)) < 1));
  document.querySelectorAll("#engineSelector button").forEach((b) => b.classList.toggle("active", Number(b.dataset.engine) === (params.engineMode || 0)));
  document.querySelectorAll("#irpSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.irp) === (params.ksIrpCount || 3));
    b.disabled = (params.engineMode || 0) !== 0;
  });
  document.querySelectorAll("#bufferSelector button").forEach((b) => b.classList.toggle("active", Number(b.dataset.buf) === (params.requestedBufferSize || 96)));
  document.querySelectorAll("#rateSelector button").forEach((b) => {
    const bit = Number(b.dataset.bit);
    b.disabled = ((telemetry.supportedSampleRatesMask ?? 0x1F) & (1 << bit)) === 0;
    b.classList.toggle("active", Math.abs(Number(b.dataset.rate) - (params.requestedSampleRate || 48000)) < 1);
  });

  // Calibrate feedback
  const learnBtn = el("learnNoiseBtn");
  const preampBtn = el("autoPreampBtn");
  if ((telemetry.learnState === 1 || telemetry.learnState === 2) && telemetry.preampCalibActive) {
    preampBtn.classList.add("learning");
    preampBtn.textContent = `Speak... ${Math.round((telemetry.learnProgress || 0) * 100)}%`;
    learnBtn.classList.remove("learning");
    learnBtn.textContent = "Learn Room Noise";
  } else if (telemetry.learnState === 1 || telemetry.learnState === 2) {
    learnBtn.classList.add("learning");
    learnBtn.textContent = `Listening... ${Math.round((telemetry.learnProgress || 0) * 100)}%`;
    preampBtn.classList.remove("learning");
    preampBtn.textContent = "Auto-Level Preamp (3s)";
  } else {
    learnBtn.classList.remove("learning");
    preampBtn.classList.remove("learning");
    preampBtn.textContent = (telemetry.learnState === 3 && Math.abs(telemetry.recommendedTrimDb || 0) + Math.abs(telemetry.recommendedPgaDb || 0) > 0.1)
      ? `Levelled (PGA ${(telemetry.recommendedPgaDb || 0).toFixed(1)} dB)` : "Auto-Level Preamp (3s)";
    learnBtn.textContent = (telemetry.learnState === 3 && (telemetry.measuredNoiseFloorDb ?? -96) > -90)
      ? `Calibrated (Floor ${telemetry.measuredNoiseFloorDb.toFixed(0)} dB)` : "Learn Room Noise";
  }

  const sr = Math.round(telemetry.activeSampleRate || params.requestedSampleRate || 48000);
  const buf = telemetry.activeBufferSize || params.requestedBufferSize || 96;
  const isUsdSync = (buf * 1000) % sr === 0;
  el("usbSyncHint").style.display = isUsdSync ? "inline" : "none";
  if (isUsdSync) el("usbSyncHint").textContent = `(${((buf * 1000) / sr).toFixed(0)}.0 ms USB Sync)`;

  const tpDb = telemetry.outputTruePeakDb !== undefined ? telemetry.outputTruePeakDb : -96.0;
  const ispGr = telemetry.dacIspReductionDb || 0.0;
  el("dacTruePeakVal").textContent = `${tpDb.toFixed(1)} dBTP`;
  el("dacIspStatus").textContent = params.dacIspGuardEnabled === false
    ? "Raw Bit-Exact DAC Bypass"
    : (ispGr < -0.05 ? `BS.1770 ISP Active: ${ispGr.toFixed(1)} dB` : "BS.1770 ISP Guard (-0.5 dBTP)");

  el("deviceDot").classList.toggle("connected", !!telemetry.deviceConnected);
  const micTag = telemetry.captureActive ? " | Mic Active" : "";
  el("deviceStatusText").textContent = telemetry.deviceConnected
    ? `${telemetry.deviceName || "Mackie EM-91CU+"}${micTag}${telemetry.streamRunning ? "" : " | Open FL Studio to arm"}`
    : "Waiting for Mackie EM-91CU+";
  el("engineSubtitle").textContent = (telemetry.activeEngineMode === 0)
    ? "WDM-KS Direct Kernel Streaming | MMCSS Pro Audio | Zero audiodg"
    : "WASAPI Exclusive Event-Driven | MMCSS Pro Audio";
  el("formatBadge").textContent = `${telemetry.activeBitDepth || 24}-bit${(telemetry.activeBitDepth === 16 && params.dacDitherShaperEnabled !== false) ? " + NS" : ""} | ${((telemetry.activeSampleRate || 48000) / 1000).toFixed(1).replace(".0", "")} kHz`;
  el("latencyBreakdown").textContent = `In ${(telemetry.inputLatencyMs || 0).toFixed(1)} | Out ${(telemetry.outputLatencyMs || 0).toFixed(1)} ms`;
  el("latencyReadout").innerHTML = `${(telemetry.roundTripLatencyMs || 0).toFixed(1)} <small>ms</small>`;

  el("inputPeakText").textContent = `${(telemetry.inputPeakDb ?? -96).toFixed(1)} dBFS`;
  el("inputMeterFill").style.width = `${dbToPercent(telemetry.inputPeakDb ?? -96)}%`;
  el("inputRmsMarker").style.left = `${dbToPercent(telemetry.inputRmsDb ?? -96)}%`;

  el("gateGrText").textContent = `${(telemetry.gateGainReductionDb || 0).toFixed(1)} dB`;
  el("gateGrFill").style.width = `${dbToPercent(-(telemetry.gateGainReductionDb || 0), 0, 60)}%`;

  const gatePill = el("gateStatePill");
  if (!params.gateEnabled) { gatePill.textContent = "Bypassed"; gatePill.className = "gate-state-pill"; }
  else if (telemetry.gateIsOpen) { gatePill.textContent = "Open"; gatePill.className = "gate-state-pill open"; }
  else { gatePill.textContent = "Closed"; gatePill.className = "gate-state-pill closed"; }

  el("deEssChip").textContent = `De-Ess: ${(telemetry.deEsserReductionDb || 0).toFixed(1)} dB`;
  el("plosChip").textContent = `Pop: ${(telemetry.plosiveReductionDb || 0).toFixed(1)} dB`;
  el("fetChip").textContent = `1176 FET: ${(telemetry.fetReductionDb || 0).toFixed(1)} dB`;
  el("levChip").textContent = `LA-2A T4B: ${(telemetry.levelerReductionDb || 0).toFixed(1)} dB`;
  el("irpBadge").textContent = `Active IRPs: ${telemetry.activeIrpCount || 3}x`;
  const ppm = telemetry.clockDriftPpm || 0;
  el("driftChip").textContent = `Drift: ${ppm >= 0 ? "+" : ""}${ppm.toFixed(1)} ppm`;
  el("cpuChip").textContent = `Audio CPU: ${(telemetry.callbackCpuPercent || 0).toFixed(1)}%`;
  el("jitterChip").textContent = `Jitter: ${Math.round(telemetry.lastJitterUs || 0)} / ${Math.round(telemetry.maxJitterUs || 0)} us`;
  el("xrunChip").textContent = `XRuns: ${telemetry.xrunCount || 0}`;

  updateFlowBadges(params, telemetry);

  // Event-driven 0% idle CPU curve redraw: only redraw when curve parameters or GR change meaningfully
  const curveSig = JSON.stringify([
    params.audioHpfEnabled, params.virtualDistanceCm, params.micModel, params.consoleEqModel,
    params.consoleLowDb, params.consoleMidDb, params.consoleAirDb, params.capsuleCorrectionEnabled,
    params.capsuleCorrectionStrength, params.beautyAmount, params.contourAmt,
    Math.round(Math.abs(telemetry.fetReductionDb || 0) * 4),
    Math.round(Math.abs(telemetry.levelerReductionDb || 0) * 4),
    document.documentElement.getAttribute("data-theme"),
  ]);
  if (curveSig !== lastCurveSig) {
    lastCurveSig = curveSig;
    drawCurve(params, telemetry);
  }

  drawJitterSparkline(telemetry.jitterHistoryUs);
  drawGateVisualizer(telemetry.inputPeakDb ?? -96, params.gateThresholdDb ?? -42, params.gateHysteresisDb ?? 4, !!params.gateEnabled, !!telemetry.gateIsOpen);
}

window.__applyMockState = applyStateMessage;

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener("message", (event) => applyStateMessage(event.data));
} else {
  const qp = new URLSearchParams(window.location.search);
  const mockMode = qp.get("mock");
  if (mockMode === "dark" || mockMode === "light") {
    applyStateMessage({
      type: "state",
      params: {
        ...INIT_PATCH,
        inputGainDb: 2.0, audioHpfEnabled: true, directMonitorMix: 0.25,
        hardwarePgaGainDb: -6.0, hardwareDacGainDb: -6.0, hardwareDacBalanceDb: 0.0,
        gateEnabled: true, gateThresholdDb: -42.0, gateHysteresisDb: 4.0,
        gateAttackMs: 1.5, gateHoldMs: 60.0, gateReleaseMs: 140.0, gateRangeDb: -60.0, gateSidechainHpfHz: 90.0,
        humNotchFreqHz: 60.0, micModel: 1, preampModel: 1, preampDriveDb: 4.0,
        consoleEqModel: 1, consoleLowDb: 2.0, consoleAirDb: 3.0,
        fetPeakCatchAmount: 0.35, vocalLevelerAmount: 0.45, beautyAmount: 0.45, deEsserAmount: 0.35,
        themeMode: mockMode === "light" ? 2 : 1,
      },
      telemetry: {
        deviceConnected: true, captureActive: true, streamRunning: true,
        activeEngineMode: 0, activeIrpCount: 3, activeBufferSize: 96, activeSampleRate: 48000, activeBitDepth: 24,
        inputLatencyMs: 2.0, outputLatencyMs: 6.0, roundTripLatencyMs: 8.0,
        hasHardwarePga: true, hardwarePgaMinDb: -63.5, hardwarePgaMaxDb: 0.0,
        hasHardwareDac: true, hardwareDacMinDb: -63.5, hardwareDacMaxDb: 0.0,
        inputPeakDb: -18.5, inputRmsDb: -26.0, outputTruePeakDb: -11.9,
        clockDriftPpm: 2.4, gateGainReductionDb: 0.0, gateIsOpen: true,
        deEsserReductionDb: -1.8, plosiveReductionDb: -0.4, fetReductionDb: -2.2, levelerReductionDb: -1.9,
        capsuleCalibrated: true, learnState: 3, learnProgress: 1.0, measuredNoiseFloorDb: -58.0,
        callbackCpuPercent: 1.4, lastJitterUs: 42, maxJitterUs: 165, xrunCount: 0,
        supportedSampleRatesMask: 0x1F, deviceName: "Mackie EM-91CU+ (WDM-KS Direct)",
        windowsDark: mockMode === "dark",
        jitterHistoryUs: [32, 48, 41, 65, 39, 52, 44, 38, 70, 45, 51, 42, 36, 48, 55, 40],
      },
    });
    setActivePresetUI("modernPop");
  }
}
