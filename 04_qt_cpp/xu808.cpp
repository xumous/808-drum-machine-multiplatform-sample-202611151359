// ============================================================================
//  xu808.cpp —— XU-808 Qt C++ 版核心实现
//
//  所有公式与 Python/C/PHP 等版本逐项一致；这样同一个 pattern
//  在不同版本里听起来是同一台鼓机，而不是“十七台不同的鼓”。
// ============================================================================
#include "xu808.h"
#include <algorithm>
#include <cstring>
#include <QFileInfo>
#include <QDataStream>

namespace xu
{
    const char* const kCodes[PADS] = {
        "BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC", "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH"
    };
    const wchar_t* const kNamesCn[PADS] = {
        L"底鼓", L"军鼓", L"低嗵", L"中嗵", L"高嗵", L"低康加", L"中康加", L"高康加",
        L"边击", L"击木", L"拍手", L"沙锤", L"牛铃", L"吊镲", L"开镲", L"闭镲"
    };
    const int kMidiNotes[PADS] = {
        36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42
    };

    static inline double clampd(double x, double lo, double hi)
    {
        return x < lo ? lo : (x > hi ? hi : x);
    }

    static constexpr double PI = 3.14159265358979323846;

    // ---------------------------------------------------------------------------
    // Pattern 默认律动
    // ---------------------------------------------------------------------------
    void Pattern::setDemoBeat()
    {
        for (auto& row : data) for (auto& s : row) s = Step{};
        steps = 16;
        name = QStringLiteral("PATTERN 1");
        auto set = [this](int p, int s, int v, bool a)
        {
            data[p][s].on = true;
            data[p][s].velocity = v;
            data[p][s].accent = a;
        };
        set(0, 0, 118, true);
        set(0, 8, 105, false);
        set(0, 14, 90, false);
        set(1, 4, 110, false);
        set(1, 12, 110, true);
        for (int s = 0; s < 16; s += 2) set(15, s, (s % 4 == 0) ? 96 : 84, false);
        set(14, 14, 92, false);
        set(10, 12, 80, false);
        for (int s = 1; s < 16; s += 2) set(11, s, 42, false);
        set(0, 15, 88, false);
        data[0][15].ratchet = 3;
    }

    // ---------------------------------------------------------------------------
    // 工程构造 / JSON
    // ---------------------------------------------------------------------------
    Project::Project()
    {
        for (int i = 0; i < PATTERNS; ++i)
        {
            patterns[i].steps = 16;
            patterns[i].name = QStringLiteral("PATTERN %1").arg(i + 1);
            patterns[i].trackLen.fill(16);
        }
        patterns[0].setDemoBeat();
        for (int i = 0; i < PADS; ++i) pads[i] = PadParams{};
        pads[0].decay = 0.55f;
        pads[0].tone = 0.45f;
        song.push_back({0, 4});
        song.push_back({0, 4});
    }

    QJsonObject Project::toJson() const
    {
        QJsonObject root;
        root["format"] = "XU808";
        root["version"] = 1;
        root["name"] = name;
        root["seed"] = double(seed);
        root["current"] = current;

        QJsonObject m;
        m["volume"] = master.volume;
        m["drive"] = master.drive;
        m["lpCutoff"] = master.lpCutoff;
        m["lpQ"] = master.lpQ;
        m["hpCutoff"] = master.hpCutoff;
        m["hpQ"] = master.hpQ;
        m["lpOn"] = master.lpOn;
        m["hpOn"] = master.hpOn;
        m["bpm"] = master.bpm;
        m["swing"] = master.swing;
        m["beatsPerBar"] = master.beatsPerBar;
        m["beatUnit"] = master.beatUnit;
        m["songMode"] = master.songMode;
        root["master"] = m;

        QJsonArray pa;
        for (const auto& p : pads)
        {
            QJsonObject o;
            o["tune"] = p.tune;
            o["decay"] = p.decay;
            o["tone"] = p.tone;
            o["snappy"] = p.snappy;
            o["tail"] = p.tail;
            o["level"] = p.level;
            o["pan"] = p.pan;
            o["mute"] = p.mute;
            o["solo"] = p.solo;
            pa.append(o);
        }
        root["pads"] = pa;

        QJsonArray songArr;
        for (const auto& e : song)
        {
            QJsonObject o;
            o["pattern"] = e.pattern;
            o["repeats"] = e.repeats;
            songArr.append(o);
        }
        QJsonObject so;
        so["enabled"] = master.songMode;
        so["entries"] = songArr;
        root["song"] = so;

        QJsonArray pats;
        for (const auto& pat : patterns)
        {
            QJsonObject o;
            o["name"] = pat.name;
            o["steps"] = pat.steps;
            o["fill"] = pat.fill;
            QJsonArray tl;
            for (int i = 0; i < PADS; ++i) tl.append(pat.trackLen[i]);
            o["trackLen"] = tl;
            QJsonArray rows;
            for (int p = 0; p < PADS; ++p)
            {
                QJsonArray row;
                for (int s = 0; s < MAX_STEPS; ++s)
                {
                    const Step& st = pat.data[p][s];
                    // 紧凑数组编码：比对象省一半空间，也更容易逐字段校验
                    QJsonArray one;
                    one.append(st.on ? 1 : 0);
                    one.append(st.velocity);
                    one.append(st.accent ? 1 : 0);
                    one.append(st.prob);
                    one.append(st.micro);
                    one.append(st.ratchet);
                    one.append(st.cond);
                    row.append(one);
                }
                rows.append(row);
            }
            o["data"] = rows;
            pats.append(o);
        }
        root["patterns"] = pats;
        return root;
    }

