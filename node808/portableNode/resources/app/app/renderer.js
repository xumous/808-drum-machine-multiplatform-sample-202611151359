/*
 * renderer.js —— 808 界面（Canvas 自绘，1280x720 逻辑坐标，与 Qt/C/Java 版一致）
 * ---------------------------------------------------------------------------
 * 渲染进程不参与 DSP：所有参数都通过 AudioWorklet 的 port 发到音频线程，
 * 每帧只读 10856 字节的状态快照。
 */
(() => {
    'use strict';
    window.addEventListener('error', (e) => {
        console.error('[n808] JS ERROR', e.message, e.filename + ':' + e.lineno);
    });
    window.addEventListener('unhandledrejection', (e) => {
        console.error('[n808] PROMISE REJECT', String(e.reason));
    });

    /* ======================= 常量 ======================= */
    const TRACK_NAMES = ['KICK', 'SNARE', 'CLAP', 'RIM', 'TOM LO', 'TOM MID', 'TOM HI', 'CONGA',
        'COWBELL', 'SHAKER', 'TAMB', 'CRASH', 'RIDE', 'HAT OPEN', 'HAT CLOSED', 'CLAVE'];
    const KNOB_NAMES = ['TUNE', 'DECAY', 'TONE', 'CHAR', 'LEVEL', 'PAN'];
    const MASTER_NAMES = ['VOLUME', 'DRIVE', 'HP', 'LP', 'RESO', 'ACCENT'];
    const COND_NAMES = ['ALWAYS', '1:2', '1:3', '1:4', 'FILL', 'NOT FILL', 'EVERY N'];
    const PAD_KEYS = ['1', '2', '3', '4', 'q', 'w', 'e', 'r', 'a', 's', 'd', 'f', 'z', 'x', 'c', 'v'];

    /* 与 src/n808_glue.c 的 N808_OP_* 一一对应 */
    const OPS = {
        STEP_ON: 1, STEP_VEL: 2, STEP_ACCENT: 3, STEP_PROB: 4, STEP_RATCHET: 5, STEP_RATE: 6,
        STEP_COND: 7, STEP_MICRO: 8, TRACK_PARAM: 9, TRACK_LEN: 10, MUTE: 11, SOLO: 12,
        MASTER: 13, BPM: 14, TIMESIG: 15, PATTERN_LEN: 16, PATTERN_SWING: 17, SELECT_PATTERN: 18,
        SONG_ON: 19, SONG_LEN: 20, SONG_ENTRY: 21, PATTERN_COUNT: 22, CLEAR_PATTERN: 23,
        CLEAR_TRACK: 24, COPY_PATTERN: 25, PLAY: 26, STOP: 27, STOP_RESET: 28, PANIC: 29, NOTE_ON: 30,
        CLOCK: 31
    };

    const COL = {
        bg: '#0e1014', panel: '#1a1d23', panel2: '#242830', panel3: '#2f343e',
        text: '#e4e7ed', dim: '#8b92a0', orange: '#ff7a30', orange2: '#7a3a18',
        green: '#46c46a', red: '#e5484d', blue: '#569cf5', cell: '#2a2e37', accentCell: '#ffc478'
    };

    /* 快照布局（与 n808_snap 对应） */
    const OFF = {STATUS: 0, CPU: 32, METERS: 36, META: 108, PARAMS: 140, STEPS: 552, FLAGS: 10792, SIZE: 10856};
    const META = {LEN: 0, SWING: 1, PAT: 2, SONG_ON: 3, PAT_COUNT: 4, TS_NUM: 5, TS_DEN: 6, SONG_LEN: 7};

    const F_LABEL = '13px "Segoe UI","Microsoft YaHei",sans-serif';
    const F_SMALL = '11px "Segoe UI","Microsoft YaHei",sans-serif';
    const F_TINY = '9px "Segoe UI","Microsoft YaHei",sans-serif';
    const F_MONO = '12px Consolas,"Courier New",monospace';

    /* ======================= 状态 ======================= */
    const S = {
        ready: false,
        mode: '',
        msg: 'BOOTING…',
        snap: new Uint8Array(OFF.SIZE),
        dv: null,
        selTrack: 0,
        selStep: 0,
        page: 0,
        drag: null,
        clockMidi: false,
        audioInfo: 'AUDIO: INIT',
        midiName: 'MIDI: OFF',
        exportBusy: false,
        frames: 0,
        snapCount: 0,
        ac: null,
        node: null,
        local: null,
        song: []
    };

    const canvas = document.getElementById('panel');
    const ctx = canvas.getContext('2d');

    /* ======================= 工具 ======================= */
    const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
    const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
    const i32 = (off) => S.dv.getInt32(off, true);
    const u32 = (off) => S.dv.getUint32(off, true);
    const f32 = (off) => S.dv.getFloat32(off, true);
    const u8 = (off) => S.snap[off];

    const metaLen = () => clamp(i32(OFF.META + META.LEN * 4), 1, 64);
    const curPat = () => i32(OFF.META + META.PAT * 4);
    const swingVal = () => i32(OFF.META + META.SWING * 4);
    const songOn = () => i32(OFF.META + META.SONG_ON * 4);
    const bpm = () => f32(OFF.PARAMS);
    const tsNum = () => i32(OFF.META + META.TS_NUM * 4) || 4;
    const tsDen = () => i32(OFF.META + META.TS_DEN * 4) || 4;
    const masterParam = (i) => f32(OFF.PARAMS + (1 + i) * 4);
    const trackParam = (t, p) => f32(OFF.PARAMS + (7 + t * 6 + p) * 4);
    const meter = (i) => f32(OFF.METERS + i * 4);
    const playing = () => i32(OFF.STATUS) !== 0;
    const playStep = () => i32(OFF.STATUS + 8);
    const frameCount = () => u32(OFF.STATUS + 24);
    const blockCount = () => u32(OFF.STATUS + 28);
    const cpuLoad = () => f32(OFF.CPU);
    const xruns = () => i32(OFF.STATUS + 20);

    function stepByte(t, s, field) {
        return u8(OFF.STEPS + (t * 64 + s) * 10 + field);
    }

    function stepMicro(t, s) {
        return S.dv.getInt16(OFF.STEPS + (t * 64 + s) * 10 + 8, true);
    }

    function trackFlag(t, i) {
        return u8(OFF.FLAGS + t * 4 + i);
    }

    /* ======================= 引擎 ======================= */
    function send(op, a, b, c, n, f) {
        if (S.mode === 'worklet' && S.node) {
            S.node.port.postMessage({t: 'cmd', op, a: a | 0, b: b | 0, c: c | 0, n: n | 0, f: f || 0});
        } else if (S.mode === 'local' && S.local) {
            S.local.send(op, a, b, c, n, f);
        }
    }

    async function startEngine() {
        let wasmBytes;
        try {
            wasmBytes = await (await fetch('app://local/wasm/core808.wasm')).arrayBuffer();
        } catch (err) {
            S.msg = 'WASM 加载失败: ' + err;
            return;
        }
        try {
            const AC = new AudioContext({sampleRate: 48000, latencyHint: 'interactive'});
            await AC.audioWorklet.addModule('audio-worklet.js');
            const node = new AudioWorkletNode(AC, 'drum808', {
                numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]
            });
            node.connect(AC.destination);
            node.port.onmessage = onAudioMessage;
            const initCopy = wasmBytes.slice(0);
            node.port.postMessage({t: 'init', wasm: initCopy, sampleRate: AC.sampleRate}, [initCopy]);
            if (AC.state === 'suspended') await AC.resume();
            S.ac = AC;
            S.node = node;
            S.mode = 'worklet';
            S.audioInfo = 'AUDIOWORKLET ' + Math.round(AC.sampleRate) + ' Hz · 2ch · wasm';
            S.msg = 'READY';
            return;
        } catch (err) {
            console.warn('AudioWorklet 不可用，降级为静默本地引擎:', err);
        }
        degrade('DEGRADED: 无音频输出');
    }

    /* 降级：同一个 wasm 在渲染进程静默运行（无声音，UI/导出仍可用） */
    let localTimer = null;

    async function degrade(reason) {
        if (S.mode === 'local') return;
        try {
            if (S.node) {
                try {
                    S.node.disconnect();
                } catch (e) { /* ignore */
                }
                S.node = null;
            }
            if (S.ac) {
                try {
                    S.ac.close();
                } catch (e) { /* ignore */
                }
                S.ac = null;
            }
            if (!S.local) S.local = await N808Engine.createLocalEngine(48000);
            S.mode = 'local';
            S.ready = true;
            S.audioInfo = 'NO AUDIO DEVICE · LOCAL ENGINE (SILENT)';
            S.msg = reason || 'DEGRADED';
            if (!localTimer) {
                localTimer = setInterval(() => {
                    if (S.local) {
                        S.local.step(1200);
                        S.snap.set(S.local.snap);
                        S.snapCount++;
                    }
                }, 25);
            }
        } catch (err2) {
            S.msg = 'ENGINE FAILED: ' + err2;
            S.audioInfo = 'NO ENGINE';
        }
    }

    function onAudioMessage(e) {
        const m = e.data;
        if (!m) return;
        switch (m.t) {
            case 'ready':
                S.ready = true;
                S.audioInfo = 'AUDIOWORKLET ' + Math.round(S.ac ? S.ac.sampleRate : 48000) + ' Hz · 2ch · wasm';
                S.msg = 'READY';
                console.log('[n808] worklet ready, snapBytes=' + m.snapBytes + ' ac=' + (S.ac ? S.ac.state : '?'));
                /* 看门狗：8s 内既没有渲染回调也没有快照，才认为音频线程没跑起来并降级 */
                setTimeout(() => {
                    if (S.snapCount === 0 && !S.sawBlock) {
                        console.warn('[n808] no audio callbacks from worklet, falling back');
                        degrade('NO AUDIO CALLBACK — LOCAL ENGINE');
                    }
                }, 8000);
                break;
            case 'firstBlock':
                console.log('[n808] first render block, frames=' + m.frames);
                S.sawBlock = true;
                S.msg = 'AUDIO CALLBACK OK';
                break;
            case 'snap': {
                const view = m.buf instanceof Uint8Array ? m.buf : new Uint8Array(m.buf);
                S.snap.set(view);
                S.snapCount++;
                /* 把乒乓缓冲区还给音频线程（必须转移 ArrayBuffer，不能转移 TypedArray） */
                const ab = view.buffer;
                if (S.node && ab && ab.byteLength === OFF.SIZE) {
                    S.node.port.postMessage({t: 'ret', buf: ab}, [ab]);
                }
                break;
            }
            case 'error':
                S.msg = 'AUDIO ERROR: ' + m.msg;
                break;
            case 'project':
                if (pendingProject) {
                    pendingProject(m);
                    pendingProject = null;
                }
                break;
            case 'projectLoaded':
                setMessage(m.rc === 0 ? 'PROJECT LOADED' : 'LOAD FAILED (' + m.rc + ')');
                break;
            case 'song':
                if (pendingSong) {
                    pendingSong(m.buf);
                    pendingSong = null;
                }
                break;
            default:
                break;
        }
    }

    /* ======================= 布局（1280x720） ======================= */
    const L = {};
    const R = (x, y, w, h) => ({x, y, w, h});

    function layout() {
        L.play = R(12, 8, 72, 30);
        L.stop = R(90, 8, 72, 30);
        L.tap = R(168, 8, 60, 30);
        L.bpmMinus = R(234, 8, 30, 30);
        L.bpm = R(268, 8, 86, 30);
        L.bpmPlus = R(358, 8, 30, 30);
        L.swing = R(400, 8, 190, 30);
        L.patPrev = R(600, 8, 30, 30);
        L.patDisp = R(634, 8, 92, 30);
        L.patNext = R(730, 8, 30, 30);
        L.lenMinus = R(772, 8, 30, 30);
        L.lenDisp = R(806, 8, 70, 30);
        L.lenPlus = R(880, 8, 30, 30);
        L.song = R(922, 8, 72, 30);
        L.songEdit = R(1000, 8, 84, 30);

        L.save = R(12, 44, 70, 28);
        L.load = R(86, 44, 70, 28);
        L.wav = R(160, 44, 70, 28);
        L.midi = R(234, 44, 70, 28);
        L.stems = R(308, 44, 76, 28);
        L.clear = R(388, 44, 64, 28);
        L.copy = R(456, 44, 64, 28);
        L.midiIn = R(524, 44, 240, 28);
        L.sync = R(772, 44, 76, 28);
        L.ts = R(854, 44, 70, 28);
        L.trkLenM = R(860, 486, 32, 26);
        L.trkLenD = R(896, 486, 128, 26);
        L.trkLenP = R(1028, 486, 32, 26);

        L.grid = R(12, 84, 830, 432);
        L.panel = R(852, 84, 416, 432);
        L.master = R(12, 526, 830, 106);
        L.pads = R(852, 526, 416, 106);
        L.songStrip = R(12, 640, 1256, 40);
        L.status = R(0, 686, 1280, 34);
        L.gridSteps = R(L.grid.x + 126, L.grid.y + 22, L.grid.w - 134, L.grid.h - 28);

        L.voiceKnobs = [];
        for (let i = 0; i < 6; i++) {
            L.voiceKnobs.push(R(862 + (i % 3) * 134, 118 + Math.floor(i / 3) * 80, 128, 78));
        }
        L.masterKnobs = [];
        for (let i = 0; i < 6; i++) L.masterKnobs.push(R(30 + i * 134, 542, 128, 86));
        L.stepSliders = [];
        for (let i = 0; i < 5; i++) L.stepSliders.push(R(860, 316 + i * 25, 400, 24));
        L.cond = R(860, 452, 210, 28);
        L.accent = R(1080, 452, 180, 28);
        L.trigger = R(1090, 94, 168, 24);
        L.pads16 = [];
        for (let i = 0; i < 16; i++) L.pads16.push(R(858 + (i % 4) * 102, 530 + Math.floor(i / 4) * 25, 98, 22));
        L.quick = [];
        for (let i = 0; i < 16; i++) L.quick.push(R(34 + i * 76, 644, 70, 30));
        L.pagePrev = R(14, 644, 16, 30);
        L.pageNext = R(1252, 644, 16, 30);
    }

    function inRect(r, x, y) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    }

    /* ======================= 绘制原语 ======================= */
    function rr(x, y, w, h, rad) {
        const r = Math.min(rad, w / 2, h / 2);
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.lineTo(x + w - r, y);
        ctx.quadraticCurveTo(x + w, y, x + w, y + r);
        ctx.lineTo(x + w, y + h - r);
        ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
        ctx.lineTo(x + r, y + h);
        ctx.quadraticCurveTo(x, y + h, x, y + h - r);
        ctx.lineTo(x, y + r);
        ctx.quadraticCurveTo(x, y, x + r, y);
        ctx.closePath();
    }

    function drawButton(rc, text, active, font) {
        ctx.lineWidth = 1;
        ctx.strokeStyle = active ? COL.orange : COL.panel3;
        ctx.fillStyle = active ? COL.orange : COL.panel2;
        rr(rc.x, rc.y, rc.w, rc.h, 6);
        ctx.fill();
        ctx.stroke();
        ctx.font = font || F_LABEL;
        ctx.fillStyle = active ? '#14100c' : COL.text;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(text, rc.x + rc.w / 2, rc.y + rc.h / 2 + 0.5);
    }

    function drawBox(rc, text, color, font) {
        ctx.lineWidth = 1;
        ctx.strokeStyle = COL.panel3;
        ctx.fillStyle = COL.panel2;
        rr(rc.x, rc.y, rc.w, rc.h, 6);
        ctx.fill();
        ctx.stroke();
        ctx.font = font || F_LABEL;
        ctx.fillStyle = color || COL.text;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(text, rc.x + rc.w / 2, rc.y + rc.h / 2 + 0.5);
    }

    function drawKnob(rc, label, v) {
        const rad = Math.max(7, Math.min((rc.h - 38) / 2, 20));
        const cx = rc.x + rc.w / 2;
        const cy = rc.y + 4 + rad;
        ctx.beginPath();
        ctx.arc(cx, cy, rad, 0, Math.PI * 2);
        ctx.fillStyle = COL.panel3;
        ctx.fill();
        ctx.lineWidth = 4;
        ctx.strokeStyle = COL.orange2;
        ctx.stroke();

        ctx.beginPath();
        ctx.arc(cx, cy, rad + 5, (135 * Math.PI) / 180, (405 * Math.PI) / 180);
        ctx.lineWidth = 2.5;
        ctx.strokeStyle = COL.orange;
        ctx.stroke();

        const ang = ((135 + 270 * v) * Math.PI) / 180;
        ctx.beginPath();
        ctx.moveTo(cx, cy);
        ctx.lineTo(cx + Math.cos(ang) * (rad - 4), cy + Math.sin(ang) * (rad - 4));
        ctx.lineWidth = 2.5;
        ctx.strokeStyle = COL.text;
        ctx.stroke();

        ctx.font = F_TINY;
        ctx.fillStyle = COL.dim;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(label, rc.x + rc.w / 2, cy + rad + 8);
        ctx.font = F_SMALL;
        ctx.fillStyle = COL.text;
        ctx.fillText(String(Math.round(v * 100)), rc.x + rc.w / 2, cy + rad + 21);
    }

    function drawSlider(rc, label, v, valText, bipolar) {
        ctx.font = F_SMALL;
        ctx.textBaseline = 'middle';
        ctx.fillStyle = COL.dim;
        ctx.textAlign = 'left';
        ctx.fillText(label, rc.x, rc.y + rc.h / 2);
        ctx.fillStyle = COL.text;
        ctx.textAlign = 'right';
        ctx.fillText(valText, rc.x + rc.w, rc.y + rc.h / 2);

        const bx = rc.x + 78;
        const bw = rc.w - 138;
        const by = rc.y + 7;
        const bh = rc.h - 14;
        ctx.fillStyle = COL.panel3;
        rr(bx, by, bw, bh, 4);
        ctx.fill();
        ctx.fillStyle = COL.orange;
        if (bipolar) {
            const mid = bx + bw / 2;
            const x = bx + v * bw;
            ctx.fillRect(Math.min(mid, x), by, Math.abs(x - mid), bh);
            ctx.strokeStyle = COL.dim;
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(mid, by - 2);
            ctx.lineTo(mid, by + bh + 2);
            ctx.stroke();
        } else {
            ctx.fillRect(bx, by, bw * v, bh);
        }
    }

    function drawMeter(rc, v) {
        ctx.fillStyle = COL.panel3;
        ctx.fillRect(rc.x, rc.y, rc.w, rc.h);
        const h = rc.h * clamp01(v);
        if (h > 0) {
            ctx.fillStyle = v > 0.9 ? COL.red : v > 0.6 ? COL.orange : COL.green;
            ctx.fillRect(rc.x, rc.y + rc.h - h, rc.w, h);
        }
    }

    function mixColor(a, b, t) {
        const pa = parseInt(a.slice(1), 16);
        const pb = parseInt(b.slice(1), 16);
        const r = ((pa >> 16) & 255) + ((((pb >> 16) & 255) - ((pa >> 16) & 255)) * t);
        const g = ((pa >> 8) & 255) + ((((pb >> 8) & 255) - ((pa >> 8) & 255)) * t);
        const bl = (pa & 255) + (((pb & 255) - (pa & 255)) * t);
        return 'rgb(' + (r | 0) + ',' + (g | 0) + ',' + (bl | 0) + ')';
    }

    /* ======================= 主绘制 ======================= */
    function paint() {
        const pat = curPat();
        const len = metaLen();

        ctx.fillStyle = COL.bg;
        ctx.fillRect(0, 0, 1280, 720);

        if (!S.ready) {
            ctx.font = F_LABEL;
            ctx.fillStyle = COL.dim;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(S.msg, 640, 360);
            return;
        }

        /* ---- 顶栏 ---- */
        drawButton(L.play, playing() ? 'PLAYING' : 'PLAY', playing());
        drawButton(L.stop, 'STOP', false);
        drawButton(L.tap, 'TAP', false);
        drawButton(L.bpmMinus, '-', false);
        drawButton(L.bpmPlus, '+', false);
        drawBox(L.bpm, bpm().toFixed(1) + ' BPM', COL.orange);

        /* swing */
        {
            const rc = L.swing;
            ctx.font = F_SMALL;
            ctx.fillStyle = COL.dim;
            ctx.textAlign = 'left';
            ctx.textBaseline = 'middle';
            ctx.fillText('SWING', rc.x, rc.y + rc.h / 2);
            const bx = rc.x + 54;
            const bw = rc.w - 54;
            const by = rc.y + 9;
            const bh = rc.h - 18;
            ctx.fillStyle = COL.panel3;
            rr(bx, by, bw, bh, 4);
            ctx.fill();
            const sw = swingVal();
            ctx.fillStyle = COL.orange;
            ctx.fillRect(bx, by, (bw * sw) / 100, bh);
            ctx.fillStyle = COL.text;
            ctx.textAlign = 'right';
            ctx.fillText(sw + '%', rc.x + 48, by + bh / 2);
        }

        drawButton(L.patPrev, '<', false);
        drawButton(L.patNext, '>', false);
        drawBox(L.patDisp, 'PAT ' + String(pat).padStart(3, '0'), COL.text);
        drawButton(L.lenMinus, '-', false);
        drawButton(L.lenPlus, '+', false);
        drawBox(L.lenDisp, 'LEN ' + len, COL.text);
        drawButton(L.song, songOn() ? 'SONG ON' : 'SONG OFF', songOn() !== 0);
        drawButton(L.songEdit, 'EDIT SONG', false);
        drawButton(L.sync, S.clockMidi ? 'SYNC MIDI' : 'SYNC INT', S.clockMidi, F_SMALL);
        drawButton(L.ts, tsNum() + '/' + tsDen(), false, F_SMALL);

        drawButton(L.save, 'SAVE', false);
        drawButton(L.load, 'LOAD', false);
        drawButton(L.wav, 'WAV', false);
        drawButton(L.midi, 'MIDI', false);
        drawButton(L.stems, 'STEMS', false);
        drawButton(L.clear, 'CLEAR', false);
        drawButton(L.copy, 'COPY>', false);
        {
            ctx.lineWidth = 1;
            ctx.strokeStyle = COL.panel3;
            ctx.fillStyle = COL.panel2;
            rr(L.midiIn.x, L.midiIn.y, L.midiIn.w, L.midiIn.h, 6);
            ctx.fill();
            ctx.stroke();
            ctx.font = F_SMALL;
            ctx.fillStyle = COL.dim;
            ctx.textAlign = 'left';
            ctx.textBaseline = 'middle';
            let text = S.midiName;
            while (ctx.measureText(text).width > L.midiIn.w - 16 && text.length > 4) {
                text = text.slice(0, -2) + '…';
            }
            ctx.fillText(text, L.midiIn.x + 8, L.midiIn.y + L.midiIn.h / 2);
        }

        /* ---- 网格 ---- */
        ctx.fillStyle = COL.panel;
        rr(L.grid.x, L.grid.y, L.grid.w, L.grid.h, 8);
        ctx.fill();

        const gs = L.gridSteps;
        const rowH = gs.h / 16;
        const colW = gs.w / len;

        ctx.font = F_TINY;
        ctx.fillStyle = COL.dim;
        ctx.textAlign = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillText('TRACK', L.grid.x + 10, L.grid.y + 13);
        ctx.textAlign = 'center';
        for (let s = 0; s < len; s += 4) {
            ctx.fillText(String(s + 1), gs.x + s * colW + colW / 2, L.grid.y + 12);
        }

        for (let t = 0; t < 16; t++) {
            const y = gs.y + t * rowH;
            ctx.fillStyle = t === S.selTrack ? '#2d2722' : (t & 1) ? '#181b21' : '#16191e';
            ctx.fillRect(L.grid.x, y, gs.x - L.grid.x, rowH);

            ctx.font = F_TINY;
            ctx.textAlign = 'left';
            ctx.fillStyle = t === S.selTrack ? COL.orange : COL.text;
            ctx.fillText(TRACK_NAMES[t], L.grid.x + 4, y + rowH / 2);

            const mr = R(L.grid.x + 72, y + 2, 22, rowH - 4);
            const sr = R(L.grid.x + 96, y + 2, 22, rowH - 4);
            ctx.fillStyle = trackFlag(t, 0) ? COL.red : COL.panel3;
            rr(mr.x, mr.y, mr.w, mr.h, 3);
            ctx.fill();
            ctx.fillStyle = trackFlag(t, 1) ? COL.green : COL.panel3;
            rr(sr.x, sr.y, sr.w, sr.h, 3);
            ctx.fill();
            ctx.font = F_TINY;
            ctx.textAlign = 'center';
            ctx.fillStyle = trackFlag(t, 0) ? COL.text : COL.dim;
            ctx.fillText('M', mr.x + mr.w / 2, mr.y + mr.h / 2);
            ctx.fillStyle = trackFlag(t, 1) ? COL.text : COL.dim;
            ctx.fillText('S', sr.x + sr.w / 2, sr.y + sr.h / 2);

            drawMeter(R(L.grid.x + 120, y + 2, 4, rowH - 4), meter(t));

            const tlen = clamp(trackFlag(t, 2) || len, 1, len);
            const phStep = playing() ? playStep() : -1;
            for (let s = 0; s < len; s++) {
                const cx = gs.x + s * colW + 1;
                const cy = y + 2;
                const cw = colW - 2;
                const ch = rowH - 4;
                const on = stepByte(t, s, 0) !== 0;
                let color = COL.cell;
                if (on) {
                    color = mixColor(COL.orange2, COL.orange, stepByte(t, s, 1) / 127);
                    if (stepByte(t, s, 2)) color = COL.accentCell;
                }
                ctx.fillStyle = color;
                rr(cx, cy, cw, ch, 2);
                ctx.fill();

                if (on && stepByte(t, s, 3) < 100) {
                    ctx.fillStyle = COL.blue;
                    ctx.fillRect(cx + 2, cy + 2, 3, 3);
                }
                if (on && stepByte(t, s, 4) > 1) {
                    ctx.fillStyle = COL.text;
                    const n = Math.min(4, stepByte(t, s, 4));
                    for (let k = 0; k < n; k++) ctx.fillRect(cx + 3 + k * 3, cy + ch - 5, 2, 3);
                }
                if (on && stepMicro(t, s) !== 0) {
                    ctx.fillStyle = stepMicro(t, s) > 0 ? COL.green : COL.red;
                    ctx.fillRect(cx + cw - 4, cy, 3, 4);
                }
                if (t === S.selTrack && s === S.selStep) {
                    ctx.strokeStyle = COL.text;
                    ctx.lineWidth = 1;
                    rr(cx, cy, cw, ch, 2);
                    ctx.stroke();
                }
                if (playing() && phStep % tlen === s) {
                    ctx.strokeStyle = COL.blue;
                    ctx.lineWidth = 1.5;
                    rr(cx, cy, cw, ch, 2);
                    ctx.stroke();
                }
            }
        }

        /* ---- 声部面板 ---- */
        ctx.fillStyle = COL.panel;
        rr(L.panel.x, L.panel.y, L.panel.w, L.panel.h, 8);
        ctx.fill();

        ctx.font = F_LABEL;
        ctx.fillStyle = COL.orange;
        ctx.textAlign = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillText('VOICE EDIT  ·  ' + TRACK_NAMES[S.selTrack], L.panel.x + 12, L.panel.y + 17);
        drawButton(L.trigger, 'TRIGGER', false, F_SMALL);

        for (let i = 0; i < 6; i++) drawKnob(L.voiceKnobs[i], KNOB_NAMES[i], trackParam(S.selTrack, i));

        ctx.font = F_LABEL;
        ctx.fillStyle = COL.text;
        ctx.fillText('STEP ' + String(S.selStep + 1).padStart(2, '0') + '  ·  ' + TRACK_NAMES[S.selTrack],
            L.panel.x + 10, L.panel.y + 210);

        {
            const t = S.selTrack;
            const s = S.selStep;
            const vel = stepByte(t, s, 1);
            const prob = stepByte(t, s, 3);
            const rat = stepByte(t, s, 4);
            const rate = stepByte(t, s, 5);
            const micro = stepMicro(t, s);
            const cond = stepByte(t, s, 6);
            const acc = stepByte(t, s, 2);
            drawSlider(L.stepSliders[0], 'VELOCITY', (vel - 1) / 126, String(vel));
            drawSlider(L.stepSliders[1], 'PROB', prob / 100, prob + '%');
            drawSlider(L.stepSliders[2], 'RATCHET', (rat - 1) / 7, 'x' + rat);
            drawSlider(L.stepSliders[3], 'RATE', (rate - 10) / 90, rate + '%');
            drawSlider(L.stepSliders[4], 'MICRO', micro / 4800 + 0.5, micro + ' smp', true);
            drawButton(L.cond, 'COND: ' + COND_NAMES[cond <= 6 ? cond : 0], cond !== 0, F_SMALL);
            drawButton(L.accent, acc ? 'ACCENT ON' : 'ACCENT', acc !== 0, F_SMALL);
            drawButton(L.trkLenM, '-', false, F_SMALL);
            drawButton(L.trkLenD, 'TRACK LEN ' + (trackFlag(t, 2) || 16), false, F_SMALL);
            drawButton(L.trkLenP, '+', false, F_SMALL);
        }

        /* ---- 主控 ---- */
        ctx.fillStyle = COL.panel;
        rr(L.master.x, L.master.y, L.master.w, L.master.h, 8);
        ctx.fill();
        ctx.font = F_TINY;
        ctx.fillStyle = COL.dim;
        ctx.textAlign = 'left';
        ctx.fillText('MASTER  ·  FILTER / SATURATION / ACCENT', L.master.x + 12, L.master.y + 10);
        for (let i = 0; i < 6; i++) drawKnob(L.masterKnobs[i], MASTER_NAMES[i], masterParam(i));

        /* ---- 打击垫 ---- */
        ctx.fillStyle = COL.panel;
        rr(L.pads.x, L.pads.y, L.pads.w, L.pads.h, 8);
        ctx.fill();
        for (let i = 0; i < 16; i++) {
            const rc = L.pads16[i];
            const sel = i === S.selTrack;
            ctx.fillStyle = sel ? COL.orange2 : COL.panel2;
            rr(rc.x, rc.y, rc.w, rc.h, 4);
            ctx.fill();
            ctx.strokeStyle = sel ? COL.orange : COL.panel3;
            ctx.lineWidth = 1;
            ctx.stroke();
            ctx.font = F_TINY;
            ctx.fillStyle = sel ? COL.text : COL.dim;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(TRACK_NAMES[i], rc.x + rc.w / 2, rc.y + rc.h / 2);
        }

        /* ---- pattern 快捷条 ---- */
        ctx.fillStyle = COL.panel;
        rr(L.songStrip.x, L.songStrip.y, L.songStrip.w, L.songStrip.h, 8);
        ctx.fill();
        drawButton(L.pagePrev, '<', false, F_TINY);
        drawButton(L.pageNext, '>', false, F_TINY);
        for (let i = 0; i < 16; i++) {
            const idx = S.page * 16 + i;
            drawButton(L.quick[i], String(idx), curPat() === idx, F_SMALL);
        }

        /* ---- 状态栏 ---- */
        ctx.fillStyle = COL.panel;
        ctx.fillRect(L.status.x, L.status.y, L.status.w, L.status.h);
        const st = S.audioInfo + '   ·   CPU ' + Math.round(cpuLoad() * 100) + '%' +
            '   ·   XRUN ' + xruns() + '   ·   ' + S.msg +
            '   ·   ' + (playing() ? 'PLAY' : 'STOP') +
            '   ·   F' + frameCount() + '   ·   BLK' + blockCount() +
            (S.mode === 'local' ? '   ·   LOCAL' : '');
        ctx.font = F_SMALL;
        ctx.fillStyle = S.mode === 'worklet' && S.ready ? COL.dim : COL.red;
        ctx.textAlign = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillText(st, 12, L.status.y + L.status.h / 2);
        if (S.exportBusy) {
            ctx.fillStyle = COL.orange;
            ctx.textAlign = 'right';
            ctx.fillText('EXPORTING…', 1268, L.status.y + L.status.h / 2);
        } else if (navigator.onLine === false) {
            /* 占位，无实际用途 */
        }
    }

    /* ======================= 交互 ======================= */
    function setMessage(m) {
        S.msg = m;
    }

    function gridAt(x, y) {
        const gs = L.gridSteps;
        const len = metaLen();
        if (!inRect(gs, x, y)) return null;
        const rowH = gs.h / 16;
        const colW = gs.w / len;
        const t = Math.floor((y - gs.y) / rowH);
        const s = Math.floor((x - gs.x) / colW);
        if (t < 0 || t >= 16 || s < 0 || s >= len) return null;
        return {t, s};
    }

    function cycleProb(t, s) {
        const seq = [100, 75, 50, 25, 10, 0];
        const cur = stepByte(t, s, 3);
        let next = 100;
        for (let i = 0; i < seq.length; i++) {
            if (seq[i] === cur) {
                next = seq[(i + 1) % seq.length];
                break;
            }
        }
        send(OPS.STEP_PROB, curPat(), t, s, next);
    }

    function cycleRatchet(t, s) {
        let cur = stepByte(t, s, 4) + 1;
        if (cur > 8) cur = 1;
        send(OPS.STEP_RATCHET, curPat(), t, s, cur);
    }

    function tapTempo() {
        const now = Date.now();
        if (tapTempo.count === undefined) {
            tapTempo.count = 0;
            tapTempo.pos = 0;
            tapTempo.last = 0;
            tapTempo.taps = [];
        }
        if (tapTempo.count > 0 && now - tapTempo.last > 2000) {
            tapTempo.count = 0;
            tapTempo.pos = 0;
            tapTempo.taps = [];
        }
        if (tapTempo.count > 0) {
            const dt = now - tapTempo.last;
            if (dt > 150 && dt < 1500) {
                tapTempo.taps.push(dt);
                tapTempo.pos++;
                tapTempo.count++;
            }
        } else {
            tapTempo.count = 1;
        }
        tapTempo.last = now;
        if (tapTempo.pos >= 2) {
            const arr = tapTempo.taps.slice(-8);
            const avg = arr.reduce((a, b) => a + b, 0) / arr.length;
            const v = 60000 / avg;
            send(OPS.BPM, 0, 0, 0, 0, clamp(v, 20, 400));
            setMessage('TAP TEMPO ' + v.toFixed(1) + ' BPM');
        }
    }

    function setStepSlider(idx, x) {
        const rc = L.stepSliders[idx];
        const bx = rc.x + 78;
        const bw = rc.w - 138;
        let t = clamp01((x - bx) / bw);
        if (idx === 4) t = clamp01(t);
        switch (idx) {
            case 0:
                send(OPS.STEP_VEL, curPat(), S.selTrack, S.selStep, Math.round(1 + t * 126));
                break;
            case 1:
                send(OPS.STEP_PROB, curPat(), S.selTrack, S.selStep, Math.round(t * 100));
                break;
            case 2:
                send(OPS.STEP_RATCHET, curPat(), S.selTrack, S.selStep, Math.round(1 + t * 7));
                break;
            case 3:
                send(OPS.STEP_RATE, curPat(), S.selTrack, S.selStep, Math.round(10 + t * 90));
                break;
            case 4:
                send(OPS.STEP_MICRO, curPat(), S.selTrack, S.selStep, Math.round((t - 0.5) * 4800));
                break;
            default:
                break;
        }
    }

    function onDown(ev) {
        const p = toLogical(ev);
        const x = p.x;
        const y = p.y;
        const mods = {ctrl: ev.ctrlKey, shift: ev.shiftKey, alt: ev.altKey};
        if (S.ac && S.ac.state === 'suspended') S.ac.resume();

        if (inRect(L.play, x, y)) {
            send(playing() ? OPS.STOP : OPS.PLAY);
            return;
        }
        if (inRect(L.stop, x, y)) {
            send(OPS.STOP_RESET);
            return;
        }
        if (inRect(L.tap, x, y)) {
            tapTempo();
            return;
        }
        if (inRect(L.bpmMinus, x, y)) {
            send(OPS.BPM, 0, 0, 0, 0, bpm() - 1);
            return;
        }
        if (inRect(L.bpmPlus, x, y)) {
            send(OPS.BPM, 0, 0, 0, 0, bpm() + 1);
            return;
        }
        if (inRect(L.bpm, x, y)) {
            S.drag = {kind: 'bpm', y, start: bpm()};
            return;
        }
        if (inRect(L.swing, x, y)) {
            const v = Math.round(((x - (L.swing.x + 54)) * 100) / (L.swing.w - 54));
            send(OPS.PATTERN_SWING, curPat(), clamp(v, 0, 100));
            return;
        }
        if (inRect(L.patPrev, x, y)) {
            send(OPS.SELECT_PATTERN, (curPat() + 255) % 256);
            return;
        }
        if (inRect(L.patNext, x, y)) {
            send(OPS.SELECT_PATTERN, (curPat() + 1) % 256);
            return;
        }
        if (inRect(L.lenMinus, x, y)) {
            send(OPS.PATTERN_LEN, curPat(), metaLen() - 1);
            return;
        }
        if (inRect(L.lenPlus, x, y)) {
            send(OPS.PATTERN_LEN, curPat(), metaLen() + 1);
            return;
        }
        if (inRect(L.song, x, y)) {
            send(OPS.SONG_ON, songOn() ? 0 : 1);
            return;
        }
        if (inRect(L.songEdit, x, y)) {
            openSongEditor();
            return;
        }
        if (inRect(L.save, x, y)) {
            saveProject();
            return;
        }
        if (inRect(L.load, x, y)) {
            loadProject();
            return;
        }
        if (inRect(L.wav, x, y)) {
            startExport(0);
            return;
        }
        if (inRect(L.midi, x, y)) {
            startExport(1);
            return;
        }
        if (inRect(L.stems, x, y)) {
            startExport(2);
            return;
        }
        if (inRect(L.clear, x, y)) {
            send(OPS.CLEAR_PATTERN, curPat());
            setMessage('PATTERN CLEARED');
            return;
        }
        if (inRect(L.copy, x, y)) {
            const dst = (curPat() + 1) % 256;
            send(OPS.COPY_PATTERN, curPat(), dst);
            send(OPS.SELECT_PATTERN, dst);
            setMessage('COPIED PATTERN ' + curPat() + ' -> ' + dst);
            return;
        }
        if (inRect(L.midiIn, x, y)) {
            cycleMidi();
            return;
        }
        if (inRect(L.sync, x, y)) {
            S.clockMidi = !S.clockMidi;
            send(OPS.CLOCK, S.clockMidi ? 1 : 0);
            setMessage(S.clockMidi ? 'MIDI CLOCK SYNC' : 'INTERNAL CLOCK');
            return;
        }
        if (inRect(L.ts, x, y)) {
            const sigs = [[4, 4], [3, 4], [5, 4], [7, 8], [6, 8], [12, 8]];
            let next = 0;
            for (let i = 0; i < sigs.length; i++) {
                if (sigs[i][0] === tsNum() && sigs[i][1] === tsDen()) {
                    next = (i + 1) % sigs.length;
                    break;
                }
            }
            send(OPS.TIMESIG, sigs[next][0], sigs[next][1]);
            return;
        }

        for (let i = 0; i < 16; i++) {
            if (inRect(L.quick[i], x, y)) {
                send(OPS.SELECT_PATTERN, S.page * 16 + i);
                return;
            }
        }
        if (inRect(L.pagePrev, x, y)) {
            if (S.page > 0) S.page--;
            return;
        }
        if (inRect(L.pageNext, x, y)) {
            if (S.page < 15) S.page++;
            return;
        }

        for (let i = 0; i < 6; i++) {
            if (inRect(L.voiceKnobs[i], x, y)) {
                const v = clamp01((L.voiceKnobs[i].y + L.voiceKnobs[i].h - 6 - y) / (L.voiceKnobs[i].h - 20));
                send(OPS.TRACK_PARAM, S.selTrack, i, 0, 0, v);
                S.drag = {kind: 'track', id: i, y, start: v};
                return;
            }
        }
        for (let i = 0; i < 6; i++) {
            if (inRect(L.masterKnobs[i], x, y)) {
                const v = clamp01((L.masterKnobs[i].y + L.masterKnobs[i].h - 6 - y) / (L.masterKnobs[i].h - 20));
                send(OPS.MASTER, i, 0, 0, 0, v);
                S.drag = {kind: 'master', id: i, y, start: v};
                return;
            }
        }
        for (let i = 0; i < 5; i++) {
            if (inRect(L.stepSliders[i], x, y)) {
                S.drag = {kind: 'slider', id: i};
                setStepSlider(i, x);
                return;
            }
        }
        if (inRect(L.cond, x, y)) {
            const c = (stepByte(S.selTrack, S.selStep, 6) + 1) % 7;
            send(OPS.STEP_COND, curPat(), S.selTrack, S.selStep, c, stepByte(S.selTrack, S.selStep, 7));
            return;
        }
        if (inRect(L.accent, x, y)) {
            send(OPS.STEP_ACCENT, curPat(), S.selTrack, S.selStep, stepByte(S.selTrack, S.selStep, 2) ? 0 : 1);
            return;
        }
        if (inRect(L.trkLenM, x, y)) {
            send(OPS.TRACK_LEN, S.selTrack, Math.max(1, (trackFlag(S.selTrack, 2) || 16) - 1));
            return;
        }
        if (inRect(L.trkLenP, x, y)) {
            send(OPS.TRACK_LEN, S.selTrack, Math.min(64, (trackFlag(S.selTrack, 2) || 16) + 1));
            return;
        }
        if (inRect(L.trigger, x, y)) {
            send(OPS.NOTE_ON, S.selTrack, 112, 0);
            return;
        }
        for (let i = 0; i < 16; i++) {
            if (inRect(L.pads16[i], x, y)) {
                send(OPS.NOTE_ON, i, 112, 0);
                S.selTrack = i;
                return;
            }
        }

        const hit = gridAt(x, y);
        if (hit) {
            S.selTrack = hit.t;
            S.selStep = hit.s;
            const pat = curPat();
            if (mods.ctrl) {
                send(OPS.STEP_ACCENT, pat, hit.t, hit.s, stepByte(hit.t, hit.s, 2) ? 0 : 1);
            } else if (mods.shift) {
                cycleProb(hit.t, hit.s);
            } else if (mods.alt) {
                cycleRatchet(hit.t, hit.s);
            } else {
                send(OPS.STEP_ON, pat, hit.t, hit.s, stepByte(hit.t, hit.s, 0) ? 0 : 1);
            }
        }
    }

    function onMove(ev) {
        if (!S.drag) return;
        const p = toLogical(ev);
        if (S.drag.kind === 'bpm') {
            const delta = (S.drag.y - p.y) / 110;
            send(OPS.BPM, 0, 0, 0, 0, clamp(S.drag.start + delta * 120, 20, 400));
        } else if (S.drag.kind === 'slider') {
            setStepSlider(S.drag.id, p.x);
        } else if (S.drag.kind === 'master') {
            const delta = (S.drag.y - p.y) / 110;
            send(OPS.MASTER, S.drag.id, 0, 0, 0, clamp01(S.drag.start + delta));
        } else if (S.drag.kind === 'track') {
            const delta = (S.drag.y - p.y) / 110;
            send(OPS.TRACK_PARAM, S.selTrack, S.drag.id, 0, 0, clamp01(S.drag.start + delta));
        }
    }

    function onWheel(ev) {
        ev.preventDefault();
        const p = toLogical(ev);
        const dir = ev.deltaY > 0 ? -1 : 1;
        const hit = gridAt(p.x, p.y);
        if (hit) {
            const v = clamp(stepByte(hit.t, hit.s, 1) + dir * 4, 1, 127);
            send(OPS.STEP_VEL, curPat(), hit.t, hit.s, v);
        } else if (inRect(L.bpm, p.x, p.y)) {
            send(OPS.BPM, 0, 0, 0, 0, clamp(bpm() + dir, 20, 400));
        }
    }

    function onKey(ev) {
        if (ev.target && ev.target.tagName === 'INPUT') return;
        if (!document.getElementById('songModal').classList.contains('hidden')) return;
        const k = ev.key;
        if (k === ' ') {
            ev.preventDefault();
            send(playing() ? OPS.STOP : OPS.PLAY);
            return;
        }
        if (k === 'ArrowLeft') {
            send(OPS.SELECT_PATTERN, (curPat() + 255) % 256);
            return;
        }
        if (k === 'ArrowRight') {
            send(OPS.SELECT_PATTERN, (curPat() + 1) % 256);
            return;
        }
        if (k === 't' || k === 'T') {
            tapTempo();
            return;
        }
        const idx = PAD_KEYS.indexOf(k.toLowerCase());
        if (idx >= 0) {
            send(OPS.NOTE_ON, idx, 112, 0);
            S.selTrack = idx;
        }
    }

    function toLogical(ev) {
        const r = canvas.getBoundingClientRect();
        return {
            x: ((ev.clientX - r.left) * 1280) / r.width,
            y: ((ev.clientY - r.top) * 720) / r.height
        };
    }

    /* ======================= Song 编辑器 ======================= */
    let pendingSong = null;

    function openSongEditor() {
        if (S.mode === 'worklet' && S.node) {
            pendingSong = (buf) => {
                const dv = new DataView(buf);
                S.song = [];
                const n = dv.getInt32(0, true);
                for (let i = 0; i < n; i++) {
                    S.song.push({pat: dv.getUint16(4 + i * 4, true), reps: dv.getUint16(6 + i * 4, true)});
                }
                renderSongList();
                document.getElementById('songModal').classList.remove('hidden');
            };
            S.node.port.postMessage({t: 'getSong'});
        } else {
            S.song = [{pat: 0, reps: 4}];
            renderSongList();
            document.getElementById('songModal').classList.remove('hidden');
        }
    }

    let songSel = 0;

    function renderSongList() {
        const box = document.getElementById('songList');
        box.innerHTML = '';
        if (!S.song.length) {
            box.innerHTML = '<div class="song-row">(EMPTY — 按 + ADD 添加)</div>';
            return;
        }
        S.song.forEach((it, i) => {
            const row = document.createElement('div');
            row.className = 'song-row' + (i === songSel ? ' sel' : '');
            row.textContent = String(i + 1).padStart(2, '0') + '   PATTERN ' +
                String(it.pat).padStart(3, '0') + '   x' + it.reps;
            row.onclick = () => {
                songSel = i;
                renderSongList();
            };
            box.appendChild(row);
        });
    }

    function songCommand(id) {
        const it = S.song[songSel];
        switch (id) {
            case 0:
                S.song.push({pat: S.song.length ? S.song[S.song.length - 1].pat : 0, reps: 4});
                songSel = S.song.length - 1;
                break;
            case 1:
                if (it) S.song.splice(songSel, 1);
                songSel = Math.max(0, Math.min(songSel, S.song.length - 1));
                break;
            case 2:
                if (songSel > 0) {
                    const tmp = S.song[songSel - 1];
                    S.song[songSel - 1] = S.song[songSel];
                    S.song[songSel] = tmp;
                    songSel--;
                }
                break;
            case 3:
                if (songSel < S.song.length - 1) {
                    const tmp = S.song[songSel + 1];
                    S.song[songSel + 1] = S.song[songSel];
                    S.song[songSel] = tmp;
                    songSel++;
                }
                break;
            case 4:
                if (it) it.pat = (it.pat + 255) % 256;
                break;
            case 5:
                if (it) it.pat = (it.pat + 1) % 256;
                break;
            case 6:
                if (it) it.reps = Math.max(1, it.reps - 1);
                break;
            case 7:
                if (it) it.reps = Math.min(64, it.reps + 1);
                break;
            default:
                break;
        }
        renderSongList();
    }

    function applySong() {
        send(OPS.SONG_LEN, Math.max(1, S.song.length));
        S.song.forEach((it, i) => send(OPS.SONG_ENTRY, i, it.pat, it.reps));
        setMessage('SONG CHAIN UPDATED (' + S.song.length + ' ENTRIES)');
        document.getElementById('songModal').classList.add('hidden');
    }

    /* ======================= 文件 / 导出 / MIDI ======================= */
    let pendingProject = null;

    function projectBytes() {
        return new Promise((resolve, reject) => {
            if (S.mode === 'local' && S.local) {
                resolve(S.local.projectBytes());
                return;
            }
            if (!S.node) {
                reject(new Error('engine not ready'));
                return;
            }
            pendingProject = (m) => {
                if (m.ok) resolve(new Uint8Array(m.buf));
                else reject(new Error('project dump failed ' + m.err));
            };
            S.node.port.postMessage({t: 'getProject'});
        });
    }

    const PROJ_FILTERS = [{name: '808 Project', extensions: ['808']}];

    async function saveProject() {
        try {
            const bytes = await projectBytes();
            const r = await window.n8.saveFile(bytes, '808-project.808', PROJ_FILTERS);
            setMessage(r.ok ? 'SAVED: ' + r.path : r.canceled ? 'SAVE CANCELED' : 'SAVE FAILED');
        } catch (err) {
            setMessage('SAVE FAILED: ' + err.message);
        }
    }

    async function loadProject() {
        const r = await window.n8.openFile(PROJ_FILTERS);
        if (!r.ok) {
            setMessage(r.canceled ? 'LOAD CANCELED' : 'LOAD FAILED');
            return;
        }
        const bytes = new Uint8Array(r.data);
        if (S.mode === 'local' && S.local) {
            const rc = S.local.loadProject(bytes);
            setMessage(rc === 0 ? 'LOADED: ' + r.name : 'LOAD FAILED (' + rc + ')');
            return;
        }
        if (!S.node) return;
        S.node.port.postMessage({t: 'loadProject', buf: bytes.buffer}, [bytes.buffer]);
    }

    async function startExport(kind) {
        if (S.exportBusy) {
            setMessage('EXPORT ALREADY RUNNING');
            return;
        }
        const isMidi = kind === 1;
        const filters = [isMidi ? {name: 'MIDI', extensions: ['mid']} : {name: 'WAV', extensions: ['wav']}];
        const def = isMidi ? '808-pattern.mid' : kind === 2 ? '808-stems.wav' : '808-pattern.wav';
        const pick = await window.n8.pickSavePath(def, filters);
        if (!pick.ok) {
            setMessage(pick.canceled ? 'EXPORT CANCELED' : 'EXPORT FAILED');
            return;
        }
        const fileName = pick.path.replace(/^.*[\\/]/, '');
        const dir = pick.path.slice(0, pick.path.length - fileName.length);

        S.exportBusy = true;
        setMessage('EXPORTING…');
        let project;
        try {
            project = await projectBytes();
        } catch (err) {
            S.exportBusy = false;
            setMessage('EXPORT FAILED: ' + err.message);
            return;
        }

        const worker = new Worker('export-worker.js');
        worker.onmessage = async (e) => {
            const m = e.data;
            if (m.t === 'error') {
                S.exportBusy = false;
                setMessage('EXPORT FAILED: ' + m.msg);
                worker.terminate();
                return;
            }
            if (m.t !== 'done') return;
            try {
                for (const f of m.files) {
                    const res = await window.n8.writePath(dir + f.name, f.data);
                    if (!res.ok) throw new Error(res.error || 'write failed');
                }
                setMessage('EXPORT COMPLETE: ' + m.files.length + ' FILE(S) → ' + dir);
            } catch (err) {
                setMessage('EXPORT WRITE FAILED: ' + err.message);
            }
            S.exportBusy = false;
            worker.terminate();
        };
        worker.postMessage({
            t: 'export',
            project: project.buffer,
            kind,
            mode: songOn() ? 1 : 0,
            loops: 2,
            sampleRate: S.ac ? S.ac.sampleRate : 48000,
            fileName
        }, [project.buffer]);
    }

    /* ---- Web MIDI ---- */
    let midiAccess = null;
    let midiInputs = [];
    let midiIndex = -1;

    async function initMidi() {
        try {
            midiAccess = await navigator.requestMIDIAccess({sysex: false});
            midiInputs = Array.from(midiAccess.inputs.values());
            S.midiName = midiInputs.length ? 'MIDI: OFF (点击切换, ' + midiInputs.length + ' 个设备)' : 'MIDI: NONE';
        } catch (err) {
            S.midiName = 'MIDI: UNAVAILABLE';
        }
    }

    function cycleMidi() {
        if (!midiInputs.length) {
            S.midiName = 'MIDI: NONE';
            setMessage('NO MIDI INPUT DEVICE');
            return;
        }
        midiIndex++;
        if (midiIndex >= midiInputs.length) {
            midiIndex = -1;
            midiInputs.forEach((i) => {
                i.onmidimessage = null;
            });
            S.midiName = 'MIDI: OFF';
            setMessage('MIDI INPUT DISABLED');
            return;
        }
        S.midiName = 'MIDI: ' + (midiInputs[midiIndex].name || 'DEVICE ' + midiIndex);
        midiInputs.forEach((i, k) => {
            i.onmidimessage = k === midiIndex ? onMidiMessage : null;
        });
        setMessage('MIDI INPUT: ' + S.midiName);
    }

    function onMidiMessage(ev) {
        const d = ev.data;
        if (!d || d.length < 1) return;
        const status = d[0];
        const type = status & 0xf0;
        if (status >= 0xf8) {
            const map = {0xf8: 4, 0xfa: 5, 0xfc: 6, 0xfb: 7};
            const kind = map[status];
            if (kind && S.node) S.node.port.postMessage({t: 'midi', kind, a: 0, b: 0, c: 0});
            return;
        }
        if (!S.node) return;
        if (type === 0x90 && d[2] > 0) {
            S.node.port.postMessage({t: 'midi', kind: 1, a: d[1], b: d[2], c: 0});
        } else if (type === 0xc0) {
            S.node.port.postMessage({t: 'midi', kind: 3, a: d[1], b: 0, c: 0});
        } else if (type === 0xb0) {
            S.node.port.postMessage({t: 'midi', kind: 2, a: d[1], b: d[2], c: 0});
        }
    }

    /* ======================= 尺寸 & 主循环 ======================= */
    function resizeTo(availW, availH) {
        const dpr = window.devicePixelRatio || 1;
        const scale = Math.max(0.2, Math.min(availW / 1280, availH / 720));
        const cssW = Math.floor(1280 * scale);
        const cssH = Math.floor(720 * scale);
        canvas.style.width = cssW + 'px';
        canvas.style.height = cssH + 'px';
        canvas.width = Math.round(cssW * dpr);
        canvas.height = Math.round(cssH * dpr);
        ctx.setTransform((cssW * dpr) / 1280, 0, 0, (cssH * dpr) / 720, 0, 0);
    }

    /* 用 #stage 的实际 CSS 尺寸做缩放基准（比 innerWidth 可靠，不受 DPI/zoom 影响） */
    const stage = document.getElementById('stage');

    function resize() {
        const r = stage.getBoundingClientRect();
        resizeTo(r.width || window.innerWidth, r.height || window.innerHeight);
    }

    /* ======================= 启动 ======================= */
    layout();
    S.dv = new DataView(S.snap.buffer);
    resize();
    window.addEventListener('resize', resize);
    if (typeof ResizeObserver !== 'undefined') {
        new ResizeObserver(() => resize()).observe(stage);
    }
    console.log('[n808] stage css', stage.getBoundingClientRect().width, stage.getBoundingClientRect().height,
        'inner', window.innerWidth, window.innerHeight, 'dpr', window.devicePixelRatio);
    canvas.addEventListener('pointerdown', onDown);
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', () => {
        S.drag = null;
    });
    canvas.addEventListener('wheel', onWheel, {passive: false});
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    window.addEventListener('keydown', onKey);

    document.querySelectorAll('.song-grid button').forEach((b) => {
        b.onclick = () => songCommand(Number(b.dataset.cmd));
    });
    document.getElementById('songApply').onclick = applySong;
    document.getElementById('songCancel').onclick = () => {
        document.getElementById('songModal').classList.add('hidden');
    };

    startEngine();
    initMidi();

    let lastPaint = 0;

    function frame(ts) {
        if (ts - lastPaint >= 30) {
            lastPaint = ts;
            paint();
        }
        requestAnimationFrame(frame);
    }

    requestAnimationFrame(frame);
})();
