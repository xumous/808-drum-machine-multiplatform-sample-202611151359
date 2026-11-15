/*
 * selftest.cjs —— 不启动 Electron，直接用 Node 验证 wasm 模块
 *   node tools/selftest.cjs
 * 覆盖：实例化 / c808_create / 音序触发 / 快照打包 / WAV+分轨+MIDI 导出 / 工程存取
 */
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
(0, eval)(fs.readFileSync(path.join(ROOT, 'app', 'wasi.js'), 'utf8'));

const OP = {
    STEP_ON: 1, STEP_VEL: 2, STEP_ACCENT: 3, STEP_PROB: 4, STEP_RATCHET: 5, STEP_RATE: 6,
    STEP_COND: 7, STEP_MICRO: 8, TRACK_PARAM: 9, TRACK_LEN: 10, MUTE: 11, SOLO: 12,
    MASTER: 13, BPM: 14, TIMESIG: 15, PATTERN_LEN: 16, PATTERN_SWING: 17, SELECT_PATTERN: 18,
    SONG_ON: 19, SONG_LEN: 20, SONG_ENTRY: 21, PATTERN_COUNT: 22, CLEAR_PATTERN: 23,
    CLEAR_TRACK: 24, COPY_PATTERN: 25, PLAY: 26, STOP: 27, STOP_RESET: 28, PANIC: 29, NOTE_ON: 30
};

function fail(msg) {
    console.error('FAIL: ' + msg);
    process.exit(1);
}

const ok = (cond, msg) => {
    if (!cond) fail(msg);
    console.log('  ok  ' + msg);
};

async function boot() {
    const files = new Map();
    let memory = null;
    const bytes = fs.readFileSync(path.join(ROOT, 'app', 'wasm', 'core808.wasm'));
    const imports = globalThis.N808WASI.createWasiImports(() => memory, files);
    const {instance} = await WebAssembly.instantiate(bytes, imports);
    memory = instance.exports.memory;
    const ex = instance.exports;
    if (ex._initialize) ex._initialize();
    return {
        ex, files, get mem() {
            return memory;
        }
    };
}

