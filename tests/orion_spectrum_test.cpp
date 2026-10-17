// N8EM fork: RSP2 panadapter maths for the Orion backend.
//
// No device: synthetic IQ through the real FFT path. Pins the offset-LO
// view rules (the 260 kHz offset falls out of the 480 kHz view), the
// window's 0 dBFS reference, and that a carrier lands on the right pixel.

#include "core/backends/orion/OrionSpectrum.h"

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
    p.setAverage(0.0);
    // A full-scale complex tone 100 kHz below the LO.
    const auto iq = tone(-100'000.0, 1.0, p.size());
    const std::vector<float>& db = p.process(iq.data());
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
    SpectrumProcessor p;
    p.setAverage(0.9);
    const auto loud = tone(-50'000.0, 1.0, p.size());
    const auto quiet = tone(-50'000.0, 0.01, p.size());
    const int bin = p.size() / 2 + static_cast<int>(std::lround(-50'000.0 / kBinHz));
    (void)p.process(loud.data());
    const float afterQuiet = p.process(quiet.data())[static_cast<std::size_t>(bin)];
    check(afterQuiet > -5.0f, "heavy averaging holds the previous level");
    p.resetAverage();
    const float reset = p.process(quiet.data())[static_cast<std::size_t>(bin)];
    check(std::fabs(reset - (-40.0f)) < 1.0f, "after a retune reset the new level shows at once");
}

int main()
{
    testViewRules();
    testWindowAndPeak();
    testAveraging();
    if (g_failures == 0) {
        std::printf("orion_spectrum_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_spectrum_test: %d failure(s)\n", g_failures);
    return 1;
}
