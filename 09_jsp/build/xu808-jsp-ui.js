/* ============================================================================
 * xu808-ui.js —— 浏览器端界面逻辑（09_jsp / 10_javaee 共用）
 * ----------------------------------------------------------------------------
 * 徐某把这一层定位成“乐谱与面板”：
 *   · 不参与任何音频样本运算，所有声音都在 AudioWorklet 的音频线程里；
 *   · 只维护一份与 XuProject.toMap() 同构的 state，任何编辑都
 *     (a) 改 state，(b) 发一条最小补丁给 worklet，(c) 重画那一个格子/滑块；
 *   · 导出时把整份 state POST 给服务端，服务端用同一套 Java 引擎离线渲染。
 * 这样“看到的”“听到的”“导出的”三者共享同一份工程语义。
 * ============================================================================ */
'use strict';

(function () {
    /* ------------------------------ 基础常量 ------------------------------ */
    const BASE = window.XU808_BASE || '';                  // WAR 上下文路径，例如 /xu808-jsp
    const INIT = window.XU808_INIT || {};
    const VERSION = window.XU808_VERSION || 'web';
    const $ = (id) => document.getElementById(id);         // 省得到处写 document.getElementById

    // 步进条件与规格书 XuProject.COND_* 一一对应
    const COND_NAMES = ['总是', '仅过门', '非过门', '每 2 循环', '每 4 循环'];
    const COND_TAGS = ['', 'F', 'N', '2', '4'];            // 格子里的小标记

    // 每个声部实际暴露哪些音色参数；Level/Pan 所有声部都有，另外追加
    // 顺序与规格书第 3 节一致：BD 有 Tune/Decay/Tone，SD 用 Snappy，CP 用 Tail……
    const PAD_PARAM_MAP = [
        ['tune', 'decay', 'tone'],          // 0 BD
        ['tune', 'snappy', 'decay'],        // 1 SD
        ['tune', 'decay', 'tone'],          // 2 LT
        ['tune', 'decay', 'tone'],          // 3 MT
        ['tune', 'decay', 'tone'],          // 4 HT
        ['tune', 'decay', 'tone'],          // 5 LC
        ['tune', 'decay', 'tone'],          // 6 MC
        ['tune', 'decay', 'tone'],          // 7 HC
        ['tune', 'decay'],                  // 8 RS
        ['tune', 'decay'],                  // 9 CL
        ['decay', 'tone', 'tail'],          // 10 CP
        ['decay', 'tone'],                  // 11 MA
        ['tune', 'decay', 'tone'],          // 12 CB
        ['decay', 'tone'],                  // 13 CY
        ['decay', 'tone'],                  // 14 OH
        ['decay', 'tone']                   // 15 CH
    ];
    const PARAM_LABELS = {
        tune: 'Tune 调音', decay: 'Decay 衰减', tone: 'Tone 音色',
        snappy: 'Snappy 噪声', tail: 'Tail 尾巴', level: 'Level 音量', pan: 'Pan 声像'
    };

    /* ------------------------------ 状态 ------------------------------ */
    /** 深拷贝，避免直接改到 window.XU808_INIT（刷新后还能回到服务端初值） */
    function clone(o) {
        return JSON.parse(JSON.stringify(o));
    }

    function clamp(x, lo, hi) {
        return x < lo ? lo : (x > hi ? hi : x);
    }

    /** 给缺字段的工程补齐默认值；服务端永远给全量，这里主要防“手工改坏的 JSON” */
    function normalize(raw) {
        const s = (raw && typeof raw === 'object') ? raw : {};
        s.master = Object.assign({
            volume: 0.8, drive: 1.35, lpCutoff: 18000, lpQ: 0.9,
            hpCutoff: 28, hpQ: 0.7, lpOn: true, hpOn: true,
            bpm: 120, swing: 0, beatsPerBar: 4, beatUnit: 4, songMode: false
        }, s.master || {});
        s.master.bpm = clamp(Number(s.master.bpm) || 120, 40, 300);
        s.master.beatsPerBar = clamp(parseInt(s.master.beatsPerBar, 10) || 4, 1, 16);
        s.master.beatUnit = (parseInt(s.master.beatUnit, 10) === 8) ? 8 : 4;
        // swing 在工程里存 0–0.75 的小数；UI 显示时再 ×100
        s.master.swing = clamp(Number(s.master.swing) || 0, 0, 0.75);

        s.pads = Array.isArray(s.pads) ? s.pads : [];
        while (s.pads.length < 16) {
            s.pads.push({
                tune: 0.5, decay: 0.5, tone: 0.5, snappy: 0.55, tail: 0.45,
                level: 0.75, pan: 0.5, mute: false, solo: false
            });
        }
        s.pads.forEach((p) => {
            p.tune = Number(p.tune ?? 0.5);
            p.decay = Number(p.decay ?? 0.5);
            p.tone = Number(p.tone ?? 0.5);
            p.snappy = Number(p.snappy ?? 0.55);
            p.tail = Number(p.tail ?? 0.45);
            p.level = Number(p.level ?? 0.75);
            p.pan = Number(p.pan ?? 0.5);
            p.mute = !!p.mute;
            p.solo = !!p.solo;
        });

        s.patterns = Array.isArray(s.patterns) ? s.patterns : [];
        while (s.patterns.length < 16) s.patterns.push({});
        s.patterns.forEach((pat, i) => {
            pat.name = pat.name || ('PATTERN ' + (i + 1));
            pat.steps = clamp(parseInt(pat.steps, 10) || 16, 1, 64);
            pat.trackLen = Array.isArray(pat.trackLen) ? pat.trackLen : [];
            while (pat.trackLen.length < 16) pat.trackLen.push(pat.steps);
            for (let p = 0; p < 16; p++) pat.trackLen[p] = clamp(parseInt(pat.trackLen[p], 10) || 16, 1, 64);
            pat.data = Array.isArray(pat.data) ? pat.data : [];
            for (let p = 0; p < 16; p++) {
                if (!Array.isArray(pat.data[p])) pat.data[p] = [];
                for (let st = 0; st < 64; st++) {
                    let one = pat.data[p][st];
                    if (!Array.isArray(one) || one.length < 7) one = [0, 100, 0, 100, 0, 1, 0];
                    // 统一成七元数组：on / velocity / accent / probability / micro / ratchet / condition
                    pat.data[p][st] = [
                        one[0] ? 1 : 0,
                        clamp(parseInt(one[1], 10) || 100, 1, 127),
                        one[2] ? 1 : 0,
                        clamp(parseInt(one[3], 10), 0, 100) || (one[3] === 0 ? 0 : 100),
                        clamp(parseInt(one[4], 10) || 0, -2400, 2400),
                        clamp(parseInt(one[5], 10) || 1, 1, 8),
                        clamp(parseInt(one[6], 10) || 0, 0, 4)
                    ];
                }
            }
        });

        s.song = s.song || {enabled: false, entries: []};
        if (!Array.isArray(s.song.entries)) s.song.entries = [];
        s.song.entries.forEach((e) => {
            e.pattern = clamp(parseInt(e.pattern, 10) || 0, 0, 15);
            e.repeats = Math.max(1, parseInt(e.repeats, 10) || 1);
        });
        s.current = clamp(parseInt(s.current, 10) || 0, 0, 15);
        s.name = s.name || '徐某的 808 工程';
        if (typeof s.seed !== 'number') s.seed = 0x808808;
        return s;
    }

    const state = normalize(clone(INIT));
    let selectedPad = 0;
    let playing = false;
    let lastPlayhead = -1;
    const cellEls = [];          // cellEls[pad][step] -> button，播放头只改这两级数组

    /* ------------------------------ 音频 ------------------------------ */
    let ctx = null;              // AudioContext
    let node = null;             // AudioWorkletNode
    let booting = null;          // 防止连点播放时重复 addModule

    /** 把一条消息送进音频线程；没启动时静默丢弃（启动后会整份重发工程） */
    function send(msg) {
        if (node) node.port.postMessage(msg);
    }

    /** 首次用户手势里创建 AudioContext：浏览器自动播放策略要求必须这么做 */
    async function ensureAudio() {
        if (ctx) {
            if (ctx.state === 'suspended') await ctx.resume();
            return;
        }
        if (booting) return booting;
        booting = (async () => {
            const AC = window.AudioContext || window.webkitAudioContext;
            if (!AC || !('audioWorklet' in AC.prototype)) {
                throw new Error('当前浏览器不支持 AudioWorklet，请使用新版 Chrome/Edge/Firefox');
            }
            ctx = new AC({sampleRate: 48000, latencyHint: 'interactive'});
            // 徐某：worklet 里 128 帧一个量子，主线程只负责 UI；
            await ctx.audioWorklet.addModule(BASE + '/js/xu808-worklet.js');
            node = new AudioWorkletNode(ctx, 'xu808-engine', {
                numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]
            });
            node.port.onmessage = onWorklet;
            node.connect(ctx.destination);
            // 启动瞬间把完整工程同步进去，之后只发补丁
            send({type: 'project', project: state});
            setStatus('音频已启动 · ' + Math.round(ctx.sampleRate) + ' Hz · AudioWorklet', 'ok');
        })();
        try {
            await booting;
        } catch (err) {
            // addModule 失败（路径错、MIME 错）时要把 ctx 清掉，否则第二次点击只会 resume 一个空图
            ctx = null;
            node = null;
            throw err;
        } finally {
            booting = null;
        }
    }

    /** 接收音频线程回传：播放头、电平、错误 */
    function onWorklet(e) {
        const m = e.data;
        if (!m || !m.type) return;
        if (m.type === 'pos') {
            paintMeters(m.meterL, m.meterR);
            setPlayhead(m.playing ? m.step : -1);
            if (m.playing !== playing) {
                playing = m.playing;
                updateTransport();
            }
        } else if (m.type === 'error') {
            setStatus('音频错误：' + m.message, 'err');
        } else if (m.type === 'ready') {
            // worklet 装载完成，这里不发数据（工程在 ensureAudio 里已经发过）
        }
    }

    /** 播放：先确保音频图存在，再让音频线程从当前 samplePos 起步 */
    async function play() {
        try {
            await ensureAudio();
            await ctx.resume();
            send({type: 'play'});
            playing = true;
            updateTransport();
            setStatus('播放中 · BPM ' + Math.round(state.master.bpm) + ' · ' + VERSION, 'ok');
        } catch (err) {
            setStatus('无法启动音频：' + (err && err.message ? err.message : err), 'err');
        }
    }

    function stop() {
        send({type: 'stop'});
        playing = false;
        updateTransport();
        setPlayhead(-1);
        setStatus('已停止', '');
    }

    function updateTransport() {
        $('btnPlay').textContent = playing ? '▶ 播放中' : '▶ 播放';
        $('btnStop').disabled = !playing;
    }

    function paintMeters(l, r) {
        $('meterL').style.width = Math.round(clamp(l, 0, 1) * 100) + '%';
        $('meterR').style.width = Math.round(clamp(r, 0, 1) * 100) + '%';
    }

    function setStatus(text, cls) {
        const el = $('status');
        el.textContent = text;
        el.className = 'status' + (cls ? ' ' + cls : '');
    }

    /* ------------------------------ 网格渲染 ------------------------------ */
    function curPattern() {
        return state.patterns[state.current];
    }

    /** 重建整张网格：步数/Pattern 改变时调用；平时只改单个格子 */
    function rebuildGrid() {
        const pat = curPattern();
        const steps = pat.steps;

        // 1) 列号表头
        const head = $('gridHead');
        head.innerHTML = '';
        const spacer = document.createElement('div');
        spacer.className = 'headSpacer';
        head.appendChild(spacer);
        const nums = document.createElement('div');
        nums.className = 'stepNums';
        for (let s = 0; s < steps; s++) {
            const n = document.createElement('div');
            n.className = 'stepNum' + (s % 4 === 0 ? ' beat' : '');
            n.textContent = String(s + 1);
            n.dataset.step = String(s);
            nums.appendChild(n);
        }
        head.appendChild(nums);

        // 2) 16 行格子
        for (let pad = 0; pad < 16; pad++) {
            const row = document.querySelector('.trackRow[data-pad="' + pad + '"]');
            if (!row) continue;
            row.classList.toggle('selected', pad === selectedPad);
            row.querySelector('.msBtn.mute').classList.toggle('on', !!state.pads[pad].mute);
            row.querySelector('.msBtn.solo').classList.toggle('on', !!state.pads[pad].solo);
            row.querySelector('.lenInput').value = pat.trackLen[pad];

            const box = row.querySelector('.steps');
            box.innerHTML = '';
            cellEls[pad] = [];
            for (let s = 0; s < steps; s++) {
                const btn = document.createElement('button');
                btn.type = 'button';
                btn.className = 'cell';
                btn.dataset.pad = String(pad);
                btn.dataset.step = String(s);
                btn.setAttribute('aria-label', '声部 ' + pad + ' 第 ' + (s + 1) + ' 步');
                btn.addEventListener('click', () => toggleStep(pad, s));
                btn.addEventListener('contextmenu', (ev) => {
                    ev.preventDefault();               // 右键不弹系统菜单，改弹步进属性
                    openStepDialog(pad, s);
                });
                box.appendChild(btn);
                cellEls[pad][s] = btn;
            }
            for (let s = 0; s < steps; s++) paintCell(pad, s);
        }

        $('stepsSel').value = String(steps);
        // 若工程来自其它平台且步数不是 16/32/64（例如 12），临时补一个选项，避免下拉框显示空白
        if ($('stepsSel').value !== String(steps)) {
            const o = document.createElement('option');
            o.value = String(steps);
            o.textContent = String(steps);
            $('stepsSel').appendChild(o);
            $('stepsSel').value = String(steps);
        }
        if (lastPlayhead >= steps) lastPlayhead = -1;
    }

    /** 只重画一个格子：开关、重音、概率点、微时三角、ratchet 数字、条件字母 */
    function paintCell(pad, step) {
        const btn = cellEls[pad] && cellEls[pad][step];
        if (!btn) return;
        const st = curPattern().data[pad][step];
        btn.className = 'cell' + (step % 4 === 0 ? ' beat' : '');
        if (st[0]) btn.classList.add('on');
        if (st[2]) btn.classList.add('accent');
        if (st[3] < 100) btn.classList.add('prob');
        if (st[4] !== 0) btn.classList.add('micro');
        if (step === lastPlayhead) btn.classList.add('playhead');

        let html = '';
        if (st[6] !== 0) html += '<span class="cond">' + COND_TAGS[st[6]] + '</span>';
        if (st[5] > 1) html += '<span class="ratchet">' + st[5] + 'x</span>';
        btn.innerHTML = html;
        btn.title = '力度 ' + st[1] + (st[2] ? ' · 重音' : '') +
            ' · 概率 ' + st[3] + '%' + (st[4] ? ' · 微时 ' + st[4] : '') +
            (st[5] > 1 ? ' · ratchet ' + st[5] : '') +
            ' · ' + COND_NAMES[st[6]];
    }

    /** 左键：开/关当前格子，并立刻把这条步进数据同步到音频线程 */
    function toggleStep(pad, step) {
        const st = curPattern().data[pad][step];
        st[0] = st[0] ? 0 : 1;
        sendStep(pad, step, st);
        paintCell(pad, step);
    }

    function sendStep(pad, step, st) {
        send({
            type: 'step',
            pattern: state.current,
            pad: pad,
            step: step,
            data: st.slice(0, 7)          // 只送七个字段，别把 UI 临时属性带过去
        });
    }

    /** 播放头：整列加 .playhead；切换列时先清旧列，避免遍历 1024 个格子 */
    function setPlayhead(step) {
        if (step === lastPlayhead) return;
        if (lastPlayhead >= 0) {
            for (let pad = 0; pad < 16; pad++) {
                const b = cellEls[pad] && cellEls[pad][lastPlayhead];
                if (b) b.classList.remove('playhead');
            }
            const oldNum = document.querySelector('.stepNum[data-step="' + lastPlayhead + '"]');
            if (oldNum) oldNum.classList.remove('playhead');
        }
        lastPlayhead = step;
        if (step >= 0) {
            for (let pad = 0; pad < 16; pad++) {
                const b = cellEls[pad] && cellEls[pad][step];
                if (b) b.classList.add('playhead');
            }
            const num = document.querySelector('.stepNum[data-step="' + step + '"]');
            if (num) num.classList.add('playhead');
        }
    }

    /* ------------------------------ 步进属性对话框 ------------------------------ */
    let dlgRef = null;   // {pad, step}

    function openStepDialog(pad, step) {
        const st = curPattern().data[pad][step];
        dlgRef = {pad: pad, step: step};
        $('stepTitle').textContent = PAD_CODES[pad] + ' · 第 ' + (step + 1) + ' 步属性';
        $('stOn').checked = !!st[0];
        $('stVel').value = st[1];
        $('stVelVal').textContent = String(st[1]);
        $('stAccent').checked = !!st[2];
        $('stProb').value = st[3];
        $('stProbVal').textContent = st[3] + '%';
        $('stMicro').value = st[4];
        $('stMicroVal').textContent = String(st[4]);
        $('stRatchet').value = String(st[5]);
        $('stCond').value = String(st[6]);
        if (typeof $('stepDlg').showModal === 'function') $('stepDlg').showModal();
        else $('stepDlg').setAttribute('open', 'open');
    }

    function applyStepDialog() {
        if (!dlgRef) return;
        const st = curPattern().data[dlgRef.pad][dlgRef.step];
        st[0] = $('stOn').checked ? 1 : 0;
        st[1] = clamp(parseInt($('stVel').value, 10) || 100, 1, 127);
        st[2] = $('stAccent').checked ? 1 : 0;
        st[3] = clamp(parseInt($('stProb').value, 10), 0, 100);
        st[4] = clamp(parseInt($('stMicro').value, 10) || 0, -2400, 2400);
        st[5] = clamp(parseInt($('stRatchet').value, 10) || 1, 1, 8);
        st[6] = clamp(parseInt($('stCond').value, 10) || 0, 0, 4);
        sendStep(dlgRef.pad, dlgRef.step, st);
        paintCell(dlgRef.pad, dlgRef.step);
    }

    /* ------------------------------ 声部参数条 ------------------------------ */
    function selectPad(pad) {
        selectedPad = pad;
        document.querySelectorAll('.trackRow').forEach((row) => {
            row.classList.toggle('selected', Number(row.dataset.pad) === pad);
        });
        buildPadInspector();
    }

    /** 按声部类型动态生成滑块，避免给军鼓显示“Tail”这种无意义参数 */
    function buildPadInspector() {
        const pad = selectedPad;
        const p = state.pads[pad];
        $('padTitle').textContent = PAD_CODES[pad] + ' · ' + PAD_NAMES[pad];
        $('padMute').classList.toggle('on', !!p.mute);
        $('padSolo').classList.toggle('on', !!p.solo);
        $('padMute').textContent = p.mute ? 'Mute 开' : 'Mute';
        $('padSolo').textContent = p.solo ? 'Solo 开' : 'Solo';

        const box = $('padParams');
        box.innerHTML = '';
        const keys = PAD_PARAM_MAP[pad].concat(['level', 'pan']);
        keys.forEach((key) => {
            const label = document.createElement('label');
            label.className = 'field';
            const text = document.createElement('span');
            text.textContent = PARAM_LABELS[key] || key;
            const val = document.createElement('span');
            val.className = 'val';
            val.textContent = Number(p[key]).toFixed(2);
            const input = document.createElement('input');
            input.type = 'range';
            input.className = 'slider';
            input.min = '0';
            input.max = '1';
            input.step = '0.01';
            input.value = String(p[key]);
            input.addEventListener('input', () => {
                const v = clamp(parseFloat(input.value) || 0, 0, 1);
                p[key] = v;
                val.textContent = v.toFixed(2);
                send({type: 'pad', pad: pad, key: key, value: v});
            });
            label.appendChild(text);
            label.appendChild(val);
            label.appendChild(input);
            box.appendChild(label);
        });
    }

    /* ------------------------------ 主效果与传输控件 ------------------------------ */
    function setBpm(bpm) {
        const v = clamp(Math.round(bpm), 40, 300);
        state.master.bpm = v;
        $('bpm').value = String(v);
        $('bpmRange').value = String(v);
        send({type: 'master', key: 'bpm', value: v});
    }

    function bindMaster() {
        // ---- BPM 数字框与滑杆双向同步 ----
        $('bpmRange').addEventListener('input', () => setBpm(parseInt($('bpmRange').value, 10)));
        $('bpm').addEventListener('change', () => setBpm(parseInt($('bpm').value, 10)));

        // ---- Swing：UI 用百分数 0–75，工程里存 0–0.75 ----
        $('swingRange').addEventListener('input', () => {
            const pct = clamp(parseInt($('swingRange').value, 10) || 0, 0, 75);
            state.master.swing = pct / 100;
            $('swingVal').textContent = pct + '%';
            send({type: 'master', key: 'swing', value: state.master.swing});
        });

        // ---- Tap Tempo：最近 8 次点击间隔，去掉最大最小后平均 ----
        let taps = [];
        $('btnTap').addEventListener('click', () => {
            const now = performance.now();
            if (taps.length && now - taps[taps.length - 1] > 3000) taps = [];   // 停顿 3 秒重新开始
            taps.push(now);
            if (taps.length > 9) taps.shift();                                  // 8 个间隔需要 9 次点击
            if (taps.length < 2) {
                setStatus('Tap：继续点击…', '');
                return;
            }
            const bpms = [];
            for (let i = 1; i < taps.length; i++) {
                const b = 60000 / (taps[i] - taps[i - 1]);
                if (b >= 30 && b <= 400) bpms.push(b);                            // 丢弃异常值
            }
            if (!bpms.length) return;
            bpms.sort((a, b) => a - b);
            if (bpms.length > 2) bpms.splice(0, 1), bpms.splice(bpms.length - 1, 1);  // 去掉最大最小
            const avg = bpms.reduce((a, b) => a + b, 0) / bpms.length;
            setBpm(avg);
            setStatus('Tap 测得 ' + Math.round(avg) + ' BPM', 'ok');
        });

        // ---- Pattern 选择：播放中由 worklet 在下一步边界切换 ----
        $('patternSel').addEventListener('change', () => {
            state.current = clamp(parseInt($('patternSel').value, 10) || 0, 0, 15);
            send({type: 'pattern', index: state.current});
            rebuildGrid();
            buildPadInspector();
        });

        // ---- 步数：1–64，默认 16/32/64 三档 ----
        $('stepsSel').addEventListener('change', () => {
            const pat = curPattern();
            pat.steps = clamp(parseInt($('stepsSel').value, 10) || 16, 1, 64);
            send({type: 'steps', value: pat.steps});
            rebuildGrid();
        });

        // ---- Song 开关与编排 ----
        $('songMode').addEventListener('change', () => {
            const on = $('songMode').checked;
            state.master.songMode = on;
            state.song.enabled = on;
            send({type: 'song', enabled: on, entries: state.song.entries});
            setStatus(on ? 'Song 模式开启（按编排顺序循环）' : 'Pattern 模式', '');
        });
        $('btnSong').addEventListener('click', openSongDialog);
        $('btnSongAdd').addEventListener('click', () => {
            state.song.entries.push({pattern: state.current, repeats: 1});
            send({type: 'song', enabled: state.master.songMode, entries: state.song.entries});
            renderSongList();
        });
        $('btnSongClose').addEventListener('click', () => $('songDlg').close());

        // ---- 主效果：每一项都即时送进音频线程 ----
        bindRange('mVol', 'mVolVal', (v) => {
            state.master.volume = v;
            send({type: 'master', key: 'volume', value: v});
        }, (v) => v.toFixed(2));
        bindRange('mDrive', 'mDriveVal', (v) => {
            state.master.drive = v;
            send({type: 'master', key: 'drive', value: v});
        }, (v) => v.toFixed(2));
        bindRange('mHpCut', 'mHpCutVal', (v) => {
            state.master.hpCutoff = v;
            send({type: 'master', key: 'hpCutoff', value: v});
        }, (v) => Math.round(v) + ' Hz');
        bindRange('mHpQ', 'mHpQVal', (v) => {
            state.master.hpQ = v;
            send({type: 'master', key: 'hpQ', value: v});
        }, (v) => v.toFixed(2));
        bindRange('mLpCut', 'mLpCutVal', (v) => {
            state.master.lpCutoff = v;
            send({type: 'master', key: 'lpCutoff', value: v});
        }, (v) => Math.round(v) + ' Hz');
        bindRange('mLpQ', 'mLpQVal', (v) => {
            state.master.lpQ = v;
            send({type: 'master', key: 'lpQ', value: v});
        }, (v) => v.toFixed(2));

        $('mHpOn').addEventListener('change', () => {
            state.master.hpOn = $('mHpOn').checked;
            send({type: 'master', key: 'hpOn', value: state.master.hpOn});
        });
        $('mLpOn').addEventListener('change', () => {
            state.master.lpOn = $('mLpOn').checked;
            send({type: 'master', key: 'lpOn', value: state.master.lpOn});
        });
        $('beatsPerBar').addEventListener('change', () => {
            state.master.beatsPerBar = parseInt($('beatsPerBar').value, 10);
            send({type: 'master', key: 'beatsPerBar', value: state.master.beatsPerBar});
        });
        $('beatUnit').addEventListener('change', () => {
            state.master.beatUnit = parseInt($('beatUnit').value, 10);
            send({type: 'master', key: 'beatUnit', value: state.master.beatUnit});
        });
        $('projName').addEventListener('change', () => {
            state.name = $('projName').value;
        });

        // ---- 声部 M/S 与试听 ----
        $('padMute').addEventListener('click', () => {
            const p = state.pads[selectedPad];
            p.mute = !p.mute;
            send({type: 'pad', pad: selectedPad, key: 'mute', value: p.mute});
            buildPadInspector();
            rowMsRefresh();
        });
        $('padSolo').addEventListener('click', () => {
            const p = state.pads[selectedPad];
            p.solo = !p.solo;
            send({type: 'pad', pad: selectedPad, key: 'solo', value: p.solo});
            buildPadInspector();
            rowMsRefresh();
        });
        $('btnAudition').addEventListener('click', async () => {
            try {
                await ensureAudio();
                await ctx.resume();
                send({type: 'trigger', pad: selectedPad, vel: 110, accent: false});
            } catch (err) {
                setStatus('试听失败：' + err.message, 'err');
            }
        });

        // ---- 每行的 M/S 按钮：事件委托，避免 32 个监听器 ----
        $('gridRows').addEventListener('click', (ev) => {
            const btn = ev.target.closest('button[data-act]');
            if (!btn) return;
            const pad = Number(btn.closest('.trackRow').dataset.pad);
            if (btn.dataset.act === 'select') {
                selectPad(pad);
                return;
            }
            const key = btn.dataset.act;   // mute | solo
            const p = state.pads[pad];
            p[key] = !p[key];
            send({type: 'pad', pad: pad, key: key, value: p[key]});
            btn.classList.toggle('on', p[key]);
            if (pad === selectedPad) buildPadInspector();
        });

        // ---- 每行轨道长度（polymeter） ----
        $('gridRows').addEventListener('change', (ev) => {
            const inp = ev.target.closest('.lenInput');
            if (!inp) return;
            const pad = Number(inp.dataset.pad);
            const v = clamp(parseInt(inp.value, 10) || 16, 1, 64);
            inp.value = String(v);
            curPattern().trackLen[pad] = v;
            send({type: 'trackLen', pattern: state.current, pad: pad, value: v});
        });

        // ---- 步进属性对话框按钮 ----
        $('stVel').addEventListener('input', () => {
            $('stVelVal').textContent = $('stVel').value;
        });
        $('stProb').addEventListener('input', () => {
            $('stProbVal').textContent = $('stProb').value + '%';
        });
        $('stMicro').addEventListener('input', () => {
            $('stMicroVal').textContent = $('stMicro').value;
        });
        $('stApply').addEventListener('click', () => {
            applyStepDialog();
            setStatus('步进属性已应用', 'ok');
        });
        $('stClear').addEventListener('click', () => {
            if (!dlgRef) return;
            const st = curPattern().data[dlgRef.pad][dlgRef.step];
            st[0] = 0;
            sendStep(dlgRef.pad, dlgRef.step, st);
            paintCell(dlgRef.pad, dlgRef.step);
            $('stOn').checked = false;
        });
        $('stClose').addEventListener('click', () => $('stepDlg').close());

        // ---- 传输与导出 ----
        $('btnPlay').addEventListener('click', play);
        $('btnStop').addEventListener('click', stop);
        $('btnWav').addEventListener('click', () => exportFile('wav'));
        $('btnMidi').addEventListener('click', () => exportFile('midi'));
        $('btnSave').addEventListener('click', saveProject);
        $('btnLoad').addEventListener('click', () => $('fileLoad').click());
        $('fileLoad').addEventListener('change', loadProject);

        // ---- 空格键 = 播放/停止（输入框里不抢键） ----
        document.addEventListener('keydown', (ev) => {
            const tag = (ev.target.tagName || '').toLowerCase();
            if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
            if (ev.code === 'Space') {
                ev.preventDefault();
                if (playing) stop(); else play();
            }
        });
    }

    /** 绑定 range + 数值标签的通用小工具 */
    function bindRange(inputId, valId, onValue, fmt) {
        const input = $(inputId);
        const val = $(valId);
        const push = () => {
            const v = parseFloat(input.value);
            val.textContent = fmt(v);
            onValue(v);
        };
        input.addEventListener('input', push);
    }

    function rowMsRefresh() {
        document.querySelectorAll('.trackRow').forEach((row) => {
            const pad = Number(row.dataset.pad);
            row.querySelector('.msBtn.mute').classList.toggle('on', !!state.pads[pad].mute);
            row.querySelector('.msBtn.solo').classList.toggle('on', !!state.pads[pad].solo);
        });
    }

    /* ------------------------------ Song 编排 ------------------------------ */
    function openSongDialog() {
        renderSongList();
        if (typeof $('songDlg').showModal === 'function') $('songDlg').showModal();
        else $('songDlg').setAttribute('open', 'open');
    }

    function renderSongList() {
        const box = $('songList');
        box.innerHTML = '';
        if (!state.song.entries.length) {
            const p = document.createElement('p');
            p.className = 'hint';
            p.textContent = '还没有条目；点“添加一条”把当前 Pattern 加进 Song 链。';
            box.appendChild(p);
        }
        state.song.entries.forEach((entry, idx) => {
            const row = document.createElement('div');
            row.className = 'songRow';

            const no = document.createElement('span');
            no.className = 'idx';
            no.textContent = String(idx + 1);

            const sel = document.createElement('select');
            sel.className = 'sel';
            for (let i = 0; i < 16; i++) {
                const o = document.createElement('option');
                o.value = String(i);
                o.textContent = 'Pattern ' + (i + 1);
                sel.appendChild(o);
            }
            sel.value = String(entry.pattern);
            sel.addEventListener('change', () => {
                entry.pattern = parseInt(sel.value, 10);
                send({type: 'song', enabled: state.master.songMode, entries: state.song.entries});
            });

            const rep = document.createElement('input');
            rep.type = 'number';
            rep.className = 'num';
            rep.min = '1';
            rep.max = '64';
            rep.value = String(entry.repeats);
            rep.title = '重复次数';
            rep.addEventListener('change', () => {
                entry.repeats = Math.max(1, parseInt(rep.value, 10) || 1);
                rep.value = String(entry.repeats);
                send({type: 'song', enabled: state.master.songMode, entries: state.song.entries});
            });

            const del = document.createElement('button');
            del.type = 'button';
            del.className = 'btn small';
            del.textContent = '删除';
            del.addEventListener('click', () => {
                state.song.entries.splice(idx, 1);
                send({type: 'song', enabled: state.master.songMode, entries: state.song.entries});
                renderSongList();
            });

            row.appendChild(no);
            row.appendChild(sel);
            row.appendChild(rep);
            row.appendChild(del);
            box.appendChild(row);
        });
    }

    /* ------------------------------ 保存 / 加载 / 导出 ------------------------------ */
    function downloadBlob(blob, filename) {
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        a.remove();
        // 延后回收，给浏览器留出开始下载的时间
        setTimeout(() => URL.revokeObjectURL(url), 4000);
    }

    function filenameFrom(resp, fallback) {
        const cd = resp.headers.get('Content-Disposition') || '';
        const m = /filename="?([^";]+)"?/.exec(cd);
        return m ? m[1] : fallback;
    }

    async function saveProject() {
        try {
            setStatus('正在让服务端规范化并生成工程文件…', '');
            const res = await fetch(BASE + '/api/save', {
                method: 'POST',
                headers: {'Content-Type': 'application/json; charset=UTF-8'},
                body: JSON.stringify(state)
            });
            if (!res.ok) throw new Error(await res.text());
            const blob = await res.blob();
            downloadBlob(blob, filenameFrom(res, 'xu808-project.json'));
            setStatus('工程已保存（服务端 XuProject 规范化）', 'ok');
        } catch (err) {
            setStatus('保存失败：' + err.message, 'err');
        }
    }

    async function loadProject(ev) {
        const file = ev.target.files && ev.target.files[0];
        if (!file) return;
        try {
            const text = await file.text();
            setStatus('正在加载工程…', '');
            const res = await fetch(BASE + '/api/load', {
                method: 'POST',
                headers: {'Content-Type': 'application/json; charset=UTF-8'},
                body: text
            });
            if (!res.ok) throw new Error(await res.text());
            const json = await res.json();
            applyLoadedProject(json);
            setStatus('工程已加载：' + file.name, 'ok');
        } catch (err) {
            setStatus('加载失败：' + err.message, 'err');
        } finally {
            ev.target.value = '';      // 允许重复选择同一个文件
        }
    }

    /** 用新工程替换内存状态、音频线程状态与全部界面 */
    function applyLoadedProject(json) {
        const next = normalize(json);
        // 逐字段写回，保持 state 这个对象引用不变（闭包里到处引用它）
        Object.keys(state).forEach((k) => {
            delete state[k];
        });
        Object.assign(state, next);
        send({type: 'project', project: state});
        syncControls();
        rebuildGrid();
        buildPadInspector();
        renderSongList();
        lastPlayhead = -1;
    }

    async function exportFile(kind) {
        try {
            const mode = $('exportMode').value === 'song' ? 'song' : 'pattern';
            let url = BASE + '/api/export/' + kind + '?mode=' + mode;
            if (kind === 'wav') url += '&track=' + encodeURIComponent($('exportTrack').value);
            setStatus('服务端正在离线渲染 ' + (kind === 'wav' ? 'WAV' : 'MIDI') + '…', '');
            const res = await fetch(url, {
                method: 'POST',
                headers: {'Content-Type': 'application/json; charset=UTF-8'},
                body: JSON.stringify(state)
            });
            if (!res.ok) throw new Error(await res.text());
            const blob = await res.blob();
            if (!blob.size) throw new Error('服务端返回了空文件');
            const fallback = 'xu808-' + mode + '.' + (kind === 'wav' ? 'wav' : 'mid');
            downloadBlob(blob, filenameFrom(res, fallback));
            setStatus('已导出 ' + (kind === 'wav' ? 'WAV' : 'MIDI') + ' · ' + blob.size + ' 字节', 'ok');
        } catch (err) {
            setStatus('导出失败：' + err.message, 'err');
        }
    }

    /* ------------------------------ 初始化 ------------------------------ */
    // 声部代号/中文名直接读服务端渲染好的行头，避免前端再抄一份常量
    const PAD_CODES = [];
    const PAD_NAMES = [];
    document.querySelectorAll('.trackRow').forEach((row) => {
        PAD_CODES.push(row.querySelector('.padBtn b').textContent.trim());
        PAD_NAMES.push(row.querySelector('.padBtn span').textContent.trim());
    });

    /** 把 state 写回所有控件（首次加载与 load 工程后共用） */
    function syncControls() {
        $('bpm').value = String(Math.round(state.master.bpm));
        $('bpmRange').value = String(Math.round(state.master.bpm));
        $('swingRange').value = String(Math.round(state.master.swing * 100));
        $('swingVal').textContent = Math.round(state.master.swing * 100) + '%';

        $('mVol').value = String(state.master.volume);
        $('mVolVal').textContent = Number(state.master.volume).toFixed(2);
        $('mDrive').value = String(state.master.drive);
        $('mDriveVal').textContent = Number(state.master.drive).toFixed(2);
        $('mHpOn').checked = !!state.master.hpOn;
        $('mHpCut').value = String(state.master.hpCutoff);
        $('mHpCutVal').textContent = Math.round(state.master.hpCutoff) + ' Hz';
        $('mHpQ').value = String(state.master.hpQ);
        $('mHpQVal').textContent = Number(state.master.hpQ).toFixed(2);
        $('mLpOn').checked = !!state.master.lpOn;
        $('mLpCut').value = String(state.master.lpCutoff);
        $('mLpCutVal').textContent = Math.round(state.master.lpCutoff) + ' Hz';
        $('mLpQ').value = String(state.master.lpQ);
        $('mLpQVal').textContent = Number(state.master.lpQ).toFixed(2);
        $('beatsPerBar').value = String(state.master.beatsPerBar);
        $('beatUnit').value = String(state.master.beatUnit);
        $('songMode').checked = !!state.master.songMode;
        $('projName').value = state.name || '';
        $('patternSel').value = String(state.current);
        $('stepsSel').value = String(curPattern().steps);
    }

    function initControls() {
        // Pattern 下拉：16 个
        for (let i = 0; i < 16; i++) {
            const o = document.createElement('option');
            o.value = String(i);
            o.textContent = 'P' + (i + 1) + ' · ' + (state.patterns[i].name || ('PATTERN ' + (i + 1)));
            $('patternSel').appendChild(o);
        }
        // 拍号：1/4–16/4 与 1/8–16/8
        for (let i = 1; i <= 16; i++) {
            const o = document.createElement('option');
            o.value = String(i);
            o.textContent = String(i);
            $('beatsPerBar').appendChild(o);
        }
        [4, 8].forEach((u) => {
            const o = document.createElement('option');
            o.value = String(u);
            o.textContent = String(u);
            $('beatUnit').appendChild(o);
        });
        // ratchet 1–8
        for (let i = 1; i <= 8; i++) {
            const o = document.createElement('option');
            o.value = String(i);
            o.textContent = i + ' 连击';
            $('stRatchet').appendChild(o);
        }
        // 条件触发
        COND_NAMES.forEach((name, i) => {
            const o = document.createElement('option');
            o.value = String(i);
            o.textContent = name;
            $('stCond').appendChild(o);
        });
    }

    function boot() {
        initControls();
        bindMaster();
        syncControls();
        rebuildGrid();
        buildPadInspector();
        renderSongList();
        updateTransport();
        setStatus('就绪 · 点击播放启动 AudioWorklet（' + VERSION + '）', '');
        // 暴露只读引用给调试用；正式逻辑不依赖它
        window.XU808_DEBUG = {state: state};
    }

    boot();
})();
