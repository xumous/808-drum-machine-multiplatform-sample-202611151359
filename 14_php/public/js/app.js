// ============================================================================
//  app.js —— Electron 渲染进程界面 + 离线导出
//
//  实时音频：AudioContext + AudioWorkletNode('xu808')。
//  离线导出：OfflineAudioContext 里再创建一个同样的 worklet，
//  这样“导出的 WAV”和“实时听到的”走的是同一份 DSP 代码。
// ============================================================================

const SR = 48000;
const PADS = 16;
const MAX_STEPS = 64;
const CODES = ["BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC", "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH"];
const NAMES = ["底鼓", "军鼓", "低嗵", "中嗵", "高嗵", "低康加", "中康加", "高康加", "边击", "击木", "拍手", "沙锤", "牛铃", "吊镲", "开镲", "闭镲"];
const MIDI_NOTES = [36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42];
const COND_NAMES = ["总是", "仅过门", "非过门", "每2循环", "每4循环"];

// ---------------------------------------------------------------------------
// 工程数据
// ---------------------------------------------------------------------------
function makeStep() {
    return {on: false, velocity: 100, accent: false, prob: 100, micro: 0, ratchet: 1, cond: 0};
}

function makePattern(idx) {
    const data = [];
    for (let p = 0; p < PADS; p++) {
        const row = [];
        for (let s = 0; s < MAX_STEPS; s++) row.push(makeStep());
        data.push(row);
    }
    return {name: "PATTERN " + (idx + 1), steps: 16, fill: false, trackLen: new Array(PADS).fill(16), data};
}

function demoPattern() {
    const pat = makePattern(0);
    const set = (p, s, v, a) => {
        pat.data[p][s].on = true;
        pat.data[p][s].velocity = v;
        pat.data[p][s].accent = !!a;
    };
    set(0, 0, 118, true);
    set(0, 8, 105);
    set(0, 14, 90);
    set(1, 4, 110);
    set(1, 12, 110, true);
    for (let s = 0; s < 16; s += 2) set(15, s, s % 4 === 0 ? 96 : 84);
    set(14, 14, 92);
    set(10, 12, 80);
    for (let s = 1; s < 16; s += 2) set(11, s, 42);
    set(0, 15, 88);
    pat.data[0][15].ratchet = 3;
    return pat;
}

function makeProject() {
    const patterns = [];
    for (let i = 0; i < 16; i++) patterns.push(makePattern(i));
    patterns[0] = demoPattern();
    const pads = [];
    for (let i = 0; i < PADS; i++)
        pads.push({
            tune: .5,
            decay: .5,
            tone: .5,
            snappy: .55,
            tail: .45,
            level: .75,
            pan: .5,
            mute: false,
            solo: false
        });
    pads[0].decay = .55;
    pads[0].tone = .45;
    return {
        format: "XU808", version: 1,
        name: "徐某的 808 工程", seed: 0x808808, current: 0,
        master: {
            volume: .8, drive: 1.35, lpCutoff: 18000, lpQ: .9, hpCutoff: 28, hpQ: .7,
            lpOn: true, hpOn: true, bpm: 120, swing: 0, beatsPerBar: 4, beatUnit: 4, songMode: false
        },
        pads, patterns,
        song: {enabled: false, entries: [{pattern: 0, repeats: 4}, {pattern: 0, repeats: 4}]}
    };
}

