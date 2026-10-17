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
    , m_avg(static_cast<std::size_t>(fftSize), 0.0)
    , m_db(static_cast<std::size_t>(fftSize), -200.0f)
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

const std::vector<float>& SpectrumProcessor::process(const std::complex<float>* iq)
{
    for (int i = 0; i < m_size; ++i) {
        const float w = m_window[static_cast<std::size_t>(i)];
        m_fft->in[i][0] = iq[i].real() * w;
        m_fft->in[i][1] = iq[i].imag() * w;
    }
    fftwf_execute(m_fft->plan);

    // A full-scale complex tone gives |X| = windowGain: that is 0 dBFS.
    const double norm = 1.0 / (m_windowGain * m_windowGain);
    const double keep = std::clamp(m_average, 0.0, 0.98);
    const int half = m_size / 2;
    for (int k = 0; k < m_size; ++k) {
        // fftshift: output bin k holds FFT bin (k + n/2) mod n, so the LO
        // lands on k = n/2.
        const int src = (k + half) % m_size;
        const double re = m_fft->out[src][0];
        const double im = m_fft->out[src][1];
        const double p = (re * re + im * im) * norm;
        double& a = m_avg[static_cast<std::size_t>(k)];
        a = m_haveAverage ? keep * a + (1.0 - keep) * p : p;
        m_db[static_cast<std::size_t>(k)] = static_cast<float>(10.0 * std::log10(std::max(a, 1e-20)));
    }
    m_haveAverage = true;
    return m_db;
}

}  // namespace AetherSDR::orion::spectrum