    Project Project::fromJson(const QJsonObject& o)
    {
        Project p; // 先构造默认工程，缺失字段自然保留默认值
        p.name = o.value("name").toString(p.name);
        p.seed = quint32(o.value("seed").toDouble(p.seed));
        p.current = qBound(0, o.value("current").toInt(0), PATTERNS - 1);

        QJsonObject m = o.value("master").toObject();
        if (!m.isEmpty())
        {
            p.master.volume = float(m.value("volume").toDouble(p.master.volume));
            p.master.drive = float(m.value("drive").toDouble(p.master.drive));
            p.master.lpCutoff = float(m.value("lpCutoff").toDouble(p.master.lpCutoff));
            p.master.lpQ = float(m.value("lpQ").toDouble(p.master.lpQ));
            p.master.hpCutoff = float(m.value("hpCutoff").toDouble(p.master.hpCutoff));
            p.master.hpQ = float(m.value("hpQ").toDouble(p.master.hpQ));
            p.master.lpOn = m.value("lpOn").toBool(p.master.lpOn);
            p.master.hpOn = m.value("hpOn").toBool(p.master.hpOn);
            p.master.bpm = float(m.value("bpm").toDouble(p.master.bpm));
            p.master.swing = float(m.value("swing").toDouble(p.master.swing));
            p.master.beatsPerBar = m.value("beatsPerBar").toInt(p.master.beatsPerBar);
            p.master.beatUnit = m.value("beatUnit").toInt(p.master.beatUnit);
            p.master.songMode = m.value("songMode").toBool(p.master.songMode);
        }
        QJsonArray pa = o.value("pads").toArray();
        for (int i = 0; i < PADS && i < pa.size(); ++i)
        {
            QJsonObject x = pa[i].toObject();
            PadParams& d = p.pads[i];
            d.tune = float(x.value("tune").toDouble(d.tune));
            d.decay = float(x.value("decay").toDouble(d.decay));
            d.tone = float(x.value("tone").toDouble(d.tone));
            d.snappy = float(x.value("snappy").toDouble(d.snappy));
            d.tail = float(x.value("tail").toDouble(d.tail));
            d.level = float(x.value("level").toDouble(d.level));
            d.pan = float(x.value("pan").toDouble(d.pan));
            d.mute = x.value("mute").toBool(d.mute);
            d.solo = x.value("solo").toBool(d.solo);
        }
        QJsonArray pats = o.value("patterns").toArray();
        for (int i = 0; i < PATTERNS && i < pats.size(); ++i)
        {
            QJsonObject x = pats[i].toObject();
            Pattern& pat = p.patterns[i];
            pat.name = x.value("name").toString(pat.name);
            pat.steps = qBound(1, x.value("steps").toInt(16), MAX_STEPS);
            pat.fill = x.value("fill").toBool(false);
            QJsonArray tl = x.value("trackLen").toArray();
            for (int t = 0; t < PADS && t < tl.size(); ++t)
                pat.trackLen[t] = qBound(1, tl[t].toInt(16), MAX_STEPS);
            QJsonArray rows = x.value("data").toArray();
            for (int pad = 0; pad < PADS && pad < rows.size(); ++pad)
            {
                QJsonArray row = rows[pad].toArray();
                for (int s = 0; s < MAX_STEPS && s < row.size(); ++s)
                {
                    QJsonArray one = row[s].toArray();
                    Step& st = pat.data[pad][s];
                    st.on = one.size() > 0 && one[0].toInt() != 0;
                    st.velocity = qBound(1, one.size() > 1 ? one[1].toInt(100) : 100, 127);
                    st.accent = one.size() > 2 && one[2].toInt() != 0;
                    st.prob = qBound(0, one.size() > 3 ? one[3].toInt(100) : 100, 100);
                    st.micro = qBound(-2400, one.size() > 4 ? one[4].toInt(0) : 0, 2400);
                    st.ratchet = qBound(1, one.size() > 5 ? one[5].toInt(1) : 1, 8);
                    st.cond = qBound(0, one.size() > 6 ? one[6].toInt(0) : 0, 4);
                }
            }
        }
        QJsonArray se = o.value("song").toObject().value("entries").toArray();
        if (!se.isEmpty())
        {
            p.song.clear();
            for (const auto& v : se)
            {
                QJsonObject e = v.toObject();
                p.song.push_back({
                    qBound(0, e.value("pattern").toInt(0), PATTERNS - 1),
                    std::max(1, e.value("repeats").toInt(1))
                });
            }
        }
        return p;
    }

