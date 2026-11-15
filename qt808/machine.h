/*
 * machine.h —— Qt Widgets 808 鼓机主界面（自绘 + 采样级音序）
 * UI 线程只读取工程快照并向无锁命令队列投递编辑；音频回调在 AudioEngine 中。
 */
#ifndef MACHINE_H
#define MACHINE_H

#include <QWidget>
#include <QFont>
#include <QRectF>
#include <QTimer>
#include <QString>
#include <QThread>

#include "core808.h"
#include "audioengine.h"

class ExportThread : public QThread
{
    Q_OBJECT
public:
    enum Kind { Wav = 0, Midi = 1, Stems = 2 };
    ExportThread(c808_engine* e, Kind kind, const QString& path, int mode,
                 int loops, QObject* parent = nullptr);
    void run() override;
signals:
    void done(int rc);
private:
    c808_engine* m_engine;
    Kind m_kind;
    QString m_path;
    int m_mode;
    int m_loops;
};

class MachineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MachineWidget(QWidget* parent = nullptr);
    ~MachineWidget() override;
    c808_engine* engine() const { return m_engine; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void closeEvent(QCloseEvent*) override;

private slots:
    void onTick();
    void onExportDone(int rc);

private:
    void layout();
    void refreshFonts();
    void drawButton(QPainter& p, const QRectF& r, const QString& text,
                    bool active, const QFont& f);
    void drawKnob(QPainter& p, const QRectF& r, const QString& label, float v);
    void drawSlider(QPainter& p, const QRectF& r, const QString& label,
                    float v, const QString& val, bool bipolar);
    void drawMeter(QPainter& p, const QRectF& r, float v);
    void clickAt(const QPointF& pos, Qt::KeyboardModifiers mods);
    void gridStepAt(const QPointF& pos, int* track, int* step);
    void setStepSlider(int idx, qreal x);
    void cycleProb(int track, int step);
    void cycleRatchet(int track, int step, int delta);
    void tapTempo();
    void saveProject();
    void loadProject();
    void startExport(int kind);
    void cycleMidi();
    void openSongEditor();
    void setMessage(const QString& s);
    qreal S(qreal v) const { return v * m_scale; }

    c808_engine* m_engine = nullptr;
    AudioEngine* m_audio = nullptr;
    bool m_ownsFallback = false;

    c808_status m_status;
    int m_selTrack = 0;
    int m_selStep = 0;
    int m_page = 0;
    qreal m_scale = 1.0;
    QString m_msg;
    QString m_midiName;
    int m_midiIndex = -1;
    QTimer* m_timer = nullptr;
    ExportThread* m_export = nullptr;
    bool m_exportBusy = false;

    int m_dragId = -1;
    qreal m_dragY = 0;
    float m_dragStart = 0.0f;

    QFont m_fTitle, m_fLabel, m_fSmall, m_fTiny, m_fMono, m_fBig;

    QRectF rcPlay, rcStop, rcTap, rcBpm, rcBpmMinus, rcBpmPlus, rcSwing;
    QRectF rcPatPrev, rcPatNext, rcPatDisp, rcLenMinus, rcLenPlus, rcLenDisp;
    QRectF rcSong, rcSave, rcLoad, rcWav, rcMidi, rcStems, rcClear, rcCopy, rcSongEdit, rcMidiIn;
    QRectF rcSync, rcTimeSig, rcTrkLenM, rcTrkLenDisp, rcTrkLenP;
    QRectF rcGrid, rcGridSteps, rcPanel, rcMaster, rcPads, rcSongStrip, rcStatus;
    QRectF rcVoiceKnobs[C808_P_COUNT];
    QRectF rcMasterKnobs[C808_M_COUNT];
    QRectF rcStepSliders[5];
    QRectF rcCond, rcAccent, rcTrigger, rcPad[16], rcPatQuick[16];
    QRectF rcPagePrev, rcPageNext;
};

#endif