(async () => {
    const A = await boot();
    const ex = A.ex;
    const SR = 48000;
    console.log('snap size     =', ex.n808_snap_size());
    console.log('project size  =', ex.n808_project_size());
    ok(ex.n808_snap_size() === 10856, '快照尺寸 10856');

    const eng = ex.c808_create(SR);
    ok(!!eng, 'c808_create');
    const l = ex.malloc(128 * 4);
    const r = ex.malloc(128 * 4);
    const snapPtr = ex.malloc(ex.n808_snap_size());
    const api = (op, a, b, c, n, f) => ex.n808_api(eng, op, a | 0, b | 0, c | 0, n | 0, f || 0);
    const snap = () => {
        ex.n808_pack_snapshot(eng, -1, 0, 0, snapPtr);
        return new DataView(ex.memory.buffer, snapPtr, ex.n808_snap_size());
    };
    const peak = () => {
        const f = new Float32Array(ex.memory.buffer);
        return Math.max(...Array.from(f.slice(l >> 2, (l >> 2) + 128)).map(Math.abs));
    };

    api(OP.PLAY);
    let nzR = 0, nzL = 0, frames = 0;
    for (let i = 0; i < SR / 128; i++) {
        ex.c808_render(eng, l, r, 128);
        const f = new Float32Array(ex.memory.buffer);
        for (let k = 0; k < 128; k++) {
            if (Math.abs(f[(l >> 2) + k]) > 1e-6) nzL++;
            if (Math.abs(f[(r >> 2) + k]) > 1e-6) nzR++;
            frames++;
        }
    }
    const dv0 = snap();
    console.log('rendered      =', frames, 'frames');
    console.log('playing/step  =', dv0.getInt32(0, true), '/', dv0.getInt32(8, true));
    console.log('non-silent    = L', nzL, ' R', nzR);
    ok(dv0.getInt32(0, true) === 1, 'transport PLAY 生效');
    ok(dv0.getInt32(8, true) > 0, '音序器步进');
    ok(nzR > 1000, '右声道有音频输出');

    console.log('meta len/swing/pat/bpm =', dv0.getInt32(108, true), dv0.getInt32(112, true),
        dv0.getInt32(116, true), dv0.getFloat32(140, true).toFixed(1));
    let cells = 0;
    const u = new Uint8Array(ex.memory.buffer, snapPtr, ex.n808_snap_size());
    for (let t = 0; t < 16; t++) for (let s = 0; s < 64; s++) if (u[552 + (t * 64 + s) * 10]) cells++;
    console.log('steps on      =', cells);
    ok(cells > 0, '默认 pattern 有音符');

    // step 编辑
    api(OP.STEP_ON, 0, 15, 3, 1);
    api(OP.STEP_ACCENT, 0, 15, 3, 1);
    api(OP.BPM, 0, 0, 0, 0, 133.5);
    ex.c808_render(eng, l, r, 128);
    const dv1 = snap();
    const u1 = new Uint8Array(ex.memory.buffer, snapPtr, ex.n808_snap_size());
    ok(u1[552 + (15 * 64 + 3) * 10] === 1, 'STEP_ON 命令生效');
    ok(u1[552 + (15 * 64 + 3) * 10 + 2] === 1, 'ACCENT 命令生效');
    ok(Math.abs(dv1.getFloat32(140, true) - 133.5) < 0.01, 'BPM 命令生效');

    // song
    api(OP.SONG_LEN, 3);
    api(OP.SONG_ENTRY, 0, 0, 2);
    api(OP.SONG_ENTRY, 1, 1, 1);
    api(OP.SONG_ENTRY, 2, 0, 1);
    ex.c808_render(eng, l, r, 128);
    const songPtr = ex.malloc(4 + 512 * 4);
    const sn = ex.n808_song_out(eng, songPtr, 4 + 512 * 4);
    ok(sn === 16, 'n808_song_out 返回 3 条 song 记录');

    // 工程存取
    const psz = ex.n808_project_size() + 20;
    const pptr = ex.malloc(psz);
    const pn = ex.n808_project_out(eng, pptr, psz);
    ok(pn === psz, 'n808_project_out 尺寸正确 (' + pn + ')');
    const proj = new Uint8Array(ex.memory.buffer, pptr, pn).slice();

    // 导出 WAV / MIDI / 分轨
    const cstr = (mod, mem, s) => {
        const b = new Uint8Array(s.length + 1);
        for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i);
        const p = mod.malloc(b.length);
        new Uint8Array(mem.buffer).set(b, p);
        return p;
    };
    api(OP.STOP_RESET);
    ex.c808_render(eng, l, r, 128);
    const rcW = ex.c808_export_wav(eng, cstr(ex, ex.memory, '/out.wav'), 0, 1, 1, 0);
    const rcM = ex.c808_export_midi(eng, cstr(ex, ex.memory, '/out.mid'), 0, 1);
    ok(rcW === 0 && A.files.has('/out.wav'), 'WAV 导出成功');
    ok(A.files.get('/out.wav').length > 200000, 'WAV 有数据 (' + A.files.get('/out.wav').length + ' 字节)');
    ok(Buffer.from(A.files.get('/out.wav').slice(0, 4)).toString('ascii') === 'RIFF', 'WAV 头正确');
    ok(rcM === 0 && Buffer.from(A.files.get('/out.mid').slice(0, 4)).toString('ascii') === 'MThd', 'MIDI 导出成功');

    const B = await boot();
    const ex2 = B.ex;
    const eng2 = ex2.c808_create(SR);
    const pp = ex2.malloc(pn);
    new Uint8Array(B.mem.buffer).set(proj, pp);
    ok(ex2.n808_project_in(eng2, pp, pn) === 0, 'n808_project_in 载入工程');
    const l2 = ex2.malloc(512), r2 = ex2.malloc(512);
    ex2.c808_render(eng2, l2, r2, 256);
    ok(ex2.n808_project_size() === ex.n808_project_size(), '两个实例的结构体布局一致');

    /* 分轨：在同一个实例里连续导出（与 export-worker 的行为一致） */
    const rcS = ex.c808_export_wav(eng, cstr(ex, A.mem, '/stems.wav'), 0, 1, 2, 1);
    ok(rcS === 0 && A.files.size === 18, '分轨导出 16 个文件 (' + (A.files.size - 2) + ')');

    console.log('ALL OK');
    process.exit(0);
})().catch((e) => fail(e && e.stack ? e.stack : e));