/** 加载旧工程时补齐缺失字段，绝不因为缺一个键就崩溃。 */
function normalizeProject(p) {
    const base = makeProject();
    if (!p || typeof p !== "object") return base;
    const out = Object.assign(base, p);
    out.master = Object.assign(base.master, p.master || {});
    out.pads = (p.pads && p.pads.length ? p.pads : base.pads).map(x => Object.assign({}, base.pads[0], x));
    out.patterns = (p.patterns && p.patterns.length ? p.patterns : base.patterns).map((pat, i) => {
        const q = makePattern(i);
        q.name = pat.name || q.name;
        q.steps = Math.max(1, Math.min(64, pat.steps || 16));
        q.fill = !!pat.fill;
        if (Array.isArray(pat.trackLen)) for (let t = 0; t < PADS; t++) q.trackLen[t] = Math.max(1, Math.min(64, pat.trackLen[t] || 16));
        if (Array.isArray(pat.data))
            for (let pad = 0; pad < PADS; pad++)
                for (let s = 0; s < MAX_STEPS; s++) {
                    const src = pat.data[pad] && pat.data[pad][s];
                    if (!src) continue;
                    q.data[pad][s] = {
                        on: !!src.on, velocity: Math.max(1, Math.min(127, src.velocity ?? 100)),
                        accent: !!src.accent, prob: Math.max(0, Math.min(100, src.prob ?? 100)),
                        micro: Math.max(-2400, Math.min(2400, src.micro ?? 0)),
                        ratchet: Math.max(1, Math.min(8, src.ratchet ?? 1)),
                        cond: Math.max(0, Math.min(4, src.cond ?? 0))
                    };
                }
        return q;
    });
    if (!out.song || !Array.isArray(out.song.entries)) out.song = {enabled: false, entries: [{pattern: 0, repeats: 4}]};
    out.current = Math.max(0, Math.min(15, out.current || 0));
    return out;
}

// PHP 版：工程由服务端嵌入到 window.__INITIAL_PROJECT__，首屏即用
let project = normalizeProject(window.__INITIAL_PROJECT__ || makeProject());

// ---------------------------------------------------------------------------
// 音频
// ---------------------------------------------------------------------------
let ctx = null;
let node = null;
let audioOk = false;
const cellEls = [];
let playhead = -1;

async function initAudio() {
    try {
        ctx = new AudioContext({latencyHint: "interactive", sampleRate: SR});
        await ctx.audioWorklet.addModule("public/js/worklet.js");
        node = new AudioWorkletNode(ctx, "xu808", {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: {project}
        });
        node.connect(ctx.destination);
        node.port.onmessage = (ev) => {
            const m = ev.data;
            if (!m) return;
            if (m.type === "state") {
                playhead = m.playing ? m.step : -1;
                updatePlayhead();
                setMeter(m.meterL, m.meterR);
                document.getElementById("status").textContent = "CPU - · XRUN " + (m.xruns || 0);
            } else if (m.type === "ready") {
                document.getElementById("status").textContent = "音频就绪 · 48kHz";
            }
        };
        audioOk = true;
        document.getElementById("status").textContent = "音频就绪 · 48kHz";
    } catch (e) {
        audioOk = false;
        document.getElementById("status").textContent = "音频不可用（仍可离线导出）";
        console.error(e);
    }
}

function post(msg) {
    if (node) node.port.postMessage(msg);
}

function sendPattern() {
    post({type: "pattern", index: project.current, pattern: project.patterns[project.current]});
}

function sendPads() {
    post({type: "pads", pads: project.pads});
}

function sendMaster() {
    post({type: "master", master: project.master});
}

function sendProject() {
    post({type: "project", project});
}

// ---------------------------------------------------------------------------
// 网格
// ---------------------------------------------------------------------------
function cellWidth() {
    const steps = project.patterns[project.current].steps;
    return Math.max(10, Math.min(30, Math.floor((window.innerWidth - 180) / steps)));
}

