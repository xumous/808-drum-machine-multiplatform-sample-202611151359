// ============================================================================
//  main.cpp —— Qt C++ 版入口
//
//  支持 --selftest：无窗口渲染一个 pattern 并写 WAV + 标记文件，
//  方便构建机验证 DSP 是否真的出声。
// ============================================================================
#include <QApplication>
#include <QFile>
#include <QTextStream>
#include "xu808.h"
#include "mainwindow.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("XU-808 Qt");
    app.setOrganizationName("徐某");

    xu::Project proj;

    bool selftest = false;
    for (int i = 1; i < argc; ++i)
        if (QString(argv[i]) == "--selftest") selftest = true;

    if (selftest)
    {
        // 注意：Engine 内含 64 个 voice，每个带 8192 个噪声样本（约 2MB），
        // 放在栈上会直接爆掉 Windows 默认 1MB 主线程栈（0xC00000FD）。
        // 徐某第一次跑 selftest 就被这个栈溢出教育了，所以必须放堆上。
        auto* eng = new xu::Engine(&proj);
        auto audio = eng->renderPattern(1);
        bool ok = xu::Engine::writeWav("selftest_qt.wav", audio);
        QFile f("selftest_qt.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream ts(&f);
            int frames = int(audio.size() / 2);
            float peak = 0;
            for (float v : audio) peak = std::max(peak, std::abs(v));
            ts << "samples=" << frames << " peak=" << peak << "\n";
            ts << (ok && frames > xu::SR && peak > 0.01f ? "SELFTEST OK" : "SELFTEST FAIL") << "\n";
        }
        delete eng;
        return ok ? 0 : 1;
    }

    MainWindow w(&proj);
    w.show();
    return app.exec();
}