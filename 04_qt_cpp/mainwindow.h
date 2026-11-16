// ============================================================================
//  mainwindow.h —— Qt C++ 版界面声明
// ============================================================================
#ifndef XU808_MAINWINDOW_H
#define XU808_MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include <QTimer>
#include "xu808.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class GridWidget;
class MeterWidget;
class ChannelStrip;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QSlider;
class QLabel;
class QCheckBox;
class QPushButton;

// 直接调用 winmm 的 waveOut：不依赖 Qt Multimedia 模块，
// 因此可以用 static Qt 构建出真正的单文件 EXE。
class XuWinAudio
{
public:
    explicit XuWinAudio(xu::Engine* e);
    ~XuWinAudio();
    bool start();
    void stop();
    bool running() const { return running_; }

private:
    static void CALLBACK waveProc(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
    void fillBuffer(int idx);
    xu::Engine* eng_;
    void* handle_ = nullptr;
    void* hdr_[4]{};
    short data_[4][xu::BLOCK * 2]{};
    int next_ = 0;
    bool running_ = false;
    float L_[xu::BLOCK]{}, R_[xu::BLOCK]{};
};

// 步进网格：自绘，左键开关、右键编辑
class GridWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GridWidget(xu::Project* p, QWidget* parent = nullptr);
    void rebuild();
    void setPlayhead(int s);
    signals :
    
    void hitPad(int pad);
    void toggleStep(int pad, int step);
    void editStep(int pad, int step);
    void toggleMute(int pad);
    void toggleSolo(int pad);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    xu::Project* proj_;
    int playhead_ = -1;
    int cols_ = 16;
    int cellW_ = 30;
    static constexpr int kCellH = 22, kLabelW = 132, kHeadH = 24;
};

class MeterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MeterWidget(QWidget* parent = nullptr);
    void setLevels(float l, float r);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    float l_ = 0, r_ = 0;
};

// 一条通道条：名字/试听、M/S、2~3 个音色滑杆、音量、声像、轨道长度
class ChannelStrip : public QWidget
{
    Q_OBJECT

public:
    ChannelStrip(int pad, xu::Project* p, QWidget* parent = nullptr);
    void syncFromProject();
    signals :
    
    void hitPad(int pad);

private:
    int pad_;
    xu::Project* proj_;
    QVector<QSlider*> paramSliders_;
    QVector<QString> paramKeys_;
    QVector<int> paramMax_;
    QSlider* level_ = nullptr;
    QSlider* pan_ = nullptr;
    QSpinBox* len_ = nullptr;
    QPushButton* muteBtn_ = nullptr;
    QPushButton* soloBtn_ = nullptr;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(xu::Project* p, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject*, QEvent*) override;

private
    slots :
    
    void onTick();

private:
    void buildUi();
    void startAudio();
    void startMidi();
    void handleMidi();
    void refreshPatternCombo();
    void syncStrips();
    void exportWav(bool song);
    void exportMidi(bool song);

    xu::Project* proj_;
    xu::Engine* eng_;
    XuWinAudio* audio_ = nullptr;
    GridWidget* grid_ = nullptr;
    QVector<ChannelStrip*> strips_;
    MeterWidget* meter_ = nullptr;
    QTimer timer_;
    QComboBox* cbPattern_ = nullptr;
    QDoubleSpinBox* spBpm_ = nullptr;
    QSlider* slBpm_ = nullptr;
    QSlider* slSwing_ = nullptr;
    QLabel* lbSwing_ = nullptr;
    QSpinBox* spSteps_ = nullptr;
    QCheckBox* cbSong_ = nullptr;
    QLabel* lbStatus_ = nullptr;
    QLabel* lbMidi_ = nullptr;
    bool updating_ = false;
};

#endif