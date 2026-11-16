/* ============================================================================
 * xu808-ui.js 鈥斺�� 娴忚鍣ㄧ鐣岄潰閫昏緫锛�09_jsp / 10_javaee 鍏辩敤锛�
 * ----------------------------------------------------------------------------
 * 寰愭煇鎶婅繖涓�灞傚畾浣嶆垚鈥滀箰璋变笌闈㈡澘鈥濓細
 *   路 涓嶅弬涓庝换浣曢煶棰戞牱鏈繍绠楋紝鎵�鏈夊０闊抽兘鍦� AudioWorklet 鐨勯煶棰戠嚎绋嬮噷锛�
 *   路 鍙淮鎶や竴浠戒笌 XuProject.toMap() 鍚屾瀯鐨� state锛屼换浣曠紪杈戦兘
 *     (a) 鏀� state锛�(b) 鍙戜竴鏉℃渶灏忚ˉ涓佺粰 worklet锛�(c) 閲嶇敾閭ｄ竴涓牸瀛�/婊戝潡锛�
 *   路 瀵煎嚭鏃舵妸鏁翠唤 state POST 缁欐湇鍔＄锛屾湇鍔＄鐢ㄥ悓涓�濂� Java 寮曟搸绂荤嚎娓叉煋銆�
 * 杩欐牱鈥滅湅鍒扮殑鈥濃�滃惉鍒扮殑鈥濃�滃鍑虹殑鈥濅笁鑰呭叡浜悓涓�浠藉伐绋嬭涔夈��
 * ============================================================================ */
'use strict';

