/*
 * audioengine.h —— Qt 5.14 QAudioOutput 回调模式封装
 * 音频回调只调用 c808_render()，不触碰任何 QObject / QWidget，不分配堆内存。
 */
#ifndef AUDIOENGINE_H
#define AUDIOENGINE_H

#include <QObject>
#include <QAudioOutput>
#include <QAudioFormat>
#include <QIODevice>
#include <QVector>

#include "core808.h"

class AudioIO : public QIODevice
{
public:
    AudioIO(c808_engine* engine, const QAudioFormat& fmt);
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char* data, qint64 len) override;
    qint64 bytesAvailable() const override;
    bool isSequential() const override { return true; }
    qint64 framesRendered() const { return m_frames; }

private:
    c808_engine* m_engine;
    QAudioFormat m_fmt;
    int m_channels;
    int m_bytesPerSample;
    QVector<float> m_l;
    QVector<float> m_r;
    qint64 m_frames = 0;
};

class AudioEngine : public QObject
{
    Q_OBJECT
public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    bool start(QString* error);
    c808_engine* engine() const { return m_engine; }
    QString info() const { return m_info; }
    QString stateText() const { return m_stateText; }
    QAudioFormat format() const { return m_fmt; }
    bool running() const { return m_running; }
    qint64 framesRendered() const { return m_io ? m_io->framesRendered() : 0; }

private slots:
    void onStateChanged(QAudio::State s);

private:
    c808_engine* m_engine = nullptr;
    QAudioOutput* m_out = nullptr;
    AudioIO* m_io = nullptr;
    QAudioFormat m_fmt;
    QString m_info;
    QString m_stateText;
    bool m_running = false;
};

#endif
