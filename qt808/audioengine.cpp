/*
 * audioengine.cpp —— QAudioOutput 拉模式（readData 在 Qt 音频线程执行）
 */
#include "audioengine.h"

#include <QAudioDeviceInfo>
#include <QDebug>
#include <cstring>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
#endif

AudioIO::AudioIO(c808_engine* engine, const QAudioFormat& fmt)
    : m_engine(engine), m_fmt(fmt)
{
    m_channels = fmt.channelCount();
    if (m_channels < 1) m_channels = 2;
    m_bytesPerSample = fmt.sampleSize() / 8;
    if (m_bytesPerSample <= 0) m_bytesPerSample = 4;
    m_l.resize(8192);
    m_r.resize(8192);
}

qint64 AudioIO::readData(char* data, qint64 maxlen)
{
    int bytesPerFrame = m_bytesPerSample * m_channels;
    int frames, i, c;
#ifdef _WIN32
    /* Qt 的音频回调线程默认普通优先级；首次回调时提升到 MMCSS Pro Audio，
       避免被 UI 重绘抢占。只执行一次，不进入稳态回调路径。 */
    static bool boosted = false;
    if (!boosted) {
        DWORD taskIndex = 0;
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        boosted = true;
    }
#endif
    if (bytesPerFrame <= 0) return 0;
    frames = (int)(maxlen / bytesPerFrame);
    if (frames <= 0) return 0;
    if (frames > m_l.size()) {
        /* 预分配上限内不应发生；为保证回调零分配，直接截断 */
        frames = m_l.size();
    }
    auto t0 = std::chrono::steady_clock::now();
    c808_render(m_engine, m_l.data(), m_r.data(), frames);
    auto t1 = std::chrono::steady_clock::now();
    m_frames += frames;
    {
        double sec = std::chrono::duration<double>(t1 - t0).count();
        double budget = (double)frames / (double)m_fmt.sampleRate();
        if (budget > 1e-9) {
            float load = float(sec / budget);
            static float ema = 0.0f;
            ema = ema * 0.85f + load * 0.15f;   /* 平滑显示，避免调度抖动夸大 */
            c808_report_cpu(m_engine, ema);
            if (load > 1.6f) c808_report_xrun(m_engine);
        }
    }

    if (m_fmt.sampleType() == QAudioFormat::Float && m_bytesPerSample == 4) {
        float* o = reinterpret_cast<float*>(data);
        for (i = 0; i < frames; ++i) {
            o[i * m_channels] = m_l[i];
            if (m_channels > 1) o[i * m_channels + 1] = m_r[i];
            for (c = 2; c < m_channels; ++c) o[i * m_channels + c] = 0.0f;
        }
    } else if (m_fmt.sampleType() == QAudioFormat::SignedInt && m_bytesPerSample == 2) {
        qint16* o = reinterpret_cast<qint16*>(data);
        for (i = 0; i < frames; ++i) {
            o[i * m_channels] = (qint16)(m_l[i] * 32767.0f);
            if (m_channels > 1) o[i * m_channels + 1] = (qint16)(m_r[i] * 32767.0f);
            for (c = 2; c < m_channels; ++c) o[i * m_channels + c] = 0;
        }
    } else if (m_fmt.sampleType() == QAudioFormat::SignedInt && m_bytesPerSample == 4) {
        qint32* o = reinterpret_cast<qint32*>(data);
        for (i = 0; i < frames; ++i) {
            o[i * m_channels] = (qint32)(m_l[i] * 2147483647.0f);
            if (m_channels > 1) o[i * m_channels + 1] = (qint32)(m_r[i] * 2147483647.0f);
            for (c = 2; c < m_channels; ++c) o[i * m_channels + c] = 0;
        }
    } else if (m_bytesPerSample == 3) {
        unsigned char* o = reinterpret_cast<unsigned char*>(data);
        for (i = 0; i < frames; ++i) {
            int v0 = (int)(m_l[i] * 8388607.0f);
            int v1 = (int)(m_r[i] * 8388607.0f);
            int base = i * m_channels * 3;
            int k;
            for (k = 0; k < 3; ++k) {
                o[base + k] = (unsigned char)((v0 >> (8 * k)) & 0xff);
                if (m_channels > 1) o[base + 3 + k] = (unsigned char)((v1 >> (8 * k)) & 0xff);
            }
            for (c = 2; c < m_channels; ++c) {
                o[base + c * 3] = 0;
                o[base + c * 3 + 1] = 0;
                o[base + c * 3 + 2] = 0;
            }
        }
    } else {
        memset(data, 0, (size_t)(frames * bytesPerFrame));
    }
    return (qint64)frames * bytesPerFrame;
}

