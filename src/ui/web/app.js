const postMsg = (payload) => {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(payload);
  }
};

const postParam = (key, value) => postMsg({ action: "setParam", key, value });
const el = (id) => document.getElementById(id);

const INIT_PATCH = {
  inputGainDb: 0.0,
  softClipEnabled: true,
  audioHpfEnabled: false,
  micModel: 0,
  virtualDistanceCm: 15,
  preampModel: 0,
  preampDriveDb: 0,
  transformerIronAmount: 0.5,
  consoleEqModel: 0,
  consoleLowDb: 0,
  consoleMidDb: 0,
  consoleAirDb: 0,
  fetPeakCatchAmount: 0,
  fetRatioMode: 0,
  vocalLevelerAmount: 0,
  beautyAmount: 0,
  deEsserAmount: 0,
  plosiveGuardEnabled: false,
  capsuleEqEnabled: false,
  molecularClip: true,
  wdfEqEnabled: true,
};

const PRESETS = {
  modernPop: {
    ...INIT_PATCH, inputGainDb: 2, micModel: 1, preampModel: 1, preampDriveDb: 4,
    consoleEqModel: 1, consoleLowDb: 2, consoleAirDb: 3, fetPeakCatchAmount: 0.35,
    vocalLevelerAmount: 0.45, beautyAmount: 0.55, deEsserAmount: 0.4, audioHpfEnabled: true, capsuleEqEnabled: true,
  },
  rnb: {
    ...INIT_PATCH, inputGainDb: 1.5, micModel: 3, preampModel: 3, preampDriveDb: 6,
    transformerIronAmount: 0.7, consoleEqModel: 2, consoleLowDb: 2.5, consoleMidDb: 1,
    vocalLevelerAmount: 0.6, beautyAmount: 0.4, deEsserAmount: 0.25, audioHpfEnabled: true,
  },
  rap: {
    ...INIT_PATCH, inputGainDb: 3, micModel: 5, preampModel: 2, preampDriveDb: 5,
    consoleEqModel: 4, consoleLowDb: 1, consoleMidDb: 2, fetPeakCatchAmount: 0.7,
    fetRatioMode: 2, beautyAmount: 0.25, deEsserAmount: 0.5, plosiveGuardEnabled: true, audioHpfEnabled: true,
  },
  acoustic: {
    ...INIT_PATCH, inputGainDb: 1, micModel: 4, preampModel: 3, preampDriveDb: 2,
    consoleEqModel: 1, consoleAirDb: 1.5, vocalLevelerAmount: 0.3, beautyAmount: 0.2, deEsserAmount: 0.15,
  },
  podcast: {
    ...INIT_PATCH, inputGainDb: 4, micModel: 2, preampModel: 4, preampDriveDb: 1,
    consoleEqModel: 3, consoleLowDb: -1, consoleMidDb: 1, fetPeakCatchAmount: 0.2,
    vocalLevelerAmount: 0.5, deEsserAmount: 0.45, plosiveGuardEnabled: true, audioHpfEnabled: true, capsuleEqEnabled: true,
  },
  raw: { ...INIT_PATCH, softClipEnabled: true, molecularClip: false },
};

const LOCK_KEYS = ["hardwarePgaGainDb", "gateEnabled", "gateThresholdDb", "gateHysteresisDb", "humNotchFreqHz"];

function dbToPercent(db, min = -60, max = 0) {
  return Math.max(0, Math.min(100, ((db - min) / (max - min)) * 100));
}

function applyTheme(mode, windowsDark) {
  let theme = "dark";
  if (mode === 2) theme = "light";
  else if (mode === 0) theme = windowsDark ? "dark" : "light";
  document.documentElement.setAttribute("data-theme", theme);

  document.querySelectorAll("#themeSwitcher button").forEach((btn) => {
    btn.classList.toggle("active", Number(btn.dataset.themeMode) === mode);
  });
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
  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim();
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
  const accent = style.getPropertyValue("--accent").trim();
  const green = style.getPropertyValue("--green").trim();
  const orange = style.getPropertyValue("--orange").trim();
  const muted = style.getPropertyValue("--text-secondary").trim();

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

  ctx.fillStyle = !gateEnabled ? muted : (gateIsOpen ? green : orange);
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
  ctx.font = "10px 'SF Mono', Consolas, monospace";
  ctx.fillText("-80 dB", 6, h - 6);
  ctx.fillText("0 dB", w - 30, h - 6);
  ctx.fillStyle = accent;
  ctx.fillText(`Open ${Math.round(threshDb)} dB`, Math.min(w - 78, openX + 5), 13);
}

// Wire controls
el("hwPgaGain").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("hwPgaVal").textContent = `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`;
  postParam("hardwarePgaGainDb", v);
});

el("hwDacGain").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("hwDacVal").textContent = `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`;
  postParam("hardwareDacGainDb", v);
});

const fmtBal = (v) => {
  if (Math.abs(v) < 0.25) return "Center";
  return v < 0 ? `L ${Math.abs(v).toFixed(1)} dB` : `R ${v.toFixed(1)} dB`;
};

el("hwDacBal").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("hwDacBalVal").textContent = fmtBal(v);
  postParam("hardwareDacBalanceDb", v);
});

el("inputGain").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("inputGainVal").textContent = `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`;
  postParam("inputGainDb", v);
});

el("directMonitor").addEventListener("input", (e) => {
  const pct = parseFloat(e.target.value);
  el("directMonitorVal").textContent = `${Math.round(pct)}%`;
  postParam("directMonitorMix", pct / 100.0);
});

