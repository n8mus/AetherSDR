// N8EM fork: the Orion backend's host receive chain (RSP2 IQ -> WDSP audio).
//
// Synthetic 1 MS/s IQ through the real mixer, decimator, resampler and
// WDSP. Pins the one fact the reference could not hand over, the spectral
// handedness (a tone above the dial is heard in USB and not in LSB, and the
// reverse below it), that the rejection is real (>60 dB; a non-blocking WDSP
// fed in bursts managed only ~30), and that CW hears a carrier on the dial.

#include "core/backends/orion/OrionRxAudio.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace AetherSDR::orion;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("  ok: %s\n", what);
    }
}

// RMS of the audio produced by `seconds` of a tone at absolute toneHz,
// captured with the RSP2 LO at loHz, after a settling second.
static double audioRms(OrionRxAudio::Mode mode, int lowHz, int highHz, qint64 dialHz,
                       qint64 loHz, double toneHz, double seconds = 2.0)
{
    OrionRxAudio rx;
    OrionRxAudio::Tuning t;
    t.dialHz = dialHz;
    t.mode = mode;
    t.lowHz = lowHz;
    t.highHz = highHz;
    t.agcMode = 0;            // fixed gain: levels compare across runs
    t.agcMaxGainDb = 20.0;
    QString error;
    if (!rx.start(t, &error)) {
        std::fprintf(stderr, "FAIL: start: %s\n", qPrintable(error));
        ++g_failures;
        return 0.0;
    }
    // The channel is built on the audio thread; feed once it is ready.
    QElapsedTimer ready;
    ready.start();
    while (!rx.isReady() && ready.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    double sumSq = 0.0;
    quint64 count = 0;
    quint64 blocks = 0;
    QObject::connect(&rx, &OrionRxAudio::audioReady, [&](const QByteArray& pcm) {
        ++blocks;
        if (blocks < 60) {
            return;   // settle: resampler, WDSP pipeline, DC blocker
        }
        const auto* f = reinterpret_cast<const float*>(pcm.constData());
        const qsizetype n = pcm.size() / static_cast<qsizetype>(sizeof(float));
        for (qsizetype k = 0; k + 1 < n; k += 2) {
            sumSq += static_cast<double>(f[k]) * f[k];
        }
        count += static_cast<quint64>(n / 2);
    });

    const double w = 2.0 * std::numbers::pi * (toneHz - static_cast<double>(loHz))
                     / OrionRxAudio::kInputRateHz;
    constexpr std::size_t kBlock = 1008;   // the SDRplay API's usual block
    std::vector<std::complex<float>> iq(kBlock);
    double phase = 0.0;
    const std::size_t total = static_cast<std::size_t>((seconds + 1.0) * OrionRxAudio::kInputRateHz);
    for (std::size_t sent = 0; sent < total; sent += kBlock) {
        for (auto& s : iq) {
            s = {static_cast<float>(0.01 * std::cos(phase)), static_cast<float>(0.01 * std::sin(phase))};
            phase += w;
        }
        rx.push(iq.data(), iq.size(), loHz);
        // Feed no faster than the chain runs: the real RSP2 is paced by its
        // clock, and an overflowing queue drops blocks, whose seams splatter
        // into the passband (measured: it faked a -25 dB "image").
        while (rx.queuedSamples() > OrionRxAudio::kMaxQueuedSamples / 4) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        if ((sent / kBlock) % 50 == 0) {
            QCoreApplication::processEvents();
        }
    }
    QElapsedTimer drain;
    drain.start();
    while (drain.elapsed() < 1500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    if (rx.droppedBlocks() != 0) {
        std::fprintf(stderr, "FAIL: the audio queue dropped %llu blocks\n",
                     static_cast<unsigned long long>(rx.droppedBlocks()));
        ++g_failures;
    }
    rx.stop();
    return count ? std::sqrt(sumSq / static_cast<double>(count)) : 0.0;
}

static double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const qint64 dial = 14'025'000;
    const qint64 lo = dial + 260'000;   // the default offset LO

    std::printf("-- sideband handedness\n");
    const double usbAbove = audioRms(OrionRxAudio::Mode::Usb, 0, 2400, dial, lo, dial + 1000.0);
    const double usbBelow = audioRms(OrionRxAudio::Mode::Usb, 0, 2400, dial, lo, dial - 1000.0);
    const double lsbAbove = audioRms(OrionRxAudio::Mode::Lsb, -2400, 0, dial, lo, dial + 1000.0);
    const double lsbBelow = audioRms(OrionRxAudio::Mode::Lsb, -2400, 0, dial, lo, dial - 1000.0);
    std::printf("  USB: above %.1f dB, below %.1f dB; LSB: above %.1f dB, below %.1f dB\n",
                db(usbAbove), db(usbBelow), db(lsbAbove), db(lsbBelow));
    check(usbAbove > 0.0, "USB produces audio");
    check(db(usbAbove) - db(usbBelow) > 60.0, "USB hears the tone above the dial, rejects below by >60 dB");
    check(db(lsbBelow) - db(lsbAbove) > 60.0, "LSB hears the tone below the dial, rejects above by >60 dB");

    std::printf("-- CW: a carrier on the dial\n");
    const double cwOn = audioRms(OrionRxAudio::Mode::Cw, -250, 250, dial, lo, dial);
    const double cwOff = audioRms(OrionRxAudio::Mode::Cw, -250, 250, dial, lo, dial + 1500.0);
    std::printf("  CW: on dial %.1f dB, 1.5 kHz off %.1f dB\n", db(cwOn), db(cwOff));
    check(db(cwOn) - db(cwOff) > 60.0, "CW hears the carrier on the dial; 1.5 kHz off is >60 dB down");

    std::printf("-- the dial anywhere in the capture\n");
    const double far = audioRms(OrionRxAudio::Mode::Usb, 0, 2400, dial, dial + 450'000, dial + 1000.0);
    check(db(far) - db(usbAbove) > -3.0 && db(far) - db(usbAbove) < 3.0,
          "same level with the dial 450 kHz below the LO");

    std::printf("-- a receiver outside the capture is silent (step 4)\n");
    const double outside = audioRms(OrionRxAudio::Mode::Usb, 0, 2400, dial, dial + 470'000,
                                    dial + 1000.0);
    check(db(outside) < db(usbAbove) - 80.0,
          "470 kHz from the LO (beyond +-460 kHz) plays silence, not a folded signal");

    if (g_failures == 0) {
        std::printf("orion_rxaudio_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_rxaudio_test: %d failure(s)\n", g_failures);
    return 1;
}