qint64 AudioIO::writeData(const char*, qint64)
{
    return -1;
}

qint64 AudioIO::bytesAvailable() const
{
    return 4096 + QIODevice::bytesAvailable();
}

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
{
}

AudioEngine::~AudioEngine()
{
    if (m_out) {
        m_out->stop();
        delete m_out;
        m_out = nullptr;
    }
    delete m_io;
    m_io = nullptr;
    if (m_engine) {
        c808_destroy(m_engine);
        m_engine = nullptr;
    }
}

bool AudioEngine::start(QString* error)
{
    QAudioDeviceInfo dev = QAudioDeviceInfo::defaultOutputDevice();

    m_fmt = QAudioFormat();
    m_fmt.setSampleRate(48000);
    m_fmt.setChannelCount(2);
    m_fmt.setCodec(QStringLiteral("audio/pcm"));
    m_fmt.setSampleType(QAudioFormat::Float);
    m_fmt.setByteOrder(QAudioFormat::LittleEndian);

    if (!dev.isNull() && !dev.isFormatSupported(m_fmt)) {
        m_fmt.setSampleType(QAudioFormat::SignedInt);
        m_fmt.setSampleSize(16);
        if (!dev.isFormatSupported(m_fmt)) {
            m_fmt = dev.nearestFormat(m_fmt);
        }
    }
    if (m_fmt.sampleRate() <= 0) m_fmt.setSampleRate(48000);
    if (m_fmt.channelCount() <= 0) m_fmt.setChannelCount(2);

    /* 即使没有可用设备也创建引擎，保证 UI 编辑/离线功能可用 */
    m_engine = c808_create(m_fmt.sampleRate());
    if (!m_engine) {
        if (error) *error = QStringLiteral("无法创建 DSP 引擎");
        return false;
    }
    c808_midi_start(m_engine, 0);

    if (dev.isNull()) {
        if (error) *error = QStringLiteral("未找到默认音频输出设备");
        return false;
    }

    m_io = new AudioIO(m_engine, m_fmt);
    if (!m_io->open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开音频数据通道");
        return false;
    }
    m_out = new QAudioOutput(dev, m_fmt, this);
    connect(m_out, &QAudioOutput::stateChanged, this, &AudioEngine::onStateChanged);
    int bytesPerFrame = (m_fmt.sampleSize() / 8) * m_fmt.channelCount();
    if (bytesPerFrame <= 0) bytesPerFrame = 4;
    m_out->setBufferSize(qMax(4096, bytesPerFrame * int(m_fmt.sampleRate() * 0.02)));
    m_out->setVolume(1.0);
    m_out->start(m_io);
    m_stateText = QStringLiteral("state=%1 error=%2")
                  .arg(int(m_out->state())).arg(int(m_out->error()));
    if (m_out->state() != QAudio::ActiveState) {
        if (error) *error = QStringLiteral("QAudioOutput 未进入活动状态 (%1)").arg(m_stateText);
        return false;
    }

    QString typeName = m_fmt.sampleType() == QAudioFormat::Float ? QStringLiteral("Float32") :
                       (m_fmt.sampleType() == QAudioFormat::SignedInt && m_fmt.sampleSize() == 16)
                           ? QStringLiteral("Int16") :
                       QStringLiteral("PCM");
    m_info = QStringLiteral("QAudioSink %1 Hz · %2 ch · %3 · buffer %4 ms")
             .arg(m_fmt.sampleRate())
             .arg(m_fmt.channelCount())
             .arg(typeName)
             .arg(m_out->bufferSize() * 1000 / (bytesPerFrame * m_fmt.sampleRate()));
    m_running = true;
    return true;
}

void AudioEngine::onStateChanged(QAudio::State s)
{
    if (!m_out) return;
    m_stateText = QStringLiteral("state=%1 error=%2")
                  .arg(int(s)).arg(int(m_out->error()));
    if (s == QAudio::ActiveState) m_running = true;
    else if (s == QAudio::StoppedState) m_running = false;
    else if (s == QAudio::IdleState) c808_report_xrun(m_engine);
}
