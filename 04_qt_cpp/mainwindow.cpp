// ============================================================================
//  mainwindow.cpp —— Qt C++ 版界面实现
//
//  UI 只做两件事：把操作写进 xu::Project，和每 33ms 读一次引擎快照。
//  音频线程由 QAudioSink 的回调驱动，永远不等 UI。
// ============================================================================
#include "mainwindow.h"

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QScrollArea>
#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QIcon>
#include <QTimer>
#include <QDateTime>
#include <QPolygon>
#include <algorithm>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace
{
    // 颜色
    const QColor C_BG(18, 18, 18), C_PANEL(28, 28, 28), C_PANEL2(35, 35, 35), C_LINE(46, 46, 46);
    const QColor C_ORANGE(255, 122, 24), C_ORANGED(168, 74, 12), C_YELLOW(255, 209, 102);
    const QColor C_CYAN(76, 201, 240), C_TEXT(232, 232, 232), C_DIM(138, 138, 138), C_RED(229, 72, 77);

    // 通道条参数（与规格书一致）
    struct PD
    {
        const char* label;
        const char* key;
        int max;
    };

    QVector<PD> paramsFor(int pad)
    {
        if (pad == 0) return {{"TUNE", "tune", 100}, {"DECAY", "decay", 100}, {"TONE", "tone", 100}};
        if (pad == 1) return {{"TUNE", "tune", 100}, {"SNAP", "snappy", 100}, {"DECAY", "decay", 100}};
        if (pad >= 2 && pad <= 4) return {{"TUNE", "tune", 100}, {"DECAY", "decay", 100}, {"COLOR", "tone", 100}};
        if (pad == 10) return {{"DECAY", "decay", 100}, {"TONE", "tone", 100}, {"TAIL", "tail", 100}};
        if (pad == 11) return {{"DECAY", "decay", 100}, {"TONE", "tone", 100}};
        if (pad == 12) return {{"TUNE", "tune", 100}, {"DECAY", "decay", 100}};
        if (pad >= 13) return {{"DECAY", "decay", 100}, {"TONE", "tone", 100}};
        return {{"TUNE", "tune", 100}, {"DECAY", "decay", 100}};
    }

    float* fieldPtr(xu::PadParams& p, const QString& key)
    {
        if (key == "tune") return &p.tune;
        if (key == "decay") return &p.decay;
        if (key == "tone") return &p.tone;
        if (key == "snappy") return &p.snappy;
        if (key == "tail") return &p.tail;
        if (key == "level") return &p.level;
        return &p.pan;
    }

    // ---------------- MIDI 输入（winmm，无第三方库） ----------------
#ifdef Q_OS_WIN
    struct MidiMsg
    {
        unsigned char s, d1, d2;
    };
    MidiMsg g_mq[256];
    volatile LONG g_mHead = 0, g_mTail = 0;
    HMIDIIN g_midi = nullptr;

    void CALLBACK midiProc(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR)
    {
        if (msg != MIM_DATA) return;
        LONG t = g_mTail;
        LONG n = (t + 1) & 255;
        if (n == g_mHead) return; // 队列满，丢弃最旧策略太复杂；这里直接丢新
        g_mq[t].s = (unsigned char)(p1 & 0xFF);
        g_mq[t].d1 = (unsigned char)((p1 >> 8) & 0xFF);
        g_mq[t].d2 = (unsigned char)((p1 >> 16) & 0xFF);
        g_mTail = n;
    }
#endif
} // namespace

// ===========================================================================
//  XuWinAudio —— winmm waveOut（四缓冲，回调驱动）
// ===========================================================================
#ifdef Q_OS_WIN
namespace
{
    struct WaveHdr
    {
        void* lpData;
        unsigned long dwBufferLength;
        unsigned long dwBytesRecorded;
        void* dwUser;
        unsigned long dwFlags;
        unsigned long dwLoops;
        void* lpNext;
        void* reserved;
    };

    struct WaveFmt
    {
        unsigned short wFormatTag;
        unsigned short nChannels;
        unsigned long nSamplesPerSec;
        unsigned long nAvgBytesPerSec;
        unsigned short nBlockAlign;
        unsigned short wBitsPerSample;
        unsigned short cbSize;
    };
}
#endif

XuWinAudio::XuWinAudio(xu::Engine* e) : eng_(e)
{
}

XuWinAudio::~XuWinAudio() { stop(); }

void XuWinAudio::fillBuffer(int idx)
{
    eng_->render(xu::BLOCK, L_, R_);
    for (int i = 0; i < xu::BLOCK; ++i)
    {
        float l = std::clamp(L_[i], -1.0f, 1.0f);
        float r = std::clamp(R_[i], -1.0f, 1.0f);
        data_[idx][i * 2] = short(l * 32767.0f);
        data_[idx][i * 2 + 1] = short(r * 32767.0f);
    }
}

