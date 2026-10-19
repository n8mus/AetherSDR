#include "core/backends/orion/OrionSpectrum.h"

#include "core/dsp/FftwPlannerLock.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>

namespace AetherSDR::orion::spectrum {

std::vector<float> blackmanHarris(int n)
{
    std::vector<float> w(static_cast<std::size_t>(n));
    constexpr double a0 = 0.35875;
    constexpr double a1 = 0.48829;
    constexpr double a2 = 0.14128;
    constexpr double a3 = 0.01168;
    const double k = 2.0 * std::numbers::pi / (n - 1);
    for (int i = 0; i < n; ++i) {
        w[static_cast<std::size_t>(i)] = static_cast<float>(
            a0 - a1 * std::cos(k * i) + a2 * std::cos(2 * k * i) - a3 * std::cos(3 * k * i));
    }
    return w;
}

std::vector<float> cropToView(const std::vector<float>& powerDb, qint64 loHz,
                              const View& view, int points)
{
    std::vector<float> out;
    const int n = static_cast<int>(powerDb.size());
    if (n == 0 || points <= 0) {
        return out;
    }
    out.resize(static_cast<std::size_t>(points));
    const double binHz = static_cast<double>(kCaptureHz) / n;
    const double left = static_cast<double>(view.centreHz - view.spanHz / 2);
    const double step = static_cast<double>(view.spanHz) / points;
    // Bin k sits at lo + (k - n/2) * binHz.
    const auto binAt = [&](double hz) { return (hz - static_cast<double>(loHz)) / binHz + n / 2.0; };
    for (int i = 0; i < points; ++i) {
        const double f0 = left + i * step;
        int k0 = static_cast<int>(std::floor(binAt(f0)));
        int k1 = static_cast<int>(std::ceil(binAt(f0 + step))) - 1;
        k0 = std::clamp(k0, 0, n - 1);
        k1 = std::clamp(k1, k0, n - 1);
        float peak = powerDb[static_cast<std::size_t>(k0)];
        for (int k = k0 + 1; k <= k1; ++k) {
            peak = std::max(peak, powerDb[static_cast<std::size_t>(k)]);
        }
        out[static_cast<std::size_t>(i)] = peak;
    }
    return out;
}

struct SpectrumProcessor::Fft {
    fftwf_complex* in = nullptr;
    fftwf_complex* out = nullptr;
    fftwf_plan plan = nullptr;
};

SpectrumProcessor::SpectrumProcessor(int fftSize)
    : m_size(fftSize)
    , m_window(blackmanHarris(fftSize))
    , m_fft(std::make_unique<Fft>())
    , m_db(static_cast<std::size_t>(fftSize), -200.0f)
    , m_temporal(static_cast<std::size_t>(fftSize))
    , m_wfTemporal(static_cast<std::size_t>(fftSize))
{
    double sum = 0.0;
    for (float w : m_window) {
        sum += w;
    }
    m_windowGain = sum;
    const auto lock = AetherSDR::fftwfPlannerLock();
    m_fft->in = fftwf_alloc_complex(static_cast<std::size_t>(fftSize));
    m_fft->out = fftwf_alloc_complex(static_cast<std::size_t>(fftSize));
    m_fft->plan = fftwf_plan_dft_1d(fftSize, m_fft->in, m_fft->out, FFTW_FORWARD, FFTW_ESTIMATE);
}

SpectrumProcessor::~SpectrumProcessor()
{
    const auto lock = AetherSDR::fftwfPlannerLock();
    if (m_fft->plan) {
        fftwf_destroy_plan(m_fft->plan);
    }
    fftwf_free(m_fft->in);
    fftwf_free(m_fft->out);
}

const std::vector<float>& SpectrumProcessor::process(const std::complex<float>* iq, int blocks,
                                                     double elapsedSeconds, qint64 loHz)
{
    blocks = std::max(1, blocks);
    m_power.assign(static_cast<std::size_t>(m_size), 0.0);
    // A full-scale complex tone gives |X| = windowGain: that is 0 dBFS.
    const double norm = 1.0 / (m_windowGain * m_windowGain * blocks);
    const int half = m_size / 2;
    for (int b = 0; b < blocks; ++b) {
        const std::complex<float>* block = iq + static_cast<std::ptrdiff_t>(b) * m_size;
        for (int i = 0; i < m_size; ++i) {
            const float w = m_window[static_cast<std::size_t>(i)];
            m_fft->in[i][0] = block[i].real() * w;
            m_fft->in[i][1] = block[i].imag() * w;
        }
        fftwf_execute(m_fft->plan);
        for (int k = 0; k < m_size; ++k) {
            // fftshift: output bin k holds FFT bin (k + n/2) mod n, so the LO
            // lands on k = n/2.
            const int src = (k + half) % m_size;
            const double re = m_fft->out[src][0];
            const double im = m_fft->out[src][1];
            m_power[static_cast<std::size_t>(k)] += (re * re + im * im) * norm;
        }
    }

    m_rawDb.resize(static_cast<std::size_t>(m_size));
    // Power in, when power is what gets averaged; dB otherwise (no average,
    // or Wt Avg). The estimator turns power into dB on the way out.
    const bool power = m_average > 0 && !m_weighted;
    for (int k = 0; k < m_size; ++k) {
        const double p = m_power[static_cast<std::size_t>(k)];
        const float db = static_cast<float>(10.0 * std::log10(std::max(p, 1e-20)));
        m_rawDb[static_cast<std::size_t>(k)] = db;
        m_db[static_cast<std::size_t>(k)] = power ? static_cast<float>(p) : db;
    }
    const double stepHz = static_cast<double>(kCaptureHz) / m_size;
    const double originHz = static_cast<double>(loHz) - static_cast<double>(kCaptureHz) / 2.0;
    // The waterfall's own copy, with its own average (WF AVG): the raw frame
    // when it is off, so the trace can be smoothed without smearing the
    // waterfall and the waterfall can be smoothed without lagging the trace.
    m_wfDb.resize(static_cast<std::size_t>(m_size));
    if (m_wfAverage > 0) {
        const bool wfPower = !m_weighted;
        for (int k = 0; k < m_size; ++k) {
            const double p = m_power[static_cast<std::size_t>(k)];
            m_wfDb[static_cast<std::size_t>(k)] =
                wfPower ? static_cast<float>(p) : m_rawDb[static_cast<std::size_t>(k)];
        }
        (void)m_wfTemporal.processFrame(m_wfDb, m_wfAverage, m_weighted, elapsedSeconds, originHz,
                                        stepHz, 0, m_wfDb.size());
    } else {
        m_wfTemporal.reset();
        m_wfDb = m_rawDb;
    }
    (void)m_temporal.processFrame(m_db, m_average, m_weighted, elapsedSeconds, originHz, stepHz,
                                  0, m_db.size());
    return m_db;
}

const std::vector<float>& WaterfallHold::push(const std::vector<float>& frame, qint64 nowMs)
{
    if (!m_frames.empty() && m_frames.back().values.size() != frame.size()) {
        m_frames.clear();
    }
    m_frames.push_back({nowMs, frame});
    while (m_frames.size() > 1
           && (m_frames.front().ms <= nowMs - m_holdMs
               || m_frames.size() > static_cast<std::size_t>(kMaxFrames))) {
        m_frames.pop_front();
    }
    m_out = m_frames.front().values;
    for (std::size_t f = 1; f < m_frames.size(); ++f) {
        const std::vector<float>& v = m_frames[f].values;
        for (std::size_t i = 0; i < m_out.size(); ++i) {
            m_out[i] = std::max(m_out[i], v[i]);
        }
    }
    return m_out;
}

}  // namespace AetherSDR::orion::spectrum