el("monitorReverb").addEventListener("input", (e) => {
  const pct = parseFloat(e.target.value);
  el("monitorReverbVal").textContent = `${Math.round(pct)}%`;
  postParam("monitorReverbAmount", pct / 100.0);
});

el("deEsserSlider").addEventListener("input", (e) => {
  const pct = parseFloat(e.target.value);
  el("deEsserVal").textContent = `${Math.round(pct)}%`;
  postParam("deEsserAmount", pct / 100.0);
});

el("fetPeakCatchSlider").addEventListener("input", (e) => {
  const pct = parseFloat(e.target.value);
  el("fetPeakCatchVal").textContent = `${Math.round(pct)}%`;
  postParam("fetPeakCatchAmount", pct / 100.0);
});

document.querySelectorAll("#fetRatioSelector button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const r = Number(btn.dataset.ratio);
    document.querySelectorAll("#fetRatioSelector button").forEach((b) => {
      b.classList.toggle("active", Number(b.dataset.ratio) === r);
    });
    postParam("fetRatioMode", r);
  });
});

el("vocalLevelerSlider").addEventListener("input", (e) => {
  const pct = parseFloat(e.target.value);
  el("vocalLevelerVal").textContent = `${Math.round(pct)}%`;
  postParam("vocalLevelerAmount", pct / 100.0);
});

el("muteBtn").addEventListener("click", () => {
  const next = !el("muteBtn").classList.contains("muted");
  el("muteBtn").classList.toggle("muted", next);
  el("muteBtn").textContent = next ? "Muted" : "Mute";
  postParam("inputMute", next);
});

el("adcClipPill").addEventListener("click", () => postMsg({ action: "clearAdcClip" }));
el("learnNoiseBtn").addEventListener("click", () => postMsg({ action: "startLearnNoise" }));
el("autoPreampBtn").addEventListener("click", () => postMsg({ action: "startPreampAutoLevel" }));
el("optimalPresetBtn").addEventListener("click", () => postMsg({ action: "applyOptimalPreset" }));

el("audioHpfToggle").addEventListener("change", (e) => postParam("audioHpfEnabled", e.target.checked));
el("softClipToggle").addEventListener("change", (e) => postParam("softClipEnabled", e.target.checked));
el("autoPadToggle").addEventListener("change", (e) => postParam("autoPadOnClipEnabled", e.target.checked));
el("gateEnableToggle").addEventListener("change", (e) => postParam("gateEnabled", e.target.checked));
el("gateLookAheadToggle").addEventListener("change", (e) => postParam("gateLookAheadEnabled", e.target.checked));
el("capsuleEqToggle").addEventListener("change", (e) => postParam("capsuleEqEnabled", e.target.checked));
el("capsuleCorrectionToggle").addEventListener("change", (e) => postParam("capsuleCorrectionEnabled", e.target.checked));
el("plosiveGuardToggle").addEventListener("change", (e) => postParam("plosiveGuardEnabled", e.target.checked));
el("crossfeedToggle").addEventListener("change", (e) => postParam("crossfeedEnabled", e.target.checked));
el("cueCarveToggle").addEventListener("change", (e) => postParam("cueCarveEnabled", e.target.checked));

document.querySelectorAll("#mic2RouteSelector button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const r = Number(btn.dataset.route);
    document.querySelectorAll("#mic2RouteSelector button").forEach((b) => {
      b.classList.toggle("active", Number(b.dataset.route) === r);
    });
    postParam("mic2RoutingMode", r);
    postParam("dualTrackSafetyMode", r === 2);
  });
});

document.querySelectorAll("#micModelSelector button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const m = Number(btn.dataset.mic);
    document.querySelectorAll("#micModelSelector button").forEach((b) => {
      b.classList.toggle("active", Number(b.dataset.mic) === m);
    });
    postParam("micModel", m);
  });
});

el("virtualDistanceSlider").addEventListener("input", (e) => {
  const cm = parseFloat(e.target.value);
  el("virtualDistanceVal").textContent = `${Math.round(cm)} cm`;
  postParam("virtualDistanceCm", cm);
});

document.querySelectorAll("#preampModelSelector button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const m = Number(btn.dataset.preamp);
    document.querySelectorAll("#preampModelSelector button").forEach((b) => {
      b.classList.toggle("active", Number(b.dataset.preamp) === m);
    });
    postParam("preampModel", m);
  });
});

document.querySelectorAll("#consoleEqSelector button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const eq = Number(btn.dataset.eq);
    document.querySelectorAll("#consoleEqSelector button").forEach((b) => {
      b.classList.toggle("active", Number(b.dataset.eq) === eq);
    });
    postParam("consoleEqModel", eq);
  });
});

el("preampDriveSlider").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("preampDriveVal").textContent = `${v.toFixed(1)} dB`;
  postParam("preampDriveDb", v);
});

el("transformerIronSlider").addEventListener("input", (e) => {
  const pct = parseFloat(e.target.value);
  el("transformerIronVal").textContent = `${Math.round(pct)}%`;
  postParam("transformerIronAmount", pct / 100.0);
});

el("consoleLowSlider").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("consoleLowVal").textContent = `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`;
  postParam("consoleLowDb", v);
});

el("consoleMidSlider").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("consoleMidVal").textContent = `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`;
  postParam("consoleMidDb", v);
});

el("consoleAirSlider").addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("consoleAirVal").textContent = `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`;
  postParam("consoleAirDb", v);
});

