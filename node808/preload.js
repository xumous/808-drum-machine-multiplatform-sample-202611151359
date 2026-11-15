/*
 * preload.js —— 只暴露文件对话框能力，渲染进程无 Node 权限
 */
const {contextBridge, ipcRenderer} = require('electron');

contextBridge.exposeInMainWorld('n8', {
    saveFile: (data, defaultName, filters) =>
        ipcRenderer.invoke('n8:saveFile', {data, defaultName, filters}),
    pickSavePath: (defaultName, filters) =>
        ipcRenderer.invoke('n8:pickSavePath', {defaultName, filters}),
    writePath: (filePath, data) =>
        ipcRenderer.invoke('n8:writePath', {filePath, data}),
    openFile: (filters) => ipcRenderer.invoke('n8:openFile', {filters})
});
