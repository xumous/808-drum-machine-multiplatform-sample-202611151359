/*
 * machine.cpp —— Qt Widgets 自绘 808 界面
 * ---------------------------------------------------------------------------
 * 视觉与纯 C 版保持一致；所有音序/音色操作通过 core808 的无锁命令队列。
 * UI 每 33ms 轮询一次状态（播放头/电平/CPU/xrun），绝不介入音频回调。
 */
#include "machine.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QDateTime>
#include <QFontMetrics>
#include <QtMath>
#include <cstring>

static const char* TRACK_NAMES[C808_TRACKS] = {
    "KICK", "SNARE", "CLAP", "RIM", "TOM LO", "TOM MID", "TOM HI", "CONGA",
    "COWBELL", "SHAKER", "TAMB", "CRASH", "RIDE", "HAT OPEN", "HAT CLOSED", "CLAVE"
};
static const char* KNOB_NAMES[C808_P_COUNT] = {
    "TUNE", "DECAY", "TONE", "CHAR", "LEVEL", "PAN"
};
static const char* MASTER_NAMES[C808_M_COUNT] = {
    "VOLUME", "DRIVE", "HP", "LP", "RESO", "ACCENT"
};
static const char* COND_NAMES[] = {
    "ALWAYS", "1:2", "1:3", "1:4", "FILL", "NOT FILL", "EVERY N"
};

static const QColor C_BG(14, 16, 20);
static const QColor C_PANEL(26, 29, 35);
static const QColor C_PANEL2(36, 40, 48);
static const QColor C_PANEL3(47, 52, 62);
static const QColor C_TEXT(228, 231, 237);
static const QColor C_DIM(139, 146, 160);
static const QColor C_ORANGE(255, 122, 48);
static const QColor C_ORANGE2(122, 58, 24);
static const QColor C_GREEN(70, 196, 106);
static const QColor C_RED(229, 72, 77);
static const QColor C_BLUE(86, 156, 245);
static const QColor C_CELL(42, 46, 55);

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline QColor mixColor(const QColor& a, const QColor& b, float t)
{
    return QColor(int(a.red() + (b.red() - a.red()) * t),
                  int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue() + (b.blue() - a.blue()) * t));
}

/* ========================================================================= */
/* Song 编辑器                                                               */
/* ========================================================================= */
class SongDialog : public QDialog
{
public:
    SongDialog(c808_engine* engine, QWidget* parent)
        : QDialog(parent), m_engine(engine)
    {
        setWindowTitle(QStringLiteral("SONG CHAIN EDITOR"));
        setMinimumSize(560, 520);
        setStyleSheet(QStringLiteral(
            "QDialog{background:#1a1d23;color:#e4e7ed;}"
            "QListWidget{background:#24282f;color:#e4e7ed;border:1px solid #2f343e;}"
            "QPushButton{background:#2f343e;color:#e4e7ed;border:1px solid #3c424d;"
            "border-radius:4px;padding:6px 10px;}"
            "QPushButton:hover{background:#3a404b;}"
            "QLabel{color:#8b92a0;}"));

        const c808_project* p = c808_project_ro(m_engine);
        int len = p ? p->songLen : 1;
        if (len < 1) len = 1;
        for (int i = 0; i < len; ++i) {
            m_items.append(qMakePair((int)p->song[i].pat, (int)p->song[i].reps));
        }

        QVBoxLayout* v = new QVBoxLayout(this);
        m_list = new QListWidget(this);
        v->addWidget(m_list, 1);

        QGridLayout* g = new QGridLayout();
        const char* labels[8] = { "+ ADD", "- DEL", "UP", "DOWN", "PAT -", "PAT +", "REP -", "REP +" };
        for (int i = 0; i < 8; ++i) {
            QPushButton* b = new QPushButton(QString::fromUtf8(labels[i]), this);
            g->addWidget(b, i / 4, i % 4);
            connect(b, &QPushButton::clicked, this, [this, i]() { command(i); });
        }
        v->addLayout(g);

        QHBoxLayout* h = new QHBoxLayout();
        QPushButton* apply = new QPushButton(QStringLiteral("APPLY & CLOSE"), this);
        QPushButton* cancel = new QPushButton(QStringLiteral("CANCEL"), this);
        h->addWidget(apply);
        h->addWidget(cancel);
        v->addLayout(h);
        connect(apply, &QPushButton::clicked, this, &SongDialog::apply);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

        refresh();
    }

private:
    void refresh()
    {
        m_list->clear();
        for (int i = 0; i < m_items.size(); ++i) {
            m_list->addItem(QStringLiteral("%1   PATTERN %2   x%3")
                            .arg(i + 1, 2, 10, QLatin1Char('0'))
                            .arg(m_items[i].first, 3, 10, QLatin1Char('0'))
                            .arg(m_items[i].second));
        }
    }

    void command(int id)
    {
        int sel = m_list->currentRow();
        switch (id) {
        case 0:
            m_items.append(qMakePair(m_items.isEmpty() ? 0 : m_items.last().first, 4));
            break;
        case 1:
            if (sel >= 0 && sel < m_items.size()) m_items.removeAt(sel);
            break;
        case 2:
            if (sel > 0) m_items.swapItemsAt(sel, sel - 1);
            break;
        case 3:
            if (sel >= 0 && sel < m_items.size() - 1) m_items.swapItemsAt(sel, sel + 1);
            break;
        case 4:
            if (sel >= 0) m_items[sel].first = (m_items[sel].first + C808_PATTERNS - 1) % C808_PATTERNS;
            break;
        case 5:
            if (sel >= 0) m_items[sel].first = (m_items[sel].first + 1) % C808_PATTERNS;
            break;
        case 6:
            if (sel >= 0) m_items[sel].second = qMax(1, m_items[sel].second - 1);
            break;
        case 7:
            if (sel >= 0) m_items[sel].second = qMin(64, m_items[sel].second + 1);
            break;
        default:
            break;
        }
        refresh();
    }

    void apply()
    {
        c808_set_song_len(m_engine, m_items.size());
        for (int i = 0; i < m_items.size(); ++i)
            c808_set_song_entry(m_engine, i, m_items[i].first, m_items[i].second);
        accept();
    }

    c808_engine* m_engine;
    QListWidget* m_list = nullptr;
    QVector<QPair<int, int>> m_items;
};

/* ========================================================================= */
/* 导出线程                                                                   */
/* ========================================================================= */
ExportThread::ExportThread(c808_engine* e, Kind kind, const QString& path,
                           int mode, int loops, QObject* parent)
    : QThread(parent), m_engine(e), m_kind(kind), m_path(path), m_mode(mode), m_loops(loops)
{
}

