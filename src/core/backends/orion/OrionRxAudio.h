#pragma once

// RSP2 receive audio for the Orion backend (step 3): one slice demodulated
// on this host by AetherSDR's own WDSP. Receive-only.
//
//   RSP2 stream callback  push(): copy the block into a FIFO (cheap)
//   audio thread          NCO mix the dial to 0 Hz -> polyphase resample
//                         1 MS/s -> 96 kHz (x12 / 125, complex)
//                         -> WDSP RXA (96 kHz in, 48 kHz DSP, 24 kHz out)
//                         -> DC block -> interleaved float stereo
//   owner thread          audioReady(), queued
//
// WDSP needs whole-number rate ratios, and 1 MS/s has none with 48 kHz, so
// one complex rational resampler does the anti-alias filter and the rate
// change together. I and Q share every tap, so they cannot slip against
// each other (a one-sample slip is a -30 dB image). The RSP2 rate itself is
// left at the on-air-verified 2 MS/s / decimation 2.
//
// CW: the Orion puts the carrier at the dial, so the receiver tunes the
// dial minus (CWL: plus) the CW pitch and demodulates USB (LSB) with the
// passband around the pitch.

#include "core/dsp/WdspChannel.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <complex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace AetherSDR::orion {

class OrionRxAudio : public QObject {
    Q_OBJECT

public:
    static constexpr int kInputRateHz = 1'000'000;
    static constexpr int kWdspInputRateHz = 96'000;
    static constexpr int kInterpolate = 12;    // 1 MHz * 12 / 125 = 96 kHz
    static constexpr int kDecimate = 125;
    static constexpr int kTapsPerPhase = 128;
    static constexpr int kWdspDspRateHz = 48'000;
    static constexpr int kAudioRateHz = 24'000;   // the seam's rate
    static constexpr int kWdspBlock = 1024;       // at 96 kHz
    static constexpr double kFilterCutoffHz = 40'000.0;
    // Half a second of IQ queued at most; older blocks are dropped so a
    // stall never turns into seconds of delay.
    static constexpr std::size_t kMaxQueuedSamples = kInputRateHz / 2;

    enum class Mode { Usb, Lsb, Cw, Cwl, Am, Fm };

    struct Tuning {
        qint64 dialHz = 0;
        Mode mode = Mode::Usb;
        int lowHz = 0;        // passband edges relative to the carrier,
        int highHz = 2400;    // as the slice shows them
        int cwPitchHz = 600;
        int agcMode = 3;      // WDSP: 0 off, 1 long, 2 slow, 3 med, 4 fast
        double agcMaxGainDb = 39.0;
        bool operator==(const Tuning&) const = default;
    };

    explicit OrionRxAudio(QObject* parent = nullptr);
    ~OrionRxAudio() override;

    // Starts the audio thread, which builds the WDSP channel first (that can
    // take seconds cold). Owner thread. A WDSP failure arrives as failed().
    bool start(const Tuning& tuning, QString* error);
    void stop();
    [[nodiscard]] bool isRunning() const { return m_worker.joinable(); }

    // Owner thread; applied by the audio thread between blocks.
    void setTuning(const Tuning& tuning);

    // RSP2 stream-callback thread: one block of 1 MS/s IQ taken at loHz.
    void push(const std::complex<float>* iq, std::size_t n, qint64 loHz);

    // What the mixer must shift by, for a tuning and an LO: the frequency
    // that ends up at 0 Hz is the dial, offset by the CW pitch in CW.
    [[nodiscard]] static qint64 centreHz(const Tuning& t);
    // The WDSP passband for the slice's edges (CW moves it to the pitch).
    [[nodiscard]] static std::pair<double, double> wdspPassband(const Tuning& t);
    [[nodiscard]] static WdspChannel::Mode wdspMode(Mode mode);

    // Blocks the audio thread dropped because the queue was full.
    [[nodiscard]] quint64 droppedBlocks() const { return m_dropped.load(); }
    // IQ samples waiting for the audio thread (any thread).
    [[nodiscard]] std::size_t queuedSamples();
    // True once the WDSP channel is built and audio can flow (any thread).
    [[nodiscard]] bool isReady() const { return m_ready.load(std::memory_order_acquire); }

signals:
    // Interleaved float L,R at 24 kHz, one WDSP output block.
    void audioReady(const QByteArray& stereoFloat);
    void failed(const QString& reason);

private:
    struct Block {
        std::vector<std::complex<float>> iq;
        qint64 loHz = 0;
    };
    void workerLoop();
    void processBlock(const Block& block);
    void applyTuningIfChanged();

    std::unique_ptr<WdspChannel> m_channel;   // built and used on the audio thread
    WdspChannel::Config m_config;
    std::atomic<bool> m_ready{false};

    std::mutex m_queueLock;
    std::condition_variable m_queueWake;
    std::deque<Block> m_queue;
    std::size_t m_queuedSamples = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<quint64> m_dropped{0};
    std::thread m_worker;

    std::mutex m_tuningLock;
    Tuning m_requested;
    Tuning m_applied;
    bool m_tuningDirty = false;

    // Audio-thread state.
    // Polyphase prototype, stored phase-major: m_phases[p * kTapsPerPhase + k]
    // is tap k of phase p, newest input first.
    std::vector<float> m_phases;
    std::vector<std::complex<float>> m_history;   // doubled delay line
    std::size_t m_historyPos = 0;
    int m_upPosition = 0;   // next output's position in the x12 stream
    double m_ncoPhase = 0.0;
    std::vector<float> m_wdspI;
    std::vector<float> m_wdspQ;
    std::vector<float> m_left;
    std::vector<float> m_right;
    float m_dcX[2] = {0.0f, 0.0f};
    float m_dcY[2] = {0.0f, 0.0f};
    float m_dcR = 0.995f;
    quint64 m_generation = 0;
};

}  // namespace AetherSDR::orion
