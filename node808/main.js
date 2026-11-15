/*
 * main.js —— Electron 主进程
 * ---------------------------------------------------------------------------
 * 渲染进程通过自定义 app:// 协议加载（secure + standard），这样 AudioWorklet
 * 的 addModule()、fetch() 和 Web MIDI 都能在安全上下文里正常工作。
 * 文件对话框 / 落盘全部留在主进程，渲染进程只拿到 ArrayBuffer。
 */
const {app, BrowserWindow, protocol, net, dialog, ipcMain, Menu, screen} = require('electron');
const path = require('path');
const fs = require('fs');
const {pathToFileURL} = require('url');

const APP_ROOT = path.join(__dirname, 'app');

protocol.registerSchemesAsPrivileged([
    {
        scheme: 'app',
        privileges: {
            standard: true,
            secure: true,
            supportFetchAPI: true,
            corsEnabled: true,
            stream: true,
            codeCache: true
        }
    }
]);

/* 允许自动播放，音频在窗口出现后立即启动 */
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

let win = null;

function createWindow() {
    /* 按屏幕可用工作区决定窗口尺寸，避免超出桌面导致右侧被裁掉 */
    const disp = screen.getPrimaryDisplay();
    const sf = disp.scaleFactor || 1;
    /* 注意：Windows 上 workAreaSize 可能是物理像素，而 BrowserWindow 用 DIP，
       这里统一除以缩放比，保证窗口在任何 DPI 下都不会超出桌面。 */
    const availW = disp.workAreaSize.width / sf;
    const availH = disp.workAreaSize.height / sf;
    const winW = Math.max(720, Math.min(1152, Math.floor(availW * 0.90)));
    const winH = Math.max(440, Math.min(Math.round(winW * 9 / 16) + 36, Math.floor(availH * 0.90)));
    console.log('[n808] workArea', JSON.stringify(disp.workAreaSize), 'scale', sf,
        'avail', Math.round(availW), Math.round(availH), '=> window', winW, winH);

    win = new BrowserWindow({
        width: winW,
        height: winH,
        x: 24,
        y: 24,
        minWidth: 720,
        minHeight: 440,
        backgroundColor: '#0e1014',
        autoHideMenuBar: true,
        useContentSize: true,
        title: '808 · SYNTHETIC DRUM MACHINE (Node)',
        icon: path.join(__dirname, '..', 'app.ico'),
        webPreferences: {
            preload: path.join(__dirname, 'preload.js'),
            contextIsolation: true,
            nodeIntegration: false,
            sandbox: false,
            backgroundThrottling: false
        }
    });

    Menu.setApplicationMenu(null);
    win.loadURL('app://local/index.html');

    win.webContents.on('console-message', (_e, level, message, line, source) => {
        console.log(`[renderer:${level}] ${message} (${source}:${line})`);
    });
    win.on('closed', () => {
        win = null;
    });
}

app.whenReady().then(() => {
    protocol.handle('app', async (request) => {
        const url = new URL(request.url);
        let rel = decodeURIComponent(url.pathname);
        if (rel === '/' || rel === '') rel = '/index.html';
        const file = path.normalize(path.join(APP_ROOT, rel));
        if (!file.startsWith(path.normalize(APP_ROOT))) {
            return new Response('forbidden', {status: 403});
        }
        if (!fs.existsSync(file)) return new Response('not found', {status: 404});
        const res = await net.fetch(pathToFileURL(file).toString());
        const headers = new Headers(res.headers);
        if (file.endsWith('.wasm')) headers.set('Content-Type', 'application/wasm');
        if (file.endsWith('.js')) headers.set('Content-Type', 'text/javascript');
        headers.set('Cache-Control', 'no-store');
        return new Response(res.body, {status: 200, headers});
    });

    ipcMain.handle('n8:saveFile', async (_e, {data, defaultName, filters}) => {
        const r = await dialog.showSaveDialog(win, {
            title: '保存',
            defaultPath: defaultName || '808-output.bin',
            filters: filters || [{name: '全部文件', extensions: ['*']}]
        });
        if (r.canceled || !r.filePath) return {ok: false, canceled: true};
        try {
            fs.writeFileSync(r.filePath, Buffer.from(data));
            return {ok: true, path: r.filePath};
        } catch (err) {
            return {ok: false, error: String(err && err.message ? err.message : err)};
        }
    });

    ipcMain.handle('n8:pickSavePath', async (_e, {defaultName, filters}) => {
        const r = await dialog.showSaveDialog(win, {
            title: '导出',
            defaultPath: defaultName || '808-output',
            filters: filters || [{name: '全部文件', extensions: ['*']}]
        });
        if (r.canceled || !r.filePath) return {ok: false, canceled: true};
        return {ok: true, path: r.filePath};
    });

    ipcMain.handle('n8:writePath', async (_e, {filePath, data}) => {
        try {
            fs.writeFileSync(filePath, Buffer.from(data));
            return {ok: true, path: filePath};
        } catch (err) {
            return {ok: false, error: String(err && err.message ? err.message : err)};
        }
    });

    ipcMain.handle('n8:openFile', async (_e, {filters}) => {
        const r = await dialog.showOpenDialog(win, {
            title: '打开',
            properties: ['openFile'],
            filters: filters || [{name: '全部文件', extensions: ['*']}]
        });
        if (r.canceled || !r.filePaths.length) return {ok: false, canceled: true};
        try {
            const buf = fs.readFileSync(r.filePaths[0]);
            return {
                ok: true,
                path: r.filePaths[0],
                name: path.basename(r.filePaths[0]),
                data: buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength)
            };
        } catch (err) {
            return {ok: false, error: String(err && err.message ? err.message : err)};
        }
    });

    createWindow();

    app.on('activate', () => {
        if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
});

app.on('window-all-closed', () => app.quit());