void ExportThread::run()
{
    QByteArray u8 = m_path.toUtf8();
    int rc = 0;
    if (m_kind == Midi)
        rc = c808_export_midi(m_engine, u8.constData(), m_mode, m_loops);
    else
        rc = c808_export_wav(m_engine, u8.constData(), m_mode, m_loops, 2, m_kind == Stems);
    emit done(rc);
}

/* ========================================================================= */
/* MachineWidget                                                              */
/* ========================================================================= */
MachineWidget::MachineWidget(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(800, 480);
    setAutoFillBackground(false);

    m_audio = new AudioEngine(this);
    QString err;
    m_audio->start(&err);
    m_engine = m_audio->engine();
    if (!m_engine) {
        m_engine = c808_create(48000.0);
        m_ownsFallback = true;
        m_msg = err.isEmpty() ? QStringLiteral("NO AUDIO") : err;
    } else {
        m_msg = m_audio->running() ? QStringLiteral("READY") : err;
    }

    if (c808_midi_device_count() > 0) {
        char name[256];
        if (c808_midi_device_name(0, name, sizeof(name)) == 0) {
            m_midiIndex = 0;
            m_midiName = QStringLiteral("MIDI: ") + QString::fromUtf8(name);
        }
    } else {
        m_midiName = QStringLiteral("MIDI: OFF");
    }

    m_scale = 1.0;
    refreshFonts();
    layout();

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &MachineWidget::onTick);
    m_timer->start();
}

MachineWidget::~MachineWidget()
{
    if (m_timer) m_timer->stop();
    if (m_export) {
        m_export->wait(15000);
    }
    if (m_ownsFallback && m_engine) {
        c808_destroy(m_engine);
        m_engine = nullptr;
    }
}

void MachineWidget::refreshFonts()
{
    QFont base(QStringLiteral("Segoe UI"));
    base.setHintingPreference(QFont::PreferFullHinting);
    m_fTitle = base;  m_fTitle.setPixelSize(qMax(8, int(20 * m_scale)));
    m_fLabel = base;  m_fLabel.setPixelSize(qMax(7, int(13 * m_scale)));
    m_fSmall = base;  m_fSmall.setPixelSize(qMax(7, int(11 * m_scale)));
    m_fTiny = base;   m_fTiny.setPixelSize(qMax(6, int(9 * m_scale)));
    m_fMono = QFont(QStringLiteral("Consolas"));
    m_fMono.setPixelSize(qMax(7, int(12 * m_scale)));
    m_fBig = base;    m_fBig.setPixelSize(qMax(10, int(26 * m_scale)));
    m_fBig.setBold(true);
}

static QRectF R(qreal x, qreal y, qreal w, qreal h, qreal s)
{
    return QRectF(x * s, y * s, w * s, h * s);
}

void MachineWidget::layout()
{
    qreal sx = width() / 1280.0;
    qreal sy = height() / 720.0;
    qreal ns = qMin(sx, sy);
    if (ns < 0.35) ns = 0.35;
    if (ns > 3.0) ns = 3.0;
    if (qAbs(ns - m_scale) > 0.01) {
        m_scale = ns;
        refreshFonts();
    }

    rcPlay = R(12, 8, 72, 30, m_scale);
    rcStop = R(90, 8, 72, 30, m_scale);
    rcTap = R(168, 8, 60, 30, m_scale);
    rcBpmMinus = R(234, 8, 30, 30, m_scale);
    rcBpm = R(268, 8, 86, 30, m_scale);
    rcBpmPlus = R(358, 8, 30, 30, m_scale);
    rcSwing = R(400, 8, 190, 30, m_scale);
    rcPatPrev = R(600, 8, 30, 30, m_scale);
    rcPatDisp = R(634, 8, 92, 30, m_scale);
    rcPatNext = R(730, 8, 30, 30, m_scale);
    rcLenMinus = R(772, 8, 30, 30, m_scale);
    rcLenDisp = R(806, 8, 70, 30, m_scale);
    rcLenPlus = R(880, 8, 30, 30, m_scale);
    rcSong = R(922, 8, 72, 30, m_scale);
    rcSongEdit = R(1000, 8, 84, 30, m_scale);

    rcSave = R(12, 44, 70, 28, m_scale);
    rcLoad = R(86, 44, 70, 28, m_scale);
    rcWav = R(160, 44, 70, 28, m_scale);
    rcMidi = R(234, 44, 70, 28, m_scale);
    rcStems = R(308, 44, 76, 28, m_scale);
    rcClear = R(388, 44, 64, 28, m_scale);
    rcCopy = R(456, 44, 64, 28, m_scale);
    rcMidiIn = R(524, 44, 240, 28, m_scale);
    rcSync = R(772, 44, 76, 28, m_scale);
    rcTimeSig = R(854, 44, 70, 28, m_scale);
    rcTrkLenM = R(860, 486, 32, 26, m_scale);
    rcTrkLenDisp = R(896, 486, 128, 26, m_scale);
    rcTrkLenP = R(1028, 486, 32, 26, m_scale);

    rcGrid = R(12, 84, 830, 432, m_scale);
    rcPanel = R(852, 84, 416, 432, m_scale);
    rcMaster = R(12, 526, 830, 106, m_scale);
    rcPads = R(852, 526, 416, 106, m_scale);
    rcSongStrip = R(12, 640, 1256, 40, m_scale);
    rcStatus = R(0, 686, 1280, 34, m_scale);
    rcGridSteps = QRectF(rcGrid.left() + S(126), rcGrid.top() + S(22),
                         rcGrid.width() - S(134), rcGrid.height() - S(28));

    for (int i = 0; i < C808_P_COUNT; ++i) {
        int col = i % 3, row = i / 3;
        rcVoiceKnobs[i] = R(862 + col * 134, 118 + row * 80, 128, 78, m_scale);
    }
    for (int i = 0; i < C808_M_COUNT; ++i)
        rcMasterKnobs[i] = R(30 + i * 134, 542, 128, 86, m_scale);
    for (int i = 0; i < 5; ++i)
        rcStepSliders[i] = R(860, 316 + i * 25, 400, 24, m_scale);
    rcCond = R(860, 452, 210, 28, m_scale);
    rcAccent = R(1080, 452, 180, 28, m_scale);
    rcTrigger = R(1090, 94, 168, 24, m_scale);
    for (int i = 0; i < 16; ++i) {
        int col = i % 4, row = i / 4;
        rcPad[i] = R(858 + col * 102, 530 + row * 25, 98, 22, m_scale);
    }
    for (int i = 0; i < 16; ++i)
        rcPatQuick[i] = R(34 + i * 76, 644, 70, 30, m_scale);
    rcPagePrev = R(14, 644, 16, 30, m_scale);
    rcPageNext = R(1252, 644, 16, 30, m_scale);
}

