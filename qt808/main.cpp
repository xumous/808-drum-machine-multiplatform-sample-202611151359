/*
 * main.cpp —— Qt 5.14 版 808 鼓机入口
 */
#include <QApplication>
#include <QIcon>
#include <QFont>
#include <QTimer>
#include "machine.h"

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("808 Drum Machine"));
    app.setOrganizationName(QStringLiteral("808"));
    app.setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    app.setFont(QFont(QStringLiteral("Segoe UI"), 9));

    MachineWidget w;
    w.setWindowTitle(QStringLiteral("808 · SYNTHETIC DRUM MACHINE"));
    w.setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    w.resize(1120, 600);
    w.show();

    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--autoplay")) {
            QTimer::singleShot(1200, &w, [&w]() { c808_transport_play(w.engine()); });
        }
    }
    return app.exec();
}
