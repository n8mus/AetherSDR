// N8EM fork: RSP2 panadapter maths for the Orion backend.
//
// No device: synthetic IQ through the real FFT path. Pins the offset-LO
// view rules (the 260 kHz offset falls out of the 480 kHz view), the
// window's 0 dBFS reference, and that a carrier lands on the right pixel.

#include "core/backends/orion/OrionSpectrum.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace AetherSDR::orion::spectrum;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static std::vector<std::complex<float>> tone(double offsetHz, double amplitude, int n)
{
    std::vector<std::complex<float>> iq(static_cast<std::size_t>(n));
    const double w = 2.0 * std::numbers::pi * offsetHz / static_cast<double>(kCaptureHz);
    for (int i = 0; i < n; ++i) {
        iq[static_cast<std::size_t>(i)] = {static_cast<float>(amplitude * std::cos(w * i)),
                                           static_cast<float>(amplitude * std::sin(w * i))};
    }
    return iq;
}

static void testViewRules()
{
    const View v{14'025'000, kMaxViewHz};
    check(kMaxViewHz == 480'000, "max view is 480 kHz (1 MHz capture, 20 kHz guard)");
    check(loForView(v) == 14'025'000 + 260'000, "a full view centred on the dial puts the LO 260 kHz up");
    check(kLoOffsetHz == 260'000, "the reference program's kLoOffsetHz falls out of the rules");
    check(viewFits(v, loForView(v)), "the view fits the LO it asked for");
    check(!viewFits({14'100'000, kMaxViewHz}, loForView(v)), "a view moved up needs a retune");
    check(viewFits({13'990'000, 200'000}, loForView(v)), "a narrower view lower down still fits");
    check(clampView({0, 2'000'000}).spanHz == kMaxViewHz, "span clamps to 480 kHz");
    check(clampView({0, 10}).spanHz == kMinViewHz, "span floor");
}

static void testWindowAndPeak()
{
    SpectrumProcessor p;
    p.setAverage(0);
    // A full-scale complex tone 100 kHz below the LO.
    const auto iq = tone(-100'000.0, 1.0, p.size());
    const std::vector<float>& db = p.process(iq.data(), 0.04, 14'285'000);
    int peakBin = 0;
    for (int k = 1; k < p.size(); ++k) {
        if (db[static_cast<std::size_t>(k)] > db[static_cast<std::size_t>(peakBin)]) {
            peakBin = k;
        }
    }
    const int expected = p.size() / 2 + static_cast<int>(std::lround(-100'000.0 / kBinHz));
    check(std::abs(peakBin - expected) <= 1, "tone 100 kHz below the LO lands on its bin");
    check(std::fabs(db[static_cast<std::size_t>(peakBin)]) < 1.0, "full-scale tone reads ~0 dBFS");
    check(db[static_cast<std::size_t>(p.size() / 2 + 3000)] < -90.0f, "far bins are down >90 dB");

    // Into a pan: LO for a view centred 100 kHz below it... the tone is the
    // view's centre, so it must be the peak point in the middle of the frame.
    const qint64 lo = 14'285'000;
    const View view{lo - 100'000, 160'000};
    const std::vector<float> frame = cropToView(db, lo, view, 800);
    int peakPoint = 0;
    for (int i = 1; i < static_cast<int>(frame.size()); ++i) {
        if (frame[static_cast<std::size_t>(i)] > frame[static_cast<std::size_t>(peakPoint)]) {
            peakPoint = i;
        }
    }
    check(std::abs(peakPoint - 400) <= 1, "the carrier is drawn at the view's centre pixel");
    check(std::fabs(frame[static_cast<std::size_t>(peakPoint)]) < 1.0,
          "peak-per-pixel keeps the carrier's level");
}

static void testAveraging()
{
    const qint64 lo = 14'285'000;
    const auto loud = tone(-50'000.0, 1.0, kFftSize);
    const auto quiet = tone(-50'000.0, 0.01, kFftSize);   // 40 dB down
    const auto bin = static_cast<std::size_t>(kFftSize / 2 + std::lround(-50'000.0 / kBinHz));

    {
        SpectrumProcessor p;
        p.setAverage(0);
        (void)p.process(loud.data(), 0.04, lo);
        const float off = p.process(quiet.data(), 0.04, lo)[bin];
        check(std::fabs(off - (-40.0f)) < 1.0f, "FFT AVG 0: no averaging, the new level at once");
    }
    {
        SpectrumProcessor p;
        p.setAverage(100);   // one second
        (void)p.process(loud.data(), 0.04, lo);
        const float held = p.process(quiet.data(), 0.04, lo)[bin];
        check(held > -5.0f, "FFT AVG 100: 40 ms into a 1 s average the old level still holds");
        p.resetAverage();
        const float reset = p.process(quiet.data(), 0.04, lo)[bin];
        check(std::fabs(reset - (-40.0f)) < 1.0f, "after a reset the new level shows at once");
    }

    // The average is a TIME constant: 0.5 s of quiet at 20 fps and at 4 fps
    // land on the same level.
    const auto afterHalfSecond = [&](double frameSeconds) {
        SpectrumProcessor p;
        p.setAverage(50);   // 0.5 s
        (void)p.process(loud.data(), frameSeconds, lo);
        float v = 0.0f;
        const int frames = static_cast<int>(std::lround(0.5 / frameSeconds));
        for (int i = 0; i < frames; ++i) {
            v = p.process(quiet.data(), frameSeconds, lo)[bin];
        }
        return v;
    };
    const float fast = afterHalfSecond(0.05);
    const float slow = afterHalfSecond(0.25);
    check(std::fabs(fast - slow) < 0.5f, "the same decay at 20 fps and at 4 fps");
    // One time constant of power averaging: loud * e^-1 dominates, about -4.3 dB.
    check(fast < -2.0f && fast > -7.0f, "one time constant later the power has fallen by ~1/e");

    // Wt Avg averages dB, so after one time constant it sits near the
    // midpoint in dB (about -25 dB), well below the power average.
    {
        SpectrumProcessor p;
        p.setAverage(50);
        p.setWeightedAverage(true);
        (void)p.process(loud.data(), 0.04, lo);
        float v = 0.0f;
        for (int i = 0; i < 13; ++i) {
            v = p.process(quiet.data(), 0.04, lo)[bin];
        }
        check(v < fast - 10.0f, "Wt Avg averages dB: a fading signal reads lower than in power");
    }

    // A retune slides the history on the RF grid: a carrier at a fixed RF
    // frequency keeps its averaged level when the LO moves 10 kHz.
    {
        SpectrumProcessor p;
        p.setAverage(100);
        (void)p.process(loud.data(), 0.04, lo);
        const double shiftHz = 10.0 * kBinHz * 16;   // a whole number of bins
        const auto moved = tone(-50'000.0 - shiftHz, 0.01, kFftSize);   // same RF, now quiet
        const auto movedBin = static_cast<std::size_t>(
            kFftSize / 2 + std::lround((-50'000.0 - shiftHz) / kBinHz));
        const float v = p.process(moved.data(), 0.04, lo + static_cast<qint64>(shiftHz))[movedBin];
        check(v > -5.0f, "after a retune the same RF bin keeps its history");
    }
}

// The waterfall gets its own copy of every frame, with its own average
// (Display -> WF AVG): FFT AVG must not reach it, and WF AVG must not
// reach the trace. This is what let the operator tame one without
// smearing the other.
static void testWaterfallIsSeparate()
{
    const qint64 lo = 14'285'000;
    const auto loud = tone(-50'000.0, 0.5, kFftSize);
    const auto quiet = tone(-50'000.0, 0.0005, kFftSize);
    const auto bin = static_cast<std::size_t>(kFftSize / 2 + std::lround(-50'000.0 / kBinHz));

    // FFT AVG alone: the trace lags a fade, the waterfall's copy does not.
    {
        SpectrumProcessor p;
        p.setAverage(100);            // one second on the trace
        p.setWaterfallAverage(0);     // none on the waterfall
        (void)p.process(loud.data(), 0.04, lo);
        const std::vector<float> trace = p.process(quiet.data(), 0.04, lo);
        const float wf = p.waterfallDb()[bin];
        check(trace[bin] > wf + 10.0f,
              "FFT AVG holds the trace up while the waterfall's copy drops at once");
    }

    // WF AVG alone: the waterfall lags, the trace does not.
    {
        SpectrumProcessor p;
        p.setAverage(0);
        p.setWaterfallAverage(100);
        (void)p.process(loud.data(), 0.04, lo);
        const std::vector<float> trace = p.process(quiet.data(), 0.04, lo);
        const float wf = p.waterfallDb()[bin];
        check(wf > trace[bin] + 10.0f,
              "WF AVG holds the waterfall up while the trace drops at once");
    }

    // Off: the waterfall's copy is the frame itself.
    {
        SpectrumProcessor p;
        p.setAverage(0);
        p.setWaterfallAverage(0);
        const std::vector<float> trace = p.process(loud.data(), 0.04, lo);
        check(std::abs(trace[bin] - p.waterfallDb()[bin]) < 0.001f,
              "with both averages off the waterfall's copy is the frame");
    }
}

// Welch: a frame built from several blocks is the mean power of all of
// them, so the noise floor steadies without the trace lagging.
static void testWelchBlocks()
{
    const qint64 lo = 14'285'000;
    const auto one = tone(-50'000.0, 0.25, kFftSize);
    std::vector<std::complex<float>> four;
    for (int b = 0; b < 4; ++b) {
        four.insert(four.end(), one.begin(), one.end());
    }
    const auto bin = static_cast<std::size_t>(kFftSize / 2 + std::lround(-50'000.0 / kBinHz));
    SpectrumProcessor p;
    const float single = p.process(one.data(), 1, 0.04, lo)[bin];
    SpectrumProcessor q;
    const float welch = q.process(four.data(), 4, 0.04, lo)[bin];
    check(std::abs(single - welch) < 0.1f,
          "four identical blocks read the same level as one (mean power, not sum)");
}

int main()
{
    testViewRules();
    testWindowAndPeak();
    testAveraging();
    testWaterfallIsSeparate();
    testWelchBlocks();
    if (g_failures == 0) {
        std::printf("orion_spectrum_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_spectrum_test: %d failure(s)\n", g_failures);
    return 1;
}
