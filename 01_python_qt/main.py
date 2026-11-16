# -*- coding: utf-8 -*-
"""
XU-808 Python 版入口。

用法：
    python main.py                     # 正常启动图形界面
    python main.py --selftest          # 无界面自检：渲染两小节并打印统计
    python main.py --export out.wav    # 无界面导出当前 pattern 为 WAV
    python main.py --midi out.mid      # 无界面导出 MIDI

徐某加 --selftest 的原因很实在：在没有声卡、没有显示器的构建机上，
也能证明 DSP 与音序器真的算出了声音，而不是“看起来能跑”。
"""

from __future__ import annotations

import os
import sys


def _app_dir() -> str:
    """兼容 PyInstaller 单文件模式：资源在 sys._MEIPASS 临时目录里。"""
    if getattr(sys, "frozen", False):
        return getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def main() -> int:
    # 延迟导入：--selftest 时不需要 Qt，避免在无显示环境里创建 QApplication
    from xu808.project import XuProject
    from xu808.engine import XuEngine, export_midi

    args = sys.argv[1:]
    project = XuProject()

    if "--selftest" in args:
        eng = XuEngine(project)
        audio = eng.render_pattern(repeats=1)
        import numpy as np
        peak = float(np.max(np.abs(audio))) if audio.size else 0.0
        rms = float(np.sqrt(np.mean(audio ** 2))) if audio.size else 0.0
        nz = int(np.count_nonzero(np.abs(audio) > 1e-4))
        print(f"XU808 SELFTEST: samples={audio.shape[0]} peak={peak:.4f} "
              f"rms={rms:.4f} nonzero={nz}")
        assert audio.shape[1] == 2, "必须是立体声"
        assert peak > 0.01, "渲染结果几乎是静音，DSP 可能有 bug"
        assert peak <= 1.0, "输出超过 0dBFS，限幅器失效"
        # 再验证停止后不会残留声音：停止并渲染 1 秒，应当接近静音
        eng2 = XuEngine(project)
        eng2.play()
        eng2.render_block(512)
        eng2.stop()
        # 注意：这里必须“连续”渲染，不能手动跳过 sample_pos。
        # 因为主滤波器的 zi 状态是按样本连续的；跳过样本相当于让滤波器
        # 带着上一块的残余能量突然继续，会产生一个测试假象。
        tail = eng2.render_block(4800)
        last = float(np.max(np.abs(tail[-1000:])))
        print(f"XU808 SELFTEST: after-stop tail-peak={last:.6f}")
        assert last < 0.01, "停止后仍有残留声音！"
        print("XU808 SELFTEST: OK")
        return 0

    if "--export" in args:
        i = args.index("--export")
        path = args[i + 1] if i + 1 < len(args) else "out.wav"
        XuEngine(project).export_wav(path, mode="pattern")
        print(f"WAV written: {path}")
        return 0

    if "--midi" in args:
        i = args.index("--midi")
        path = args[i + 1] if i + 1 < len(args) else "out.mid"
        export_midi(project, path)
        print(f"MIDI written: {path}")
        return 0

    # ---------------- 正常 GUI 启动 ----------------
    from PySide6.QtWidgets import QApplication
    from PySide6.QtGui import QIcon
    from xu808.ui import XuMainWindow

    # Windows 任务栏图标：不设置 AppUserModelID 时，任务栏会显示 python.exe 的图标
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("XuMou.XU808.Python")
        except Exception:
            pass

    app = QApplication(sys.argv)
    app.setApplicationName("XU-808 鼓机")
    app.setOrganizationName("徐某")
    icon_path = os.path.join(_app_dir(), "app.ico")
    if os.path.exists(icon_path):
        app.setWindowIcon(QIcon(icon_path))
    win = XuMainWindow(project)
    if os.path.exists(icon_path):
        win.setWindowIcon(QIcon(icon_path))
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
