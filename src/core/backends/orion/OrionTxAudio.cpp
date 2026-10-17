#include "core/backends/orion/OrionTxAudio.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <vector>

namespace AetherSDR::orion {

OrionTxAudio::OrionTxAudio(QObject* parent)
    : QObject(parent)
{
}

OrionTxAudio::~OrionTxAudio()
{
    close();
}

bool OrionTxAudio::open(const QString& deviceSpec, QString* error)
{
    close();
    m_written = 0;
    m_dropped = 0;
    m_last = 0.0f;
    if (deviceSpec == QLatin1String(kNullDevice)) {
        m_null = true;
        m_open = true;
        m_deviceName = QStringLiteral("(null sink)");
        return true;
    }

    const QString wanted = deviceSpec.isEmpty() ? QString::fromLatin1(kSignaLinkNode) : deviceSpec;
    QAudioDevice found;
    for (const QAudioDevice& d : QMediaDevices::audioOutputs()) {
        const QString id = QString::fromUtf8(d.id());
        // The node name lives in the id on the PipeWire/Pulse backend.
        if (id.contains(wanted, Qt::CaseInsensitive)
            || (!deviceSpec.isEmpty() && d.description().contains(wanted, Qt::CaseInsensitive))) {
            found = d;
            break;
        }
    }
    if (found.isNull()) {
        if (error) {
            *error = tr("no audio output matches \"%1\" (is the SignaLink plugged in?)").arg(wanted);
        }
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(kOutputRateHz);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!found.isFormatSupported(format)) {
        format.setChannelCount(2);   // the codec's native layout; mono is duplicated
    }
    m_sink = std::make_unique<QAudioSink>(found, format);
    m_sink->setBufferSize(format.bytesForDuration(kBufferMs * 1000));
    m_io = m_sink->start();
    if (!m_io) {
        if (error) {
            *error = tr("cannot open audio output %1").arg(found.description());
        }
        m_sink.reset();
        return false;
    }
    m_deviceName = found.description();
    m_open = true;
    return true;
}

void OrionTxAudio::close()
{
    m_gate = false;
    if (m_sink) {
        m_sink->reset();   // drop, never drain into a radio that may be keyed
        m_sink->stop();
        m_sink.reset();
    }
    m_io = nullptr;
    m_open = false;
    m_null = false;
}

void OrionTxAudio::setGate(bool open)
{
    if (m_gate == open) {
        return;
    }
    m_gate = open;
    if (!open && m_sink && m_io) {
        // Whatever was queued belongs to the transmission that ended.
        m_sink->reset();
        m_io = m_sink->start();
    }
    m_last = 0.0f;
}

void OrionTxAudio::submit(const QByteArray& int16Stereo, int sampleRateHz, float gain)
{
    const qsizetype frames = int16Stereo.size() / static_cast<qsizetype>(2 * sizeof(qint16));
    if (!m_open || frames <= 0) {
        return;
    }
    if (!m_gate) {
        m_dropped += static_cast<quint64>(frames);
        return;
    }
    // 24 kHz -> 48 kHz: one interpolated sample between each pair. Voice
    // never reaches the 12-24 kHz images, and the radio's TX filter
    // (900-3900 Hz) removes them in any case.
    const int factor = sampleRateHz > 0 ? std::max(1, kOutputRateHz / sampleRateHz) : 2;
    const auto* in = reinterpret_cast<const qint16*>(int16Stereo.constData());
    const int channels = m_sink ? m_sink->format().channelCount() : 1;
    std::vector<qint16> out;
    out.reserve(static_cast<std::size_t>(frames * factor * channels));
    const auto clip = [](float x) {
        return static_cast<qint16>(std::lround(std::clamp(x, -32768.0f, 32767.0f)));
    };
    for (qsizetype f = 0; f < frames; ++f) {
        const float mono = 0.5f * (static_cast<float>(in[2 * f]) + static_cast<float>(in[2 * f + 1])) * gain;
        for (int k = 1; k <= factor; ++k) {
            const float t = static_cast<float>(k) / factor;
            const qint16 s = clip(m_last + (mono - m_last) * t);
            for (int c = 0; c < channels; ++c) {
                out.push_back(s);
            }
        }
        m_last = mono;
    }
    const quint64 monoSamples = static_cast<quint64>(frames * factor);
    if (m_null) {
        m_written += monoSamples;
        return;
    }
    if (!m_io) {
        m_dropped += static_cast<quint64>(frames);
        return;
    }
    const qint64 bytes = static_cast<qint64>(out.size() * sizeof(qint16));
    const qint64 wrote = m_io->write(reinterpret_cast<const char*>(out.data()), bytes);
    if (wrote > 0) {
        m_written += static_cast<quint64>(wrote) / sizeof(qint16) / static_cast<quint64>(channels);
    }
    if (wrote < bytes) {
        m_dropped += static_cast<quint64>(bytes - std::max<qint64>(wrote, 0))
                     / sizeof(qint16) / static_cast<quint64>(channels);
    }
}

int OrionTxAudio::queuedMs() const
{
    if (!m_sink || !m_open) {
        return 0;
    }
    const qsizetype queued = m_sink->bufferSize() - m_sink->bytesFree();
    if (queued <= 0) {
        return 0;
    }
    return static_cast<int>(m_sink->format().durationForBytes(static_cast<qint32>(queued)) / 1000);
}

}  // namespace AetherSDR::orion