function buildGrid() {
    const grid = document.getElementById("grid");
    grid.innerHTML = "";
    cellEls.length = 0;
    const pat = project.patterns[project.current];
    document.documentElement.style.setProperty("--cw", cellWidth() + "px");

    const head = document.createElement("div");
    head.className = "grid-head";
    const lab = document.createElement("div");
    lab.className = "lab";
    head.appendChild(lab);
    for (let c = 0; c < pat.steps; c++) {
        const n = document.createElement("div");
        n.className = "num" + (c % 4 === 0 ? " beat" : "");
        n.textContent = c + 1;
        head.appendChild(n);
    }
    grid.appendChild(head);

    for (let pad = 0; pad < PADS; pad++) {
        const row = document.createElement("div");
        row.className = "grid-row";
        const label = document.createElement("div");
        label.className = "pad-label";

        const code = document.createElement("span");
        code.className = "pad-code";
        code.textContent = CODES[pad];
        label.appendChild(code);

        const name = document.createElement("span");
        name.className = "pad-name";
        name.textContent = NAMES[pad];
        label.appendChild(name);

        for (let j = 0; j < 2; j++) {
            const ms = document.createElement("span");
            ms.className = "ms " + (j === 0 ? "m" : "s");
            ms.textContent = j === 0 ? "M" : "S";
            ms.title = j === 0 ? "静音" : "独奏";
            ms.addEventListener("click", (e) => {
                e.stopPropagation();
                const pd = project.pads[pad];
                if (j === 0) pd.mute = !pd.mute; else pd.solo = !pd.solo;
                ms.classList.toggle("on", j === 0 ? pd.mute : pd.solo);
                sendPads();
                buildStrips();
            });
            if (j === 0 ? project.pads[pad].mute : project.pads[pad].solo) ms.classList.add("on");
            label.appendChild(ms);
        }
        // 点击名字试听
        code.addEventListener("click", () => post({type: "trigger", pad, vel: 112, accent: false}));
        row.appendChild(label);

        const rowCells = [];
        for (let c = 0; c < pat.steps; c++) {
            const st = pat.data[pad][c];
            const cell = document.createElement("div");
            cell.className = "cell" + (c % 4 === 0 ? " beat" : "") + (st.on ? (st.accent ? " accent" : " on") : "");
            cell.dataset.pad = pad;
            cell.dataset.step = c;
            if (st.ratchet > 1) cell.textContent = "R" + st.ratchet;
            else if (st.prob < 100) cell.textContent = st.prob + "%";
            cell.addEventListener("click", () => {
                const s2 = pat.data[pad][c];
                s2.on = !s2.on;
                refreshCell(pad, c);
                if (s2.on) post({type: "trigger", pad, vel: s2.velocity, accent: s2.accent});
                sendPattern();
            });
            cell.addEventListener("contextmenu", (e) => {
                e.preventDefault();
                openStepModal(pad, c);
            });
            rowCells.push(cell);
            row.appendChild(cell);
        }
        cellEls.push(rowCells);
        grid.appendChild(row);
    }
    updatePlayhead();
}

function refreshCell(pad, step) {
    const st = project.patterns[project.current].data[pad][step];
    const el = cellEls[pad] && cellEls[pad][step];
    if (!el) return;
    el.classList.toggle("on", st.on && !st.accent);
    el.classList.toggle("accent", st.on && st.accent);
    el.textContent = st.ratchet > 1 ? "R" + st.ratchet : (st.prob < 100 ? st.prob + "%" : "");
}

function updatePlayhead() {
    if (playhead === -1) {
        for (const row of cellEls) for (const el of row) el.classList.remove("playhead");
        return;
    }
    for (let pad = 0; pad < cellEls.length; pad++) {
        const row = cellEls[pad];
        for (let c = 0; c < row.length; c++) row[c].classList.toggle("playhead", c === playhead);
    }
}

// ---------------------------------------------------------------------------
// 步进属性弹窗
// ---------------------------------------------------------------------------
let editing = {pad: 0, step: 0};

function openStepModal(pad, step) {
    editing = {pad, step};
    const st = project.patterns[project.current].data[pad][step];
    document.getElementById("modalTitle").textContent =
        `步进属性 — ${CODES[pad]} ${pad + 1}/${step + 1}`;
    document.getElementById("mOn").checked = st.on;
    document.getElementById("mAcc").checked = st.accent;
    document.getElementById("mVel").value = st.velocity;
    document.getElementById("mProb").value = st.prob;
    document.getElementById("mMicro").value = st.micro;
    document.getElementById("mRat").value = st.ratchet;
    const cond = document.getElementById("mCond");
    if (!cond.options.length) for (const n of COND_NAMES) cond.add(new Option(n, cond.length));
    cond.value = st.cond;
    document.getElementById("modal").classList.remove("hidden");
}