void CALLBACK XuWinAudio::waveProc(HWAVEOUT, UINT msg, DWORD_PTR inst,
                                   DWORD_PTR, DWORD_PTR)
{
#ifdef Q_OS_WIN
    if (msg != WOM_DONE) return;
    auto* self = reinterpret_cast<XuWinAudio*>(inst);
    if (!self || !self->running_) return;
    int idx = self->next_;
    self->next_ = (self->next_ + 1) & 3;
    self->fillBuffer(idx);
    waveOutWrite((HWAVEOUT)self->handle_, (WAVEHDR*)self->hdr_[idx], sizeof(WAVEHDR));
#endif
}

bool XuWinAudio::start()
{
#ifdef Q_OS_WIN
    if (running_) return true;
    WaveFmt fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = xu::SR;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = xu::SR * 4;
    HWAVEOUT h = nullptr;
    if (waveOutOpen(&h, WAVE_MAPPER, (WAVEFORMATEX*)&fmt,
                    (DWORD_PTR)waveProc, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        return false;
    handle_ = h;
    for (int i = 0; i < 4; ++i)
    {
        auto* hdr = new WAVEHDR{};
        hdr->lpData = (LPSTR)data_[i];
        hdr->dwBufferLength = xu::BLOCK * 4;
        waveOutPrepareHeader(h, hdr, sizeof(WAVEHDR));
        fillBuffer(i);
        waveOutWrite(h, hdr, sizeof(WAVEHDR));
        hdr_[i] = hdr;
    }
    next_ = 0;
    running_ = true;
    return true;
#else
    return false;
#endif
}

void XuWinAudio::stop()
{
#ifdef Q_OS_WIN
    if (!running_) return;
    running_ = false;
    HWAVEOUT h = (HWAVEOUT)handle_;
    waveOutReset(h);
    for (int i = 0; i < 4; ++i)
    {
        if (hdr_[i])
        {
            waveOutUnprepareHeader(h, (WAVEHDR*)hdr_[i], sizeof(WAVEHDR));
            delete (WAVEHDR*)hdr_[i];
            hdr_[i] = nullptr;
        }
    }
    waveOutClose(h);
    handle_ = nullptr;
#endif
}

// ===========================================================================
//  GridWidget
// ===========================================================================
GridWidget::GridWidget(xu::Project* p, QWidget* parent) : QWidget(parent), proj_(p)
{
    setMouseTracking(true);
    rebuild();
}

void GridWidget::rebuild()
{
    cols_ = std::max(1, proj_->cur().steps);
    cellW_ = 32;
    int w = kLabelW + cols_ * cellW_;
    if (w > 1500)
    {
        cellW_ = std::max(10, (1500 - kLabelW) / cols_);
        w = kLabelW + cols_ * cellW_;
    }
    setFixedSize(w, kHeadH + xu::PADS * kCellH);
    update();
}

void GridWidget::setPlayhead(int s)
{
    if (s != playhead_)
    {
        playhead_ = s;
        update();
    }
}

void GridWidget::paintEvent(QPaintEvent*)
{
    QPainter g(this);
    g.fillRect(rect(), C_BG);
    const xu::Pattern& pat = proj_->cur();
    g.fillRect(0, 0, width(), kHeadH, C_PANEL);
    // 顶部步号
    g.setFont(QFont("Segoe UI", 7));
    for (int c = 0; c < cols_; ++c)
    {
        int x = kLabelW + c * cellW_;
        if (c % 4 == 0) g.fillRect(x, 0, 1, height(), QColor(58, 58, 58));
        g.setPen(C_DIM);
        g.drawText(QRect(x, 0, cellW_, kHeadH), Qt::AlignCenter, QString::number(c + 1));
    }
    // 左侧标签
    g.fillRect(0, kHeadH, kLabelW, height() - kHeadH, C_PANEL2);
    for (int r = 0; r < xu::PADS; ++r)
    {
        int y = kHeadH + r * kCellH;
        g.fillRect(kLabelW, y, width() - kLabelW, kCellH,
                   QColor(r % 2 == 0 ? 22 : 26, r % 2 == 0 ? 22 : 26, r % 2 == 0 ? 22 : 26));
        g.setFont(QFont("Consolas", 8, QFont::Bold));
        g.setPen(C_ORANGE);
        g.drawText(QRect(5, y, 34, kCellH), Qt::AlignVCenter | Qt::AlignLeft, xu::kCodes[r]);
        g.setFont(QFont("Microsoft YaHei UI", 7));
        g.setPen(C_TEXT);
        g.drawText(QRect(40, y, 50, kCellH), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromWCharArray(xu::kNamesCn[r]));
        const xu::PadParams& pd = proj_->pads[r];
        for (int j = 0; j < 2; ++j)
        {
            bool on = j == 0 ? pd.mute : pd.solo;
            int bx = 92 + j * 18;
            g.fillRect(bx, y + 4, 15, kCellH - 8, on ? (j == 0 ? C_RED : C_YELLOW) : QColor(42, 42, 42));
            g.setPen(on ? QColor(18, 18, 18) : C_DIM);
            g.setFont(QFont("Segoe UI", 6, QFont::Bold));
            g.drawText(QRect(bx, y + 4, 15, kCellH - 8), Qt::AlignCenter, j == 0 ? "M" : "S");
        }
        g.setPen(C_LINE);
        g.drawLine(0, y + kCellH - 1, width(), y + kCellH - 1);
    }
    // 格子
    for (int c = 0; c < cols_; ++c)
    {
        int x = kLabelW + c * cellW_;
        for (int r = 0; r < xu::PADS; ++r)
        {
            const xu::Step& st = pat.data[r][c];
            int y = kHeadH + r * kCellH;
            QRect cell(x + 2, y + 2, cellW_ - 4, kCellH - 4);
            if (st.on)
            {
                QColor base = st.accent ? C_YELLOW : C_ORANGE;
                int a = 110 + int(145 * (st.velocity / 127.0));
                base.setAlpha(a);
                g.fillRect(cell, base);
                if (st.prob < 100 || st.ratchet > 1)
                {
                    g.setPen(QColor(26, 16, 8));
                    g.setFont(QFont("Consolas", 5, QFont::Bold));
                    g.drawText(cell, Qt::AlignCenter,
                               st.ratchet > 1 ? "R" : QString::number(st.prob));
                }
            }
            else
            {
                g.fillRect(cell, QColor(16, 16, 16));
                g.setPen(QColor(39, 39, 39));
                g.drawRect(cell);
            }
        }
    }
    // 播放头
    if (playhead_ >= 0 && playhead_ < cols_)
    {
        int x = kLabelW + playhead_ * cellW_;
        g.fillRect(x, kHeadH, cellW_, height() - kHeadH, QColor(76, 201, 240, 26));
        g.setPen(QPen(C_CYAN, 2));
        g.drawLine(x, kHeadH, x, height());
        g.drawLine(x + cellW_, kHeadH, x + cellW_, height());
        QPolygon tri;
        tri << QPoint(x + 7, 0) << QPoint(x + cellW_ - 7, 0)
            << QPoint(x + cellW_ / 2, 10);
        g.setBrush(C_CYAN);
        g.setPen(Qt::NoPen);
        g.drawPolygon(tri);
    }
    g.setPen(C_LINE);
    g.setBrush(Qt::NoBrush);
    g.drawRect(rect().adjusted(0, 0, -1, -1));
}

void GridWidget::mousePressEvent(QMouseEvent* e)
{
    int x = int(e->position().x()), y = int(e->position().y());
    if (y < kHeadH) return;
    int r = (y - kHeadH) / kCellH;
    if (r < 0 || r >= xu::PADS) return;
    if (x < kLabelW)
    {
        for (int j = 0; j < 2; ++j)
        {
            int bx = 92 + j * 18;
            if (x >= bx && x <= bx + 15)
            {
                if (j == 0) emit toggleMute(r);
                else emit toggleSolo(r);
                return;
            }
        }
        emit hitPad(r);
        return;
    }
    int c = (x - kLabelW) / cellW_;
    if (c < 0 || c >= cols_) return;
    if (e->button() == Qt::RightButton) emit editStep(r, c);
    else emit toggleStep(r, c);
}

// ===========================================================================
//  MeterWidget
// ===========================================================================
MeterWidget::MeterWidget(QWidget* parent) : QWidget(parent) { setFixedSize(150, 22); }

void MeterWidget::setLevels(float l, float r)
{
    l_ = l;
    r_ = r;
    update();
}

void MeterWidget::paintEvent(QPaintEvent*)
{
    QPainter g(this);
    g.fillRect(rect(), QColor(12, 12, 12));
    for (int i = 0; i < 2; ++i)
    {
        float v = std::clamp(i == 0 ? l_ : r_, 0.0f, 1.0f);
        int y = 3 + i * 9;
        g.fillRect(2, y, width() - 4, 7, QColor(26, 26, 26));
        int w = int((width() - 4) * v);
        if (w > 0)
        {
            QLinearGradient lg(2, 0, 2 + w, 0);
            lg.setColorAt(0, QColor(61, 220, 132));
            lg.setColorAt(0.72, C_YELLOW);
            lg.setColorAt(1, C_RED);
            g.fillRect(2, y, w, 7, lg);
        }
    }
    g.setPen(C_LINE);
    g.drawRect(0, 0, width() - 1, height() - 1);
}

// ===========================================================================
//  ChannelStrip
// ===========================================================================
ChannelStrip::ChannelStrip(int pad, xu::Project* p, QWidget* parent)
    : QWidget(parent), pad_(pad), proj_(p)
{
    setFixedWidth(76);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* name = new QPushButton(QString("%1\n%2").arg(xu::kCodes[pad])
                                                  .arg(QString::fromWCharArray(xu::kNamesCn[pad])));
    name->setFixedHeight(34);
    name->setStyleSheet(QString("QPushButton{font-size:9px;color:%1;background:#232323;"
            "border:1px solid #3a2a1a;border-radius:4px;}")
        .arg(C_ORANGE.name()));
    connect(name, &QPushButton::clicked, this, [this] { emit hitPad(pad_); });
    lay->addWidget(name);

    auto* ms = new QHBoxLayout();
    ms->setSpacing(2);
    muteBtn_ = new QPushButton("M");
    soloBtn_ = new QPushButton("S");
    for (auto* b : {muteBtn_, soloBtn_})
    {
        b->setCheckable(true);
        b->setFixedHeight(16);
        b->setStyleSheet("QPushButton{font-size:8px;padding:0;}");
    }
    muteBtn_->setChecked(proj_->pads[pad_].mute);
    soloBtn_->setChecked(proj_->pads[pad_].solo);
    connect(muteBtn_, &QPushButton::toggled, this, [this](bool v) { proj_->pads[pad_].mute = v; });
    connect(soloBtn_, &QPushButton::toggled, this, [this](bool v) { proj_->pads[pad_].solo = v; });
    ms->addWidget(muteBtn_);
    ms->addWidget(soloBtn_);
    lay->addLayout(ms);

    auto pdef = paramsFor(pad_);
    for (const PD& d : pdef)
    {
        auto* lab = new QLabel(d.label);
        lab->setAlignment(Qt::AlignCenter);
        lab->setStyleSheet(QString("color:%1;font-size:7px;").arg(C_DIM.name()));
        lay->addWidget(lab);
        auto* s = new QSlider(Qt::Vertical);
        s->setRange(0, 100);
        s->setFixedHeight(40);
        s->setValue(int(*fieldPtr(proj_->pads[pad_], d.key) * 100));
        connect(s, &QSlider::valueChanged, this, [this, key = QString(d.key)](int v)
        {
            *fieldPtr(proj_->pads[pad_], key) = v / 100.0f;
        });
        lay->addWidget(s, 0, Qt::AlignHCenter);
        paramSliders_.append(s);
        paramKeys_.append(d.key);
        paramMax_.append(d.max);
    }
    // 音量 / 声像
    for (const auto& pair : QVector<QPair<QString, QString>>{{"LEVEL", "level"}, {"PAN", "pan"}})
    {
        auto* lab = new QLabel(pair.first);
        lab->setAlignment(Qt::AlignCenter);
        lab->setStyleSheet(QString("color:%1;font-size:7px;").arg(C_DIM.name()));
        lay->addWidget(lab);
        auto* s = new QSlider(Qt::Vertical);
        s->setRange(0, 100);
        s->setFixedHeight(pair.first == "LEVEL" ? 34 : 26);
        s->setValue(int(*fieldPtr(proj_->pads[pad_], pair.second) * 100));
        connect(s, &QSlider::valueChanged, this, [this, key = pair.second, s](int v)
        {
            *fieldPtr(proj_->pads[pad_], key) = (key == "level") ? v / 100.0f : v / 100.0f;
            (void)s;
        });
        lay->addWidget(s, 0, Qt::AlignHCenter);
        if (pair.first == "LEVEL") level_ = s;
        else pan_ = s;
    }
    len_ = new QSpinBox();
    len_->setRange(1, 64);
    len_->setValue(proj_->cur().trackLen[pad_]);
    len_->setFixedHeight(18);
    len_->setStyleSheet("QSpinBox{font-size:8px;}");
    connect(len_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v)
    {
        proj_->cur().trackLen[pad_] = v;
    });
    lay->addWidget(len_);
    lay->addStretch(1);
}

void ChannelStrip::syncFromProject()
{
    for (int i = 0; i < paramSliders_.size(); ++i)
    {
        paramSliders_[i]->blockSignals(true);
        paramSliders_[i]->setValue(int(*fieldPtr(proj_->pads[pad_], paramKeys_[i]) * 100));
        paramSliders_[i]->blockSignals(false);
    }
    muteBtn_->blockSignals(true);
    soloBtn_->blockSignals(true);
    muteBtn_->setChecked(proj_->pads[pad_].mute);
    soloBtn_->setChecked(proj_->pads[pad_].solo);
    muteBtn_->blockSignals(false);
    soloBtn_->blockSignals(false);
    len_->blockSignals(true);
    len_->setValue(proj_->cur().trackLen[pad_]);
    len_->blockSignals(false);
}

// ===========================================================================
//  MainWindow
// ===========================================================================
MainWindow::MainWindow(xu::Project* p, QWidget* parent)
    : QMainWindow(parent), proj_(p), eng_(new xu::Engine(p))
{
    setWindowTitle(QStringLiteral("XU-808 鼓机 - 徐某 (Qt C++)"));
    resize(1520, 920);
    setStyleSheet(QString("QWidget{background:%1;color:%2;font-family:'Microsoft YaHei UI';font-size:11px;}"
            "QPushButton{background:%3;border:1px solid %4;border-radius:4px;padding:3px 8px;}"
            "QPushButton:hover{border-color:%5;}"
            "QPushButton:checked{background:%5;color:#111;}"
            "QComboBox,QSpinBox,QDoubleSpinBox{background:%3;border:1px solid %4;border-radius:3px;padding:2px;}"
            "QSlider::groove:horizontal{height:6px;background:#101010;border-radius:3px;}"
            "QSlider::handle:horizontal{width:12px;margin:-4px 0;background:%5;border-radius:3px;}"
            "QSlider::groove:vertical{width:5px;background:#101010;border-radius:2px;}"
            "QSlider::handle:vertical{height:10px;margin:0 -4px;background:%5;border-radius:2px;}")
        .arg(C_BG.name(), C_TEXT.name(), C_PANEL2.name(), C_LINE.name(), C_ORANGE.name()));
    buildUi();
    startAudio();
    startMidi();
    connect(&timer_, &QTimer::timeout, this, &MainWindow::onTick);
    timer_.start(33);
    if (QFile::exists("app.ico")) setWindowIcon(QIcon("app.ico"));
}

MainWindow::~MainWindow()
{
    eng_->stop();
    if (audio_)
    {
        audio_->stop();
        delete audio_;
        audio_ = nullptr;
    }
    delete eng_;
#ifdef Q_OS_WIN
    if (g_midi)
    {
        midiInStop(g_midi);
        midiInReset(g_midi);
        midiInClose(g_midi);
        g_midi = nullptr;
    }
#endif
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 4, 8, 4);
    root->setSpacing(6);

    // ---------- 顶部 ----------
    auto* top = new QHBoxLayout();
    auto* logoBox = new QVBoxLayout();
    auto* logo = new QLabel("XU-808");
    logo->setStyleSheet(QString("color:%1;font-family:Consolas;font-size:24px;font-weight:900;").arg(C_ORANGE.name()));
    auto* sub = new QLabel("RHYTHM COMPOSER · 徐某 · 16 VOICE");
    sub->setStyleSheet(QString("color:%1;font-size:8px;").arg(C_DIM.name()));
    logoBox->addWidget(logo);
    logoBox->addWidget(sub);
    top->addLayout(logoBox);
    top->addSpacing(12);

    auto* bPlay = new QPushButton("▶ PLAY");
    auto* bStop = new QPushButton("■ STOP");
    for (auto* b : {bPlay, bStop})
    {
        b->setFixedSize(88, 40);
        b->setStyleSheet("font-weight:bold;");
    }
    connect(bPlay, &QPushButton::clicked, this, [this] { eng_->play(); });
    connect(bStop, &QPushButton::clicked, this, [this]
    {
        eng_->stop();
        grid_->setPlayhead(-1);
    });
    top->addWidget(bPlay);
    top->addWidget(bStop);

    top->addWidget(new QLabel("BPM"));
    spBpm_ = new QDoubleSpinBox();
    spBpm_->setRange(40, 300);
    spBpm_->setDecimals(1);
    spBpm_->setValue(proj_->master.bpm);
    spBpm_->setFixedWidth(74);
    top->addWidget(spBpm_);
    slBpm_ = new QSlider(Qt::Horizontal);
    slBpm_->setRange(40, 300);
    slBpm_->setValue(int(proj_->master.bpm));
    slBpm_->setFixedWidth(100);
    connect(slBpm_, &QSlider::valueChanged, this, [this](int v) { spBpm_->setValue(v); });
    connect(spBpm_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v)
    {
        proj_->master.bpm = float(v);
        if (!updating_)
        {
            updating_ = true;
            slBpm_->setValue(int(v));
            updating_ = false;
        }
    });
    top->addWidget(slBpm_);
    auto* bTap = new QPushButton("TAP");
    bTap->setFixedSize(48, 40);
    connect(bTap, &QPushButton::clicked, this, [this]
    {
        static QVector<qint64> taps;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!taps.isEmpty() && now - taps.last() > 2000) taps.clear();
        taps.append(now);
        if (taps.size() > 8) taps.removeFirst();
        if (taps.size() >= 2)
        {
            QVector<double> iv;
            for (int i = 1; i < taps.size(); ++i)
            {
                double d = (taps[i] - taps[i - 1]) / 1000.0;
                if (d > 0.15 && d < 2.0) iv.append(d);
            }
            if (iv.size() >= 2)
            {
                iv.removeOne(*std::max_element(iv.begin(), iv.end()));
                iv.removeOne(*std::min_element(iv.begin(), iv.end()));
            }
            if (!iv.isEmpty())
            {
                double avg = 0;
                for (double x : iv) avg += x;
                avg /= iv.size();
                spBpm_->setValue(std::clamp(60.0 / avg, 40.0, 300.0));
            }
        }
    });
    top->addWidget(bTap);

    top->addWidget(new QLabel("SWING"));
    slSwing_ = new QSlider(Qt::Horizontal);
    slSwing_->setRange(0, 75);
    slSwing_->setValue(int(proj_->master.swing * 100));
    slSwing_->setFixedWidth(100);
    lbSwing_ = new QLabel(QString::number(slSwing_->value()) + "%");
    lbSwing_->setStyleSheet(QString("color:%1;").arg(C_ORANGE.name()));
    connect(slSwing_, &QSlider::valueChanged, this, [this](int v)
    {
        proj_->master.swing = v / 100.0f;
        lbSwing_->setText(QString::number(v) + "%");
    });
    top->addWidget(slSwing_);
    top->addWidget(lbSwing_);

    top->addWidget(new QLabel("PATTERN"));
    cbPattern_ = new QComboBox();
    refreshPatternCombo();
    connect(cbPattern_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i)
    {
        eng_->requestPattern(i);
        spSteps_->blockSignals(true);
        spSteps_->setValue(proj_->cur().steps);
        spSteps_->blockSignals(false);
        grid_->rebuild();
        syncStrips();
    });
    top->addWidget(cbPattern_);
    spSteps_ = new QSpinBox();
    spSteps_->setRange(1, 64);
    spSteps_->setValue(proj_->cur().steps);
    spSteps_->setFixedWidth(52);
    connect(spSteps_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v)
    {
        proj_->cur().steps = v;
        grid_->rebuild();
    });
    top->addWidget(spSteps_);

    cbSong_ = new QCheckBox("SONG");
    cbSong_->setChecked(proj_->master.songMode);
    connect(cbSong_, &QCheckBox::toggled, this, [this](bool v) { proj_->master.songMode = v; });
    top->addWidget(cbSong_);

    auto* bSong = new QPushButton("歌曲链…");
    connect(bSong, &QPushButton::clicked, this, [this]
    {
        QDialog dlg(this);
        dlg.setWindowTitle("歌曲链 - XU-808");
        dlg.resize(460, 340);
        auto* v = new QVBoxLayout(&dlg);
        auto* table = new QTableWidget(0, 2, &dlg);
        table->setHorizontalHeaderLabels({"Pattern", "重复"});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        for (const auto& e : proj_->song)
        {
            int r = table->rowCount();
            table->insertRow(r);
            auto* cb = new QComboBox();
            for (int i = 0; i < xu::PATTERNS; ++i) cb->
                addItem(QString("%1 %2").arg(i + 1).arg(proj_->patterns[i].name));
            cb->setCurrentIndex(std::clamp(e.pattern, 0, xu::PATTERNS - 1));
            table->setCellWidget(r, 0, cb);
            auto* sp = new QSpinBox();
            sp->setRange(1, 99);
            sp->setValue(e.repeats);
            table->setCellWidget(r, 1, sp);
        }
        v->addWidget(table);
        auto* row = new QHBoxLayout();
        auto* add = new QPushButton("添加");
        auto* del = new QPushButton("删除");
        row->addWidget(add);
        row->addWidget(del);
        row->addStretch(1);
        v->addLayout(row);
        connect(add, &QPushButton::clicked, [table]
        {
            int r = table->rowCount();
            table->insertRow(r);
            auto* cb = new QComboBox();
            for (int i = 1; i <= 16; ++i) cb->addItem(QString::number(i));
            table->setCellWidget(r, 0, cb);
            auto* sp = new QSpinBox();
            sp->setRange(1, 99);
            table->setCellWidget(r, 1, sp);
        });
        connect(del, &QPushButton::clicked, [table]
        {
            if (table->currentRow() >= 0) table->removeRow(table->currentRow());
        });
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        v->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() == QDialog::Accepted)
        {
            proj_->song.clear();
            for (int r = 0; r < table->rowCount(); ++r)
            {
                auto* cb = qobject_cast<QComboBox*>(table->cellWidget(r, 0));
                auto* sp = qobject_cast<QSpinBox*>(table->cellWidget(r, 1));
                if (cb && sp) proj_->song.push_back({cb->currentIndex(), sp->value()});
            }
        }
    });
    top->addWidget(bSong);
    top->addStretch(1);

    lbMidi_ = new QLabel("MIDI: -");
    lbMidi_->setStyleSheet(QString("color:%1;font-size:9px;").arg(C_DIM.name()));
    top->addWidget(lbMidi_);
    root->addLayout(top);

    // ---------- 网格 ----------
    grid_ = new GridWidget(proj_);
    connect(grid_, &GridWidget::hitPad, this, [this](int p) { eng_->triggerPad(p, 112, 0); });
    connect(grid_, &GridWidget::toggleStep, this, [this](int p, int s)
    {
        auto& st = proj_->cur().data[p][s];
        st.on = !st.on;
        if (st.on) eng_->triggerPad(p, st.velocity, st.accent);
        grid_->update();
    });
    connect(grid_, &GridWidget::toggleMute, this, [this](int p)
    {
        proj_->pads[p].mute = !proj_->pads[p].mute;
        grid_->update();
        syncStrips();
    });
    connect(grid_, &GridWidget::toggleSolo, this, [this](int p)
    {
        proj_->pads[p].solo = !proj_->pads[p].solo;
        grid_->update();
        syncStrips();
    });
    connect(grid_, &GridWidget::editStep, this, [this](int p, int s)
    {
        auto& st = proj_->cur().data[p][s];
        QDialog dlg(this);
        dlg.setWindowTitle("步进属性 - 徐某的 808");
        auto* form = new QFormLayout(&dlg);
        auto* on = new QCheckBox("启用");
        on->setChecked(st.on);
        auto* acc = new QCheckBox("重音 Accent");
        acc->setChecked(st.accent);
        auto* vel = new QSpinBox();
        vel->setRange(1, 127);
        vel->setValue(st.velocity);
        auto* prob = new QSpinBox();
        prob->setRange(0, 100);
        prob->setValue(st.prob);
        auto* mic = new QSpinBox();
        mic->setRange(-2400, 2400);
        mic->setValue(st.micro);
        auto* rat = new QSpinBox();
        rat->setRange(1, 8);
        rat->setValue(st.ratchet);
        auto* cond = new QComboBox();
        cond->addItems({"总是", "仅过门", "非过门", "每2循环", "每4循环"});
        cond->setCurrentIndex(std::clamp(st.cond, 0, 4));
        form->addRow(on);
        form->addRow(acc);
        form->addRow("力度 Velocity", vel);
        form->addRow("概率 Probability", prob);
        form->addRow("微时 Micro(样本)", mic);
        form->addRow("滚奏 Ratchet", rat);
        form->addRow("条件", cond);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(bb);
        connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() == QDialog::Accepted)
        {
            st.on = on->isChecked();
            st.accent = acc->isChecked();
            st.velocity = vel->value();
            st.prob = prob->value();
            st.micro = mic->value();
            st.ratchet = rat->value();
            st.cond = cond->currentIndex();
            grid_->update();
        }
    });
    auto* gridScroll = new QScrollArea();
    gridScroll->setWidget(grid_);
    gridScroll->setWidgetResizable(false);
    gridScroll->setFixedHeight(grid_->height() + 20);
    root->addWidget(gridScroll);

    // ---------- 通道条 ----------
    auto* stripScroll = new QScrollArea();
    stripScroll->setWidgetResizable(true);
    stripScroll->setFixedHeight(330);
    auto* holder = new QWidget();
    auto* hl = new QHBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(2);
    for (int i = 0; i < xu::PADS; ++i)
    {
        auto* s = new ChannelStrip(i, proj_);
        connect(s, &ChannelStrip::hitPad, this, [this](int p) { eng_->triggerPad(p, 112, 0); });
        hl->addWidget(s);
        strips_.append(s);
    }
    hl->addStretch(1);
    stripScroll->setWidget(holder);
    root->addWidget(stripScroll);

    // ---------- 主效果 ----------
    auto* mg = new QGroupBox("MASTER · 徐某的母线");
    auto* ml = new QHBoxLayout(mg);
    auto addMaster = [&](const QString& label, int lo, int hi, int val, auto setter)
    {
        auto* box = new QVBoxLayout();
        auto* lab = new QLabel(label);
        lab->setStyleSheet(QString("color:%1;font-size:8px;").arg(C_DIM.name()));
        auto* s = new QSlider(Qt::Horizontal);
        s->setRange(lo, hi);
        s->setValue(std::clamp(val, lo, hi));
        s->setFixedWidth(120);
        auto* vl = new QLabel(QString::number(val));
        vl->setStyleSheet(QString("color:%1;font-size:8px;").arg(C_ORANGE.name()));
        connect(s, &QSlider::valueChanged, this, [setter, vl](int v)
        {
            setter(v);
            vl->setText(QString::number(v));
        });
        box->addWidget(lab);
        box->addWidget(s);
        box->addWidget(vl);
        ml->addLayout(box);
    };
    addMaster("LP 低通", 200, 20000, int(proj_->master.lpCutoff), [this](int v) { proj_->master.lpCutoff = float(v); });
    addMaster("RES 共鸣", 50, 800, int(proj_->master.lpQ * 100), [this](int v) { proj_->master.lpQ = v / 100.0f; });
    addMaster("HP 高通", 10, 800, int(proj_->master.hpCutoff), [this](int v) { proj_->master.hpCutoff = float(v); });
    addMaster("DRIVE", 100, 800, int(proj_->master.drive * 100), [this](int v) { proj_->master.drive = v / 100.0f; });
    addMaster("VOL", 0, 100, int(proj_->master.volume * 100), [this](int v) { proj_->master.volume = v / 100.0f; });
    meter_ = new MeterWidget();
    ml->addWidget(meter_);
    lbStatus_ = new QLabel("CPU 0% · XRUN 0");
    lbStatus_->setStyleSheet(QString("color:%1;font-size:9px;").arg(C_DIM.name()));
    ml->addWidget(lbStatus_);
    root->addWidget(mg);

    // 菜单式按钮：保存/加载/导出放在底部，避免顶部过挤
    auto* bottom = new QHBoxLayout();
    auto* bSave = new QPushButton("保存工程");
    auto* bLoad = new QPushButton("打开工程");
    auto* bWav = new QPushButton("导出 WAV");
    auto* bMidi = new QPushButton("导出 MIDI");
    auto* bClock = new QCheckBox("MIDI CLK");
    connect(bSave, &QPushButton::clicked, this, [this]
    {
        QString f = QFileDialog::getSaveFileName(this, "保存工程", "xu808.xu808", "XU-808 工程 (*.xu808 *.json)");
        if (!f.isEmpty()) { if (!proj_->save(f)) QMessageBox::critical(this, "保存失败", "无法写入文件"); }
    });
    connect(bLoad, &QPushButton::clicked, this, [this]
    {
        QString f = QFileDialog::getOpenFileName(this, "打开工程", "", "XU-808 工程 (*.xu808 *.json)");
        if (f.isEmpty()) return;
        xu::Project np;
        if (!xu::Project::load(f, np))
        {
            QMessageBox::critical(this, "加载失败", "文件格式错误");
            return;
        }
        eng_->stop();
        *proj_ = np;
        eng_->proj = proj_;
        refreshPatternCombo();
        grid_->rebuild();
        syncStrips();
    });
    connect(bWav, &QPushButton::clicked, this, [this] { exportWav(proj_->master.songMode); });
    connect(bMidi, &QPushButton::clicked, this, [this] { exportMidi(proj_->master.songMode); });
    connect(bClock, &QCheckBox::toggled, this, [this](bool v) { eng_->clockExternal = v; });
    bottom->addWidget(bSave);
    bottom->addWidget(bLoad);
    bottom->addWidget(bWav);
    bottom->addWidget(bMidi);
    bottom->addWidget(bClock);
    bottom->addStretch(1);
    root->addLayout(bottom);
}

