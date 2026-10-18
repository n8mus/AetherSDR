#pragma once

// The CW skimmer's IQ feed (step 8): the RSP2 stream callback copies each
// block in here and returns; a worker thread hands blocks to the skimmer's
// decoder bank, never on the SDRplay callback (the console did the same: a
// 24-channel bank costs more than a core at 1 MS/s).
//
// The decoders' channel offsets are relative to the LO the channels were laid
// out for. The LO follows the panadapter view, so blocks taken at any other
// LO are dropped until the skimmer has re-laid its channels for the new one
// (setLayoutLo, after the next full-capture spectrum).

#include <QtGlobal>

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace AetherSDR::orion {

namespace skim {
class SkimmerEngine;
}

class OrionSkimmerFeed {
public:
    // ~1 s of 1 MS/s IQ; blocks of 10 ms reach the decoders.
    static constexpr std::size_t kRingSamples = std::size_t{1} << 20;
    static constexpr std::size_t kBlockSamples = 10'000;

    explicit OrionSkimmerFeed(skim::SkimmerEngine* engine);
    ~OrionSkimmerFeed();
    OrionSkimmerFeed(const OrionSkimmerFeed&) = delete;
    OrionSkimmerFeed& operator=(const OrionSkimmerFeed&) = delete;

    void start();
    void stop();   // joins the worker

    // Stream thread: copy and return.
    void push(const std::complex<float>* d, std::size_t n, qint64 loHz);

    // Owner thread.
    void setLayoutLo(qint64 loHz) { m_layoutLo.store(loHz); }
    void setPaused(bool paused) { m_paused.store(paused); }   // e.g. while transmitting

    [[nodiscard]] quint64 processedSamples() const { return m_processed.load(); }
    [[nodiscard]] quint64 droppedSamples() const { return m_dropped.load(); }

private:
    void workerLoop();

    skim::SkimmerEngine* m_engine;
    std::mutex m_lock;
    std::condition_variable m_wake;
    std::vector<std::complex<float>> m_ring;
    std::size_t m_write = 0;
    std::size_t m_fill = 0;
    std::thread m_worker;
    bool m_stop = false;
    std::atomic<qint64> m_layoutLo{0};
    std::atomic<bool> m_paused{false};
    std::atomic<quint64> m_processed{0};
    std::atomic<quint64> m_dropped{0};
};

}  // namespace AetherSDR::orion