function initModal() {
    document.getElementById("mCancel").addEventListener("click", () =>
        document.getElementById("modal").classList.add("hidden"));
    document.getElementById("mOk").addEventListener("click", () => {
        const st = project.patterns[project.current].data[editing.pad][editing.step];
        st.on = document.getElementById("mOn").checked;
        st.accent = document.getElementById("mAcc").checked;
        st.velocity = Math.max(1, Math.min(127, +document.getElementById("mVel").value));
        st.prob = Math.max(0, Math.min(100, +document.getElementById("mProb").value));
        st.micro = Math.max(-2400, Math.min(2400, +document.getElementById("mMicro").value));
        st.ratchet = Math.max(1, Math.min(8, +document.getElementById("mRat").value));
        st.cond = +document.getElementById("mCond").value;
        refreshCell(editing.pad, editing.step);
        sendPattern();
        document.getElementById("modal").classList.add("hidden");
    });
    document.getElementById("modal").addEventListener("click", (e) => {
        if (e.target.id === "modal") e.currentTarget.classList.add("hidden");
    });
}

// ---------------------------------------------------------------------------
// 通道条
// ---------------------------------------------------------------------------
function paramsFor(pad) {
    if (pad === 0) return [["TUNE", "tune", 100], ["DECAY", "decay", 100], ["TONE", "tone", 100]];
    if (pad === 1) return [["TUNE", "tune", 100], ["SNAP", "snappy", 100], ["DECAY", "decay", 100]];
    if (pad >= 2 && pad <= 4) return [["TUNE", "tune", 100], ["DECAY", "decay", 100], ["COLOR", "tone", 100]];
    if (pad === 10) return [["DECAY", "decay", 100], ["TONE", "tone", 100], ["TAIL", "tail", 100]];
    if (pad === 11) return [["DECAY", "decay", 100], ["TONE", "tone", 100]];
    if (pad === 12) return [["TUNE", "tune", 100], ["DECAY", "decay", 100]];
    if (pad >= 13) return [["DECAY", "decay", 100], ["TONE", "tone", 100]];
    return [["TUNE", "tune", 100], ["DECAY", "decay", 100]];
}

function buildStrips() {
    const host = document.getElementById("strips");
    host.innerHTML = "";
    for (let pad = 0; pad < PADS; pad++) {
        const strip = document.createElement("div");
        strip.className = "strip";

        const name = document.createElement("button");
        name.className = "name";
        name.textContent = CODES[pad] + " " + NAMES[pad];
        name.addEventListener("click", () => post({type: "trigger", pad, vel: 112, accent: false}));
        strip.appendChild(name);

        const msRow = document.createElement("div");
        msRow.className = "ms-row";
        for (let j = 0; j < 2; j++) {
            const ms = document.createElement("span");
            ms.className = "ms " + (j === 0 ? "m" : "s") + ((j === 0 ? project.pads[pad].mute : project.pads[pad].solo) ? " on" : "");
            ms.textContent = j === 0 ? "M" : "S";
            ms.addEventListener("click", () => {
                const pd = project.pads[pad];
                if (j === 0) pd.mute = !pd.mute; else pd.solo = !pd.solo;
                ms.classList.toggle("on", j === 0 ? pd.mute : pd.solo);
                sendPads();
                buildGrid();
            });
            msRow.appendChild(ms);
        }
        strip.appendChild(msRow);

        const addSlider = (label, key, max) => {
            const l = document.createElement("span");
            l.className = "plabel";
            l.textContent = label;
            strip.appendChild(l);
            const s = document.createElement("input");
            s.type = "range";
            s.min = 0;
            s.max = max;
            s.value = Math.round(project.pads[pad][key] * max);
            s.className = "vert";
            s.addEventListener("input", () => {
                project.pads[pad][key] = +s.value / max;
                sendPads();
            });
            strip.appendChild(s);
        };
        for (const [label, key, max] of paramsFor(pad)) addSlider(label, key, max);
        addSlider("LEVEL", "level", 125);
        addSlider("PAN", "pan", 100);

        const len = document.createElement("input");
        len.type = "number";
        len.min = 1;
        len.max = 64;
        len.className = "len";
        len.value = project.patterns[project.current].trackLen[pad];
        len.title = "本轨循环长度（polymeter）";
        len.addEventListener("change", () => {
            project.patterns[project.current].trackLen[pad] = Math.max(1, Math.min(64, +len.value));
            sendPattern();
        });
        strip.appendChild(len);
        host.appendChild(strip);
    }
}

