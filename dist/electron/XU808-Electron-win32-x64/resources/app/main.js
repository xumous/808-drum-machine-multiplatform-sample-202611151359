// ============================================================================
//  main.js —— Electron 主进程
//
//  徐某把主进程写得很薄：只负责开窗口、设图标、退出时清理。
//  音频与 DSP 全部在渲染进程的 AudioWorklet 里，
//  这样主进程的事件循环再忙也不会影响鼓点。
// ============================================================================
const {app, BrowserWindow} = require('electron');
const path = require('path');

// 让 AudioContext 不需要用户手势就能启动（鼓机应用必须打开即响）。
// 这是 Chromium 的启动参数，必须写在 app.ready 之前。
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

// 让任务栏显示 XU-808 而不是 electron.exe
if (process.platform === 'win32') {
    app.setAppUserModelId('XuMou.XU808.Electron');
}

function createWindow() {
    const win = new BrowserWindow({
        width: 1520,
        height: 940,
        minWidth: 1100,
        minHeight: 720,
        backgroundColor: '#121212',
        title: 'XU-808 鼓机 - 徐某 (Electron)',
        icon: path.join(__dirname, 'app.ico'),
        webPreferences: {
            // 本应用不加载任何远程内容，关闭 nodeIntegration 更安全；
            // 文件保存用浏览器标准的 Blob 下载，无需 IPC。
            nodeIntegration: false,
            contextIsolation: true,
            autoplayPolicy: 'no-user-gesture-required'
        }
    });
    win.setMenuBarVisibility(false);
    win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
}

app.whenReady().then(() => {
    createWindow();
    app.on('activate', () => {
        if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
});

app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') app.quit();
});
