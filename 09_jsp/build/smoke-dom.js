// 临时冒烟测试：用 jsdom 在真实 HTTP 页面上执行 UI 脚本，检查网格/下拉框/事件绑定。
// 徐某把它放在 build 目录，仅用于验证，不会打进 WAR。
const {JSDOM, VirtualConsole} = require('jsdom');

const url = process.argv[2] || 'http://127.0.0.1:18080/xu808-jsp/';
const errs = [];
const vc = new VirtualConsole();
vc.on('jsdomError', (e) => errs.push('jsdomError: ' + e.message));
vc.on('error', (...a) => errs.push('console.error: ' + a.join(' ')));
process.on('unhandledRejection', (e) => errs.push('unhandledRejection: ' + e));

JSDOM.fromURL(url, {
    resources: 'usable',
    runScripts: 'dangerously',
    pretendToBeVisual: true,
    virtualConsole: vc
}).then((dom) => {
    setTimeout(() => {
        const w = dom.window, d = w.document;
        const out = {};
        try {
            out.title = d.title;
            out.rows = d.querySelectorAll('.trackRow').length;
            out.cells = d.querySelectorAll('.cell').length;
            out.patternOptions = d.querySelectorAll('#patternSel option').length;
            out.beatsOptions = d.querySelectorAll('#beatsPerBar option').length;
            out.padParams = d.querySelectorAll('#padParams .field').length;
            out.songRows = d.querySelectorAll('#songList .songRow').length;
            out.status = d.querySelector('#status')?.textContent;
            out.debugReady = !!w.XU808_DEBUG;

            // 1) 点击一个关闭的格子，应该变成 on
            const cell = d.querySelector('.trackRow[data-pad="9"] .cell[data-step="0"]');
            out.beforeClick = cell ? cell.className : 'missing';
            if (cell) cell.click();
            out.afterClick = cell ? cell.className : 'missing';

            // 2) 选择 LT 声部，检查参数条标题
            const padBtn = d.querySelector('.trackRow[data-pad="2"] .padBtn');
            if (padBtn) padBtn.click();
            out.padTitle = d.querySelector('#padTitle')?.textContent;

            // 3) BPM 改成 140，检查 state 是否同步
            const bpm = d.querySelector('#bpm');
            if (bpm) {
                bpm.value = '140';
                bpm.dispatchEvent(new w.Event('change', {bubbles: true}));
            }
            out.bpm = w.XU808_DEBUG?.state?.master?.bpm;

            // 4) 步数切到 32，格子应变成 16×32
            const steps = d.querySelector('#stepsSel');
            if (steps) {
                steps.value = '32';
                steps.dispatchEvent(new w.Event('change', {bubbles: true}));
            }
            out.cellsAfter32 = d.querySelectorAll('.cell').length;
            out.stepsState = w.XU808_DEBUG?.state?.patterns?.[0]?.steps;

            out.errors = errs;
            console.log(JSON.stringify(out, null, 2));
        } catch (e) {
            out.fatal = String(e && e.stack ? e.stack : e);
            out.errors = errs;
            console.log(JSON.stringify(out, null, 2));
        }
        dom.window.close();
        process.exit(0);
    }, 2500);
}).catch((e) => {
    console.log('LOAD_FAILED ' + e);
    process.exit(1);
});