let abBypassState = false;
let lastAbMatchDb = 0.0;
el("abBypassBtn").addEventListener("click", () => {
  abBypassState = !abBypassState;
  const abBtn = el("abBypassBtn");
  abBtn.classList.toggle("learning", abBypassState);
  abBtn.textContent = abBypassState ? `B: Raw (${lastAbMatchDb >= 0 ? "+" : ""}${lastAbMatchDb.toFixed(1)} dB)` : "A: Studio Chain";
  postParam("abLevelMatchedBypass", abBypassState);
});
el("dacIspGuardToggle").addEventListener("change", (e) => postParam("dacIspGuardEnabled", e.target.checked));
el("dacDitherShaperToggle").addEventListener("change", (e) => postParam("dacDitherShaperEnabled", e.target.checked));
el("ultrasonicLpfToggle").addEventListener("change", (e) => postParam("ultrasonicLpfEnabled", e.target.checked));
el("driftAsrcToggle").addEventListener("change", (e) => postParam("driftAsrcEnabled", e.target.checked));
el("autopilotToggle").addEventListener("change", (e) => postParam("autopilotEnabled", e.target.checked));
el("pCoreToggle").addEventListener("change", (e) => postParam("pCorePriority", e.target.checked));
el("xrunGuardToggle").addEventListener("change", (e) => postParam("adaptiveXRunGuard", e.target.checked));

const bindSlider = (id, labelId, key, fmt) => {
  el(id).addEventListener("input", (e) => {
    const v = parseFloat(e.target.value);
    el(labelId).textContent = fmt(v);
    postParam(key, v);
  });
};

bindSlider("gateThreshold", "gateThresholdVal", "gateThresholdDb", (v) => `${v} dB`);
bindSlider("gateRange", "gateRangeVal", "gateRangeDb", (v) => `${v} dB`);
bindSlider("gateAttack", "gateAttackVal", "gateAttackMs", (v) => `${v.toFixed(1)} ms`);
bindSlider("gateHold", "gateHoldVal", "gateHoldMs", (v) => `${v} ms`);
bindSlider("gateRelease", "gateReleaseVal", "gateReleaseMs", (v) => `${v} ms`);
bindSlider("gateHpf", "gateHpfVal", "gateSidechainHpfHz", (v) => (v === 0 ? "Off" : `${v} Hz`));

el("xrunChip").addEventListener("click", () => postMsg({ action: "resetXRuns" }));

document.querySelectorAll('input[type="range"]').forEach((slider) => {
  slider.addEventListener("dblclick", () => {
    const def = slider.dataset.default !== undefined ? slider.dataset.default : slider.defaultValue;
    slider.value = def;
    slider.dispatchEvent(new Event("input"));
  });
});

document.querySelectorAll("#themeSwitcher button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("themeMode", Number(btn.dataset.themeMode)));
});

document.querySelectorAll("#humNotchSelector button").forEach((btn) => {
  btn.addEventListener("click", () => postParam("humNotchFreqHz", Number(btn.dataset.hum)));
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
  btn.addEventListener("click", () => {
    if (!btn.disabled) postParam("requestedSampleRate", Number(btn.dataset.rate));
  });
});

let activePreset = "";
let undoStack = [];
let redoStack = [];
let slotA = null;
let slotB = null;
let latestParams = {};
let curveDirty = true;
let tickCurve = 0;

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
}

function applyPatch(patch, skipLocked) {
  const skip = skipLocked ? lockedKeys() : [];
  Object.entries(patch).forEach(([k, v]) => {
    if (skip.indexOf(k) >= 0) return;
    postParam(k, v);
  });
  curveDirty = true;
}

document.querySelectorAll("#tabBar button").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.body.dataset.page = btn.dataset.tab;
    document.querySelectorAll("#tabBar button").forEach((b) => b.classList.toggle("active", b === btn));
  });
});
const beauty = el("beautyAmount");
if (beauty) beauty.addEventListener("input", () => postParam("beautyAmount", Number(beauty.value)));
const eco = el("ecoToggle");
if (eco) eco.addEventListener("change", () => postParam("ecoMode", eco.checked));
const voice = (id, state) => {
  const node = el(id);
  if (node) node.addEventListener("click", () => postParam("voiceLoopState", state));
};
voice("voiceRecBtn", 1);
voice("voicePlayBtn", 2);
voice("voiceStopBtn", 0);

document.querySelectorAll("#presetPills button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const name = btn.dataset.preset;
    const turningOff = activePreset === name;
    pushUndo();
    activePreset = turningOff ? "" : name;
    document.querySelectorAll("#presetPills button").forEach((b) => b.classList.toggle("active", b === btn && !turningOff));
    applyPatch(turningOff ? INIT_PATCH : PRESETS[name], true);
  });
});

const hyst = el("gateHysteresis");
if (hyst) hyst.addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("gateHysteresisVal").textContent = `${v.toFixed(1)} dB`;
  postParam("gateHysteresisDb", v);
  curveDirty = true;
});
const shield = el("shieldStrength");
if (shield) shield.addEventListener("input", (e) => {
  const v = parseFloat(e.target.value);
  el("shieldStrengthVal").textContent = `${Math.round(v)}%`;
  postParam("capsuleCorrectionStrength", v / 100);
  curveDirty = true;
});

