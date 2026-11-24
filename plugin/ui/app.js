// Stoneridge Vocal Chain v7.1.0 Interactive Sonore UI (Strict 7-bit ASCII, 0% Idle CPU)
(function (root) {
  'use strict';

  const PARAM_IDS = ['input', 'trim', 'fet', 'ratio', 'la2a', 'beauty', 'low', 'air', 'preset'];
  const KNOB_IDS = ['input', 'trim', 'fet', 'ratio', 'la2a', 'beauty', 'low', 'air'];
  const RATIO_LABELS = ['4:1', '8:1', 'British'];

  const PRODUCER_PRESETS = {
    0: {
      name: 'INIT | Default Patch',
      tag: 'Flat Linear',
      mic: 'Flat Reference',
      preamp: 'Transparent Gain',
      eq: 'WDF Bypass / Flat',
      deEss: 'Off',
      values: { input: 0, trim: 0, fet: 0, ratio: 0, la2a: 0, beauty: 0, low: 0, air: 0, preset: 0 }
    },
    1: {
      name: 'Modern Pop Gloss',
      tag: 'K67 | Neve | Pultec',
      mic: 'U67 Tube (9.5 kHz BAOAB)',
      preamp: 'Neve 1073 Class-A (+4 dB)',
      eq: 'WDF Pultec EQP-1A',
      deEss: '40% + Plosive Guard',
      values: { input: 4 / 12, trim: 2.0, fet: 0.35, ratio: 0, la2a: 0.45, beauty: 0.55, low: 2.0, air: 3.0, preset: 1 }
    },
    2: {
      name: 'Warm R&B Velvet',
      tag: 'M7 | V76 | LA-2A',
      mic: 'U47 M7 Capsule (7.2 kHz)',
      preamp: 'Telefunken V76 (+5 dB)',
      eq: 'API / Warm Shelf',
      deEss: '25% Gentle De-Ess',
      values: { input: 5 / 12, trim: 1.5, fet: 0.15, ratio: 0, la2a: 0.60, beauty: 0.40, low: 2.5, air: 1.0, preset: 2 }
    },
    3: {
      name: 'Punchy Rap Authority',
      tag: 'SM7B | API | British',
      mic: 'SM7B Dynamic (4.8 kHz)',
      preamp: 'API 312 Punch (+5 dB)',
      eq: 'Console Tight Presence',
      deEss: '50% + Plosive Guard',
      values: { input: 5 / 12, trim: 3.0, fet: 0.70, ratio: 2, la2a: 0.20, beauty: 0.25, low: 1.0, air: 0.5, preset: 3 }
    },
    4: {
      name: 'Intimate Acoustic',
      tag: 'CK12 | V76 | Soft Opto',
      mic: 'C12 CK12 Brass (11.2 kHz)',
      preamp: 'Telefunken V76 (+2 dB)',
      eq: 'WDF Pultec Gentle Sheen',
      deEss: '15% Light Control',
      values: { input: 2 / 12, trim: 1.0, fet: 0.10, ratio: 0, la2a: 0.30, beauty: 0.20, low: 0.5, air: 1.5, preset: 4 }
    },
    5: {
      name: 'Podcast Broadcast',
      tag: 'K87 | Solid | Tight',
      mic: 'U87 K87 Studio (8.5 kHz)',
      preamp: 'Solid State Crisp (+1 dB)',
      eq: 'Cut Mud (-1 dB Low)',
      deEss: '45% + Plosive Guard',
      values: { input: 1 / 12, trim: 4.0, fet: 0.20, ratio: 0, la2a: 0.50, beauty: 0.15, low: -1.0, air: 0.5, preset: 5 }
    },
    6: {
      name: 'Raw Reference',
      tag: 'Flat Transparent',
      mic: 'Flat Reference (Bypassed)',
      preamp: 'Unity Wire (0 dB)',
      eq: 'Flat Unity Bypass',
      deEss: 'Off',
      values: { input: 0, trim: 0, fet: 0, ratio: 0, la2a: 0, beauty: 0, low: 0, air: 0, preset: 6 }
    }
  };

  const STAGE_META = {
    shield: {
      num: 1,
      title: 'Subsonic Shield & ADAA1 Guard',
      desc: '80 Hz rumble HPF, adaptive 50/60 Hz mains notch, plosive guard, and zero-overshoot polynomial ADAA clipper before oversampling.',
      focusParam: null,
      group: 'preamp'
    },
    mic: {
      num: 2,
      title: 'Mic Locker (BAOAB Capsule + ADAA2)',
      desc: 'Stokes-Einstein BAOAB physical diaphragm resonator with 4-tap headbasket diffraction and restoring-torque molecular ADAA2 saturation in the 192 kHz domain.',
      focusParam: 'input',
      group: 'preamp'
    },
    preamp: {
      num: 3,
      title: 'Class-A Preamp & Transformer Iron',
      desc: 'Series Jiles-Atherton magnetic core hysteresis coupled with Koren triode grid bias recovery and 192 kHz domain drive staging.',
      focusParam: 'input',
      group: 'preamp'
    },
    gate: {
      num: 4,
      title: 'Look-Ahead Gate & Dynamic De-Esser',
      desc: '1 ms look-ahead noise gate and 6.5 kHz split-band sibilance attenuation engaged dynamically by producer voicings or ASIO chain sync.',
      focusParam: null,
      group: 'fet'
    },
    fet: {
      num: 5,
      title: '1176 JFET Peak Limiter',
      desc: 'Shichman-Hodges JFET triode shunt nonlinearity catching fast vocal transients across 4:1, 8:1, and British All-Buttons-In modes.',
      focusParam: 'fet',
      group: 'fet'
    },
    la2a: {
      num: 6,
      title: 'LA-2A Continuous-Time Gray-Box Opto',
      desc: 'Sample-accurate R37 + CdS optical cell ODE paired with 8-state continuous-time state-space residual and HA-100X transformer model.',
      focusParam: 'la2a',
      group: 'fet'
    },
    eq: {
      num: 7,
      title: 'WDF Passive Pultec & Air EQ',
      desc: 'Wave Digital Filter 92 Hz boost + 245 Hz cut passive bridge paired with 12 kHz WDF RLC series tank for non-additive analog weight and air.',
      focusParam: 'low',
      group: 'eq'
    },
    beauty: {
      num: 8,
      title: 'Psychoacoustic Beauty & 6-Sample ISP Guard',
      desc: 'ISO 226 loudness contour, Hilbert quadrature 2f warmth, 3-band CTKEO presence articulation, NY glue, Trim, and BS.1770-4 True-Peak limiter.',
      focusParam: 'beauty',
      group: 'eq'
    }
  };

  function createMockBridge() {
    const defs = {
      input:  { id: 'input',  label: 'Input',  unit: 'dB', min: 0,   max: 1,  default: 0, step: 0 },
      trim:   { id: 'trim',   label: 'Trim',   unit: 'dB', min: -12, max: 12, default: 0, step: 0 },
      fet:    { id: 'fet',    label: '1176',   unit: '%',  min: 0,   max: 1,  default: 0, step: 0 },
      ratio:  { id: 'ratio',  label: 'Ratio',  unit: '',   min: 0,   max: 2,  default: 0, step: 3, names: RATIO_LABELS },
      la2a:   { id: 'la2a',   label: 'LA-2A',  unit: '%',  min: 0,   max: 1,  default: 0, step: 0 },
      beauty: { id: 'beauty', label: 'Beauty', unit: '%',  min: 0,   max: 1,  default: 0, step: 0 },
      low:    { id: 'low',    label: 'Low',    unit: 'dB', min: -6,  max: 6,  default: 0, step: 0 },
      air:    { id: 'air',    label: 'Air',    unit: 'dB', min: -6,  max: 8,  default: 0, step: 0 },
      preset: { id: 'preset', label: 'Preset', unit: '',   min: 0,   max: 6,  default: 0, step: 0 }
    };
    const vals = {};
    PARAM_IDS.forEach((id) => { vals[id] = defs[id].default; });
    const changeCbs = [];
    const presetNames = [
      'INIT | Default Patch',
      'Modern Pop Gloss',
      'Warm R&B Velvet',
      'Punchy Rap Authority',
      'Intimate Acoustic',
      'Podcast Broadcast',
      'Raw Reference',
      'Live ASIO Tracking Chain'
    ];

    function emit(idx, val) {
      const snap = Object.assign({}, vals);
      changeCbs.forEach((cb) => {
        try { cb(snap, idx, val); } catch (err) {}
      });
    }

    return {
      name: 'Stoneridge Vocal Chain (Preview)',
      vendor: 'Stoneridge',
      params: PARAM_IDS.map((id, i) => Object.assign({ __i: i, value: vals[id] }, defs[id])),
      presets: presetNames,
      get(r) {
        return Object.prototype.hasOwnProperty.call(vals, r) ? vals[r] : undefined;
      },
      set(r, v) {
        if (!Object.prototype.hasOwnProperty.call(defs, r)) return;
        const d = defs[r];
        let clamped = Math.max(d.min, Math.min(d.max, Number(v) || 0));
        if (d.step > 1) {
          clamped = Math.round(clamped);
        }
        vals[r] = clamped;
        const idx = PARAM_IDS.indexOf(r);
        this.params[idx].value = clamped;
        emit(idx, clamped);
      },
      begin() {},
      end() {},
      on(cb) { if (typeof cb === 'function') changeCbs.push(cb); },
      onLevel() {},
      onGR() {},
      onSpectrum() {},
      contextMenu() {},
      loadPreset(i) {
        if (i === 7) {
          Object.keys(PRODUCER_PRESETS[0].values).forEach((k) => this.set(k, PRODUCER_PRESETS[0].values[k]));
          this.set('preset', 0.75);
          return;
        }
        const entry = PRODUCER_PRESETS[i] || PRODUCER_PRESETS[0];
        Object.keys(entry.values).forEach((k) => this.set(k, entry.values[k]));
      }
    };
  }

  function boot() {
    const bridge = root.sonore || createMockBridge();
    const state = {
      values: { input: 0, trim: 0, fet: 0, ratio: 0, la2a: 0, beauty: 0, low: 0, air: 0, preset: 0 },
      selectedStage: 'preamp',
      eco: false,
      theme: 'dark',
      activeSlot: 'A',
      slots: {
        A: { input: 0, trim: 0, fet: 0, ratio: 0, la2a: 0, beauty: 0, low: 0, air: 0, preset: 0 },
        B: { input: 0, trim: 0, fet: 0, ratio: 0, la2a: 0, beauty: 0, low: 0, air: 0, preset: 0 }
      },
      lastMeterPaintMs: 0,
      toastTimer: null,
      curveDirty: false
    };

    const el = {
      ratePill: document.getElementById('ratePill'),
      presetReadout: document.getElementById('presetReadout'),
      chainStatusMeta: document.getElementById('chainStatusMeta'),
      slotABtn: document.getElementById('slotABtn'),
      slotBBtn: document.getElementById('slotBBtn'),
      liveChainBtn: document.getElementById('liveChainBtn'),
      ecoBtn: document.getElementById('ecoBtn'),
      ecoBtnText: document.getElementById('ecoBtnText'),
      ecoOledBadge: document.getElementById('ecoOledBadge'),
      presetMenuBtn: document.getElementById('presetMenuBtn'),
      clearPatchBtn: document.getElementById('clearPatchBtn'),
      themeBtn: document.getElementById('themeBtn'),
      flowStrip: document.getElementById('flowStrip'),
      presetsGrid: document.getElementById('presetsGrid'),
      ratioGroup: document.getElementById('ratioGroup'),
      curveCanvas: document.getElementById('curveCanvas'),
      meterOutDb: document.getElementById('meterOutDb'),
      meterGrDb: document.getElementById('meterGrDb'),
      outBarFill: document.getElementById('outBarFill'),
      grBarFill: document.getElementById('grBarFill'),
      outValLabel: document.getElementById('outValLabel'),
      grValLabel: document.getElementById('grValLabel'),
      inspKicker: document.getElementById('inspKicker'),
      inspTitle: document.getElementById('inspTitle'),
      inspDesc: document.getElementById('inspDesc'),
      inspVoicingChip: document.getElementById('inspVoicingChip'),
      inspParamChip: document.getElementById('inspParamChip'),
      inspResetStageBtn: document.getElementById('inspResetStageBtn'),
      vcToast: document.getElementById('vcToast')
    };

    const sliders = {};
    const valLabels = {};
    ['input', 'trim', 'fet', 'la2a', 'beauty', 'low', 'air'].forEach((id) => {
      sliders[id] = document.getElementById('ctrl_' + id);
      valLabels[id] = document.getElementById('val_' + id);
    });

    function showToast(msg) {
      if (!el.vcToast) return;
      el.vcToast.textContent = msg;
      el.vcToast.hidden = false;
      if (state.toastTimer) clearTimeout(state.toastTimer);
      state.toastTimer = setTimeout(() => {
        el.vcToast.hidden = true;
      }, 1900);
    }

    function applyTheme(theme) {
      state.theme = (theme === 'light') ? 'light' : 'dark';
      document.documentElement.setAttribute('data-theme', state.theme);
      if (el.themeBtn) {
        el.themeBtn.textContent = (state.theme === 'dark') ? 'Light' : 'Dark';
      }
      try {
        localStorage.setItem('sr_vocalchain_theme', state.theme);
      } catch (e) {}
      scheduleCurveDraw();
    }

    function applyEco(enabled, announce) {
      state.eco = !!enabled;
      document.body.classList.toggle('eco-mode', state.eco);
      if (el.ecoBtn) {
        el.ecoBtn.classList.toggle('active', state.eco);
        el.ecoBtn.setAttribute('aria-pressed', state.eco ? 'true' : 'false');
      }
      if (el.ecoBtnText) {
        el.ecoBtnText.textContent = state.eco ? 'ECO: ON' : 'ECO';
      }
      if (el.ecoOledBadge) {
        el.ecoOledBadge.hidden = !state.eco;
      }
      if (announce) {
        showToast(state.eco ? 'ECO SAVER ON (Reduced UI Rate)' : 'ECO SAVER OFF (Full Precision UI)');
      }
      scheduleCurveDraw();
    }

    function syncFromBridge(valuesMap) {
      PARAM_IDS.forEach((id) => {
        let v = valuesMap && Object.prototype.hasOwnProperty.call(valuesMap, id)
          ? Number(valuesMap[id])
          : Number(bridge.get(id));
        if (!Number.isFinite(v)) v = 0;
        state.values[id] = v;
      });
      state.slots[state.activeSlot] = Object.assign({}, state.values);
      renderControls();
      renderPresetAndFlowStatus();
      renderInspector();
      scheduleCurveDraw();
    }

    function writePatch(nextValues) {
      PARAM_IDS.forEach((id) => {
        const target = Object.prototype.hasOwnProperty.call(nextValues, id) ? Number(nextValues[id]) : 0;
        bridge.begin(id);
        bridge.set(id, target);
        bridge.end(id);
      });
    }

    function setSingleParam(id, val) {
      bridge.begin(id);
      bridge.set(id, val);
      bridge.end(id);
    }

    function formatDb(v) {
      const n = Math.abs(v) < 0.05 ? 0 : v;
      return (n > 0 ? '+' : '') + n.toFixed(1) + ' dB';
    }

    function formatPct(v) {
      return Math.round(Math.max(0, Math.min(1, v)) * 100) + '%';
    }

    function isPresetEdited(idx) {
      const entry = PRODUCER_PRESETS[idx];
      if (!entry) return false;
      for (let i = 0; i < KNOB_IDS.length; i += 1) {
        const id = KNOB_IDS[i];
        if (Math.abs((state.values[id] || 0) - (entry.values[id] || 0)) > 0.025) {
          return true;
        }
      }
      return false;
    }

    function getPresetMode() {
      const pv = state.values.preset || 0;
      if (pv >= 0.5 && pv < 1.0) return { mode: 'live', idx: -1 };
      const idx = Math.floor(pv + 0.001);
      if (idx >= 1 && idx <= 6) return { mode: 'preset', idx };
      return { mode: 'init', idx: 0 };
    }

    function renderControls() {
      const v = state.values;
      if (sliders.input) sliders.input.value = String(v.input);
      if (valLabels.input) valLabels.input.textContent = formatPct(v.input) + ' (' + formatDb(v.input * 12) + ')';

      if (sliders.trim) sliders.trim.value = String(v.trim);
      if (valLabels.trim) valLabels.trim.textContent = formatDb(v.trim);

      if (sliders.fet) sliders.fet.value = String(v.fet);
      if (valLabels.fet) valLabels.fet.textContent = formatPct(v.fet);

      if (sliders.la2a) sliders.la2a.value = String(v.la2a);
      if (valLabels.la2a) valLabels.la2a.textContent = formatPct(v.la2a);

      if (sliders.beauty) sliders.beauty.value = String(v.beauty);
      if (valLabels.beauty) valLabels.beauty.textContent = formatPct(v.beauty);

      if (sliders.low) sliders.low.value = String(v.low);
      if (valLabels.low) valLabels.low.textContent = formatDb(v.low);

      if (sliders.air) sliders.air.value = String(v.air);
      if (valLabels.air) valLabels.air.textContent = formatDb(v.air);

      const ratioIdx = Math.max(0, Math.min(2, Math.round(v.ratio || 0)));
      if (el.ratioGroup) {
        el.ratioGroup.querySelectorAll('[data-ratio]').forEach((btn) => {
          btn.classList.toggle('active', Number(btn.dataset.ratio) === ratioIdx);
        });
      }
    }

    function renderPresetAndFlowStatus() {
      const pm = getPresetMode();
      const v = state.values;
      const allManualZero = KNOB_IDS.every((id) => Math.abs(v[id] || 0) < 0.005);

      if (el.liveChainBtn) {
        el.liveChainBtn.classList.toggle('active', pm.mode === 'live');
      }

      if (el.presetsGrid) {
        el.presetsGrid.querySelectorAll('[data-preset]').forEach((btn) => {
          btn.classList.toggle('active', pm.mode === 'preset' && Number(btn.dataset.preset) === pm.idx);
        });
      }

      if (el.presetReadout) {
        if (pm.mode === 'live') {
          el.presetReadout.textContent = 'LIVE ASIO CHAIN | Recalled live_tracking_chain.json';
        } else if (pm.mode === 'preset') {
          const entry = PRODUCER_PRESETS[pm.idx];
          const suffix = isPresetEdited(pm.idx) ? ' (Edited)' : '';
          el.presetReadout.textContent = entry.name + suffix + ' | ' + entry.tag;
        } else if (allManualZero) {
          el.presetReadout.textContent = 'INIT | Default Patch';
        } else {
          el.presetReadout.textContent = 'Custom Manual Chain | Analog Rack Active';
        }
      }

      if (el.chainStatusMeta) {
        el.chainStatusMeta.textContent = (pm.mode === 'live')
          ? 'Source: AppData\\StoneridgeDSP\\live_tracking_chain.json | 6-smp ISP'
          : 'Preset ' + (pm.mode === 'preset' ? pm.idx : 0) + ' | 6-smp BS.1770-4 ISP Guard';
      }

      const stageActive = {
        shield: true,
        mic: pm.mode === 'live' || (pm.mode === 'preset' && pm.idx !== 6),
        preamp: pm.mode === 'live' || v.input > 0.01 || (pm.mode === 'preset' && pm.idx !== 6),
        gate: pm.mode === 'live' || (pm.mode === 'preset' && pm.idx !== 6),
        fet: pm.mode === 'live' || v.fet > 0.01,
        la2a: pm.mode === 'live' || v.la2a > 0.01,
        eq: pm.mode === 'live' || Math.abs(v.low) > 0.05 || Math.abs(v.air) > 0.05,
        beauty: pm.mode === 'live' || v.beauty > 0.01 || Math.abs(v.trim) > 0.05
      };

      if (el.flowStrip) {
        el.flowStrip.querySelectorAll('.vc-stage-pill').forEach((pill) => {
          const st = pill.dataset.stage;
          pill.classList.toggle('active', st === state.selectedStage);
          pill.classList.toggle('stage-on', !!stageActive[st]);
        });
      }

      if (el.ratePill && bridge.sampleRate > 0) {
        el.ratePill.textContent = Math.round(bridge.sampleRate / 1000) + 'k -> 192k';
      }
    }

    function renderInspector() {
      const meta = STAGE_META[state.selectedStage] || STAGE_META.preamp;
      const pm = getPresetMode();
      const presetInfo = (pm.mode === 'live')
        ? { mic: 'ASIO Tracking Snapshot', preamp: 'ASIO Tracking Chain', eq: 'ASIO Tracking EQ', deEss: 'ASIO De-Ess' }
        : (PRODUCER_PRESETS[pm.idx] || PRODUCER_PRESETS[0]);

      if (el.inspKicker) el.inspKicker.textContent = 'STAGE ' + meta.num + ' INSPECTOR';
      if (el.inspTitle) el.inspTitle.textContent = meta.title;
      if (el.inspDesc) el.inspDesc.textContent = meta.desc;

      let voicing = 'Active Voicing: Custom Manual';
      if (state.selectedStage === 'mic') voicing = 'Capsule: ' + presetInfo.mic;
      else if (state.selectedStage === 'preamp') voicing = 'Preamp: ' + presetInfo.preamp;
      else if (state.selectedStage === 'gate') voicing = 'Sibilance: ' + presetInfo.deEss;
      else if (state.selectedStage === 'eq') voicing = 'Console EQ: ' + presetInfo.eq;
      else if (state.selectedStage === 'fet') voicing = '1176 Ratio: ' + (RATIO_LABELS[Math.round(state.values.ratio || 0)] || '4:1');
      else if (state.selectedStage === 'la2a') voicing = 'LA-2A Leveler: ' + formatPct(state.values.la2a);
      else if (state.selectedStage === 'beauty') voicing = 'Beauty + Trim: ' + formatPct(state.values.beauty) + ' / ' + formatDb(state.values.trim);
      else voicing = 'Shield: 80 Hz HPF + ADAA1 Soft-Clip';

      if (el.inspVoicingChip) el.inspVoicingChip.textContent = voicing;
      if (el.inspParamChip) {
        if (meta.focusParam === 'input') {
          el.inspParamChip.textContent = 'Param: Input ' + formatPct(state.values.input);
        } else if (meta.focusParam === 'fet') {
          el.inspParamChip.textContent = 'Param: 1176 ' + formatPct(state.values.fet) + ' (' + (RATIO_LABELS[Math.round(state.values.ratio || 0)] || '4:1') + ')';
        } else if (meta.focusParam === 'la2a') {
          el.inspParamChip.textContent = 'Param: LA-2A ' + formatPct(state.values.la2a);
        } else if (meta.focusParam === 'low') {
          el.inspParamChip.textContent = 'Params: Low ' + formatDb(state.values.low) + ' | Air ' + formatDb(state.values.air);
        } else if (meta.focusParam === 'beauty') {
          el.inspParamChip.textContent = 'Params: Beauty ' + formatPct(state.values.beauty) + ' | Trim ' + formatDb(state.values.trim);
        } else {
          el.inspParamChip.textContent = 'Automatic / Preset Linked';
        }
      }

      document.querySelectorAll('.vc-rack-card').forEach((card) => {
        card.classList.toggle('stage-highlight', card.dataset.stageGroup === meta.group);
      });
      document.querySelectorAll('.vc-control').forEach((ctrl) => {
        const s = ctrl.dataset.stage;
        const match = (s === state.selectedStage)
          || (state.selectedStage === 'eq' && s === 'eq')
          || (state.selectedStage === 'beauty' && s === 'beauty');
        ctrl.classList.toggle('insp-focus', match);
      });
    }

    function evalShelfResponseDb(freqHz, fcHz, gainDb, highShelf) {
      if (Math.abs(gainDb) < 0.02) return 0;
      const ratio = freqHz / fcHz;
      const weight = highShelf
        ? (ratio * ratio) / (1 + ratio * ratio)
        : 1 / (1 + ratio * ratio);
      return gainDb * weight;
    }

    function evalCompositeDb(freqHz) {
      const v = state.values;
      // 1. WDF Pultec Low (92 Hz boost/cut + ~245 Hz asymmetric attenuation dip on boost)
      let db = evalShelfResponseDb(freqHz, 92, v.low || 0, false);
      if ((v.low || 0) > 0.2) {
        const dipCenter = Math.log2(freqHz / 245);
        db -= (v.low * 0.22) * Math.exp(-dipCenter * dipCenter * 1.4);
      }
      // 2. WDF Air (12 kHz resonant RLC shelf)
      db += evalShelfResponseDb(freqHz, 12000, v.air || 0, true);
      // 3. Beauty ISO 226 gentle smiling contour + CTKEO upper-mid clarity lift
      const b = v.beauty || 0;
      if (b > 0.01) {
        db += evalShelfResponseDb(freqHz, 110, b * 2.2, false);
        const presDist = Math.log2(freqHz / 3600);
        db += (b * 1.8) * Math.exp(-presDist * presDist * 1.8);
        db += evalShelfResponseDb(freqHz, 9500, b * 1.6, true);
      }
      // 4. Preamp warm saturation tilt + Output trim
      db += ((v.input || 0) * 1.4) * (1 / (1 + Math.pow(freqHz / 280, 2)));
      db += (v.trim || 0);
      return Math.max(-12, Math.min(12, db));
    }

    function scheduleCurveDraw() {
      if (state.curveDirty) return;
      state.curveDirty = true;
      root.requestAnimationFrame(() => {
        state.curveDirty = false;
        drawCurveCanvas();
      });
    }

    function drawCurveCanvas() {
      const canvas = el.curveCanvas;
      if (!canvas) return;
      const rect = canvas.getBoundingClientRect();
      const dpr = Math.min(root.devicePixelRatio || 1, 2);
      const w = Math.max(240, Math.floor((rect.width || 560) * dpr));
      const h = Math.max(120, Math.floor((rect.height || 170) * dpr));
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
      }

      const ctx = canvas.getContext('2d');
      if (!ctx) return;

      const styles = getComputedStyle(document.documentElement);
      const lineColor = styles.getPropertyValue('--sr-line').trim() || 'rgba(255,255,255,0.12)';
      const mutedColor = styles.getPropertyValue('--sr-muted').trim() || '#98989f';
      const accentColor = styles.getPropertyValue('--sr-accent').trim() || '#0a84ff';
      const accentSoft = styles.getPropertyValue('--sr-accent-soft').trim() || 'rgba(10,132,255,0.18)';

      ctx.clearRect(0, 0, w, h);

      const padL = 34 * dpr;
      const padR = 12 * dpr;
      const padT = 14 * dpr;
      const padB = 20 * dpr;
      const plotW = Math.max(10, w - padL - padR);
      const plotH = Math.max(10, h - padT - padB);
      const fMin = 20;
      const fMax = 20000;
      const logSpan = Math.log10(fMax / fMin);

      function freqToX(f) {
        return padL + (Math.log10(f / fMin) / logSpan) * plotW;
      }
      function dbToY(db) {
        return padT + ((12 - db) / 24) * plotH;
      }

      ctx.lineWidth = 1 * dpr;
      ctx.font = Math.round(9.5 * dpr) + 'px "SF Mono", Consolas, monospace';

      // Horizontal dB gridlines
      [-6, 0, 6].forEach((db) => {
        const y = dbToY(db);
        ctx.strokeStyle = (db === 0) ? mutedColor : lineColor;
        ctx.globalAlpha = (db === 0) ? 0.35 : 0.7;
        ctx.beginPath();
        ctx.moveTo(padL, y);
        ctx.lineTo(padL + plotW, y);
        ctx.stroke();
        ctx.globalAlpha = 1;
        ctx.fillStyle = mutedColor;
        ctx.fillText((db > 0 ? '+' : '') + db, 6 * dpr, y + 3 * dpr);
      });

      // Vertical log frequency markers
      [50, 100, 500, 1000, 5000, 10000].forEach((f) => {
        const x = freqToX(f);
        ctx.strokeStyle = lineColor;
        ctx.beginPath();
        ctx.moveTo(x, padT);
        ctx.lineTo(x, padT + plotH);
        ctx.stroke();
        ctx.fillStyle = mutedColor;
        const lbl = f >= 1000 ? (f / 1000) + 'k' : String(f);
        ctx.fillText(lbl, x - 8 * dpr, h - 5 * dpr);
      });

      // Composite frequency curve (zero shadowBlur)
      const steps = 128;
      ctx.beginPath();
      for (let i = 0; i <= steps; i += 1) {
        const f = fMin * Math.pow(fMax / fMin, i / steps);
        const x = freqToX(f);
        const y = dbToY(evalCompositeDb(f));
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }

      ctx.strokeStyle = accentColor;
      ctx.lineWidth = 2.2 * dpr;
      ctx.stroke();

      ctx.lineTo(padL + plotW, dbToY(0));
      ctx.lineTo(padL, dbToY(0));
      ctx.closePath();
      ctx.fillStyle = accentSoft;
      ctx.fill();
    }

    function openPresetPopover() {
      const existing = document.querySelector('.sr-custom-select-menu');
      if (existing) {
        existing.remove();
        return;
      }
      if (!el.presetMenuBtn) return;

      const menu = document.createElement('div');
      menu.className = 'sr-custom-select-menu';
      const rect = el.presetMenuBtn.getBoundingClientRect();
      menu.style.top = (rect.bottom + 6) + 'px';
      menu.style.left = Math.max(8, Math.min(root.innerWidth - 220, rect.right - 210)) + 'px';

      const pm = getPresetMode();
      const items = [
        { label: 'INIT | Default Patch', action: () => writePatch(PRODUCER_PRESETS[0].values), active: pm.mode === 'init' },
        { label: 'Modern Pop Gloss', action: () => applyProducerPreset(1), active: pm.mode === 'preset' && pm.idx === 1 },
        { label: 'Warm R&B Velvet', action: () => applyProducerPreset(2), active: pm.mode === 'preset' && pm.idx === 2 },
        { label: 'Punchy Rap Authority', action: () => applyProducerPreset(3), active: pm.mode === 'preset' && pm.idx === 3 },
        { label: 'Intimate Acoustic', action: () => applyProducerPreset(4), active: pm.mode === 'preset' && pm.idx === 4 },
        { label: 'Podcast Broadcast', action: () => applyProducerPreset(5), active: pm.mode === 'preset' && pm.idx === 5 },
        { label: 'Raw Reference', action: () => applyProducerPreset(6), active: pm.mode === 'preset' && pm.idx === 6 },
        { label: 'Live ASIO Tracking Chain', action: () => toggleLiveChain(), active: pm.mode === 'live' }
      ];

      items.forEach((it) => {
        const b = document.createElement('button');
        b.type = 'button';
        b.textContent = it.label;
        if (it.active) b.classList.add('active');
        b.addEventListener('click', (e) => {
          e.stopPropagation();
          menu.remove();
          it.action();
        });
        menu.appendChild(b);
      });

      document.body.appendChild(menu);
      const onDocClick = () => {
        menu.remove();
        document.removeEventListener('click', onDocClick);
      };
      setTimeout(() => document.addEventListener('click', onDocClick), 0);
    }

    function applyProducerPreset(idx) {
      const pm = getPresetMode();
      if (pm.mode === 'preset' && pm.idx === idx) {
        writePatch(PRODUCER_PRESETS[0].values);
        showToast('Deselected preset -> INIT | Default Patch');
        return;
      }
      const entry = PRODUCER_PRESETS[idx];
      if (!entry) return;
      writePatch(entry.values);
      showToast('Loaded: ' + entry.name);
    }

    function toggleLiveChain() {
      const pm = getPresetMode();
      if (pm.mode === 'live') {
        writePatch(PRODUCER_PRESETS[0].values);
        showToast('Cleared Live ASIO Chain -> INIT');
        return;
      }
      const patch = Object.assign({}, PRODUCER_PRESETS[0].values, { preset: 0.75 });
      writePatch(patch);
      showToast('Loaded Live ASIO Tracking Chain (0.75)');
    }

    function bindControlEvents() {
      Object.keys(sliders).forEach((id) => {
        const slider = sliders[id];
        if (!slider) return;
        let dragging = false;

        slider.addEventListener('pointerdown', () => {
          dragging = true;
          const pm = getPresetMode();
          if (pm.mode === 'live') {
            setSingleParam('preset', 0);
          }
          bridge.begin(id);
        });
        slider.addEventListener('input', () => {
          const val = Number(slider.value);
          if (!dragging) bridge.begin(id);
          bridge.set(id, val);
          if (!dragging) bridge.end(id);
        });
        const finishDrag = () => {
          if (!dragging) return;
          dragging = false;
          bridge.end(id);
        };
        slider.addEventListener('pointerup', finishDrag);
        slider.addEventListener('pointercancel', finishDrag);
        slider.addEventListener('contextmenu', (e) => {
          e.preventDefault();
          if (typeof bridge.contextMenu === 'function') {
            bridge.contextMenu(id, e.clientX, e.clientY);
          }
        });
      });

      if (el.ratioGroup) {
        el.ratioGroup.querySelectorAll('[data-ratio]').forEach((btn) => {
          btn.addEventListener('click', () => {
            const pm = getPresetMode();
            if (pm.mode === 'live') setSingleParam('preset', 0);
            setSingleParam('ratio', Number(btn.dataset.ratio));
          });
          btn.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            if (typeof bridge.contextMenu === 'function') {
              bridge.contextMenu('ratio', e.clientX, e.clientY);
            }
          });
        });
      }

      if (el.presetsGrid) {
        el.presetsGrid.querySelectorAll('[data-preset]').forEach((btn) => {
          btn.addEventListener('click', () => applyProducerPreset(Number(btn.dataset.preset)));
        });
      }

      if (el.flowStrip) {
        el.flowStrip.querySelectorAll('.vc-stage-pill').forEach((pill) => {
          pill.addEventListener('click', () => {
            state.selectedStage = pill.dataset.stage || 'preamp';
            renderPresetAndFlowStatus();
            renderInspector();
          });
        });
      }

      if (el.inspResetStageBtn) {
        el.inspResetStageBtn.addEventListener('click', () => {
          const st = state.selectedStage;
          if (st === 'preamp' || st === 'mic') setSingleParam('input', 0);
          else if (st === 'fet') { setSingleParam('fet', 0); setSingleParam('ratio', 0); }
          else if (st === 'la2a') setSingleParam('la2a', 0);
          else if (st === 'eq') { setSingleParam('low', 0); setSingleParam('air', 0); }
          else if (st === 'beauty') { setSingleParam('beauty', 0); setSingleParam('trim', 0); }
          showToast('Reset stage: ' + st.toUpperCase());
        });
      }

      if (el.clearPatchBtn) {
        el.clearPatchBtn.addEventListener('click', () => {
          writePatch(PRODUCER_PRESETS[0].values);
          showToast('Reset to INIT | Default Patch');
        });
      }

      if (el.liveChainBtn) el.liveChainBtn.addEventListener('click', toggleLiveChain);
      if (el.presetMenuBtn) el.presetMenuBtn.addEventListener('click', openPresetPopover);
      if (el.ecoBtn) el.ecoBtn.addEventListener('click', () => applyEco(!state.eco, true));
      if (el.themeBtn) el.themeBtn.addEventListener('click', () => applyTheme(state.theme === 'dark' ? 'light' : 'dark'));

      [el.slotABtn, el.slotBBtn].forEach((btn, idx) => {
        if (!btn) return;
        const slot = idx === 0 ? 'A' : 'B';
        btn.addEventListener('click', () => {
          state.slots[state.activeSlot] = Object.assign({}, state.values);
          state.activeSlot = slot;
          el.slotABtn.classList.toggle('active', slot === 'A');
          el.slotBBtn.classList.toggle('active', slot === 'B');
          writePatch(state.slots[slot]);
          showToast('Recalled Slot ' + slot);
        });
      });

      root.addEventListener('resize', scheduleCurveDraw);
    }

    // Bind telemetry streams (fire only while audio moves -> 0% idle CPU)
    if (typeof bridge.on === 'function') {
      bridge.on((valuesMap) => syncFromBridge(valuesMap));
    }

    if (typeof bridge.onLevel === 'function') {
      bridge.onLevel((lin, db) => {
        const now = performance.now();
        if (state.eco && (now - state.lastMeterPaintMs) < 83) return;
        state.lastMeterPaintMs = now;
        const dbfs = Number.isFinite(db) ? db : -100;
        const pct = Math.max(0, Math.min(100, ((dbfs + 60) / 60) * 100));
        const lbl = dbfs <= -80 ? '-INF' : dbfs.toFixed(1) + ' dB';
        if (el.meterOutDb) el.meterOutDb.textContent = lbl + (dbfs > -80 ? 'FS' : '');
        if (el.outValLabel) el.outValLabel.textContent = lbl;
        if (el.outBarFill) el.outBarFill.style.width = pct.toFixed(1) + '%';
      });
    }

    if (typeof bridge.onGR === 'function') {
      bridge.onGR((grDb) => {
        const g = Math.max(0, Math.min(24, Number(grDb) || 0));
        const pct = Math.min(100, (g / 18) * 100);
        const txt = 'GR ' + g.toFixed(1) + ' dB';
        if (el.meterGrDb) el.meterGrDb.textContent = txt;
        if (el.grValLabel) el.grValLabel.textContent = g.toFixed(1) + ' dB';
        if (el.grBarFill) el.grBarFill.style.width = pct.toFixed(1) + '%';
      });
    }

    const query = new URLSearchParams(root.location.search || '');
    const qTheme = query.get('theme') || query.get('mock');
    let savedTheme = 'dark';
    try {
      savedTheme = localStorage.getItem('sr_vocalchain_theme') || 'dark';
    } catch (e) {}
    applyTheme(qTheme === 'light' || qTheme === 'dark' ? qTheme : savedTheme);
    applyEco(false, false);

    bindControlEvents();
    syncFromBridge(null);
  }

  let booted = false;
  function startOnce() {
    if (booted) return;
    booted = true;
    boot();
  }

  root.addEventListener('sonore:ready', startOnce);
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => {
      if (root.sonore || document.readyState !== 'loading') startOnce();
    });
  } else {
    startOnce();
  }
})(window);
