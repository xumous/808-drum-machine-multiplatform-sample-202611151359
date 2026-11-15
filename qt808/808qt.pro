QT += core gui widgets multimedia
CONFIG += c++14 release
TEMPLATE = app
TARGET = 808qt
CONFIG -= debug_and_release
SOURCES += main.cpp audioengine.cpp machine.cpp \
           ../core808/core808.c ../core808/core808_io.c
HEADERS += audioengine.h machine.h
INCLUDEPATH += ../core808
LIBS += -lwinmm -lavrt -lole32
RC_ICONS = ../app.ico
RESOURCES += app.qrc
QMAKE_CXXFLAGS_RELEASE += -O2
