#pragma once

// The SDRplay RSP2 as the Orion backend's panadapter receiver (step 2:
// spectrum only, receive-only).
//
// Three threads, one direction:
//   SDRplay stream callback  -> copies IQ into a ring (never blocks long)
//   spectrum worker          -> FFT of the newest samples at the pan's rate
//   owner thread             <- frameReady(), queued; nothing else crosses
// The owner (OrionBackend) never sees the worker (IRadioBackend rule 3).
//
// Device facts from the reference program (on-air verified on this RSP2):
// 2 MS/s with hardware decimation 2 -> 1 MS/s, zero IF, 1.536 MHz analog
// bandwidth, AGC off, DC and IQ correction on. Gain defaults gRdB 40 /
// LNA 6 because the shared feed is hot. ADC overload events are
// acknowledged in the event callback, as the API requires.

#include "core/backends/orion/OrionSpectrum.h"
#include "core/backends/orion/SdrPlayApi.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace AetherSDR::orion {

class OrionRxAudio;

class Rsp2Receiver : public QObject {
    Q_OBJECT

public:
    struct Settings {
        int gainReductionDb = 40;   // gRdB, 20..59
        int lnaState = 6;           // 0..8
        bool antennaB = false;      // A = the Orion's feed, B = the Omni's
    };

    // RSP2 LNA-state gain reduction, 0..420 MHz (SDRplay API specification).
    static constexpr int kLnaReductionDb[9] = {0, 10, 15, 21, 24, 34, 39, 45, 64};
    // dBFS + gain reductions + this = the level the pan shows. Uncalibrated
    // (the scale is ours, not the radio's): chosen so a quiet HF band sits
    // near -120. A calibration control is a later step.
    static constexpr double kLevelOffsetDb = -100.0;

    explicit Rsp2Receiver(QObject* parent = nullptr);
    ~Rsp2Receiver() override;

    // Opens the API, selects the first RSP2 and starts streaming with the LO
    // at loHz. On failure returns false with an operator-readable reason.
    bool start(qint64 loHz, const Settings& settings, QString* error);
    void stop();
    [[nodiscard]] bool isRunning() const { return m_running; }

    // The receive-audio chains (one per slice) that also get every IQ block,
    // with the LO it was taken at. Set before start() and left alone until
    // after stop(): the stream callback reads them without a lock.
    void setAudioSinks(OrionRxAudio* main, OrionRxAudio* sub)
    {
        m_audioSinks = {main, sub};
    }

    // While the Orion transmits: hardware gain to minimum (gRdB 59, LNA 8)
    // so the RSP2's front end is not driven into overload by our own signal;
    // off restores the operator's gain. Owner thread.
    void setTxGuard(bool on);

    void setLo(qint64 loHz);
    [[nodiscard]] qint64 lo() const { return m_requestedLo; }
    void setView(const spectrum::View& view, int points);
    void setFrameRate(int fps);
    void setAverage(int percent);

    // Counters for health and the delivery-rate check (any thread).
    [[nodiscard]] quint64 samplesReceived() const { return m_samples.load(); }
    [[nodiscard]] unsigned overloads() const { return m_overloads.load(); }
    [[nodiscard]] double levelOffsetDb() const;

signals:
    // One pan frame: float dB per point across the current view.
    void frameReady(const QByteArray& frame);
    void deviceLost(const QString& reason);

private:
    static void streamCallback(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                               unsigned int numSamples, unsigned int reset, void* context);
    static void eventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                              sdrplay_api_EventParamsT* params, void* context);
    void onStream(const short* xi, const short* xq, unsigned int n, bool rfChanged);
    void workerLoop();

    const SdrPlayApi* m_api = nullptr;
    bool m_apiOpen = false;
    bool m_running = false;
    sdrplay_api_DeviceT m_device{};
    sdrplay_api_DeviceParamsT* m_params = nullptr;
    Settings m_settings;
    qint64 m_requestedLo = 0;
    bool m_txGuard = false;

    // Ring of the newest IQ, written by the stream callback.
    std::mutex m_ringLock;
    std::condition_variable m_ringWake;
    std::vector<std::complex<float>> m_ring;
    std::size_t m_writeIndex = 0;
    quint64 m_samplesSinceRetune = 0;
    bool m_resetAverage = false;
    std::atomic<qint64> m_pendingLo{0};
    std::atomic<qint64> m_activeLo{0};
    std::array<OrionRxAudio*, 2> m_audioSinks{nullptr, nullptr};
    std::vector<std::complex<float>> m_scratch;   // stream-callback thread only

    // What the worker draws (owner writes, worker reads).
    std::mutex m_viewLock;
    spectrum::View m_view;
    int m_points = 1024;
    int m_frameIntervalMs = 40;
    double m_average = 0.5;

    std::thread m_worker;
    std::atomic<bool> m_stopWorker{false};
    std::atomic<quint64> m_samples{0};
    std::atomic<unsigned> m_overloads{0};
    quint64 m_generation = 0;
};

}  // namespace AetherSDR::orion