// ---------------------------------------------------------------------------
// 顶部与主效果
// ---------------------------------------------------------------------------
function initTop() {
    const bpm = document.getElementById("bpm");
    const bpmRange = document.getElementById("bpmRange");
    bpm.addEventListener("input", () => {
        project.master.bpm = Math.max(40, Math.min(300, +bpm.value || 120));
        bpmRange.value = project.master.bpm;
        sendMaster();
    });
    bpmRange.addEventListener("input", () => {
        bpm.value = bpmRange.value;
        project.master.bpm = +bpmRange.value;
        sendMaster();
    });

    document.getElementById("swing").addEventListener("input", (e) => {
        project.master.swing = +e.target.value / 100;
        document.getElementById("swingLabel").textContent = e.target.value + "%";
        sendMaster();
    });

    const sel = document.getElementById("pattern");
    for (let i = 0; i < 16; i++) sel.add(new Option(String(i + 1).padStart(2, "0") + " " + project.patterns[i].name, i));
    sel.value = project.current;
    sel.addEventListener("change", () => {
        project.current = +sel.value;
        post({type: "current", index: project.current});
        document.getElementById("steps").value = project.patterns[project.current].steps;
        buildGrid();
        buildStrips();
    });

    const steps = document.getElementById("steps");
    steps.value = project.patterns[project.current].steps;
    steps.addEventListener("change", () => {
        project.patterns[project.current].steps = Math.max(1, Math.min(64, +steps.value));
        buildGrid();
        sendPattern();
    });

    document.getElementById("songMode").addEventListener("change", (e) => {
        project.master.songMode = e.target.checked;
        project.song.enabled = e.target.checked;
        sendMaster();
    });

    document.getElementById("btnPlay").addEventListener("click", () => {
        if (ctx && ctx.state === "suspended") ctx.resume();
        post({type: "play"});
        document.getElementById("btnPlay").classList.add("on");
    });
    document.getElementById("btnStop").addEventListener("click", () => {
        post({type: "stop"});
        document.getElementById("btnPlay").classList.remove("on");
        playhead = -1;
        updatePlayhead();
    });

    document.getElementById("btnTap").addEventListener("click", () => {
        const now = performance.now() / 1000;
        if (window._taps && window._taps.length && now - window._taps[window._taps.length - 1] > 2) window._taps = [];
        window._taps = window._taps || [];
        window._taps.push(now);
        if (window._taps.length > 8) window._taps.shift();
        if (window._taps.length >= 2) {
            let iv = [];
            for (let i = 1; i < window._taps.length; i++) {
                const d = window._taps[i] - window._taps[i - 1];
                if (d > 0.15 && d < 2.0) iv.push(d);
            }
            if (iv.length >= 2) {
                iv.sort((a, b) => a - b);
                iv = iv.slice(1, -1);
            }
            if (!iv.length) return;
            const avg = iv.reduce((a, b) => a + b, 0) / iv.length;
            const v = Math.max(40, Math.min(300, 60 / avg));
            bpm.value = v.toFixed(1);
            bpmRange.value = v;
            project.master.bpm = v;
            sendMaster();
        }
    });

    document.getElementById("midiClock").addEventListener("change", (e) =>
        post({type: "clockExternal", value: e.target.checked}));

    document.getElementById("btnSong").addEventListener("click", () => {
        const lines = project.song.entries.map(e => (e.pattern + 1) + "," + e.repeats).join("\n");
        const s = prompt("歌曲链：每行“Pattern号,重复次数”", lines);
        if (s === null) return;
        const entries = [];
        for (const line of s.split(/\r?\n/)) {
            if (!line.trim()) continue;
            const [a, b] = line.split(",");
            entries.push({
                pattern: Math.max(0, Math.min(15, (parseInt(a) || 1) - 1)),
                repeats: Math.max(1, parseInt(b) || 1)
            });
        }
        project.song.entries = entries;
    });

    document.getElementById("btnSave").addEventListener("click", async () => {
        // PHP MVC：工程保存到服务端 data/project.json
        try {
            const res = await fetch("index.php?action=save", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify(project)
            });
            const j = await res.json();
            document.getElementById("status").textContent = j.ok ? "工程已保存到服务器" : "保存失败";
        } catch (e) {
            document.getElementById("status").textContent = "保存失败：" + e.message;
        }
    });
    document.getElementById("btnLoad").addEventListener("click", () =>
        document.getElementById("fileLoad").click());
    document.getElementById("fileLoad").addEventListener("change", (e) => {
        const f = e.target.files[0];
        if (!f) return;
        const r = new FileReader();
        r.onload = () => {
            try {
                project = normalizeProject(JSON.parse(r.result));
                sendProject();
                rebuildAll();
            } catch (err) {
                alert("工程文件格式错误：" + err.message);
            }
        };
        r.readAsText(f, "utf-8");
        e.target.value = "";
    });

    document.getElementById("btnWav").addEventListener("click", async () => {
        try {
            document.getElementById("status").textContent = "正在离线渲染…";
            const buffer = await renderOffline();
            download("xu808.wav", encodeWav(buffer));
            document.getElementById("status").textContent = "WAV 已导出";
        } catch (e) {
            alert("导出失败：" + e.message);
            document.getElementById("status").textContent = "导出失败";
        }
    });
    document.getElementById("btnMidi").addEventListener("click", async () => {
        // MIDI 由 PHP 服务端生成，浏览器只负责下载
        try {
            const res = await fetch("index.php?action=exportMidi", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify(project)
            });
            const blob = await res.blob();
            download("xu808.mid", blob);
            document.getElementById("status").textContent = "MIDI 已导出（服务端）";
        } catch (e) {
            alert("导出失败：" + e.message);
        }
    });
}

