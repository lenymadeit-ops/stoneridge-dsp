const postMsg = (payload) => {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(payload);
  }
};

const postParam = (key, value) => postMsg({ action: "setParam", key, value });
const el = (id) => document.getElementById(id);

const PRESETS = {
  raw: {
    inputGainDb: 0.0,
    softClipEnabled: false,
    audioHpfEnabled: false,
    gateEnabled: false,
    gateLookAheadEnabled: false,
    humNotchFreqHz: 0,
    capsuleEqEnabled: false,
    deEsserAmount: 0.0,
    plosiveGuardEnabled: false,
    vocalLevelerAmount: 0.0,
  },
  podcast: {
    inputGainDb: 6.0,
    softClipEnabled: true,
    audioHpfEnabled: true,
    gateEnabled: true,
    gateLookAheadEnabled: false,
    gateThresholdDb: -44,
    gateRangeDb: -50,
    gateAttackMs: 2.0,
    gateHoldMs: 90,
    gateReleaseMs: 180,
    gateSidechainHpfHz: 85,
    capsuleEqEnabled: true,
    deEsserAmount: 0.35,
    plosiveGuardEnabled: true,
    vocalLevelerAmount: 0.40,
  },
  tight: {
    inputGainDb: 4.0,
    softClipEnabled: true,
    audioHpfEnabled: true,
    gateEnabled: true,
    gateLookAheadEnabled: true,
    gateThresholdDb: -34,
    gateRangeDb: -70,
    gateAttackMs: 0.8,
    gateHoldMs: 40,
    gateReleaseMs: 90,
    gateSidechainHpfHz: 110,
    capsuleEqEnabled: false,
    deEsserAmount: 0.20,
    plosiveGuardEnabled: true,
    vocalLevelerAmount: 0.0,
  },
  studio: {
    inputGainDb: 8.0,
    softClipEnabled: true,
    audioHpfEnabled: true,
    gateEnabled: true,
    gateLookAheadEnabled: true,
    gateThresholdDb: -46,
    gateRangeDb: -45,
    gateAttackMs: 1.5,
    gateHoldMs: 110,
    gateReleaseMs: 220,
    gateSidechainHpfHz: 90,
    capsuleEqEnabled: true,
    deEsserAmount: 0.45,
    plosiveGuardEnabled: true,
    vocalLevelerAmount: 0.55,
  },
};

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

document.querySelectorAll("#presetPills button").forEach((btn) => {
  btn.addEventListener("click", () => {
    const p = PRESETS[btn.dataset.preset];
    if (!p) return;
    Object.entries(p).forEach(([k, v]) => postParam(k, v));
  });
});

function applyStateMessage(msg) {
  if (!msg || msg.type !== "state") return;
  const { params, telemetry } = msg;

  applyTheme(params.themeMode, telemetry.windowsDark);

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
    ? `${telemetry.deviceName || "Mackie EM-91CU+"}${micTag}`
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