void MainWindow::refreshPatternCombo()
{
    if (!cbPattern_) return;
    cbPattern_->blockSignals(true);
    cbPattern_->clear();
    for (int i = 0; i < xu::PATTERNS; ++i)
        cbPattern_->addItem(QString("%1 %2").arg(i + 1, 2, 10, QChar('0')).arg(proj_->patterns[i].name));
    cbPattern_->setCurrentIndex(std::clamp(proj_->current, 0, xu::PATTERNS - 1));
    cbPattern_->blockSignals(false);
}

void MainWindow::syncStrips() { for (auto* s : strips_) s->syncFromProject(); }

void MainWindow::startAudio()
{
    audio_ = new XuWinAudio(eng_);
    if (!audio_->start())
        lbStatus_->setText("无音频设备（界面仍可用，可离线导出）");
}

void MainWindow::startMidi()
{
#ifdef Q_OS_WIN
    if (midiInGetNumDevs() == 0)
    {
        lbMidi_->setText("MIDI: 无设备");
        return;
    }
    if (midiInOpen(&g_midi, 0, (DWORD_PTR)midiProc, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR)
    {
        midiInStart(g_midi);
        lbMidi_->setText("MIDI: 已连接");
        lbMidi_->setStyleSheet(QString("color:%1;font-size:9px;").arg(C_ORANGE.name()));
    }
#endif
}

void MainWindow::handleMidi()
{
#ifdef Q_OS_WIN
    while (g_mHead != g_mTail)
    {
        MidiMsg m = g_mq[g_mHead];
        g_mHead = (g_mHead + 1) & 255;
        unsigned char cmd = m.s & 0xF0;
        if (cmd == 0x90 && m.d2 > 0)
        {
            for (int p = 0; p < xu::PADS; ++p)
                if (xu::kMidiNotes[p] == m.d1)
                {
                    eng_->triggerPad(p, m.d2, m.d2 >= 120);
                    break;
                }
        }
        else if (cmd == 0xB0)
        {
            if (m.d1 == 7) proj_->master.volume = m.d2 / 127.0f;
            else if (m.d1 == 74) proj_->master.lpCutoff = float(20.0 * std::pow(1000.0, m.d2 / 127.0));
        }
        else if (cmd == 0xC0)
        {
            eng_->requestPattern(m.d2 % xu::PATTERNS);
        }
        else if (m.s == 0xFA) eng_->play();
        else if (m.s == 0xFC) eng_->stop();
        else if (m.s == 0xF8 && eng_->clockExternal) eng_->externalTick();
    }
#endif
}

void MainWindow::onTick()
{
    handleMidi();
    grid_->setPlayhead(eng_->playing ? eng_->stepIndex : -1);
    meter_->setLevels(eng_->meterL.load(), eng_->meterR.load());
    lbStatus_->setText(QString("CPU %1%  ·  XRUN %2")
                       .arg(double(eng_->cpu.load()) * 100.0, 0, 'f', 1)
                       .arg(eng_->xruns.load()));
    if (cbPattern_&& cbPattern_
    ->
    currentIndex() != proj_->current && !eng_->playing
    )
    {
        cbPattern_->blockSignals(true);
        cbPattern_->setCurrentIndex(proj_->current);
        cbPattern_->blockSignals(false);
    }
}

void MainWindow::exportWav(bool song)
{
    QString f = QFileDialog::getSaveFileName(this, "导出 WAV", "xu808.wav", "WAV (*.wav)");
    if (f.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = eng_->exportWav(f, song);
    QApplication::restoreOverrideCursor();
    if (!ok) QMessageBox::critical(this, "导出失败", "无法写入 WAV");
    else lbStatus_->setText("WAV 已导出：" + f);
}

void MainWindow::exportMidi(bool song)
{
    QString f = QFileDialog::getSaveFileName(this, "导出 MIDI", "xu808.mid", "MIDI (*.mid)");
    if (f.isEmpty()) return;
    if (!eng_->exportMidi(f, song)) QMessageBox::critical(this, "导出失败", "无法写入 MIDI");
    else lbStatus_->setText("MIDI 已导出：" + f);
}

bool MainWindow::eventFilter(QObject* o, QEvent* e)
{
    return QMainWindow::eventFilter(o, e);
}