void MachineWidget::resizeEvent(QResizeEvent*)
{
    layout();
    update();
}

void MachineWidget::onTick()
{
    if (!m_audio || !m_audio->running()) {
        float l[256], r[256];
        c808_render(m_engine, l, r, 256);
    }
    c808_get_status(m_engine, &m_status);
    update();
}

void MachineWidget::drawButton(QPainter& p, const QRectF& r, const QString& text,
                               bool active, const QFont& f)
{
    p.setPen(QPen(active ? C_ORANGE : C_PANEL3, qMax(1.0, S(1))));
    p.setBrush(active ? C_ORANGE : C_PANEL2);
    p.drawRoundedRect(r, S(6), S(6));
    p.setFont(f);
    p.setPen(active ? QColor(20, 16, 12) : C_TEXT);
    p.drawText(r, Qt::AlignCenter, text);
}

void MachineWidget::drawKnob(QPainter& p, const QRectF& r, const QString& label, float v)
{
    qreal rad = qMin((r.height() - S(38)) / 2.0, S(20));
    if (rad < S(7)) rad = S(7);
    QPointF c(r.center().x(), r.top() + S(4) + rad);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(C_ORANGE2, qMax(1.0, S(4))));
    p.setBrush(C_PANEL3);
    p.drawEllipse(c, rad, rad);

    QRectF arc(c.x() - rad - S(5), c.y() - rad - S(5),
               (rad + S(5)) * 2, (rad + S(5)) * 2);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(C_ORANGE, qMax(1.0, S(2.5))));
    p.drawArc(arc, 225 * 16, -270 * 16);

    qreal ang = qDegreesToRadians(135.0 + 270.0 * v);
    p.setPen(QPen(C_TEXT, qMax(1.0, S(2.5))));
    p.drawLine(c, QPointF(c.x() + qCos(ang) * (rad - S(4)),
                          c.y() - qSin(ang) * (rad - S(4))));

    p.setFont(m_fTiny);
    p.setPen(C_DIM);
    QRectF lr(r.left(), c.y() + rad + S(1), r.width(), S(14));
    p.drawText(lr, Qt::AlignCenter, label);
    p.setFont(m_fSmall);
    p.setPen(C_TEXT);
    QRectF vr(r.left(), c.y() + rad + S(15), r.width(), S(14));
    p.drawText(vr, Qt::AlignCenter, QString::number(int(v * 100.0f + 0.5f)));
    p.restore();
}

void MachineWidget::drawSlider(QPainter& p, const QRectF& r, const QString& label,
                               float v, const QString& val, bool bipolar)
{
    QRectF lab(r.left(), r.top(), S(72), r.height());
    QRectF valr(r.right() - S(58), r.top(), S(58), r.height());
    QRectF bar(r.left() + S(78), r.top() + S(7), r.width() - S(138), r.height() - S(14));
    p.setFont(m_fSmall);
    p.setPen(C_DIM);
    p.drawText(lab, Qt::AlignLeft | Qt::AlignVCenter, label);
    p.setPen(C_TEXT);
    p.drawText(valr, Qt::AlignRight | Qt::AlignVCenter, val);

    p.setPen(Qt::NoPen);
    p.setBrush(C_PANEL3);
    p.drawRoundedRect(bar, S(4), S(4));
    if (bipolar) {
        qreal mid = bar.center().x();
        qreal x = bar.left() + v * bar.width();
        QRectF f(qMin(mid, x), bar.top(), qAbs(x - mid), bar.height());
        p.setBrush(C_ORANGE);
        p.drawRect(f);
        p.setPen(QPen(C_DIM, qMax(1.0, S(1))));
        p.drawLine(QPointF(mid, bar.top() - S(2)), QPointF(mid, bar.bottom() + S(2)));
    } else {
        QRectF f(bar.left(), bar.top(), bar.width() * v, bar.height());
        p.setBrush(C_ORANGE);
        p.drawRect(f);
    }
}

void MachineWidget::drawMeter(QPainter& p, const QRectF& r, float v)
{
    p.setPen(Qt::NoPen);
    p.setBrush(C_PANEL3);
    p.drawRect(r);
    qreal h = r.height() * clamp01(v);
    if (h > 0) {
        QRectF f(r.left(), r.bottom() - h, r.width(), h);
        p.setBrush(v > 0.9f ? C_RED : (v > 0.6f ? C_ORANGE : C_GREEN));
        p.drawRect(f);
    }
}

