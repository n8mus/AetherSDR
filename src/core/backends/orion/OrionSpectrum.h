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

#include "core/dsp/SpectrumTemporalAverage.h"

#include <QtGlobal>

#include <algorithm>
#include <complex>
#include <deque>
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
// plan; not thread-safe (one per worker). Averaging is AetherSDR's shared
// time-based estimator (as the RTL backend uses it): FFT AVG is a time
// constant of 10 ms per step, so it is the same at any frame rate, and a
// retune slides the history by whole bins on the RF grid.
class SpectrumProcessor {
public:
    explicit SpectrumProcessor(int fftSize = kFftSize);
    ~SpectrumProcessor();
    SpectrumProcessor(const SpectrumProcessor&) = delete;
    SpectrumProcessor& operator=(const SpectrumProcessor&) = delete;

    // The Display panel's FFT AVG (0 = off, 100 = one second) and Wt Avg
    // (off: average power; on: average dB, a smoother trace).
    void setAverage(int steps) { m_average = std::clamp(steps, 0, 100); }
    void setWeightedAverage(bool on) { m_weighted = on; }
    void resetAverage()
    {
        m_temporal.reset();
        m_wfTemporal.reset();
    }
    [[nodiscard]] int size() const { return m_size; }

    // iq must hold size() samples taken at loHz, elapsedSeconds after the
    // previous frame. Returns size() values in dB, DC-centred.
    const std::vector<float>& process(const std::complex<float>* iq, double elapsedSeconds,
                                      qint64 loHz)
    {
        return process(iq, 1, elapsedSeconds, loHz);
    }
    // Welch: iq holds blocks * size() consecutive samples, all at loHz; the
    // frame is their mean power, so every sample since the last frame counts
    // (the console's SpectrumComputer), then FFT AVG as above.
    const std::vector<float>& process(const std::complex<float>* iq, int blocks,
                                      double elapsedSeconds, qint64 loHz);
    // The waterfall's own copy of the frame, in dB: the raw frame, with the
    // waterfall's OWN time average applied (setWaterfallAverage), never the
    // trace's. 0 = the raw frame, so a dit stays a dit.
    [[nodiscard]] const std::vector<float>& waterfallDb() const { return m_wfDb; }
    // The waterfall's average, 0..100 on FFT AVG's scale (10 ms per step).
    void setWaterfallAverage(int percent) { m_wfAverage = std::clamp(percent, 0, 100); }
    [[nodiscard]] int waterfallAverage() const { return m_wfAverage; }

private:
    struct Fft;
    int m_size;
    std::vector<float> m_window;
    double m_windowGain = 1.0;
    std::unique_ptr<Fft> m_fft;
    std::vector<float> m_db;
    std::vector<float> m_rawDb;
    std::vector<float> m_wfDb;
    std::vector<double> m_power;
    int m_average = 0;
    int m_wfAverage = 0;
    bool m_weighted = false;
    AetherSDR::SpectrumTemporalAverage m_temporal;
    AetherSDR::SpectrumTemporalAverage m_wfTemporal;
};

// The waterfall's own frames (fork rule 2, the console's behaviour): the raw
// frames, before FFT AVG, so averaging smooths the trace without smearing
// the waterfall, and peak-held over the waterfall's row interval, so a CW
// dit that falls between two rows still lands on the waterfall. A frame of
// another size (view or width change) starts it over; so does reset().
class WaterfallHold {
public:
    static constexpr int kMaxFrames = 64;
    void setHoldMs(int ms) { m_holdMs = std::max(0, ms); }
    [[nodiscard]] int holdMs() const { return m_holdMs; }
    void reset() { m_frames.clear(); }
    // One raw frame taken at nowMs. Returns the peak of every frame younger
    // than the hold, this one always included.
    const std::vector<float>& push(const std::vector<float>& frame, qint64 nowMs);

private:
    struct Entry {
        qint64 ms = 0;
        std::vector<float> values;
    };
    std::deque<Entry> m_frames;
    std::vector<float> m_out;
    int m_holdMs = 0;
};

}  // namespace AetherSDR::orion::spectrum
