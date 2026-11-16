# XU-808 Qt C++ 工程文件
# 用 Qt 6.8.1 的 static-mingw 套件构建，可得到不依赖 Qt DLL 的单文件 EXE。
QT += core gui widgets
CONFIG += c++17 release
CONFIG -= debug_and_release
TEMPLATE = app
TARGET = XU808-Qt
SOURCES += main.cpp xu808.cpp mainwindow.cpp
HEADERS += xu808.h mainwindow.h
RC_FILE = app.rc
LIBS += -lwinmm -lole32
# Windows 下静态链接 GCC 运行库，避免目标机器缺 libgcc/libstdc++
QMAKE_LFLAGS += -static -static-libgcc -static-libstdc++