void MachineWidget::paintEvent(QPaintEvent*)
{
    const c808_project* proj = c808_project_ro(m_engine);
    QPainter p(this);
    p.fillRect(rect(), C_BG);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (!proj) return;

    int patIdx = proj->currentPattern;
    int len = clampi(proj->patterns[patIdx].len, 1, C808_STEPS);
    QString tmp;

    /* ---------------- 顶部 ---------------- */
    drawButton(p, rcPlay, m_status.playing ? QStringLiteral("PLAYING") : QStringLiteral("PLAY"),
               m_status.playing, m_fLabel);
    drawButton(p, rcStop, QStringLiteral("STOP"), false, m_fLabel);
    drawButton(p, rcTap, QStringLiteral("TAP"), false, m_fLabel);
    drawButton(p, rcBpmMinus, QStringLiteral("-"), false, m_fLabel);
    drawButton(p, rcBpmPlus, QStringLiteral("+"), false, m_fLabel);
    p.setPen(QPen(C_PANEL3, qMax(1.0, S(1))));
    p.setBrush(C_PANEL2);
    p.drawRoundedRect(rcBpm, S(6), S(6));
    p.setFont(m_fLabel);
    p.setPen(C_ORANGE);
    p.drawText(rcBpm, Qt::AlignCenter, QStringLiteral("%1 BPM").arg(proj->bpm, 0, 'f', 1));

    {
        QRectF txt = rcSwing;
        QRectF bar = rcSwing.adjusted(S(54), S(9), 0, -S(9));
        p.setFont(m_fSmall);
        p.setPen(C_DIM);
        p.drawText(QRectF(txt.left(), txt.top(), S(52), txt.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SWING"));
        p.setPen(Qt::NoPen);
        p.setBrush(C_PANEL3);
        p.drawRoundedRect(bar, S(4), S(4));
        int sw = proj->patterns[patIdx].swing;
        p.setBrush(C_ORANGE);
        p.drawRect(QRectF(bar.left(), bar.top(), bar.width() * sw / 100.0, bar.height()));
        p.setPen(C_TEXT);
        p.drawText(QRectF(txt.left(), txt.top(), S(52), txt.height()),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1%").arg(sw));
    }

    drawButton(p, rcPatPrev, QStringLiteral("<"), false, m_fLabel);
    drawButton(p, rcPatNext, QStringLiteral(">"), false, m_fLabel);
    p.setPen(QPen(C_PANEL3, qMax(1.0, S(1))));
    p.setBrush(C_PANEL2);
    p.drawRoundedRect(rcPatDisp, S(6), S(6));
    p.setFont(m_fLabel);
    p.setPen(C_TEXT);
    p.drawText(rcPatDisp, Qt::AlignCenter, QStringLiteral("PAT %1").arg(patIdx, 3, 10, QLatin1Char('0')));
    drawButton(p, rcLenMinus, QStringLiteral("-"), false, m_fLabel);
    drawButton(p, rcLenPlus, QStringLiteral("+"), false, m_fLabel);
    p.setPen(QPen(C_PANEL3, qMax(1.0, S(1))));
    p.setBrush(C_PANEL2);
    p.drawRoundedRect(rcLenDisp, S(6), S(6));
    p.setFont(m_fLabel);
    p.setPen(C_TEXT);
    p.drawText(rcLenDisp, Qt::AlignCenter, QStringLiteral("LEN %1").arg(len));
    drawButton(p, rcSong, proj->songOn ? QStringLiteral("SONG ON") : QStringLiteral("SONG OFF"),
               proj->songOn != 0, m_fLabel);
    drawButton(p, rcSongEdit, QStringLiteral("EDIT SONG"), false, m_fLabel);

    drawButton(p, rcSave, QStringLiteral("SAVE"), false, m_fLabel);
    drawButton(p, rcLoad, QStringLiteral("LOAD"), false, m_fLabel);
    drawButton(p, rcWav, QStringLiteral("WAV"), false, m_fLabel);
    drawButton(p, rcMidi, QStringLiteral("MIDI"), false, m_fLabel);
    drawButton(p, rcStems, QStringLiteral("STEMS"), false, m_fLabel);
    drawButton(p, rcClear, QStringLiteral("CLEAR"), false, m_fLabel);
    drawButton(p, rcCopy, QStringLiteral("COPY>"), false, m_fLabel);
    p.setPen(QPen(C_PANEL3, qMax(1.0, S(1))));
    p.setBrush(C_PANEL2);
    p.drawRoundedRect(rcMidiIn, S(6), S(6));
    p.setFont(m_fSmall);
    p.setPen(C_DIM);
    p.drawText(rcMidiIn.adjusted(S(8), 0, -S(8), 0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(m_fSmall).elidedText(m_midiName, Qt::ElideRight,
                                                 int(rcMidiIn.width() - S(16))));
    drawButton(p, rcSync, proj->clockSource ? QStringLiteral("SYNC MIDI") : QStringLiteral("SYNC INT"),
               proj->clockSource != 0, m_fSmall);
    drawButton(p, rcTimeSig, QStringLiteral("%1/%2").arg(proj->tsNum).arg(proj->tsDen),
               false, m_fSmall);

    /* ---------------- 网格 ---------------- */
    p.setPen(Qt::NoPen);
    p.setBrush(C_PANEL);
    p.drawRoundedRect(rcGrid, S(8), S(8));

    qreal rowH = rcGridSteps.height() / 16.0;
    qreal colW = rcGridSteps.width() / len;

    p.setFont(m_fTiny);
    p.setPen(C_DIM);
    p.drawText(QRectF(rcGrid.left() + S(10), rcGrid.top() + S(4), S(80), S(18)),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("TRACK"));
    for (int s = 0; s < len; s += 4) {
        QRectF nr(rcGridSteps.left() + s * colW, rcGrid.top() + S(3), colW, S(18));
        p.drawText(nr, Qt::AlignCenter, QString::number(s + 1));
    }

    for (int t = 0; t < 16; ++t) {
        qreal y = rcGridSteps.top() + t * rowH;
        QRectF row(rcGrid.left(), y, rcGridSteps.left() - rcGrid.left(), rowH);
        p.fillRect(row, t == m_selTrack ? QColor(45, 39, 34) :
                   ((t & 1) ? QColor(24, 27, 33) : QColor(22, 25, 30)));
        p.setFont(m_fTiny);
        p.setPen(t == m_selTrack ? C_ORANGE : C_TEXT);
        p.drawText(QRectF(rcGrid.left() + S(4), y, S(66), rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8(TRACK_NAMES[t]));

        QRectF mr(rcGrid.left() + S(72), y + S(2), S(22), rowH - S(4));
        QRectF sr(rcGrid.left() + S(96), y + S(2), S(22), rowH - S(4));
        p.setPen(Qt::NoPen);
        p.setBrush(proj->mute[t] ? C_RED : C_PANEL3);
        p.drawRoundedRect(mr, S(3), S(3));
        p.setBrush(proj->solo[t] ? C_GREEN : C_PANEL3);
        p.drawRoundedRect(sr, S(3), S(3));
        p.setFont(m_fTiny);
        p.setPen(proj->mute[t] ? C_TEXT : C_DIM);
        p.drawText(mr, Qt::AlignCenter, QStringLiteral("M"));
        p.setPen(proj->solo[t] ? C_TEXT : C_DIM);
        p.drawText(sr, Qt::AlignCenter, QStringLiteral("S"));

        drawMeter(p, QRectF(rcGrid.left() + S(120), y + S(2), S(4), rowH - S(4)),
                  m_status.meter[t]);

        int tlen = clampi(proj->trackLen[t], 1, len);
        for (int s = 0; s < len; ++s) {
            QRectF cell(rcGridSteps.left() + s * colW + S(1), y + S(2),
                        colW - S(2), rowH - S(4));
            const c808_pattern* q = &proj->patterns[patIdx];
            bool on = q->on[t][s] != 0;
            QColor c = C_CELL;
            if (on) {
                c = mixColor(C_ORANGE2, C_ORANGE, q->vel[t][s] / 127.0f);
                if (q->accent[t][s]) c = QColor(255, 196, 120);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(cell, S(2), S(2));

            if (on && q->prob[t][s] < 100) {
                p.setBrush(C_BLUE);
                p.drawRect(QRectF(cell.left() + S(2), cell.top() + S(2), S(3), S(3)));
            }
            if (on && q->ratchet[t][s] > 1) {
                p.setBrush(C_TEXT);
                int n = qMin(4, (int)q->ratchet[t][s]);
                for (int k = 0; k < n; ++k) {
                    p.drawRect(QRectF(cell.left() + S(3) + k * S(3),
                                      cell.bottom() - S(5), S(2), S(3)));
                }
            }
            if (on && q->micro[t][s] != 0) {
                p.setBrush(q->micro[t][s] > 0 ? C_GREEN : C_RED);
                p.drawRect(QRectF(cell.right() - S(4), cell.top(), S(3), S(4)));
            }
            if (t == m_selTrack && s == m_selStep) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(C_TEXT, qMax(1.0, S(1))));
                p.drawRoundedRect(cell, S(2), S(2));
            }
            if (m_status.playing && m_status.step % tlen == s) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(C_BLUE, qMax(1.0, S(1.5))));
                p.drawRoundedRect(cell, S(2), S(2));
            }
        }
    }

    /* ---------------- 声部面板 ---------------- */
    p.setPen(Qt::NoPen);
    p.setBrush(C_PANEL);
    p.drawRoundedRect(rcPanel, S(8), S(8));
    p.setFont(m_fLabel);
    p.setPen(C_ORANGE);
    p.drawText(QRectF(rcPanel.left() + S(12), rcPanel.top() + S(7),
                      rcPanel.width() - S(190), S(20)),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("VOICE EDIT  ·  %1").arg(QString::fromUtf8(TRACK_NAMES[m_selTrack])));
    drawButton(p, rcTrigger, QStringLiteral("TRIGGER"), false, m_fSmall);
    for (int i = 0; i < C808_P_COUNT; ++i) {
        float v = (i == C808_P_TUNE) ? proj->tune[m_selTrack] :
                  (i == C808_P_DECAY) ? proj->decay[m_selTrack] :
                  (i == C808_P_TONE) ? proj->tone[m_selTrack] :
                  (i == C808_P_CHAR) ? proj->chara[m_selTrack] :
                  (i == C808_P_LEVEL) ? proj->level[m_selTrack] : proj->pan[m_selTrack];
        drawKnob(p, rcVoiceKnobs[i], QString::fromUtf8(KNOB_NAMES[i]), v);
    }

    const c808_pattern* q = &proj->patterns[patIdx];
    p.setFont(m_fLabel);
    p.setPen(C_TEXT);
    p.drawText(QRectF(rcPanel.left() + S(10), rcPanel.top() + S(200),
                      rcPanel.width() - S(20), S(20)),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("STEP %1  ·  %2")
               .arg(m_selStep + 1, 2, 10, QLatin1Char('0'))
               .arg(QString::fromUtf8(TRACK_NAMES[m_selTrack])));
    {
        int vel = q->vel[m_selTrack][m_selStep];
        int prob = q->prob[m_selTrack][m_selStep];
        int rat = q->ratchet[m_selTrack][m_selStep];
        int rate = q->rate[m_selTrack][m_selStep];
        int micro = q->micro[m_selTrack][m_selStep];
        int cond = q->cond[m_selTrack][m_selStep];
        int acc = q->accent[m_selTrack][m_selStep];
        drawSlider(p, rcStepSliders[0], QStringLiteral("VELOCITY"), (vel - 1) / 126.0f,
                   QString::number(vel), false);
        drawSlider(p, rcStepSliders[1], QStringLiteral("PROB"), prob / 100.0f,
                   QStringLiteral("%1%").arg(prob), false);
        drawSlider(p, rcStepSliders[2], QStringLiteral("RATCHET"), (rat - 1) / 7.0f,
                   QStringLiteral("x%1").arg(rat), false);
        drawSlider(p, rcStepSliders[3], QStringLiteral("RATE"), (rate - 10) / 90.0f,
                   QStringLiteral("%1%").arg(rate), false);
        drawSlider(p, rcStepSliders[4], QStringLiteral("MICRO"),
                   micro / 4800.0f + 0.5f, QStringLiteral("%1 smp").arg(micro), true);
        drawButton(p, rcCond, QStringLiteral("COND: %1")
                   .arg(QString::fromUtf8(COND_NAMES[cond <= 6 ? cond : 0])), cond != 0, m_fSmall);
        drawButton(p, rcAccent, acc ? QStringLiteral("ACCENT ON") : QStringLiteral("ACCENT"),
                   acc != 0, m_fSmall);
        drawButton(p, rcTrkLenM, QStringLiteral("-"), false, m_fSmall);
        drawButton(p, rcTrkLenDisp, QStringLiteral("TRACK LEN %1").arg(proj->trackLen[m_selTrack]),
                   false, m_fSmall);
        drawButton(p, rcTrkLenP, QStringLiteral("+"), false, m_fSmall);
    }

    /* ---------------- 主控 ---------------- */
    p.setPen(Qt::NoPen);
    p.setBrush(C_PANEL);
    p.drawRoundedRect(rcMaster, S(8), S(8));
    p.setFont(m_fTiny);
    p.setPen(C_DIM);
    p.drawText(QRectF(rcMaster.left() + S(12), rcMaster.top() + S(2), rcMaster.width() - S(24), S(16)),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("MASTER  ·  FILTER / SATURATION / ACCENT"));
    for (int i = 0; i < C808_M_COUNT; ++i)
        drawKnob(p, rcMasterKnobs[i], QString::fromUtf8(MASTER_NAMES[i]), proj->master[i]);

    /* ---------------- 打击垫 ---------------- */
    p.setPen(Qt::NoPen);
    p.setBrush(C_PANEL);
    p.drawRoundedRect(rcPads, S(8), S(8));
    for (int i = 0; i < 16; ++i) {
        bool sel = (i == m_selTrack);
        p.setBrush(sel ? C_ORANGE2 : C_PANEL2);
        p.setPen(QPen(sel ? C_ORANGE : C_PANEL3, qMax(1.0, S(1))));
        p.drawRoundedRect(rcPad[i], S(4), S(4));
        p.setFont(m_fTiny);
        p.setPen(sel ? C_TEXT : C_DIM);
        p.drawText(rcPad[i], Qt::AlignCenter, QString::fromUtf8(TRACK_NAMES[i]));
    }

    /* ---------------- Pattern 快捷条 ---------------- */
    p.setPen(Qt::NoPen);
    p.setBrush(C_PANEL);
    p.drawRoundedRect(rcSongStrip, S(8), S(8));
    drawButton(p, rcPagePrev, QStringLiteral("<"), false, m_fTiny);
    drawButton(p, rcPageNext, QStringLiteral(">"), false, m_fTiny);
    for (int i = 0; i < 16; ++i) {
        int idx = m_page * 16 + i;
        drawButton(p, rcPatQuick[i], QString::number(idx), proj->currentPattern == idx, m_fSmall);
    }

    /* ---------------- 状态栏 ---------------- */
    p.fillRect(rcStatus, C_PANEL);
    QString audio = m_audio ? m_audio->info() : QString();
    if (audio.isEmpty()) audio = QStringLiteral("NO AUDIO DEVICE");
    if (m_audio && !m_audio->stateText().isEmpty())
        audio += QStringLiteral(" [") + m_audio->stateText() + QStringLiteral("]");
    tmp = QStringLiteral("%1   ·   CPU %2%   ·   XRUN %3   ·   %4   ·   %5   ·   F%6")
          .arg(audio)
          .arg(int(m_status.cpuLoad * 100.0f))
          .arg(m_status.xruns)
          .arg(m_msg)
          .arg(m_status.playing ? QStringLiteral("PLAY") : QStringLiteral("STOP"))
          .arg(m_audio ? m_audio->framesRendered() : 0);
    p.setFont(m_fSmall);
    p.setPen((m_audio && m_audio->running()) ? C_DIM : C_RED);
    p.drawText(rcStatus.adjusted(S(12), 0, -S(12), 0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(m_fSmall).elidedText(tmp, Qt::ElideRight,
                                                 int(rcStatus.width() - S(24))));
    if (m_exportBusy) {
        p.setPen(C_ORANGE);
        p.drawText(rcStatus.adjusted(S(12), 0, -S(12), 0),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("EXPORTING..."));
    }
}

/* ========================================================================= */
/* 交互                                                                       */
/* ========================================================================= */
void MachineWidget::gridStepAt(const QPointF& pos, int* track, int* step)
{
    *track = -1;
    *step = -1;
    const c808_project* proj = c808_project_ro(m_engine);
    int len = proj ? clampi(proj->patterns[proj->currentPattern].len, 1, C808_STEPS) : 16;
    if (!rcGridSteps.contains(pos)) return;
    qreal rowH = rcGridSteps.height() / 16.0;
    qreal colW = rcGridSteps.width() / len;
    int t = int((pos.y() - rcGridSteps.top()) / rowH);
    int s = int((pos.x() - rcGridSteps.left()) / colW);
    if (t >= 0 && t < 16 && s >= 0 && s < len) {
        *track = t;
        *step = s;
    }
}

void MachineWidget::cycleProb(int track, int step)
{
    const c808_project* proj = c808_project_ro(m_engine);
    static const int seq[] = { 100, 75, 50, 25, 10, 0 };
    int cur = proj->patterns[proj->currentPattern].prob[track][step];
    int next = 100;
    for (int i = 0; i < 6; ++i) {
        if (seq[i] == cur) { next = seq[(i + 1) % 6]; break; }
    }
    c808_set_step_prob(m_engine, proj->currentPattern, track, step, next);
}

void MachineWidget::cycleRatchet(int track, int step, int delta)
{
    const c808_project* proj = c808_project_ro(m_engine);
    int cur = proj->patterns[proj->currentPattern].ratchet[track][step] + delta;
    if (cur < 1) cur = 8;
    if (cur > 8) cur = 1;
    c808_set_step_ratchet(m_engine, proj->currentPattern, track, step, cur);
}

void MachineWidget::setStepSlider(int idx, qreal x)
{
    const c808_project* proj = c808_project_ro(m_engine);
    QRectF r = rcStepSliders[idx];
    qreal t = (x - (r.left() + S(78))) / (r.width() - S(138));
    t = qBound(0.0, t, 1.0);
    int pat = proj->currentPattern;
    switch (idx) {
    case 0: c808_set_step_vel(m_engine, pat, m_selTrack, m_selStep, int(1 + t * 126.0)); break;
    case 1: c808_set_step_prob(m_engine, pat, m_selTrack, m_selStep, int(t * 100.0)); break;
    case 2: c808_set_step_ratchet(m_engine, pat, m_selTrack, m_selStep, int(1 + t * 7.0)); break;
    case 3: c808_set_step_rate(m_engine, pat, m_selTrack, m_selStep, int(10 + t * 90.0)); break;
    case 4: c808_set_step_micro(m_engine, pat, m_selTrack, m_selStep, int((t - 0.5) * 4800.0)); break;
    default: break;
    }
}

void MachineWidget::tapTempo()
{
    static qint64 taps[8];
    static int count = 0;
    static int pos = 0;
    static qint64 last = 0;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (count > 0 && now - last > 2000) { count = 0; pos = 0; }
    if (count > 0) {
        qint64 dt = now - last;
        if (dt > 150 && dt < 1500) {
            taps[pos % 8] = dt;
            pos++;
            count++;
        }
    } else {
        count = 1;
    }
    last = now;
    if (pos >= 2) {
        int n = qMin(pos, 8);
        qint64 sum = 0;
        for (int i = 0; i < n; ++i) sum += taps[i];
        double bpm = 60000.0 / (double(sum) / n);
        c808_set_bpm(m_engine, float(bpm));
        setMessage(QStringLiteral("TAP TEMPO %1 BPM").arg(bpm, 0, 'f', 1));
    }
}

void MachineWidget::clickAt(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    const c808_project* proj = c808_project_ro(m_engine);
    if (!proj) return;
    int t, s;

    if (rcPlay.contains(pos)) {
        if (m_status.playing) c808_transport_stop(m_engine);
        else c808_transport_play(m_engine);
        return;
    }
    if (rcStop.contains(pos)) { c808_transport_stop_reset(m_engine); return; }
    if (rcTap.contains(pos)) { tapTempo(); return; }
    if (rcBpmMinus.contains(pos)) { c808_set_bpm(m_engine, proj->bpm - 1.0f); return; }
    if (rcBpmPlus.contains(pos)) { c808_set_bpm(m_engine, proj->bpm + 1.0f); return; }
    if (rcBpm.contains(pos)) {
        m_dragId = 100; m_dragY = pos.y(); m_dragStart = proj->bpm; return;
    }
    if (rcSwing.contains(pos)) {
        int v = int((pos.x() - rcSwing.left() - S(54)) * 100.0 /
                    (rcSwing.width() - S(54)));
        c808_set_pattern_swing(m_engine, proj->currentPattern, clampi(v, 0, 100));
        return;
    }
    if (rcPatPrev.contains(pos)) {
        c808_select_pattern(m_engine, (proj->currentPattern + C808_PATTERNS - 1) % C808_PATTERNS);
        return;
    }
    if (rcPatNext.contains(pos)) {
        c808_select_pattern(m_engine, (proj->currentPattern + 1) % C808_PATTERNS);
        return;
    }
    if (rcLenMinus.contains(pos)) {
        c808_set_pattern_len(m_engine, proj->currentPattern,
                             proj->patterns[proj->currentPattern].len - 1);
        return;
    }
    if (rcLenPlus.contains(pos)) {
        c808_set_pattern_len(m_engine, proj->currentPattern,
                             proj->patterns[proj->currentPattern].len + 1);
        return;
    }
    if (rcSong.contains(pos)) { c808_set_song_on(m_engine, !proj->songOn); return; }
    if (rcSongEdit.contains(pos)) { openSongEditor(); return; }
    if (rcSave.contains(pos)) { saveProject(); return; }
    if (rcLoad.contains(pos)) { loadProject(); return; }
    if (rcWav.contains(pos)) { startExport(0); return; }
    if (rcMidi.contains(pos)) { startExport(1); return; }
    if (rcStems.contains(pos)) { startExport(2); return; }
    if (rcClear.contains(pos)) { c808_clear_pattern(m_engine, proj->currentPattern); return; }
    if (rcCopy.contains(pos)) {
        int dst = (proj->currentPattern + 1) % C808_PATTERNS;
        c808_copy_pattern(m_engine, proj->currentPattern, dst);
        c808_select_pattern(m_engine, dst);
        setMessage(QStringLiteral("COPIED PATTERN %1 -> %2").arg(proj->currentPattern).arg(dst));
        return;
    }
    if (rcMidiIn.contains(pos)) { cycleMidi(); return; }
    if (rcSync.contains(pos)) {
        c808_set_clock_source(m_engine, proj->clockSource ? 0 : 1);
        return;
    }
    if (rcTimeSig.contains(pos)) {
        static const int sigs[6][2] = {{4,4},{3,4},{5,4},{7,8},{6,8},{12,8}};
        int next = 0;
        for (int i = 0; i < 6; ++i) {
            if (sigs[i][0] == proj->tsNum && sigs[i][1] == proj->tsDen) {
                next = (i + 1) % 6;
                break;
            }
        }
        c808_set_timesig(m_engine, sigs[next][0], sigs[next][1]);
        return;
    }

    for (int i = 0; i < 16; ++i) {
        if (rcPatQuick[i].contains(pos)) {
            c808_select_pattern(m_engine, m_page * 16 + i);
            return;
        }
    }
    if (rcPagePrev.contains(pos)) { if (m_page > 0) { m_page--; update(); } return; }
    if (rcPageNext.contains(pos)) {
        if (m_page < C808_PATTERNS / 16 - 1) { m_page++; update(); }
        return;
    }

    for (int i = 0; i < C808_P_COUNT; ++i) {
        if (rcVoiceKnobs[i].contains(pos)) {
            float v = clamp01(float((rcVoiceKnobs[i].bottom() - S(6) - pos.y()) /
                                    (rcVoiceKnobs[i].height() - S(20))));
            c808_set_track_param(m_engine, m_selTrack, i, v);
            m_dragId = i; m_dragY = pos.y(); m_dragStart = v;
            return;
        }
    }
    for (int i = 0; i < C808_M_COUNT; ++i) {
        if (rcMasterKnobs[i].contains(pos)) {
            float v = clamp01(float((rcMasterKnobs[i].bottom() - S(6) - pos.y()) /
                                    (rcMasterKnobs[i].height() - S(20))));
            c808_set_master(m_engine, i, v);
            m_dragId = 200 + i; m_dragY = pos.y(); m_dragStart = v;
            return;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (rcStepSliders[i].contains(pos)) {
            m_dragId = 300 + i;
            setStepSlider(i, pos.x());
            return;
        }
    }
    if (rcCond.contains(pos)) {
        int c = (proj->patterns[proj->currentPattern].cond[m_selTrack][m_selStep] + 1) % 7;
        c808_set_step_cond(m_engine, proj->currentPattern, m_selTrack, m_selStep, c,
                           proj->patterns[proj->currentPattern].condN[m_selTrack][m_selStep]);
        return;
    }
    if (rcAccent.contains(pos)) {
        int a = !proj->patterns[proj->currentPattern].accent[m_selTrack][m_selStep];
        c808_set_step_accent(m_engine, proj->currentPattern, m_selTrack, m_selStep, a);
        return;
    }
    if (rcTrkLenM.contains(pos)) {
        c808_set_track_len(m_engine, m_selTrack, proj->trackLen[m_selTrack] - 1);
        return;
    }
    if (rcTrkLenP.contains(pos)) {
        c808_set_track_len(m_engine, m_selTrack, proj->trackLen[m_selTrack] + 1);
        return;
    }
    if (rcTrigger.contains(pos)) {
        c808_note_on(m_engine, m_selTrack, 112, 0);
        return;
    }
    for (int i = 0; i < 16; ++i) {
        if (rcPad[i].contains(pos)) {
            c808_note_on(m_engine, i, 112, 0);
            m_selTrack = i;
            update();
            return;
        }
    }

    gridStepAt(pos, &t, &s);
    if (t >= 0 && s >= 0) {
        m_selTrack = t;
        m_selStep = s;
        if (mods & Qt::ControlModifier) {
            int a = !proj->patterns[proj->currentPattern].accent[t][s];
            c808_set_step_accent(m_engine, proj->currentPattern, t, s, a);
        } else if (mods & Qt::ShiftModifier) {
            cycleProb(t, s);
        } else if (mods & Qt::AltModifier) {
            cycleRatchet(t, s, 1);
        } else {
            int on = !proj->patterns[proj->currentPattern].on[t][s];
            c808_set_step_on(m_engine, proj->currentPattern, t, s, on);
        }
        update();
    }
}

void MachineWidget::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        clickAt(ev->pos(), ev->modifiers());
    } else if (ev->button() == Qt::RightButton) {
        int t, s;
        gridStepAt(ev->pos(), &t, &s);
        if (t >= 0) {
            m_selTrack = t;
            m_selStep = s;
            update();
        }
    }
}

void MachineWidget::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_dragId < 0) return;
    float delta = float((m_dragY - ev->pos().y()) / S(110));
    if (m_dragId == 100) {
        c808_set_bpm(m_engine, m_dragStart + delta * 120.0f);
    } else if (m_dragId >= 300) {
        setStepSlider(m_dragId - 300, ev->pos().x());
    } else if (m_dragId >= 200) {
        c808_set_master(m_engine, m_dragId - 200, m_dragStart + delta);
    } else {
        c808_set_track_param(m_engine, m_selTrack, m_dragId, m_dragStart + delta);
    }
}