function initMaster() {
    const set = (id, valId, fn, fmt) => {
        const el = document.getElementById(id);
        const out = document.getElementById(valId);
        const update = () => {
            const v = +el.value;
            fn(v);
            out.textContent = fmt ? fmt(v) : v;
            sendMaster();
        };
        el.addEventListener("input", update);
        update();
    };
    set("lp", "lpV", v => project.master.lpCutoff = v);
    set("rez", "rezV", v => project.master.lpQ = v / 100, v => (v / 100).toFixed(2));
    set("hp", "hpV", v => project.master.hpCutoff = v);
    set("drive", "driveV", v => project.master.drive = v / 100, v => (v / 100).toFixed(2));
    set("vol", "volV", v => project.master.volume = v / 100);
}

function setMeter(l, r) {
    document.getElementById("meterL").style.width = Math.min(100, l * 100) + "%";
    document.getElementById("meterR").style.width = Math.min(100, r * 100) + "%";
}

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------
async function renderOffline() {
    const pat = project.patterns[project.current];
    let steps = pat.steps;
    if (project.master.songMode && project.song.entries.length) {
        steps = 0;
        for (const e of project.song.entries) {
            const p = project.patterns[Math.max(0, Math.min(15, e.pattern))];
            steps += p.steps * Math.max(1, e.repeats);
        }
    }
    const bpm = Math.max(40, Math.min(300, project.master.bpm));
    const stepSamples = SR * 60 / bpm / 4;
    const frames = Math.ceil(steps * stepSamples) + SR * 3;
    const off = new OfflineAudioContext(2, frames, SR);
    await off.audioWorklet.addModule("public/js/worklet.js");
    const n = new AudioWorkletNode(off, "xu808", {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: {
            project: JSON.parse(JSON.stringify(project)),
            autoStart: true, offline: true
        }
    });
    n.connect(off.destination);
    return await off.startRendering();
}