(function () {
    /* ------------------------------ 鍩虹甯搁噺 ------------------------------ */
    const BASE = window.XU808_BASE || '';                  // WAR 涓婁笅鏂囪矾寰勶紝渚嬪 /xu808-jsp
    const INIT = window.XU808_INIT || {};
    const VERSION = window.XU808_VERSION || 'web';
    const $ = (id) => document.getElementById(id);         // 鐪佸緱鍒板鍐� document.getElementById

    // 姝ヨ繘鏉′欢涓庤鏍间功 XuProject.COND_* 涓�涓�瀵瑰簲
    const COND_NAMES = ['鎬绘槸', '浠呰繃闂�', '闈炶繃闂�', '姣� 2 寰幆', '姣� 4 寰幆'];
    const COND_TAGS = ['', 'F', 'N', '2', '4'];            // 鏍煎瓙閲岀殑灏忔爣璁�

    // 姣忎釜澹伴儴瀹為檯鏆撮湶鍝簺闊宠壊鍙傛暟锛汱evel/Pan 鎵�鏈夊０閮ㄩ兘鏈夛紝鍙﹀杩藉姞
    // 椤哄簭涓庤鏍间功绗� 3 鑺備竴鑷达細BD 鏈� Tune/Decay/Tone锛孲D 鐢� Snappy锛孋P 鐢� Tail鈥︹��
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
        tune: 'Tune 璋冮煶', decay: 'Decay 琛板噺', tone: 'Tone 闊宠壊',
        snappy: 'Snappy 鍣０', tail: 'Tail 灏惧反', level: 'Level 闊抽噺', pan: 'Pan 澹板儚'
    };

    /* ------------------------------ 鐘舵�� ------------------------------ */
    /** 娣辨嫹璐濓紝閬垮厤鐩存帴鏀瑰埌 window.XU808_INIT锛堝埛鏂板悗杩樿兘鍥炲埌鏈嶅姟绔垵鍊硷級 */
    function clone(o) {
        return JSON.parse(JSON.stringify(o));
    }

    function clamp(x, lo, hi) {
        return x < lo ? lo : (x > hi ? hi : x);
    }

    /** 缁欑己瀛楁鐨勫伐绋嬭ˉ榻愰粯璁ゅ�硷紱鏈嶅姟绔案杩滅粰鍏ㄩ噺锛岃繖閲屼富瑕侀槻鈥滄墜宸ユ敼鍧忕殑 JSON鈥� */
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
        // swing 鍦ㄥ伐绋嬮噷瀛� 0鈥�0.75 鐨勫皬鏁帮紱UI 鏄剧ず鏃跺啀 脳100
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
                    // 缁熶竴鎴愪竷鍏冩暟缁勶細on / velocity / accent / probability / micro / ratchet / condition
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
        s.name = s.name || '寰愭煇鐨� 808 宸ョ▼';
        if (typeof s.seed !== 'number') s.seed = 0x808808;
        return s;
    }

    const state = normalize(clone(INIT));
    let selectedPad = 0;
    let playing = false;
    let lastPlayhead = -1;
    const cellEls = [];          // cellEls[pad][step] -> button锛屾挱鏀惧ご鍙敼杩欎袱绾ф暟缁�

    /* ------------------------------ 闊抽 ------------------------------ */
    let ctx = null;              // AudioContext
    let node = null;             // AudioWorkletNode
    let booting = null;          // 闃叉杩炵偣鎾斁鏃堕噸澶� addModule

    /** 鎶婁竴鏉℃秷鎭�佽繘闊抽绾跨▼锛涙病鍚姩鏃堕潤榛樹涪寮冿紙鍚姩鍚庝細鏁翠唤閲嶅彂宸ョ▼锛� */
    function send(msg) {
        if (node) node.port.postMessage(msg);
    }

    /** 棣栨鐢ㄦ埛鎵嬪娍閲屽垱寤� AudioContext锛氭祻瑙堝櫒鑷姩鎾斁绛栫暐瑕佹眰蹇呴』杩欎箞鍋� */
    async function ensureAudio() {
        if (ctx) {
            if (ctx.state === 'suspended') await ctx.resume();
            return;
        }
        if (booting) return booting;
        booting = (async () => {
            const AC = window.AudioContext || window.webkitAudioContext;
            if (!AC || !('audioWorklet' in AC.prototype)) {
                throw new Error('褰撳墠娴忚鍣ㄤ笉鏀寔 AudioWorklet锛岃浣跨敤鏂扮増 Chrome/Edge/Firefox');
            }
            ctx = new AC({sampleRate: 48000, latencyHint: 'interactive'});
            // 寰愭煇锛歸orklet 閲� 128 甯т竴涓噺瀛愶紝涓荤嚎绋嬪彧璐熻矗 UI锛�
            await ctx.audioWorklet.addModule(BASE + '/js/xu808-worklet.js');
            node = new AudioWorkletNode(ctx, 'xu808-engine', {
                numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]
            });
            node.port.onmessage = onWorklet;
            node.connect(ctx.destination);
            // 鍚姩鐬棿鎶婂畬鏁村伐绋嬪悓姝ヨ繘鍘伙紝涔嬪悗鍙彂琛ヤ竵
            send({type: 'project', project: state});
            setStatus('闊抽宸插惎鍔� 路 ' + Math.round(ctx.sampleRate) + ' Hz 路 AudioWorklet', 'ok');
        })();
        try {
            await booting;
        } catch (err) {
            // addModule 澶辫触锛堣矾寰勯敊銆丮IME 閿欙級鏃惰鎶� ctx 娓呮帀锛屽惁鍒欑浜屾鐐瑰嚮鍙細 resume 涓�涓┖鍥�
            ctx = null;
            node = null;
            throw err;
        } finally {
            booting = null;
        }
    }

    /** 鎺ユ敹闊抽绾跨▼鍥炰紶锛氭挱鏀惧ご銆佺數骞炽�侀敊璇� */
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
            setStatus('闊抽閿欒锛�' + m.message, 'err');
        } else if (m.type === 'ready') {
            // worklet 瑁呰浇瀹屾垚锛岃繖閲屼笉鍙戞暟鎹紙宸ョ▼鍦� ensureAudio 閲屽凡缁忓彂杩囷級
        }
    }

    /** 鎾斁锛氬厛纭繚闊抽鍥惧瓨鍦紝鍐嶈闊抽绾跨▼浠庡綋鍓� samplePos 璧锋 */
    async function play() {
        try {
            await ensureAudio();
            await ctx.resume();
            send({type: 'play'});
            playing = true;
            updateTransport();
            setStatus('鎾斁涓� 路 BPM ' + Math.round(state.master.bpm) + ' 路 ' + VERSION, 'ok');
        } catch (err) {
            setStatus('鏃犳硶鍚姩闊抽锛�' + (err && err.message ? err.message : err), 'err');
        }
    }

    function stop() {
        send({type: 'stop'});
        playing = false;
        updateTransport();
        setPlayhead(-1);
        setStatus('宸插仠姝�', '');
    }

    function updateTransport() {
        $('btnPlay').textContent = playing ? '鈻� 鎾斁涓�' : '鈻� 鎾斁';
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

    /* ------------------------------ 缃戞牸娓叉煋 ------------------------------ */
    function curPattern() {
        return state.patterns[state.current];
    }

    /** 閲嶅缓鏁村紶缃戞牸锛氭鏁�/Pattern 鏀瑰彉鏃惰皟鐢紱骞虫椂鍙敼鍗曚釜鏍煎瓙 */
    function rebuildGrid() {
        const pat = curPattern();
        const steps = pat.steps;

        // 1) 鍒楀彿琛ㄥご
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

        // 2) 16 琛屾牸瀛�
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
                btn.setAttribute('aria-label', '澹伴儴 ' + pad + ' 绗� ' + (s + 1) + ' 姝�');
                btn.addEventListener('click', () => toggleStep(pad, s));
                btn.addEventListener('contextmenu', (ev) => {
                    ev.preventDefault();               // 鍙抽敭涓嶅脊绯荤粺鑿滃崟锛屾敼寮规杩涘睘鎬�
                    openStepDialog(pad, s);
                });
                box.appendChild(btn);
                cellEls[pad][s] = btn;
            }
            for (let s = 0; s < steps; s++) paintCell(pad, s);
        }

        $('stepsSel').value = String(steps);
        // 鑻ュ伐绋嬫潵鑷叾瀹冨钩鍙颁笖姝ユ暟涓嶆槸 16/32/64锛堜緥濡� 12锛夛紝涓存椂琛ヤ竴涓�夐」锛岄伩鍏嶄笅鎷夋鏄剧ず绌虹櫧
        if ($('stepsSel').value !== String(steps)) {
            const o = document.createElement('option');
            o.value = String(steps);
            o.textContent = String(steps);
            $('stepsSel').appendChild(o);
            $('stepsSel').value = String(steps);
        }
        if (lastPlayhead >= steps) lastPlayhead = -1;
    }

    /** 鍙噸鐢讳竴涓牸瀛愶細寮�鍏炽�侀噸闊炽�佹鐜囩偣銆佸井鏃朵笁瑙掋�乺atchet 鏁板瓧銆佹潯浠跺瓧姣� */
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
        btn.title = '鍔涘害 ' + st[1] + (st[2] ? ' 路 閲嶉煶' : '') +
            ' 路 姒傜巼 ' + st[3] + '%' + (st[4] ? ' 路 寰椂 ' + st[4] : '') +
            (st[5] > 1 ? ' 路 ratchet ' + st[5] : '') +
            ' 路 ' + COND_NAMES[st[6]];
    }

    /** 宸﹂敭锛氬紑/鍏冲綋鍓嶆牸瀛愶紝骞剁珛鍒绘妸杩欐潯姝ヨ繘鏁版嵁鍚屾鍒伴煶棰戠嚎绋� */
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
            data: st.slice(0, 7)          // 鍙�佷竷涓瓧娈碉紝鍒妸 UI 涓存椂灞炴�у甫杩囧幓
        });
    }

    /** 鎾斁澶达細鏁村垪鍔� .playhead锛涘垏鎹㈠垪鏃跺厛娓呮棫鍒楋紝閬垮厤閬嶅巻 1024 涓牸瀛� */
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

    /* ------------------------------ 姝ヨ繘灞炴�у璇濇 ------------------------------ */
    let dlgRef = null;   // {pad, step}

    function openStepDialog(pad, step) {
        const st = curPattern().data[pad][step];
        dlgRef = {pad: pad, step: step};
        $('stepTitle').textContent = PAD_CODES[pad] + ' 路 绗� ' + (step + 1) + ' 姝ュ睘鎬�';
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

    /* ------------------------------ 澹伴儴鍙傛暟鏉� ------------------------------ */
    function selectPad(pad) {
        selectedPad = pad;
        document.querySelectorAll('.trackRow').forEach((row) => {
            row.classList.toggle('selected', Number(row.dataset.pad) === pad);
        });
        buildPadInspector();
    }

    /** 鎸夊０閮ㄧ被鍨嬪姩鎬佺敓鎴愭粦鍧楋紝閬垮厤缁欏啗榧撴樉绀衡�淭ail鈥濊繖绉嶆棤鎰忎箟鍙傛暟 */
    function buildPadInspector() {
        const pad = selectedPad;
        const p = state.pads[pad];
        $('padTitle').textContent = PAD_CODES[pad] + ' 路 ' + PAD_NAMES[pad];
        $('padMute').classList.toggle('on', !!p.mute);
        $('padSolo').classList.toggle('on', !!p.solo);
        $('padMute').textContent = p.mute ? 'Mute 寮�' : 'Mute';
        $('padSolo').textContent = p.solo ? 'Solo 寮�' : 'Solo';

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

    /* ------------------------------ 涓绘晥鏋滀笌浼犺緭鎺т欢 ------------------------------ */
    function setBpm(bpm) {
        const v = clamp(Math.round(bpm), 40, 300);
        state.master.bpm = v;
        $('bpm').value = String(v);
        $('bpmRange').value = String(v);
        send({type: 'master', key: 'bpm', value: v});
    }

    function bindMaster() {
        // ---- BPM 鏁板瓧妗嗕笌婊戞潌鍙屽悜鍚屾 ----
        $('bpmRange').addEventListener('input', () => setBpm(parseInt($('bpmRange').value, 10)));
        $('bpm').addEventListener('change', () => setBpm(parseInt($('bpm').value, 10)));

        // ---- Swing锛歎I 鐢ㄧ櫨鍒嗘暟 0鈥�75锛屽伐绋嬮噷瀛� 0鈥�0.75 ----
        $('swingRange').addEventListener('input', () => {
            const pct = clamp(parseInt($('swingRange').value, 10) || 0, 0, 75);
            state.master.swing = pct / 100;
            $('swingVal').textContent = pct + '%';
            send({type: 'master', key: 'swing', value: state.master.swing});
        });

        // ---- Tap Tempo锛氭渶杩� 8 娆＄偣鍑婚棿闅旓紝鍘绘帀鏈�澶ф渶灏忓悗骞冲潎 ----
        let taps = [];
        $('btnTap').addEventListener('click', () => {
            const now = performance.now();
            if (taps.length && now - taps[taps.length - 1] > 3000) taps = [];   // 鍋滈】 3 绉掗噸鏂板紑濮�
            taps.push(now);
            if (taps.length > 9) taps.shift();                                  // 8 涓棿闅旈渶瑕� 9 娆＄偣鍑�
            if (taps.length < 2) {
                setStatus('Tap锛氱户缁偣鍑烩��', '');
                return;
            }
            const bpms = [];
            for (let i = 1; i < taps.length; i++) {
                const b = 60000 / (taps[i] - taps[i - 1]);
                if (b >= 30 && b <= 400) bpms.push(b);                            // 涓㈠純寮傚父鍊�
            }
            if (!bpms.length) return;
            bpms.sort((a, b) => a - b);
            if (bpms.length > 2) bpms.splice(0, 1), bpms.splice(bpms.length - 1, 1);  // 鍘绘帀鏈�澶ф渶灏�
            const avg = bpms.reduce((a, b) => a + b, 0) / bpms.length;
            setBpm(avg);
            setStatus('Tap 娴嬪緱 ' + Math.round(avg) + ' BPM', 'ok');
        });

        // ---- Pattern 閫夋嫨锛氭挱鏀句腑鐢� worklet 鍦ㄤ笅涓�姝ヨ竟鐣屽垏鎹� ----
        $('patternSel').addEventListener('change', () => {
            state.current = clamp(parseInt($('patternSel').value, 10) || 0, 0, 15);
            send({type: 'pattern', index: state.current});
            rebuildGrid();
            buildPadInspector();
        });

        // ---- 姝ユ暟锛�1鈥�64锛岄粯璁� 16/32/64 涓夋。 ----
        $('stepsSel').addEventListener('change', () => {
            const pat = curPattern();
            pat.steps = clamp(parseInt($('stepsSel').value, 10) || 16, 1, 64);
            send({type: 'steps', value: pat.steps});
            rebuildGrid();
        });

        // ---- Song 寮�鍏充笌缂栨帓 ----
        $('songMode').addEventListener('change', () => {
            const on = $('songMode').checked;
            state.master.songMode = on;
            state.song.enabled = on;
            send({type: 'song', enabled: on, entries: state.song.entries});
            setStatus(on ? 'Song 妯″紡寮�鍚紙鎸夌紪鎺掗『搴忓惊鐜級' : 'Pattern 妯″紡', '');
        });
        $('btnSong').addEventListener('click', openSongDialog);
        $('btnSongAdd').addEventListener('click', () => {
            state.song.entries.push({pattern: state.current, repeats: 1});
            send({type: 'song', enabled: state.master.songMode, entries: state.song.entries});
            renderSongList();
        });
        $('btnSongClose').addEventListener('click', () => $('songDlg').close());

        // ---- 涓绘晥鏋滐細姣忎竴椤归兘鍗虫椂閫佽繘闊抽绾跨▼ ----
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

        // ---- 澹伴儴 M/S 涓庤瘯鍚� ----
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
                setStatus('璇曞惉澶辫触锛�' + err.message, 'err');
            }
        });

        // ---- 姣忚鐨� M/S 鎸夐挳锛氫簨浠跺鎵橈紝閬垮厤 32 涓洃鍚櫒 ----
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

        // ---- 姣忚杞ㄩ亾闀垮害锛坧olymeter锛� ----
        $('gridRows').addEventListener('change', (ev) => {
            const inp = ev.target.closest('.lenInput');
            if (!inp) return;
            const pad = Number(inp.dataset.pad);
            const v = clamp(parseInt(inp.value, 10) || 16, 1, 64);
            inp.value = String(v);
            curPattern().trackLen[pad] = v;
            send({type: 'trackLen', pattern: state.current, pad: pad, value: v});
        });

        // ---- 姝ヨ繘灞炴�у璇濇鎸夐挳 ----
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
            setStatus('姝ヨ繘灞炴�у凡搴旂敤', 'ok');
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

        // ---- 浼犺緭涓庡鍑� ----
        $('btnPlay').addEventListener('click', play);
        $('btnStop').addEventListener('click', stop);
        $('btnWav').addEventListener('click', () => exportFile('wav'));
        $('btnMidi').addEventListener('click', () => exportFile('midi'));
        $('btnSave').addEventListener('click', saveProject);
        $('btnLoad').addEventListener('click', () => $('fileLoad').click());
        $('fileLoad').addEventListener('change', loadProject);

        // ---- 绌烘牸閿� = 鎾斁/鍋滄锛堣緭鍏ユ閲屼笉鎶㈤敭锛� ----
        document.addEventListener('keydown', (ev) => {
            const tag = (ev.target.tagName || '').toLowerCase();
            if (tag === 'input' || tag === 'select' || tag === 'textarea') return;
            if (ev.code === 'Space') {
                ev.preventDefault();
                if (playing) stop(); else play();
            }
        });
    }

    /** 缁戝畾 range + 鏁板�兼爣绛剧殑閫氱敤灏忓伐鍏� */
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

    /* ------------------------------ Song 缂栨帓 ------------------------------ */
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
            p.textContent = '杩樻病鏈夋潯鐩紱鐐光�滄坊鍔犱竴鏉♀�濇妸褰撳墠 Pattern 鍔犺繘 Song 閾俱��';
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
            rep.title = '閲嶅娆℃暟';
            rep.addEventListener('change', () => {
                entry.repeats = Math.max(1, parseInt(rep.value, 10) || 1);
                rep.value = String(entry.repeats);
                send({type: 'song', enabled: state.master.songMode, entries: state.song.entries});
            });

            const del = document.createElement('button');
            del.type = 'button';
            del.className = 'btn small';
            del.textContent = '鍒犻櫎';
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

    /* ------------------------------ 淇濆瓨 / 鍔犺浇 / 瀵煎嚭 ------------------------------ */
    function downloadBlob(blob, filename) {
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        a.remove();
        // 寤跺悗鍥炴敹锛岀粰娴忚鍣ㄧ暀鍑哄紑濮嬩笅杞界殑鏃堕棿
        setTimeout(() => URL.revokeObjectURL(url), 4000);
    }

    function filenameFrom(resp, fallback) {
        const cd = resp.headers.get('Content-Disposition') || '';
        const m = /filename="?([^";]+)"?/.exec(cd);
        return m ? m[1] : fallback;
    }

    async function saveProject() {
        try {
            setStatus('姝ｅ湪璁╂湇鍔＄瑙勮寖鍖栧苟鐢熸垚宸ョ▼鏂囦欢鈥�', '');
            const res = await fetch(BASE + '/api/save', {
                method: 'POST',
                headers: {'Content-Type': 'application/json; charset=UTF-8'},
                body: JSON.stringify(state)
            });
            if (!res.ok) throw new Error(await res.text());
            const blob = await res.blob();
            downloadBlob(blob, filenameFrom(res, 'xu808-project.json'));
            setStatus('宸ョ▼宸蹭繚瀛橈紙鏈嶅姟绔� XuProject 瑙勮寖鍖栵級', 'ok');
        } catch (err) {
            setStatus('淇濆瓨澶辫触锛�' + err.message, 'err');
        }
    }

    async function loadProject(ev) {
        const file = ev.target.files && ev.target.files[0];
        if (!file) return;
        try {
            const text = await file.text();
            setStatus('姝ｅ湪鍔犺浇宸ョ▼鈥�', '');
            const res = await fetch(BASE + '/api/load', {
                method: 'POST',
                headers: {'Content-Type': 'application/json; charset=UTF-8'},
                body: text
            });
            if (!res.ok) throw new Error(await res.text());
            const json = await res.json();
            applyLoadedProject(json);
            setStatus('宸ョ▼宸插姞杞斤細' + file.name, 'ok');
        } catch (err) {
            setStatus('鍔犺浇澶辫触锛�' + err.message, 'err');
        } finally {
            ev.target.value = '';      // 鍏佽閲嶅閫夋嫨鍚屼竴涓枃浠�
        }
    }

    /** 鐢ㄦ柊宸ョ▼鏇挎崲鍐呭瓨鐘舵�併�侀煶棰戠嚎绋嬬姸鎬佷笌鍏ㄩ儴鐣岄潰 */
    function applyLoadedProject(json) {
        const next = normalize(json);
        // 閫愬瓧娈靛啓鍥烇紝淇濇寔 state 杩欎釜瀵硅薄寮曠敤涓嶅彉锛堥棴鍖呴噷鍒板寮曠敤瀹冿級
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
            setStatus('鏈嶅姟绔鍦ㄧ绾挎覆鏌� ' + (kind === 'wav' ? 'WAV' : 'MIDI') + '鈥�', '');
            const res = await fetch(url, {
                method: 'POST',
                headers: {'Content-Type': 'application/json; charset=UTF-8'},
                body: JSON.stringify(state)
            });
            if (!res.ok) throw new Error(await res.text());
            const blob = await res.blob();
            if (!blob.size) throw new Error('鏈嶅姟绔繑鍥炰簡绌烘枃浠�');
            const fallback = 'xu808-' + mode + '.' + (kind === 'wav' ? 'wav' : 'mid');
            downloadBlob(blob, filenameFrom(res, fallback));
            setStatus('宸插鍑� ' + (kind === 'wav' ? 'WAV' : 'MIDI') + ' 路 ' + blob.size + ' 瀛楄妭', 'ok');
        } catch (err) {
            setStatus('瀵煎嚭澶辫触锛�' + err.message, 'err');
        }
    }

    /* ------------------------------ 鍒濆鍖� ------------------------------ */
    // 澹伴儴浠ｅ彿/涓枃鍚嶇洿鎺ヨ鏈嶅姟绔覆鏌撳ソ鐨勮澶达紝閬垮厤鍓嶇鍐嶆妱涓�浠藉父閲�
    const PAD_CODES = [];
    const PAD_NAMES = [];
    document.querySelectorAll('.trackRow').forEach((row) => {
        PAD_CODES.push(row.querySelector('.padBtn b').textContent.trim());
        PAD_NAMES.push(row.querySelector('.padBtn span').textContent.trim());
    });

    /** 鎶� state 鍐欏洖鎵�鏈夋帶浠讹紙棣栨鍔犺浇涓� load 宸ョ▼鍚庡叡鐢級 */
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
        // Pattern 涓嬫媺锛�16 涓�
        for (let i = 0; i < 16; i++) {
            const o = document.createElement('option');
            o.value = String(i);
            o.textContent = 'P' + (i + 1) + ' 路 ' + (state.patterns[i].name || ('PATTERN ' + (i + 1)));
            $('patternSel').appendChild(o);
        }
        // 鎷嶅彿锛�1/4鈥�16/4 涓� 1/8鈥�16/8
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
        // ratchet 1鈥�8
        for (let i = 1; i <= 8; i++) {
            const o = document.createElement('option');
            o.value = String(i);
            o.textContent = i + ' 杩炲嚮';
            $('stRatchet').appendChild(o);
        }
        // 鏉′欢瑙﹀彂
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
        setStatus('灏辩华 路 鐐瑰嚮鎾斁鍚姩 AudioWorklet锛�' + VERSION + '锛�', '');
        // 鏆撮湶鍙寮曠敤缁欒皟璇曠敤锛涙寮忛�昏緫涓嶄緷璧栧畠
        window.XU808_DEBUG = {state: state};
    }

    boot();
})();