void MachineWidget::mouseReleaseEvent(QMouseEvent*)
{
    m_dragId = -1;
}

void MachineWidget::wheelEvent(QWheelEvent* ev)
{
    QPointF pos = ev->posF();
    int t, s;
    gridStepAt(pos, &t, &s);
    const c808_project* proj = c808_project_ro(m_engine);
    if (t >= 0 && s >= 0) {
        int v = proj->patterns[proj->currentPattern].vel[t][s] +
                (ev->angleDelta().y() > 0 ? 4 : -4);
        c808_set_step_vel(m_engine, proj->currentPattern, t, s, clampi(v, 1, 127));
    } else if (rcBpm.contains(pos)) {
        c808_set_bpm(m_engine, proj->bpm + (ev->angleDelta().y() > 0 ? 1.0f : -1.0f));
    }
}

void MachineWidget::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Space) {
        if (m_status.playing) c808_transport_stop(m_engine);
        else c808_transport_play(m_engine);
        return;
    }
    if (ev->key() == Qt::Key_Left) {
        c808_select_pattern(m_engine,
            (c808_project_ro(m_engine)->currentPattern + C808_PATTERNS - 1) % C808_PATTERNS);
        return;
    }
    if (ev->key() == Qt::Key_Right) {
        c808_select_pattern(m_engine,
            (c808_project_ro(m_engine)->currentPattern + 1) % C808_PATTERNS);
        return;
    }
    if (ev->key() == Qt::Key_T) { tapTempo(); return; }
    static const int keys[16] = {
        Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4,
        Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R,
        Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F,
        Qt::Key_Z, Qt::Key_X, Qt::Key_C, Qt::Key_V
    };
    for (int i = 0; i < 16; ++i) {
        if (ev->key() == keys[i]) {
            c808_note_on(m_engine, i, 112, 0);
            m_selTrack = i;
            update();
            return;
        }
    }
    QWidget::keyPressEvent(ev);
}