function encodeWav(buffer) {
    const ch = buffer.numberOfChannels;
    const frames = buffer.length;
    const data = new DataView(new ArrayBuffer(44 + frames * ch * 2));
    const ws = (o, s) => {
        for (let i = 0; i < s.length; i++) data.setUint8(o + i, s.charCodeAt(i));
    };
    ws(0, "RIFF");
    data.setUint32(4, 36 + frames * ch * 2, true);
    ws(8, "WAVE");
    ws(12, "fmt ");
    data.setUint32(16, 16, true);
    data.setUint16(20, 1, true);
    data.setUint16(22, ch, true);
    data.setUint32(24, SR, true);
    data.setUint32(28, SR * ch * 2, true);
    data.setUint16(32, ch * 2, true);
    data.setUint16(34, 16, true);
    ws(36, "data");
    data.setUint32(40, frames * ch * 2, true);
    const chans = [];
    for (let c = 0; c < ch; c++) chans.push(buffer.getChannelData(c));
    let o = 44;
    for (let i = 0; i < frames; i++) {
        for (let c = 0; c < ch; c++) {
            const x = Math.max(-1, Math.min(1, chans[c][i]));
            data.setInt16(o, x * 32767, true);
            o += 2;
        }
    }
    return new Blob([data.buffer], {type: "audio/wav"});
}

function vlqBytes(v) {
    v = Math.max(0, v);
    const out = [v & 0x7F];
    v >>= 7;
    while (v > 0) {
        out.unshift((v & 0x7F) | 0x80);
        v >>= 7;
    }
    return out;
}

function exportMidiBlob() {
    const ppq = 480;
    const bpm = Math.max(40, Math.min(300, project.master.bpm));
    const stepSamples = SR * 60 / bpm / 4;
    const seq = [];
    if (project.master.songMode && project.song.entries.length) {
        for (const e of project.song.entries)
            for (let k = 0; k < Math.max(1, e.repeats); k++)
                seq.push(Math.max(0, Math.min(15, e.pattern)));
    } else seq.push(project.current);

    const notes = [];
    let cursor = 0;
    for (const pi of seq) {
        const pat = project.patterns[pi];
        for (let s = 0; s < pat.steps; s++) {
            const swingTick = (s % 2) ? Math.round(project.master.swing * 60) : 0;
            for (let pad = 0; pad < PADS; pad++) {
                const tl = Math.max(1, Math.min(64, pat.trackLen[pad]));
                const idx = (cursor + s) % tl;
                const st = pat.data[pad][idx];
                if (!st.on || st.prob <= 0) continue;
                const vel = Math.max(1, Math.min(127, Math.round(st.velocity * (st.accent ? 1.3 : 1))));
                const base = (cursor + s) * 120;
                const micro = Math.round(st.micro / stepSamples * 120);
                for (let i = 0; i < Math.max(1, Math.min(8, st.ratchet)); i++) {
                    notes.push({
                        tick: Math.max(0, base + swingTick + micro + Math.round(i * 120 / st.ratchet)),
                        note: MIDI_NOTES[pad], vel
                    });
                }
            }
        }
        cursor += pat.steps;
    }
    notes.sort((a, b) => a.tick - b.tick || a.note - b.note);

    const tr = [];
    const push = (...b) => tr.push(...b);
    push(...vlqBytes(0), 0xFF, 0x03, ...vlqBytes(11), ...[...("XU-808 by XuMou")].map(c => c.charCodeAt(0)));
    const tempo = Math.round(60000000 / bpm);
    push(...vlqBytes(0), 0xFF, 0x51, 0x03, (tempo >> 16) & 255, (tempo >> 8) & 255, tempo & 255);
    push(...vlqBytes(0), 0xFF, 0x58, 0x04, project.master.beatsPerBar,
        Math.round(Math.log2(Math.max(1, project.master.beatUnit))), 24, 8);
    let last = 0;
    for (const n of notes) {
        push(...vlqBytes(n.tick - last), 0x99, n.note, n.vel);
        push(...vlqBytes(10), 0x89, n.note, 0x40);
        last = n.tick + 10;
    }
    push(...vlqBytes(0), 0xFF, 0x2F, 0x00);

    const track = new Uint8Array(tr);
    const bytes = [];
    const ascii = s => [...s].map(c => c.charCodeAt(0));
    bytes.push(...ascii("MThd"));
    bytes.push(0, 0, 0, 6, 0, 1, 0, 1, (ppq >> 8) & 255, ppq & 255);
    bytes.push(...ascii("MTrk"));
    bytes.push((track.length >>> 24) & 255, (track.length >>> 16) & 255, (track.length >>> 8) & 255, track.length & 255);
    bytes.push(...track);
    return new Blob([new Uint8Array(bytes)], {type: "audio/midi"});
}

