#pragma once

// RSP2 panadapter maths for the Orion backend: the offset-LO view rules and
// the FFT that turns IQ into a pan frame. No device and no threads here, so
// all of it is unit-testable.
//
// OFFSET LO. The RSP2 is zero-IF, so its LO carries a DC hump. The view is
// therefore always kept BELOW the LO, with a guard: the LO sits a guard
// above the view's top edge. With the default 480 kHz view centred on the
// dial that puts the LO exactly 260 kHz above the dial, the reference
// program's kLoOffsetHz, and the hump is never on screen.
//
//   capture   LO - 500 kHz ............ LO ............ LO + 500 kHz
//   view      [ centre - span/2 , centre + span/2 ]  then 20 kHz, then LO
//
// Frequencies here are absolute RF in Hz.

#include <QtGlobal>

#include <complex>
#include <memory>
#include <vector>

namespace AetherSDR::orion::spectrum {

inline constexpr qint64 kCaptureHz = 1'000'000;   // 2 MS/s, hardware decimation 2
inline constexpr qint64 kSampleRateHz = 2'000'000;
inline constexpr int kHardwareDecimation = 2;
inline constexpr qint64 kDcGuardHz = 20'000;
inline constexpr qint64 kMaxViewHz = kCaptureHz / 2 - kDcGuardHz;   // 480 kHz
inline constexpr qint64 kMinViewHz = 10'000;
inline constexpr qint64 kLoOffsetHz = kMaxViewHz / 2 + kDcGuardHz;  // 260 kHz
inline constexpr int kFftSize = 16384;                              // ~61 Hz bins
inline constexpr double kBinHz = static_cast<double>(kCaptureHz) / kFftSize;

struct View {
    qint64 centreHz = 0;
    qint64 spanHz = kMaxViewHz;
    bool operator==(const View&) const = default;
};

[[nodiscard]] inline View clampView(View v)
{
    v.spanHz = v.spanHz < kMinViewHz ? kMinViewHz : (v.spanHz > kMaxViewHz ? kMaxViewHz : v.spanHz);
    return v;
}
// The LO that keeps the whole view below the DC guard.
[[nodiscard]] inline qint64 loForView(const View& v)
{
    return v.centreHz + v.spanHz / 2 + kDcGuardHz;
}
// True when the view lies inside the usable, DC-free part of a capture at lo.
[[nodiscard]] inline bool viewFits(const View& v, qint64 loHz)
{
    const qint64 low = v.centreHz - v.spanHz / 2;
    const qint64 high = v.centreHz + v.spanHz / 2;
    return low >= loHz - kCaptureHz / 2 && high <= loHz - kDcGuardHz;
}

// 4-term Blackman-Harris: -92 dB sidelobes, enough for a hot shared feed.
[[nodiscard]] std::vector<float> blackmanHarris(int n);

// One pan frame from a full power spectrum (dB, DC-centred: bin n/2 is the
// LO). Each output point is the PEAK of the bins it covers, so a narrow
// carrier never disappears between pixels; bins narrower than a point are
// sampled at the nearest bin.
[[nodiscard]] std::vector<float> cropToView(const std::vector<float>& powerDb, qint64 loHz,
                                            const View& view, int points);

// IQ -> averaged power spectrum in dB relative to full scale. Owns the FFT
// plan; not thread-safe (one per worker).
class SpectrumProcessor {
public:
    explicit SpectrumProcessor(int fftSize = kFftSize);
    ~SpectrumProcessor();
    SpectrumProcessor(const SpectrumProcessor&) = delete;
    SpectrumProcessor& operator=(const SpectrumProcessor&) = delete;

    // average: 0 = none, approaching 1 = heavy (exponential, linear power).
    void setAverage(double average) { m_average = average; }
    void resetAverage() { m_haveAverage = false; }
    [[nodiscard]] int size() const { return m_size; }

    // iq must hold size() samples. Returns size() values, DC-centred.
    const std::vector<float>& process(const std::complex<float>* iq);

private:
    struct Fft;
    int m_size;
    std::vector<float> m_window;
    double m_windowGain = 1.0;
    std::unique_ptr<Fft> m_fft;
    std::vector<double> m_avg;
    std::vector<float> m_db;
    double m_average = 0.5;
    bool m_haveAverage = false;
};

}  // namespace AetherSDR::orion::spectrum