/* ========================================================================= */
/* 文件 / 导出 / MIDI / Song                                                  */
/* ========================================================================= */
void MachineWidget::setMessage(const QString& s)
{
    m_msg = s;
    update();
}

void MachineWidget::saveProject()
{
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存工程"),
        QStringLiteral("808-project.808"), QStringLiteral("808 Project (*.808)"));
    if (path.isEmpty()) return;
    QByteArray u8 = path.toUtf8();
    if (c808_save_project(m_engine, u8.constData()) == 0)
        setMessage(QStringLiteral("SAVED: ") + path);
    else
        setMessage(QStringLiteral("SAVE FAILED"));
}

void MachineWidget::loadProject()
{
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("加载工程"),
        QString(), QStringLiteral("808 Project (*.808)"));
    if (path.isEmpty()) return;
    QByteArray u8 = path.toUtf8();
    if (c808_load_project(m_engine, u8.constData()) == 0)
        setMessage(QStringLiteral("LOADED: ") + path);
    else
        setMessage(QStringLiteral("LOAD FAILED"));
}

void MachineWidget::startExport(int kind)
{
    if (m_exportBusy) {
        setMessage(QStringLiteral("EXPORT ALREADY RUNNING"));
        return;
    }
    QString filter = kind == 1 ? QStringLiteral("MIDI (*.mid)") : QStringLiteral("WAV (*.wav)");
    QString def = kind == 1 ? QStringLiteral("808-pattern.mid") :
                  kind == 2 ? QStringLiteral("808-stems.wav") : QStringLiteral("808-pattern.wav");
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出"), def, filter);
    if (path.isEmpty()) return;

    const c808_project* proj = c808_project_ro(m_engine);
    int mode = proj && proj->songOn ? 1 : 0;
    m_exportBusy = true;
    setMessage(QStringLiteral("EXPORTING..."));
    m_export = new ExportThread(m_engine, ExportThread::Kind(kind), path, mode, 2, this);
    connect(m_export, &ExportThread::done, this, &MachineWidget::onExportDone,
            Qt::QueuedConnection);
    m_export->start();
}