function download(name, blob) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
        URL.revokeObjectURL(a.href);
        a.remove();
    }, 1500);
}

// ---------------------------------------------------------------------------
// 键盘与 MIDI
// ---------------------------------------------------------------------------
function initKeys() {
    const keys = "ZXCVBNMASDFGHJKL";
    window.addEventListener("keydown", (e) => {
        if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "SELECT")) return;
        if (e.code === "Space") {
            e.preventDefault();
            if (node) post({type: playhead === -1 ? "play" : "stop"});
            return;
        }
        if (/^Digit[1-8]$/.test(e.code)) {
            const i = +e.code.slice(5) - 1;
            const sel = document.getElementById("pattern");
            sel.value = i;
            sel.dispatchEvent(new Event("change"));
            return;
        }
        const c = e.key.toUpperCase();
        const idx = keys.indexOf(c);
        if (idx >= 0) post({type: "trigger", pad: idx, vel: 110, accent: false});
    });
}

async function initWebMidi() {
    const label = document.getElementById("midiStatus");
    if (!navigator.requestMIDIAccess) {
        label.textContent = "MIDI: 不支持";
        return;
    }
    try {
        const access = await navigator.requestMIDIAccess({sysex: false});
        const attach = () => {
            let any = false;
            access.inputs.forEach(input => {
                any = true;
                input.onmidimessage = (m) => {
                    const d = m.data;
                    if (d.length < 3) {
                        if (d[0] === 0xF8) post({type: "midiClock"});
                        else if (d[0] === 0xFA) post({type: "play"});
                        else if (d[0] === 0xFC) post({type: "stop"});
                        return;
                    }
                    const cmd = d[0] & 0xF0;
                    if (cmd === 0x90 && d[2] > 0) {
                        const pad = MIDI_NOTES.indexOf(d[1]);
                        if (pad >= 0) post({type: "trigger", pad, vel: d[2], accent: d[2] >= 120});
                    } else if (cmd === 0xB0) {
                        if (d[1] === 7) {
                            project.master.volume = d[2] / 127;
                            sendMaster();
                        } else if (d[1] === 74) {
                            project.master.lpCutoff = 20 * Math.pow(1000, d[2] / 127);
                            sendMaster();
                        }
                    } else if (cmd === 0xC0) {
                        project.current = d[1] % 16;
                        post({type: "current", index: project.current});
                    }
                };
            });
            label.textContent = any ? "MIDI: 已连接" : "MIDI: 无设备";
            label.style.color = any ? "#ff7a18" : "#8a8a8a";
        };
        attach();
        access.onstatechange = attach;
    } catch (e) {
        label.textContent = "MIDI: 不可用";
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
function rebuildAll() {
    buildGrid();
    buildStrips();
    const sel = document.getElementById("pattern");
    sel.value = project.current;
    document.getElementById("steps").value = project.patterns[project.current].steps;
    document.getElementById("bpm").value = project.master.bpm;
    document.getElementById("bpmRange").value = project.master.bpm;
    document.getElementById("swing").value = Math.round(project.master.swing * 100);
    document.getElementById("swingLabel").textContent = Math.round(project.master.swing * 100) + "%";
    document.getElementById("songMode").checked = project.master.songMode;
}

window.addEventListener("resize", () => {
    document.documentElement.style.setProperty("--cw", cellWidth() + "px");
});

initModal();
initTop();
initMaster();
buildGrid();
buildStrips();
initKeys();
initWebMidi();
initAudio();