    bool Project::save(const QString& path) const
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
        return true;
    }

    bool Project::load(const QString& path, Project& out)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
        out = fromJson(doc.object());
        return true;
    }

    // ---------------------------------------------------------------------------
    // Biquad
    // ---------------------------------------------------------------------------
    void Biquad::setLowPass(double fc, double q)
    {
        fc = clampd(fc, 20.0, SR * 0.49);
        q = clampd(q, 0.1, 20.0);
        if (kind_ == 1 && std::abs(fc - fc_) < 0.5 && std::abs(q - q_) < 1e-3) return;
        double w0 = 2 * PI * fc / SR, cw = std::cos(w0), alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
        b0_ = (1 - cw) * 0.5 / a0;
        b1_ = (1 - cw) / a0;
        b2_ = b0_;
        a1_ = (-2 * cw) / a0;
        a2_ = (1 - alpha) / a0;
        kind_ = 1;
        fc_ = fc;
        q_ = q;
    }

    void Biquad::setHighPass(double fc, double q)
    {
        fc = clampd(fc, 10.0, SR * 0.49);
        q = clampd(q, 0.1, 20.0);
        if (kind_ == 2 && std::abs(fc - fc_) < 0.5 && std::abs(q - q_) < 1e-3) return;
        double w0 = 2 * PI * fc / SR, cw = std::cos(w0), alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
        b0_ = (1 + cw) * 0.5 / a0;
        b1_ = -(1 + cw) / a0;
        b2_ = b0_;
        a1_ = (-2 * cw) / a0;
        a2_ = (1 - alpha) / a0;
        kind_ = 2;
        fc_ = fc;
        q_ = q;
    }

    void Biquad::setBandPass(double fc, double q)
    {
        fc = clampd(fc, 20.0, SR * 0.49);
        q = clampd(q, 0.1, 30.0);
        if (kind_ == 3 && std::abs(fc - fc_) < 0.5 && std::abs(q - q_) < 1e-3) return;
        double w0 = 2 * PI * fc / SR, cw = std::cos(w0), alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
        b0_ = alpha / a0;
        b1_ = 0;
        b2_ = -alpha / a0;
        a1_ = (-2 * cw) / a0;
        a2_ = (1 - alpha) / a0;
        kind_ = 3;
        fc_ = fc;
        q_ = q;
    }

    // ---------------------------------------------------------------------------
    // Voice
    // ---------------------------------------------------------------------------
    Voice::Voice(int pad) : pad(pad)
    {
        quint32 st = 0x808u + quint32(pad) * 7919u;
        for (auto& v : noise_)
        {
            st = st * 1664525u + 1013904223u;
            v = float(int(st >> 8) / 8388608.0 - 1.0);
        }
    }

    void Voice::trigger(qint64 when, float gain, bool accent)
    {
        active = true;
        start = when;
        stop = 0x7FFFFFFFFFFFFFFFLL;
        gain_ = gain;
        accent_ = accent;
        fadeLen_ = 64;
        npos_ = 0;
        bp_.reset();
        hp_.reset();
        bp2_.reset();
    }

    void Voice::kill(qint64 when, int fade)
    {
        if (!active) return;
        fade = std::max(1, fade);
        // stop 必须在 when 之后留出 fade 长度，斜坡才有空间；
        // stop=when 会让未来样本全部越界，等于硬切。
        stop = std::min(stop, when + fade);
        fadeLen_ = fade;
    }

    double Voice::takeNoise()
    {
        if (npos_ >= int(noise_.size())) npos_ = 0;
        return double(noise_[npos_++]);
    }

    static double square(double phase) { return std::sin(phase) >= 0.0 ? 1.0 : -1.0; }

    double Voice::synth(double t, const PadParams& p)
    {
        const int pad = this->pad;
        if (pad == 0)
        {
            double fEnd = 40.0 + p.tune * 40.0;
            double fStart = fEnd * (1.6 + p.tone * 1.9);
            double tau = 0.018 + (1.0 - p.decay) * 0.012;
            double df = fStart - fEnd;
            double phase = 2 * PI * (fEnd * t + df * tau * (1.0 - std::exp(-t / tau)));
            double dec = (0.12 + p.decay * 1.48) * (accent_ ? 1.18 : 1.0);
            double amp = std::exp(-t / dec);
            double n = takeNoise();
            hp_.setHighPass(1800.0 + p.tone * 3000.0, 0.8);
            double click = hp_.process(n) * std::exp(-t / 0.0035) * (0.06 + p.tone * 0.32);
            double body = std::tanh(std::sin(phase) * (1.15 + p.decay * 1.2)) * 0.92;
            return body * amp + click;
        }
        if (pad == 1)
        {
            double f1 = 150.0 + p.tune * 110.0;
            double f2 = f1 * (1.78 + p.tone * 0.22);
            double decO = 0.055 + p.decay * 0.14;
            double osc = 0.55 * std::sin(2 * PI * f1 * t) * std::exp(-t / decO)
                + 0.34 * std::sin(2 * PI * f2 * t) * std::exp(-t / (decO * 0.72));
            double n = takeNoise();
            bp_.setBandPass(1200.0 + p.snappy * 3800.0, 0.9 + p.tone * 0.8);
            double nz = bp_.process(n);
            double decN = (0.075 + p.decay * 0.16) * (accent_ ? 0.85 : 1.0);
            nz *= std::exp(-t / decN) * (0.25 + p.snappy * 0.7);
            return osc * 0.7 + nz;
        }
        if (pad >= 2 && pad <= 7)
        {
            double baseF, decayS, clickAmt, sweep;
            if (pad <= 4)
            {
                baseF = pad == 2 ? 80.0 : pad == 3 ? 120.0 : 170.0;
                decayS = 0.15 + p.decay * 0.75;
                clickAmt = 0.04 + p.tone * 0.22;
                sweep = 1.9;
            }
            else
            {
                baseF = pad == 5 ? 220.0 : pad == 6 ? 300.0 : 400.0;
                decayS = 0.10 + p.decay * 0.55;
                clickAmt = 0.02 + p.tone * 0.12;
                sweep = 1.35;
            }
            double f0 = baseF * (0.6 + p.tune);
            double tau = pad <= 4 ? 0.035 : 0.020;
            double phase = 2 * PI * (f0 * t + f0 * (sweep - 1.0) * tau * (1.0 - std::exp(-t / tau)));
            double amp = std::exp(-t / decayS);
            double n = takeNoise();
            hp_.setHighPass(2500.0, 0.7);
            double click = hp_.process(n) * std::exp(-t / 0.004) * clickAmt;
            return std::sin(phase) * amp * 0.9 + click;
        }
        if (pad == 8)
        {
            double f = 1200.0 + p.tune * 1400.0;
            double dec = 0.012 + p.decay * 0.05;
            double sq = square(2 * PI * f * t);
            bp_.setBandPass(f, 2.2);
            double body = bp_.process(sq) * std::exp(-t / dec);
            double n = takeNoise();
            hp_.setHighPass(3000.0, 0.8);
            double tick = hp_.process(n) * std::exp(-t / 0.004) * 0.35;
            return body * 0.8 + tick;
        }
        if (pad == 9)
        {
            double f = 2000.0 + p.tune * 1000.0;
            double dec = 0.018 + p.decay * 0.035;
            return std::sin(2 * PI * f * t) * std::exp(-t / dec) * 0.9;
        }
        if (pad == 10)
        {
            double n = takeNoise();
            bp_.setBandPass(800.0 + p.tone * 1700.0, 1.1);
            double nz = bp_.process(n);
            double env = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                double tk = k * 0.009;
                if (t >= tk) env += std::exp(-(t - tk) / 0.011) * 0.55;
            }
            double tail = std::exp(-t / (0.05 + p.decay * 0.35)) * (0.15 + p.tail * 0.55);
            return nz * (env + tail);
        }
        if (pad == 11)
        {
            double n = takeNoise();
            hp_.setHighPass(4000.0 + p.tone * 5000.0, 0.9);
            double nz = hp_.process(n);
            double env = (1.0 - std::exp(-t / 0.0015)) * std::exp(-t / (0.018 + p.decay * 0.12));
            return nz * env * 0.85;
        }
        if (pad == 12)
        {
            double f = 400.0 + p.tune * 300.0;
            double sq1 = square(2 * PI * f * t);
            double sq2 = square(2 * PI * f * 1.48 * t);
            bp_.setBandPass(2400.0 + p.tone * 1200.0, 1.4);
            double body = bp_.process(sq1 * 0.5 + sq2 * 0.5);
            double dec = 0.05 + p.decay * 0.35;
            return body * std::exp(-t / dec) * 0.85;
        }
        // 13..15
        static const double ratios[6] = {1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523};
        double root = 320.0 * (0.75 + p.tune * 0.5);
        double metal = 0.0;
        for (double r : ratios) metal += square(2 * PI * root * r * t);
        metal /= 6.0;
        hp_.setHighPass(5200.0 + p.tone * 2600.0, 0.8);
        metal = hp_.process(metal);
        bp_.setBandPass(7600.0 + p.tone * 3600.0, 0.9);
        metal = bp_.process(metal);
        double dec = (pad == 15)
                         ? 0.028 + p.decay * 0.20
                         : (pad == 14)
                         ? 0.12 + p.decay * 0.65
                         : 0.25 + p.decay * 1.75;
        double env = std::exp(-t / dec);
        double outp = metal * env;
        if (pad == 13)
        {
            double n = takeNoise();
            bp2_.setBandPass(10000.0, 0.7);
            outp += bp2_.process(n) * env * 0.12;
        }
        return outp * 0.75;
    }

    void Voice::render(float* L, float* R, int n, qint64 b0, qint64 b1, const PadParams& p)
    {
        if (!active) return;
        qint64 a = std::max(b0, start);
        qint64 b = std::min(b1, stop);
        if (b <= a)
        {
            if (b1 > stop) active = false;
            return;
        }
        int m = int(b - a);
        int off = int(a - b0);
        if (m <= 0 || off >= n) return;
        double pan = clampd(p.pan, 0, 1);
        double lg = std::cos(pan * PI * 0.5), rg = std::sin(pan * PI * 0.5);
        double g = double(gain_) * std::max(0.0f, p.level) * 0.42;
        for (int i = 0; i < m; ++i)
        {
            qint64 abs = a + i;
            double t = double(abs - start) / double(SR);
            double s = synth(t, p);
            if (stop < 0x7FFFFFFFFFFFFFFFLL)
            {
                qint64 dist = stop - abs;
                if (dist < fadeLen_)
                    s *= dist <= 0 ? 0.0 : double(dist) / double(fadeLen_);
            }
            L[off + i] += float(s * g * lg);
            R[off + i] += float(s * g * rg);
        }
        if (b1 - start > 4LL * SR) active = false;
    }

    // ---------------------------------------------------------------------------
    // Engine
    // ---------------------------------------------------------------------------
    Engine::Engine(Project* p) : proj(p)
    {
        pending_.reserve(4096);
        manual_.reserve(1024);
        offL_.assign(8192, 0.0f);
        offR_.assign(8192, 0.0f);
        for (int i = 0; i < PADS; ++i)
            for (int v = 0; v < MAX_VOICES; ++v)
                voices_[i][v] = Voice(i);
        rng = p->seed ? p->seed : 0x808808;
    }

    void Engine::play()
    {
        if (playing) return;
        playing = true;
        stepIndex = 0;
        globalStep = 0;
        cycleCount = 0;
        fillNow = false;
        songIndex = 0;
        songRepeat = 0;
        nextStepAbs = samplePos;
        pending_.clear();
    }

    void Engine::stop()
    {
        playing = false;
        for (auto& pool : voices_)
            for (auto& v : pool) v.kill(samplePos, 480);
        pending_.clear();
    }

    int Engine::stepLen() const
    {
        float bpm = std::clamp(proj->master.bpm, 40.0f, 300.0f);
        int n = int(SR * 60.0 / bpm / 4.0 + 0.5);
        return std::max(64, n);
    }

    quint32 Engine::nextRand()
    {
        quint32 x = rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng = x;
        return x;
    }

    bool Engine::condOk(int cond) const
    {
        switch (cond)
        {
        case CondAlways: return true;
        case CondFill: return fillNow;
        case CondNotFill: return !fillNow;
        case CondEvery2: return (cycleCount % 2) == 0;
        case CondEvery4: return (cycleCount % 4) == 0;
        default: return true;
        }
    }

    void Engine::emitStep(qint64 stepAbs)
    {
        Pattern& pat = proj->patterns[qBound(0, proj->current, PATTERNS - 1)];
        int sl = stepLen();
        int swingOff = (stepIndex & 1) ? int(sl * proj->master.swing * 0.5) : 0;
        qint64 base = stepAbs + swingOff;
        for (int pad = 0; pad < PADS; ++pad)
        {
            int tl = std::clamp(int(pat.trackLen[pad]), 1, MAX_STEPS);
            int idx = int(globalStep % tl);
            const Step& st = pat.data[pad][idx];
            if (!st.on || !condOk(st.cond) || st.prob <= 0) continue;
            if (st.prob < 100 && int(nextRand() % 100) >= st.prob) continue;
            float vel = std::clamp(st.velocity / 127.0f, 0.02f, 1.0f);
            if (st.accent) vel = std::min(1.25f, vel * 1.30f);
            int r = std::clamp(st.ratchet, 1, 8);
            for (int i = 0; i < r; ++i)
            {
                qint64 t = base + st.micro + qint64(i * double(sl) / r);
                float v = vel * float(std::pow(0.92, i));
                pending_.push_back({t, pad, v, st.accent});
            }
        }
    }

    void Engine::advanceAfterStep()
    {
        Pattern& pat = proj->patterns[qBound(0, proj->current, PATTERNS - 1)];
        ++stepIndex;
        ++globalStep;
        if (stepIndex < std::max(1, pat.steps)) return;
        stepIndex = 0;
        ++cycleCount;
        fillNow = (cycleCount % 4) == 3;
        if (pendingPattern >= 0)
        {
            proj->current = std::clamp(pendingPattern, 0, PATTERNS - 1);
            pendingPattern = -1;
            songRepeat = 0;
            return;
        }
        if (proj->master.songMode && !proj->song.empty())
        {
            int si = songIndex % int(proj->song.size());
            SongEntry& en = proj->song[si];
            ++songRepeat;
            if (songRepeat >= std::max(1, en.repeats))
            {
                songRepeat = 0;
                songIndex = (songIndex + 1) % int(proj->song.size());
                proj->current = std::clamp(proj->song[songIndex].pattern, 0, PATTERNS - 1);
            }
        }
    }

    bool Engine::audible(int pad) const
    {
        if (soloOverride >= 0) return pad == soloOverride;
        bool anySolo = false;
        for (const auto& p : proj->pads) if (p.solo)
        {
            anySolo = true;
            break;
        }
        const PadParams& p = proj->pads[pad];
        if (p.mute) return false;
        if (anySolo && !p.solo) return false;
        return true;
    }

    void Engine::trigger(int pad, qint64 when, float vel, bool accent)
    {
        if (pad == 15 || pad == 14)
            for (auto& v : voices_[14]) v.kill(when, 16);
        Voice * target = nullptr;
        for (auto& v : voices_[pad]) if (!v.active)
        {
            target = &v;
            break;
        }
        if (!target)
        {
            target = &voices_[pad][0];
            for (int i = 1; i < MAX_VOICES; ++i)
                if (voices_[pad][i].start < target->start) target = &voices_[pad][i];
            target->kill(when, 64);
        }
        target->trigger(when, vel, accent);
    }

    void Engine::collectEvents(qint64 b0, qint64 b1, std::vector<Event>& out)
    {
        out.clear();
        size_t keep = 0;
        for (size_t i = 0; i < manual_.size(); ++i)
        {
            Event ev = manual_[i];
            if (ev.t < b1)
            {
                ev.t = std::max(ev.t, b0);
                out.push_back(ev);
            }
            else
            {
                manual_[keep++] = ev;
            }
        }
        manual_.resize(keep);
        if (playing && !clockExternal)
        {
            int guard = 0;
            while (nextStepAbs < b1 && guard++ < 64)
            {
                emitStep(std::max(nextStepAbs, b0));
                nextStepAbs += stepLen();
                advanceAfterStep();
            }
        }
        keep = 0;
        for (size_t i = 0; i < pending_.size(); ++i)
        {
            Event ev = pending_[i];
            if (ev.t < b1)
            {
                ev.t = std::max(ev.t, b0);
                out.push_back(ev);
            }
            else
            {
                pending_[keep++] = ev;
            }
        }
        pending_.resize(keep);
        std::stable_sort(out.begin(), out.end(),
                         [](const Event& a, const Event& b) { return a.t < b.t; });
    }

    void Engine::renderVoices(float* L, float* R, int n, qint64 b0, qint64 a, qint64 b)
    {
        if (b <= a) return;
        for (int pad = 0; pad < PADS; ++pad)
        {
            if (!audible(pad)) continue;
            const PadParams& p = proj->pads[pad];
            for (auto& v : voices_[pad])
                if (v.active) v.render(L, R, n, b0, b, p);
        }
    }

    void Engine::render(int n, float* L, float* R)
    {
        qint64 b0 = samplePos, b1 = b0 + n;
        std::memset(L, 0, sizeof(float) * n);
        std::memset(R, 0, sizeof(float) * n);

        // 用独立的 evbuf_ 收集本块事件；绝不能和 pending_ 复用，
        // 否则 collectEvents 的输入/输出同一对象会在 resize 时丢事件。
        collectEvents(b0, b1, evbuf_);

        qint64 cur = b0;
        for (const Event& ev : evbuf_)
        {
            if (ev.t > cur)
            {
                renderVoices(L, R, n, b0, cur, ev.t);
                cur = ev.t;
            }
            if (ev.t < b1) trigger(ev.pad, ev.t, ev.vel, ev.accent);
        }
        if (cur < b1) renderVoices(L, R, n, b0, cur, b1);

        Master& m = proj->master;
        if (m.hpOn)
        {
            hpL_.setHighPass(m.hpCutoff, m.hpQ);
            hpR_.setHighPass(m.hpCutoff, m.hpQ);
            for (int i = 0; i < n; ++i)
            {
                L[i] = float(hpL_.process(L[i]));
                R[i] = float(hpR_.process(R[i]));
            }
        }
        if (m.lpOn)
        {
            lpL_.setLowPass(m.lpCutoff, m.lpQ);
            lpR_.setLowPass(m.lpCutoff, m.lpQ);
            for (int i = 0; i < n; ++i)
            {
                L[i] = float(lpL_.process(L[i]));
                R[i] = float(lpR_.process(R[i]));
            }
        }
        float drive = std::clamp(m.drive, 1.0f, 8.0f);
        float norm = float(std::tanh(double(drive)));
        float vol = std::clamp(m.volume, 0.0f, 1.0f);
        float pl = 0, pr = 0;
        for (int i = 0; i < n; ++i)
        {
            float xl = L[i], xr = R[i];
            if (drive > 1.001f)
            {
                xl = float(std::tanh(double(xl) * drive)) / norm;
                xr = float(std::tanh(double(xr) * drive)) / norm;
            }
            xl = xl / (1.0f + std::abs(xl)); // 软限幅
            xr = xr / (1.0f + std::abs(xr));
            xl *= vol;
            xr *= vol;
            L[i] = xl;
            R[i] = xr;
            pl = std::max(pl, std::abs(xl));
            pr = std::max(pr, std::abs(xr));
        }
        meterL = std::max(pl, meterL.load() * 0.75f);
        meterR = std::max(pr, meterR.load() * 0.75f);

        samplePos = b1;
        for (auto& pool : voices_)
            for (auto& v : pool)
                if (v.active && b0 - v.start > 4LL * SR) v.active = false;
    }

    void Engine::externalTick()
    {
        if (!playing) return;
        if (++clockPulse < 6) return;
        clockPulse = 0;
        emitStep(samplePos);
        advanceAfterStep();
        nextStepAbs = samplePos + stepLen();
    }

    void Engine::triggerPad(int pad, int vel, int accent)
    {
        if (pad < 0 || pad >= PADS) return;
        vel = std::clamp(vel, 1, 127);
        manual_.push_back({samplePos + 32, pad, vel / 127.0f, accent != 0});
    }

    void Engine::requestPattern(int idx)
    {
        idx = std::clamp(idx, 0, PATTERNS - 1);
        if (playing) pendingPattern = idx;
        else proj->current = idx;
    }

    // ---------------------------------------------------------------------------
    // 离线渲染与导出：与实时路径共用 render()
    // ---------------------------------------------------------------------------
    std::vector<float> Engine::renderSeconds(double seconds, int soloPad)
    {
        int total = int(seconds * SR);
        std::vector<float> out(size_t(total) * 2, 0.0f);
        std::vector<float> L(BLOCK, 0.0f), R(BLOCK, 0.0f);
        soloOverride = soloPad;
        play();
        int done = 0;
        while (done < total)
        {
            int n = std::min(BLOCK, total - done);
            render(n, L.data(), R.data());
            for (int i = 0; i < n; ++i)
            {
                out[size_t(done + i) * 2] = L[i];
                out[size_t(done + i) * 2 + 1] = R[i];
            }
            done += n;
        }
        return out;
    }

    std::vector<float> Engine::renderPattern(int repeats)
    {
        // Engine 里有 64 个 voice（每个 8192 噪声样本），总大小约 2.5MB；
        // Project 也有约 0.5MB。二者都绝不能放栈上，否则必爆栈。
        auto* copy = new Project(*proj);
        auto* e = new Engine(copy);
        double sec = double(copy->patterns[copy->current].steps) * std::max(1, repeats)
            * e->stepLen() / double(SR) + 3.0;
        auto out = e->renderSeconds(sec);
        delete e;
        delete copy;
        return out;
    }

    std::vector<float> Engine::renderSong()
    {
        auto* copy = new Project(*proj);
        auto* e = new Engine(copy);
        long long steps = 0;
        if (!copy->song.empty())
        {
            for (const auto& en : copy->song)
            {
                int idx = std::clamp(en.pattern, 0, PATTERNS - 1);
                steps += (long long)copy->patterns[idx].steps * std::max(1, en.repeats);
            }
        }
        if (steps <= 0) steps = copy->patterns[copy->current].steps;
        double sec = double(steps) * e->stepLen() / double(SR) + 3.0;
        auto out = e->renderSeconds(sec);
        delete e;
        delete copy;
        return out;
    }

    bool Engine::writeWav(const QString& path, const std::vector<float>& interleaved)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        const quint32 frames = quint32(interleaved.size() / 2);
        const quint32 dataBytes = frames * 4;
        auto w32 = [&](quint32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto w16 = [&](quint16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4);
        w32(36 + dataBytes);
        f.write("WAVE", 4);
        f.write("fmt ", 4);
        w32(16);
        w16(1);
        w16(2);
        w32(SR);
        w32(SR * 4);
        w16(4);
        w16(16);
        f.write("data", 4);
        w32(dataBytes);
        std::vector<qint16> pcm(interleaved.size());
        for (size_t i = 0; i < interleaved.size(); ++i)
        {
            float x = std::clamp(interleaved[i], -1.0f, 1.0f);
            pcm[i] = qint16(x * 32767.0f);
        }
        f.write(reinterpret_cast<const char*>(pcm.data()), qint64(pcm.size() * 2));
        return true;
    }

    bool Engine::exportWav(const QString& path, bool song)
    {
        return writeWav(path, song ? renderSong() : renderPattern());
    }

    bool Engine::exportMidi(const QString& path, bool song)
    {
        const int ppq = 480;
        double bpm = std::clamp(double(proj->master.bpm), 40.0, 300.0);
        double stepSamples = SR * 60.0 / bpm / 4.0;
        std::vector<int> seq;
        if (song && !proj->song.empty())
        {
            for (const auto& en : proj->song)
                for (int k = 0; k < std::max(1, en.repeats); ++k)
                    seq.push_back(std::clamp(en.pattern, 0, PATTERNS - 1));
        }
        else
        {
            seq.push_back(proj->current);
        }
        struct N
        {
            int tick, note, vel;
        };
        std::vector<N> notes;
        long long cursor = 0;
        for (int pi : seq)
        {
            const Pattern& pat = proj->patterns[pi];
            for (int s = 0; s < pat.steps; ++s)
            {
                int swingTick = (s % 2) ? int(proj->master.swing * 60.0) : 0;
                for (int pad = 0; pad < PADS; ++pad)
                {
                    int tl = std::clamp(pat.trackLen[pad], 1, MAX_STEPS);
                    int idx = int((cursor + s) % tl);
                    const Step& st = pat.data[pad][idx];
                    if (!st.on || st.prob <= 0) continue;
                    int vel = std::clamp(int(st.velocity * (st.accent ? 1.3f : 1.0f)), 1, 127);
                    int baseTick = int((cursor + s) * 120);
                    int microTick = int(st.micro / stepSamples * 120.0);
                    int r = std::clamp(st.ratchet, 1, 8);
                    for (int i = 0; i < r; ++i)
                        notes.push_back({
                            std::max(0, baseTick + swingTick + microTick + int(i * 120.0 / r)),
                            kMidiNotes[pad], vel
                        });
                }
            }
            cursor += pat.steps;
        }
        std::stable_sort(notes.begin(), notes.end(), [](const N& a, const N& b)
        {
            return a.tick != b.tick ? a.tick < b.tick : a.note < b.note;
        });
        auto vlq = [](QByteArray& b, int v)
        {
            v = std::max(0, v);
            QByteArray tmp;
            tmp.append(char(v & 0x7F));
            v >>= 7;
            while (v > 0)
            {
                tmp.prepend(char((v & 0x7F) | 0x80));
                v >>= 7;
            }
            b.append(tmp);
        };
        QByteArray track;
        const QByteArray name = "XU-808 by XuMou";
        vlq(track, 0);
        track.append(char(0xFF)).append(char(0x03));
        vlq(track, name.size());
        track.append(name);
        int tempo = int(60000000.0 / bpm);
        vlq(track, 0);
        track.append(char(0xFF)).append(char(0x51)).append(char(0x03));
        track.append(char(tempo >> 16)).append(char(tempo >> 8)).append(char(tempo));
        vlq(track, 0);
        track.append(char(0xFF)).append(char(0x58)).append(char(0x04));
        track.append(char(proj->master.beatsPerBar));
        int du = 0, bu = std::max(1, proj->master.beatUnit);
        while (bu > 1)
        {
            bu >>= 1;
            ++du;
        }
        track.append(char(du)).append(char(24)).append(char(8));
        int last = 0;
        for (const N& n : notes)
        {
            vlq(track, n.tick - last);
            track.append(char(0x99)).append(char(n.note)).append(char(n.vel));
            vlq(track, 10);
            track.append(char(0x89)).append(char(n.note)).append(char(0x40));
            last = n.tick + 10;
        }
        vlq(track, 0);
        track.append(char(0xFF)).append(char(0x2F)).append(char(0x00));

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        auto w32 = [&](quint32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto w16 = [&](quint16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("MThd", 4);
        w32(6);
        w16(1);
        w16(1);
        w16(quint16(ppq));
        f.write("MTrk", 4);
        w32(quint32(track.size()));
        f.write(track);
        return true;
    }
} // namespace xu