void MachineWidget::onExportDone(int rc)
{
    m_exportBusy = false;
    setMessage(rc == 0 ? QStringLiteral("EXPORT COMPLETE")
                       : QStringLiteral("EXPORT FAILED (%1)").arg(rc));
    if (m_export) {
        m_export->wait();
        m_export->deleteLater();
        m_export = nullptr;
    }
}

void MachineWidget::cycleMidi()
{
    int n = c808_midi_device_count();
    if (n <= 0) {
        m_midiName = QStringLiteral("MIDI: NONE");
        setMessage(QStringLiteral("NO MIDI INPUT DEVICE"));
        return;
    }
    m_midiIndex++;
    if (m_midiIndex >= n) {
        m_midiIndex = -1;
        c808_midi_stop(m_engine);
        m_midiName = QStringLiteral("MIDI: OFF");
        setMessage(QStringLiteral("MIDI INPUT DISABLED"));
        return;
    }
    if (c808_midi_start(m_engine, m_midiIndex) == 0) {
        char name[256];
        c808_midi_device_name(m_midiIndex, name, sizeof(name));
        m_midiName = QStringLiteral("MIDI: ") + QString::fromUtf8(name);
        setMessage(QStringLiteral("MIDI INPUT: ") + QString::fromUtf8(name));
    } else {
        m_midiName = QStringLiteral("MIDI: ERROR");
        setMessage(QStringLiteral("MIDI OPEN FAILED"));
    }
}

void MachineWidget::openSongEditor()
{
    SongDialog dlg(m_engine, this);
    dlg.exec();
}

void MachineWidget::closeEvent(QCloseEvent* ev)
{
    if (m_exportBusy) {
        setMessage(QStringLiteral("EXPORT IN PROGRESS - PLEASE WAIT"));
        if (ev) ev->ignore();
        return;
    }
    if (m_timer) m_timer->stop();
    if (ev) ev->accept();
}
