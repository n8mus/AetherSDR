#pragma once

// TX audio to the SignaLink (step 6): AetherSDR's processed transmit audio,
// 24 kHz int16 stereo off the seam, to the SignaLink's PipeWire node as
// 48 kHz mono.
//
// SAFETY. A SignaLink keys the radio by itself when audio reaches it (its
// own VOX-style PTT line). So nothing is ever written unless the gate is
// open, and the backend opens the gate only after IT keyed the radio by CAT,
// on the PC audio path. Closing the gate drops whatever is buffered at once.
//
// The device is found by its PipeWire node name, never "USB AUDIO CODEC"
// alone: an Alesis mixer's PCM2900 matched that too (live-found). Never
// plughw, which would lock every other program out of the codec.

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QAudioSink;
class QIODevice;

namespace AetherSDR::orion {

class OrionTxAudio : public QObject {
    Q_OBJECT

public:
    // The SignaLink's PipeWire node, as `pactl list short sinks` shows it.
    static constexpr const char* kSignaLinkNode = "BurrBrown_from_Texas_Instruments_USB_AUDIO_CODEC";
    // "none": a sink that only counts samples (tests; never makes a sound).
    static constexpr const char* kNullDevice = "none";
    static constexpr int kOutputRateHz = 48'000;
    static constexpr int kBufferMs = 200;

    explicit OrionTxAudio(QObject* parent = nullptr);
    ~OrionTxAudio() override;

    // deviceSpec: empty = the SignaLink; "none" = null sink; anything else is
    // matched against device ids and descriptions. False with a reason when
    // no such device exists.
    bool open(const QString& deviceSpec, QString* error);
    void close();
    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] QString deviceName() const { return m_deviceName; }

    // Audio passes only while the gate is open. Closing drops the buffer.
    void setGate(bool open);
    [[nodiscard]] bool gateOpen() const { return m_gate; }

    // One block from submitTxAudio(): int16 interleaved stereo. gain is
    // linear (the operator's mic level on the PC path). Dropped when the
    // gate is shut.
    void submit(const QByteArray& int16Stereo, int sampleRateHz, float gain);

    // Milliseconds of audio still queued for the codec.
    [[nodiscard]] int queuedMs() const;
    // Mono output samples written since open (tests).
    [[nodiscard]] quint64 samplesWritten() const { return m_written; }
    [[nodiscard]] quint64 samplesDropped() const { return m_dropped; }

private:
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice* m_io = nullptr;
    bool m_open = false;
    bool m_null = false;
    bool m_gate = false;
    QString m_deviceName;
    float m_last = 0.0f;   // previous mono sample, for 2x interpolation
    quint64 m_written = 0;
    quint64 m_dropped = 0;
};

}  // namespace AetherSDR::orion