const ecoBtn = el("ecoBtn");
function setEco(on) {
  postParam("ecoMode", !!on);
  document.body.classList.toggle("eco-on", !!on);
  if (ecoBtn) ecoBtn.textContent = on ? "ECO: ON" : "ECO: OFF";
  const badge = el("ecoBadge");
  if (badge) badge.style.display = on ? "inline-flex" : "none";
  const dot = el("ecoDot");
  if (dot) dot.classList.toggle("lit", !!on);
  if (on) {
    const toast = el("ecoToast");
    if (toast) {
      toast.classList.add("show");
      setTimeout(() => toast.classList.remove("show"), 1600);
    }
  }
}
if (ecoBtn) ecoBtn.addEventListener("click", () => setEco(!(latestParams.ecoMode)));

if (el("abBypassBtn")) el("abBypassBtn").addEventListener("click", () => {
  pushUndo();
  postParam("abLevelMatchedBypass", !latestParams.abLevelMatchedBypass);
  postParam("abAuditionMonitorOnly", true);
});
if (el("slotABtn")) el("slotABtn").addEventListener("click", () => {
  if (slotA && el("slotABtn").dataset.armed === "1") { pushUndo(); applyPatch(slotA, false); }
  else { slotA = { ...latestParams }; el("slotABtn").dataset.armed = "1"; el("slotABtn").textContent = "Slot A *"; }
});
if (el("slotBBtn")) el("slotBBtn").addEventListener("click", () => {
  if (slotB && el("slotBBtn").dataset.armed === "1") { pushUndo(); applyPatch(slotB, false); }
  else { slotB = { ...latestParams }; el("slotBBtn").dataset.armed = "1"; el("slotBBtn").textContent = "Slot B *"; }
});
document.addEventListener("keydown", (e) => {
  if (!(e.ctrlKey || e.metaKey)) return;
  if (e.key === "z" && undoStack.length) {
    redoStack.push({ ...latestParams });
    applyPatch(undoStack.pop(), false);
    e.preventDefault();
  } else if (e.key === "y" && redoStack.length) {
    undoStack.push({ ...latestParams });
    applyPatch(redoStack.pop(), false);
    e.preventDefault();
  }
});
if (el("profileTrack")) el("profileTrack").addEventListener("click", () => {
  postParam("requestedBufferSize", 96);
  postParam("requestedSampleRate", 48000);
});
if (el("profileMix")) el("profileMix").addEventListener("click", () => postParam("requestedBufferSize", 512));
if (el("syncPluginBtn")) el("syncPluginBtn").addEventListener("click", () => postMsg({ action: "syncToPlugin" }));

const flowCopy = {
  shield: "Shield correction, before the mic model.",
  mic: "Mic Locker capsule and head-amp.",
  pre: "Preamp iron and tube or IC stage.",
  gate: "Gate, plosive guard, and de-esser.",
  fet: "1176 JFET, including All-Buttons British.",
  la2a: "LA-2A gray-box optical leveler.",
  eq: "Console EQ in the oversampled analog rack.",
  beauty: "Psychoacoustic beauty, monitor stays separate.",
};
document.querySelectorAll("#flowStrip button").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll("#flowStrip button").forEach((b) => b.classList.toggle("active", b === btn));
    const detail = el("flowDetail");
    if (detail) detail.textContent = flowCopy[btn.dataset.inspect] || "";
  });
});

let abx = { trial: 0, correct: 0, hidden: 0, running: false };
function binomP(k) {
  let p = 0;
  const choose = (n, r) => {
    let c = 1;
    for (let i = 0; i < r; i++) c = c * (n - i) / (i + 1);
    return c;
  };
  for (let i = k; i <= 10; i++) p += choose(10, i);
  return p / 1024;
}
function abxHear(which) {
  const raw = which === "B" || (which === "X" && abx.hidden === 1);
  postParam("abLevelMatchedBypass", raw);
  postParam("abAuditionMonitorOnly", true);
  postParam("freezeAbxGain", true);
}
if (el("abxStart")) el("abxStart").addEventListener("click", () => {
  abx = { trial: 1, correct: 0, hidden: Math.random() < 0.5 ? 0 : 1, running: true };
  postParam("voiceLoopState", 2);
  postParam("freezeAbxGain", true);
  el("abxStatus").textContent = "Trial 1 of 10. Hear A, B, then X.";
  el("abxScore").textContent = "Score 0/10";
});
if (el("abxHearA")) el("abxHearA").addEventListener("click", () => abxHear("A"));
if (el("abxHearB")) el("abxHearB").addEventListener("click", () => abxHear("B"));
if (el("abxHearX")) el("abxHearX").addEventListener("click", () => abxHear("X"));
function abxAnswer(isA) {
  if (!abx.running) return;
  const guess = isA ? 0 : 1;
  if (guess === abx.hidden) abx.correct += 1;
  if (abx.trial >= 10) {
    abx.running = false;
    const p = binomP(abx.correct);
    el("abxScore").textContent = `Score ${abx.correct}/10  p=${p.toFixed(4)}`;
    el("abxStatus").textContent = "Finished. Loudness match unlocked.";
    postParam("freezeAbxGain", false);
    postParam("abLevelMatchedBypass", false);
    return;
  }
  abx.trial += 1;
  abx.hidden = Math.random() < 0.5 ? 0 : 1;
  el("abxStatus").textContent = `Trial ${abx.trial} of 10. Hear A, B, then X.`;
  el("abxScore").textContent = `Score ${abx.correct}/10`;
}
if (el("abxPickA")) el("abxPickA").addEventListener("click", () => abxAnswer(true));
if (el("abxPickB")) el("abxPickB").addEventListener("click", () => abxAnswer(false));

