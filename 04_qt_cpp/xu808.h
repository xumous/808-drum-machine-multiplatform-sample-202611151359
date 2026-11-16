// ============================================================================
//  xu808.h —— XU-808 Qt C++ 版核心（工程数据 + DSP + 音序引擎）
//
//  徐某把 DSP 与音序器放在纯 C++ 层，UI 只通过普通字段读写参数：
//    · 音频线程由 QAudioSink 的 pull 模式驱动，每次 readData 渲染一块；
//    · 所有事件用“绝对样本时刻”表达，QTimer 绝不参与音序；
//    · voice 的声音是时间的解析函数，衰减完自动回收，不会越叠越多。
// ============================================================================
#ifndef XU808_CORE_H
#define XU808_CORE_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QtGlobal>
#include <array>
#include <vector>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <algorithm>

namespace xu
{
    constexpr int SR = 48000; // 采样率
    constexpr int PADS = 16; // 声部数
    constexpr int MAX_VOICES = 4; // 每声部复音
    constexpr int BLOCK = 256; // 音频块
    constexpr int MAX_STEPS = 64; // 单 pattern 最大步数
    constexpr int PATTERNS = 16; // pattern 数量

    // 条件触发（数值即存储值）
    enum Cond { CondAlways = 0, CondFill = 1, CondNotFill = 2, CondEvery2 = 3, CondEvery4 = 4 };

    extern const char* const kCodes[PADS];
    extern const wchar_t* const kNamesCn[PADS];
    extern const int kMidiNotes[PADS];

    // ---- 步进 ----
    struct Step
    {
        bool on = false;
        int velocity = 100;
        bool accent = false;
        int prob = 100;
        int micro = 0;
        int ratchet = 1;
        int cond = CondAlways;
    };

    // ---- Pattern ----
    struct Pattern
    {
        QString name;
        int steps = 16;
        bool fill = false;
        std::array<std::array<Step, MAX_STEPS>, PADS> data{};
        std::array<int, PADS> trackLen{};
        Pattern() { trackLen.fill(16); }
        void setDemoBeat();
    };

    // ---- 每声部参数 ----
    struct PadParams
    {
        float tune = 0.50f;
        float decay = 0.50f;
        float tone = 0.50f;
        float snappy = 0.55f;
        float tail = 0.45f;
        float level = 0.75f;
        float pan = 0.50f;
        bool mute = false;
        bool solo = false;
    };

    // ---- 主效果 ----
    struct Master
    {
        float volume = 0.80f;
        float drive = 1.35f;
        float lpCutoff = 18000.0f;
        float lpQ = 0.90f;
        float hpCutoff = 28.0f;
        float hpQ = 0.70f;
        bool lpOn = true;
        bool hpOn = true;
        float bpm = 120.0f;
        float swing = 0.0f;
        int beatsPerBar = 4;
        int beatUnit = 4;
        bool songMode = false;
    };

    struct SongEntry
    {
        int pattern = 0;
        int repeats = 1;
    };

    // ---- 工程 ----
    struct Project
    {
        QString name = QStringLiteral("徐某的 808 工程");
        std::array<Pattern, PATTERNS> patterns;
        std::array<PadParams, PADS> pads;
        Master master;
        std::vector<SongEntry> song;
        int current = 0;
        quint32 seed = 0x808808;
        Project();
        Pattern& cur() { return patterns[std::clamp(current, 0, PATTERNS - 1)]; }
        QJsonObject toJson() const;
        static Project fromJson(const QJsonObject& o);
        bool save(const QString& path) const;
        static bool load(const QString& path, Project& out);
    };

    // ---- RBJ 双二阶 ----
    class Biquad
    {
    public:
        void setLowPass(double fc, double q);
        void setHighPass(double fc, double q);
        void setBandPass(double fc, double q);

        inline double process(double x)
        {
            double y = b0_ * x + z1_;
            z1_ = b1_ * x - a1_ * y + z2_;
            z2_ = b2_ * x - a2_ * y;
            return y;
        }

        void reset() { z1_ = z2_ = 0.0; }

    private:
        double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
        int kind_ = 0;
        double fc_ = 0, q_ = 1;
    };

    // ---- 一个鼓槌 ----
    class Voice
    {
    public:
        explicit Voice(int pad = 0);
        void trigger(qint64 when, float gain, bool accent);
        void kill(qint64 when, int fade = 24);
        // 把 [b0,b1) 叠加到左右声道（L/R 的索引用 b0 作原点）
        void render(float* L, float* R, int n, qint64 b0, qint64 b1, const PadParams& p);
        bool active = false;
        qint64 start = 0;
        qint64 stop = 0x7FFFFFFFFFFFFFFFLL;
        int pad = 0;

    private:
        double synth(double t, const PadParams& p);
        double takeNoise();
        float gain_ = 1.0f;
        bool accent_ = false;
        int fadeLen_ = 64;
        std::array<float, 8192> noise_{};
        int npos_ = 0;
        Biquad bp_, hp_, bp2_;
    };

    struct Event
    {
        qint64 t;
        int pad;
        float vel;
        bool accent;
    };

    // ---- 引擎 ----
    class Engine
    {
    public:
        explicit Engine(Project* p);
        Project* proj;

        std::atomic<bool> playing{false};
        qint64 samplePos = 0;
        qint64 nextStepAbs = 0;
        int stepIndex = 0;
        qint64 globalStep = 0;
        int cycleCount = 0;
        bool fillNow = false;
        int pendingPattern = -1;
        int songIndex = 0;
        int songRepeat = 0;
        quint32 rng = 0x808808;
        bool clockExternal = false;
        int clockPulse = 0;
        int soloOverride = -1;

        std::atomic<float> meterL{0}, meterR{0}, cpu{0};
        std::atomic<int> xruns{0};

        void play();
        void stop();
        void toggle() { playing ? stop() : play(); }
        void render(int n, float* L, float* R);
        void triggerPad(int pad, int vel, int accent);
        void requestPattern(int idx);
        void externalTick();

        std::vector<float> renderPattern(int repeats = 1);
        std::vector<float> renderSong();
        bool exportWav(const QString& path, bool song);
        bool exportMidi(const QString& path, bool song);
        static bool writeWav(const QString& path, const std::vector<float>& interleaved);

    private:
        void emitStep(qint64 stepAbs);
        void advanceAfterStep();
        void collectEvents(qint64 b0, qint64 b1, std::vector<Event>& out);
        void trigger(int pad, qint64 when, float vel, bool accent);
        void renderVoices(float* L, float* R, int n, qint64 b0, qint64 a, qint64 b);
        bool audible(int pad) const;
        int stepLen() const;
        quint32 nextRand();
        bool condOk(int cond) const;
        std::vector<float> renderSeconds(double seconds, int soloPad = -1);

        std::array<std::array<Voice, MAX_VOICES>, PADS> voices_;
        std::vector<Event> pending_;
        std::vector<Event> manual_;
        std::vector<Event> evbuf_; // 本块事件缓冲；与 pending_ 分开，避免别名覆盖
        Biquad lpL_, lpR_, hpL_, hpR_;
        std::vector<float> offL_, offR_;
    };
} // namespace xu

#endif