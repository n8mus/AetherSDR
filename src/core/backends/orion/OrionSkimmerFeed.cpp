#include "core/backends/orion/OrionSkimmerFeed.h"

#include "core/backends/orion/skimmer/SkimmerEngine.h"

#include <algorithm>

namespace AetherSDR::orion {

OrionSkimmerFeed::OrionSkimmerFeed(skim::SkimmerEngine* engine)
    : OrionSkimmerFeed([engine](const std::complex<float>* d, std::size_t n) {
          engine->processIq(d, n);   // gated by its own enable
      }, true)
{
}

OrionSkimmerFeed::OrionSkimmerFeed(Consumer consumer, bool gateOnLayoutLo)
    : m_consumer(std::move(consumer))
    , m_gateOnLayoutLo(gateOnLayoutLo)
    , m_ring(kRingSamples)
{
}

OrionSkimmerFeed::~OrionSkimmerFeed()
{
    stop();
}

void OrionSkimmerFeed::start()
{
    stop();
    {
        std::lock_guard lock(m_lock);
        m_stop = false;
        m_write = 0;
        m_fill = 0;
    }
    m_worker = std::thread([this] { workerLoop(); });
}

void OrionSkimmerFeed::stop()
{
    {
        std::lock_guard lock(m_lock);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void OrionSkimmerFeed::push(const std::complex<float>* d, std::size_t n, qint64 loHz)
{
    // Blocks the channels were not laid out for, or taken while paused, would
    // only decode as junk: they never enter the ring.
    if (m_paused.load(std::memory_order_relaxed)
        || (m_gateOnLayoutLo && loHz != m_layoutLo.load(std::memory_order_relaxed))) {
        return;
    }
    {
        std::lock_guard lock(m_lock);
        if (m_stop) {
            return;
        }
        if (m_fill + n > m_ring.size()) {
            // The decoders fell behind: drop this block whole rather than
            // splice time inside a character.
            m_dropped.fetch_add(n, std::memory_order_relaxed);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            m_ring[m_write] = d[i];
            m_write = (m_write + 1) & (m_ring.size() - 1);
        }
        m_fill += n;
    }
    m_wake.notify_one();
}

void OrionSkimmerFeed::workerLoop()
{
    std::vector<std::complex<float>> block(kBlockSamples);
    while (true) {
        {
            std::unique_lock lock(m_lock);
            m_wake.wait(lock, [this] { return m_stop || m_fill >= kBlockSamples; });
            if (m_stop) {
                return;
            }
            std::size_t read = (m_write + m_ring.size() - m_fill) & (m_ring.size() - 1);
            for (std::size_t i = 0; i < kBlockSamples; ++i) {
                block[i] = m_ring[read];
                read = (read + 1) & (m_ring.size() - 1);
            }
            m_fill -= kBlockSamples;
        }
        m_consumer(block.data(), block.size());
        m_processed.fetch_add(kBlockSamples, std::memory_order_relaxed);
    }
}

}  // namespace AetherSDR::orion