function drawCurve(params, telemetry) {
  const canvas = el("curveCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = "#0a84ff";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  const low = params.consoleLowDb || 0;
  const mid = params.consoleMidDb || 0;
  const air = params.consoleAirDb || 0;
  const shieldAmt = params.capsuleCorrectionStrength || 0;
  const gr = Math.abs((telemetry && telemetry.fetReductionDb) || 0) + Math.abs((telemetry && telemetry.levelerReductionDb) || 0);
  for (let x = 0; x < w; x++) {
    const t = x / w;
    const db = low * Math.exp(-Math.pow((t - 0.12) / 0.12, 2))
      + mid * Math.exp(-Math.pow((t - 0.42) / 0.14, 2))
      + air * Math.exp(-Math.pow((t - 0.82) / 0.16, 2))
      + shieldAmt * 2.5 * Math.exp(-Math.pow((t - 0.7) / 0.2, 2))
      - gr * 0.15 * Math.exp(-Math.pow((t - 0.55) / 0.2, 2));
    const y = h * 0.55 - db * 4;
    if (x === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

function applyStateMessage(msg) {
  if (!msg || msg.type !== "state") return;
  const { params, telemetry } = msg;
  latestParams = params;

  applyTheme(params.themeMode, telemetry.windowsDark);
  const stream = el("streamPill");
  if (stream) {
    stream.textContent = telemetry.streamRunning ? "DAW Stream Active" : "Standby";
    stream.classList.toggle("pill-ok", !!telemetry.streamRunning);
  }
  const safe = el("recordingSafe");
  if (safe) safe.style.display = params.abAuditionMonitorOnly !== false ? "inline-flex" : "none";
  const ab = el("abBypassBtn");
  if (ab) {
    const match = telemetry.abMatchGainDb || 0;
    ab.textContent = params.abLevelMatchedBypass
      ? `B: Raw (${match >= 0 ? "+" : ""}${match.toFixed(1)} dB)`
      : "A: Studio Chain";
  }
  if (params.ecoMode) document.body.classList.add("eco-on");
  else document.body.classList.remove("eco-on");
  if (ecoBtn) ecoBtn.textContent = params.ecoMode ? "ECO: ON" : "ECO: OFF";
  const badge = el("ecoBadge");
  if (badge) badge.style.display = params.ecoMode ? "inline-flex" : "none";
  const dot = el("ecoDot");
  if (dot) dot.classList.toggle("lit", !!params.ecoMode);
  const live = !!telemetry.streamRunning;
  ["autoPreampBtn", "learnNoiseBtn"].forEach((id) => {
    const node = el(id);
    if (node) node.disabled = !live;
  });
  const hint = el("setupHint");
  if (hint) hint.style.display = live ? "none" : "block";
  ["stepConnect", "stepLevel", "stepNoise"].forEach((id, i) => {
    const node = el(id);
    if (!node) return;
    const done = i === 0 ? live : (i === 1 ? telemetry.learnState === 3 && Math.abs(telemetry.recommendedTrimDb || 0) + Math.abs(telemetry.recommendedPgaDb || 0) > 0.05 : telemetry.measuredNoiseFloorDb > -90);
    node.classList.toggle("done", !!done);
  });
  const syncBadge = el("rateSyncBadge");
  if (syncBadge) {
    const req = Math.round(params.requestedSampleRate || 0);
    const act = Math.round(telemetry.activeSampleRate || 0);
    const reqB = params.requestedBufferSize || 0;
    const actB = telemetry.activeBufferSize || 0;
    syncBadge.textContent = (req === act && reqB === actB) ? "Requested matches Active" : `Requested ${reqB}/${req} -> Active ${actB}/${act}`;
  }
  if (curveDirty || (live && (tickCurve++ % 8 === 0))) {
    drawCurve(params, telemetry);
    curveDirty = false;
  }

  const syncSlider = (id, labelId, val, fmt) => {
    const node = el(id);
    if (node && document.activeElement !== node) {
      node.value = val;
      el(labelId).textContent = fmt(val);
    }
  };

  // Hardware codec range setup
  el("hwPgaGain").min = telemetry.hardwarePgaMinDb;
  el("hwPgaGain").max = telemetry.hardwarePgaMaxDb;
  el("hwPgaGain").disabled = !telemetry.hasHardwarePga;
  el("hwDacGain").min = telemetry.hardwareDacMinDb;
  el("hwDacGain").max = telemetry.hardwareDacMaxDb;
  el("hwDacGain").disabled = !telemetry.hasHardwareDac;

  syncSlider("hwPgaGain", "hwPgaVal", params.hardwarePgaGainDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("hwDacGain", "hwDacVal", params.hardwareDacGainDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("hwDacBal", "hwDacBalVal", params.hardwareDacBalanceDb || 0, fmtBal);
  el("hwDacBal").disabled = !telemetry.hasHardwareDac;
  syncSlider("inputGain", "inputGainVal", params.inputGainDb, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("directMonitor", "directMonitorVal", Math.round((params.directMonitorMix || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("monitorReverb", "monitorReverbVal", Math.round((params.monitorReverbAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("deEsserSlider", "deEsserVal", Math.round((params.deEsserAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("fetPeakCatchSlider", "fetPeakCatchVal", Math.round((params.fetPeakCatchAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  document.querySelectorAll("#fetRatioSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.ratio) === (params.fetRatioMode || 0));
  });
  syncSlider("vocalLevelerSlider", "vocalLevelerVal", Math.round((params.vocalLevelerAmount || 0) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("gateThreshold", "gateThresholdVal", params.gateThresholdDb, (v) => `${Math.round(v)} dB`);
  syncSlider("gateRange", "gateRangeVal", params.gateRangeDb, (v) => `${Math.round(v)} dB`);
  syncSlider("gateAttack", "gateAttackVal", params.gateAttackMs, (v) => `${v.toFixed(1)} ms`);
  syncSlider("gateHold", "gateHoldVal", params.gateHoldMs, (v) => `${Math.round(v)} ms`);
  syncSlider("gateRelease", "gateReleaseVal", params.gateReleaseMs, (v) => `${Math.round(v)} ms`);
  syncSlider("gateHpf", "gateHpfVal", params.gateSidechainHpfHz, (v) => (v === 0 ? "Off" : `${Math.round(v)} Hz`));
  if (el("gateHysteresis")) syncSlider("gateHysteresis", "gateHysteresisVal", params.gateHysteresisDb || 0, (v) => `${Number(v).toFixed(1)} dB`);
  if (el("shieldStrength")) syncSlider("shieldStrength", "shieldStrengthVal", Math.round((params.capsuleCorrectionStrength || 0.85) * 100), (v) => `${Math.round(v)}%`);

  el("muteBtn").classList.toggle("muted", params.inputMute);
  el("muteBtn").textContent = params.inputMute ? "Muted" : "Mute";
  el("hwMuteBadge").style.display = telemetry.hardwareMuted ? "inline-flex" : "none";
  el("adcClipPill").style.display = telemetry.adcClipDetected ? "inline-flex" : "none";
  el("monoBadge").style.display = telemetry.captureMonoActive ? "inline-flex" : "none";

  el("audioHpfToggle").checked = !!params.audioHpfEnabled;
  el("softClipToggle").checked = params.softClipEnabled;
  el("autoPadToggle").checked = !!params.autoPadOnClipEnabled;
  el("gateEnableToggle").checked = params.gateEnabled;
  el("gateLookAheadToggle").checked = !!params.gateLookAheadEnabled;
  el("capsuleEqToggle").checked = !!params.capsuleEqEnabled;
  el("capsuleCorrectionToggle").checked = !!params.capsuleCorrectionEnabled;
  el("capsuleCorrectionToggle").disabled = !telemetry.capsuleCalibrated;
  el("capsuleCalibBadge").textContent = telemetry.capsuleCalibrated ? "Shield Calibrated" : "Needs Sweep";
  el("plosiveGuardToggle").checked = !!params.plosiveGuardEnabled;
  el("crossfeedToggle").checked = !!params.crossfeedEnabled;
  el("cueCarveToggle").checked = !!params.cueCarveEnabled;

  const routeMode = params.mic2RoutingMode !== undefined ? params.mic2RoutingMode : (params.dualTrackSafetyMode ? 2 : 0);
  document.querySelectorAll("#mic2RouteSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.route) === routeMode);
  });

  const micIdx = params.micModel || 0;
  const micNames = ["Raw EM-91", "Sony C800G", "Neumann U87 Ai", "ELA M 251E", "Neumann U47", "Shure SM7B"];
  document.querySelectorAll("#micModelSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.mic) === micIdx);
  });
  el("micModelBadge").textContent = `Mic: ${micNames[micIdx] || "Raw EM-91"}`;
  syncSlider("virtualDistanceSlider", "virtualDistanceVal", params.virtualDistanceCm ?? 15, (v) => `${Math.round(v)} cm`);

  const preampIdx = params.preampModel || 0;
  const preampNames = ["Clean Bypass", "Neve 1073", "API 312", "V76 Tube", "SSL 9000J"];
  document.querySelectorAll("#preampModelSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.preamp) === preampIdx);
  });
  el("preampModeBadge").textContent = preampNames[preampIdx] || "Clean Bypass";

  const eqIdx = params.consoleEqModel || 0;
  const eqNames = ["Bypass", "Pultec EQP-1A", "1073 Inductor", "SSL 4000G", "API 550A"];
  document.querySelectorAll("#consoleEqSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.eq) === eqIdx);
  });
  el("consoleEqBadge").textContent = `EQ: ${eqNames[eqIdx] || "Bypass"}`;

  syncSlider("preampDriveSlider", "preampDriveVal", params.preampDriveDb || 0, (v) => `${v.toFixed(1)} dB`);
  syncSlider("transformerIronSlider", "transformerIronVal", Math.round((params.transformerIronAmount ?? 0.5) * 100), (v) => `${Math.round(v)}%`);
  syncSlider("consoleLowSlider", "consoleLowVal", params.consoleLowDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("consoleMidSlider", "consoleMidVal", params.consoleMidDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);
  syncSlider("consoleAirSlider", "consoleAirVal", params.consoleAirDb || 0, (v) => `${v >= 0 ? "+" : ""}${v.toFixed(1)} dB`);

  abBypassState = !!params.abLevelMatchedBypass;
  lastAbMatchDb = telemetry.abMatchGainDb || 0;
  const abBtn = el("abBypassBtn");
  abBtn.classList.toggle("learning", abBypassState);
  abBtn.textContent = abBypassState ? `B: Raw (${lastAbMatchDb >= 0 ? "+" : ""}${lastAbMatchDb.toFixed(1)} dB)` : "A: Studio Chain";
  el("dacIspGuardToggle").checked = params.dacIspGuardEnabled !== false;
  el("dacDitherShaperToggle").checked = params.dacDitherShaperEnabled !== false;
  el("ultrasonicLpfToggle").checked = params.ultrasonicLpfEnabled !== false;
  el("driftAsrcToggle").checked = params.driftAsrcEnabled !== false;
  el("autopilotToggle").checked = params.autopilotEnabled !== false;
  el("pCoreToggle").checked = params.pCorePriority;
  el("xrunGuardToggle").checked = params.adaptiveXRunGuard;

  document.querySelectorAll("#humNotchSelector button").forEach((b) => {
    b.classList.toggle("active", Math.abs(Number(b.dataset.hum) - (params.humNotchFreqHz || 0)) < 1);
  });
  document.querySelectorAll("#engineSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.engine) === (params.engineMode || 0));
  });
  document.querySelectorAll("#irpSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.irp) === (params.ksIrpCount || 3));
    b.disabled = (params.engineMode || 0) !== 0;
  });
  document.querySelectorAll("#bufferSelector button").forEach((b) => {
    b.classList.toggle("active", Number(b.dataset.buf) === params.requestedBufferSize);
  });
  document.querySelectorAll("#rateSelector button").forEach((b) => {
    const bit = Number(b.dataset.bit);
    b.disabled = (telemetry.supportedSampleRatesMask & (1 << bit)) === 0;
    b.classList.toggle("active", Math.abs(Number(b.dataset.rate) - params.requestedSampleRate) < 1);
  });

  // Learn Room Noise & Preamp Auto-Level button feedback
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
  } else if (telemetry.learnState === 3) {
    learnBtn.classList.remove("learning");
    preampBtn.classList.remove("learning");
    if (Math.abs(telemetry.recommendedPgaDb || 0) > 0.1 || Math.abs(telemetry.recommendedTrimDb || 0) > 0.1) {
      preampBtn.textContent = `Levelled (PGA ${(telemetry.recommendedPgaDb || 0).toFixed(1)} dB)`;
    } else {
      preampBtn.textContent = "Auto-Level Preamp (3s)";
    }
    learnBtn.textContent = (telemetry.measuredNoiseFloorDb > -90)
      ? `Calibrated (Floor ${telemetry.measuredNoiseFloorDb.toFixed(0)} dB)`
      : "Learn Room Noise";
  } else {
    learnBtn.classList.remove("learning");
    preampBtn.classList.remove("learning");
    learnBtn.textContent = "Learn Room Noise";
    preampBtn.textContent = "Auto-Level Preamp (3s)";
  }

  // USB SOF frame sync note (1.000 ms integer alignment @ 48 kHz = 96 / 192 / 384...)
  const sr = Math.round(telemetry.activeSampleRate || params.requestedSampleRate || 48000);
  const buf = telemetry.activeBufferSize || params.requestedBufferSize || 96;
  const isUsdSync = (buf * 1000) % sr === 0;
  const syncHint = el("usbSyncHint");
  if (syncHint) {
    syncHint.style.display = isUsdSync ? "inline" : "none";
    syncHint.textContent = `(${((buf * 1000) / sr).toFixed(0)}.0 ms USB Sync)`;
  }

  // DAC True-Peak & ISP telemetry
  const tpDb = telemetry.outputTruePeakDb !== undefined ? telemetry.outputTruePeakDb : -96.0;
  const ispGr = telemetry.dacIspReductionDb || 0.0;
  el("dacTruePeakVal").textContent = `${tpDb.toFixed(1)} dBTP`;
  el("dacIspStatus").textContent = params.dacIspGuardEnabled === false
    ? "Raw Bit-Exact DAC Bypass"
    : (ispGr < -0.05 ? `BS.1770 ISP Active: ${ispGr.toFixed(1)} dB` : "BS.1770 ISP Guard (-0.5 dBTP)");

  // Live Telemetry
  el("deviceDot").classList.toggle("connected", telemetry.deviceConnected);
  const micTag = telemetry.captureActive ? " | Mic Active" : "";
  el("deviceStatusText").textContent = telemetry.deviceConnected
    ? `${telemetry.deviceName || "Mackie EM-91CU+"}${micTag}${telemetry.streamRunning ? "" : " | Open FL Studio to arm"}`
    : "Waiting for Mackie EM-91CU+";
  el("engineSubtitle").textContent = (telemetry.activeEngineMode === 0)
    ? "WDM-KS Direct Kernel Streaming | MMCSS Pro Audio | Zero audiodg"
    : "WASAPI Exclusive Event-Driven | MMCSS Pro Audio";
  el("formatBadge").textContent = `${telemetry.activeBitDepth}-bit${(telemetry.activeBitDepth === 16 && params.dacDitherShaperEnabled !== false) ? " + NS" : ""} | ${(telemetry.activeSampleRate / 1000).toFixed(1).replace(".0", "")} kHz`;
  el("latencyBreakdown").textContent = `In ${(telemetry.inputLatencyMs || 0).toFixed(1)} | Out ${(telemetry.outputLatencyMs || 0).toFixed(1)} ms`;
  el("latencyReadout").innerHTML = `${telemetry.roundTripLatencyMs.toFixed(1)} <small>ms</small>`;

  el("inputPeakText").textContent = `${telemetry.inputPeakDb.toFixed(1)} dBFS`;
  el("inputMeterFill").style.width = `${dbToPercent(telemetry.inputPeakDb)}%`;
  el("inputRmsMarker").style.left = `${dbToPercent(telemetry.inputRmsDb)}%`;

  el("gateGrText").textContent = `${telemetry.gateGainReductionDb.toFixed(1)} dB`;
  el("gateGrFill").style.width = `${dbToPercent(-telemetry.gateGainReductionDb, 0, 60)}%`;

  const gatePill = el("gateStatePill");
  if (!params.gateEnabled) {
    gatePill.textContent = "Bypassed";
    gatePill.className = "gate-state-pill";
  } else if (telemetry.gateIsOpen) {
    gatePill.textContent = "Open";
    gatePill.className = "gate-state-pill open";
  } else {
    gatePill.textContent = "Closed";
    gatePill.className = "gate-state-pill closed";
  }

  el("deEssChip").textContent = `De-Ess: ${(telemetry.deEsserReductionDb || 0).toFixed(1)} dB`;
  el("plosChip").textContent = `Pop: ${(telemetry.plosiveReductionDb || 0).toFixed(1)} dB`;
  el("fetChip").textContent = `1176 FET: ${(telemetry.fetReductionDb || 0).toFixed(1)} dB`;
  el("levChip").textContent = `LA-2A T4B: ${(telemetry.levelerReductionDb || 0).toFixed(1)} dB`;
  el("irpBadge").textContent = `Active IRPs: ${telemetry.activeIrpCount || 3}x`;
  const ppm = telemetry.clockDriftPpm || 0;
  el("driftChip").textContent = `Drift: ${ppm >= 0 ? "+" : ""}${ppm.toFixed(1)} ppm`;
  el("cpuChip").textContent = `Audio CPU: ${telemetry.callbackCpuPercent.toFixed(1)}%`;
  el("jitterChip").textContent = `Jitter: ${Math.round(telemetry.lastJitterUs)} / ${Math.round(telemetry.maxJitterUs || 0)} us`;
  el("xrunChip").textContent = `XRuns: ${telemetry.xrunCount}`;

  drawJitterSparkline(telemetry.jitterHistoryUs);
  drawGateVisualizer(telemetry.inputPeakDb, params.gateThresholdDb, params.gateHysteresisDb, params.gateEnabled, telemetry.gateIsOpen);
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
        inputGainDb: 6.0,
        inputMute: false,
        softClipEnabled: true,
        audioHpfEnabled: true,
        directMonitorMix: 0.25,
        hardwarePgaGainDb: -6.0,
        hardwareDacGainDb: -6.0,
        hardwareDacBalanceDb: 0.0,
        autoPadOnClipEnabled: false,
        gateEnabled: true,
        gateLookAheadEnabled: false,
        gateThresholdDb: -42.0,
        gateHysteresisDb: 4.0,
        gateAttackMs: 1.5,
        gateHoldMs: 60.0,
        gateReleaseMs: 140.0,
        gateRangeDb: -60.0,
        gateSidechainHpfHz: 90.0,
        humNotchFreqHz: 60.0,
        capsuleEqEnabled: true,
        deEsserAmount: 0.35,
        plosiveGuardEnabled: true,
        vocalLevelerAmount: 0.45,
        dualTrackSafetyMode: false,
        ultrasonicLpfEnabled: true,
        driftAsrcEnabled: true,
        dacIspGuardEnabled: true,
        dacDitherShaperEnabled: true,
        engineMode: 0,
        ksIrpCount: 3,
        autopilotEnabled: true,
        requestedBufferSize: 96,
        requestedSampleRate: 48000,
        pCorePriority: true,
        adaptiveXRunGuard: true,
        themeMode: mockMode === "light" ? 2 : 1,
      },
      telemetry: {
        deviceConnected: true,
        captureActive: true,
        streamRunning: true,
        activeEngineMode: 0,
        activeIrpCount: 3,
        activeBufferSize: 96,
        activeSampleRate: 48000,
        activeBitDepth: 24,
        inputLatencyMs: 2.0,
        outputLatencyMs: 6.0,
        roundTripLatencyMs: 8.0,
        hasHardwarePga: true,
        hardwarePgaMinDb: -63.5,
        hardwarePgaMaxDb: 0.0,
        hasHardwareDac: true,
        hardwareDacMinDb: -63.5,
        hardwareDacMaxDb: 0.0,
        hardwareMuted: false,
        captureMonoActive: false,
        supportsWaveRt: false,
        inputPeakDb: -18.5,
        inputRmsDb: -26.0,
        outputPeakDb: -12.4,
        outputTruePeakDb: -11.9,
        dacIspReductionDb: 0.0,
        clockDriftPpm: 4.2,
        preampCalibActive: false,
        recommendedPgaDb: -6.0,
        recommendedTrimDb: 0.0,
        gateGainReductionDb: 0.0,
        gateIsOpen: true,
        deEsserReductionDb: -2.4,
        plosiveReductionDb: -0.8,
        levelerReductionDb: -1.5,
        adcClipDetected: false,
        learnState: 3,
        learnProgress: 1.0,
        measuredNoiseFloorDb: -58.0,
        callbackCpuPercent: 14.2,
        lastJitterUs: 48,
        maxJitterUs: 215,
        xrunCount: 0,
        supportedSampleRatesMask: 0b01011,
        deviceName: "Mackie EM-91CU+ (WDM-KS Direct)",
        windowsDark: mockMode === "dark",
        jitterHistoryUs: [32, 48, 41, 65, 39, 52, 44, 38, 70, 45, 51, 42, 36, 48, 55, 40],
      },
    });
  }
}
