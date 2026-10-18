// SPDX-License-Identifier: GPL-2.0-or-later
// CW skimmer (step 8) against synthetic IQ. Three parts:
//  1. The engine, as the console's skimtest checks it: CW stations calling CQ
//     in the 20 m CW segment, averaged-spectrum frames as the host would hand
//     them; every real call spotted on its frequency, nothing else (ported
//     from the Orion console's tests/skimtest.cpp).
//  2. The DX-cluster telnet server: login chatter, spot line format, the
//     per-call throttle (ported from the console's tests/skimsrvtest.cpp).
//  3. The AetherSDR glue (OrionSkimmer + OrionSkimmerFeed): the feed's own
//     thread, AetherSDR's cty.dat as the call validator, and blocks taken at
//     an LO the channels were not laid out for never reach the decoders.
//
//   cmake --build build --target orion_skimmer_test && ./build/orion_skimmer_test

#include "core/backends/orion/OrionSkimmer.h"
#include "core/backends/orion/OrionSkimmerFeed.h"
#include "core/backends/orion/skimmer/SkimServer.h"
#include "core/backends/orion/skimmer/SkimmerEngine.h"
#ifdef AETHER_ORION_SDRPLAY
#include "core/backends/orion/skimmer/SkimStft.h"
#endif

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QTcpSocket>

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using namespace AetherSDR::orion;
using skim::SkimmerEngine;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

const QHash<QChar, QString>& morse()
{
    static const QHash<QChar, QString> t = {
        {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
        {'E', "."},    {'F', "..-."}, {'G', "--."},  {'H', "...."},
        {'I', ".."},   {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"},   {'N', "-."},   {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
        {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"},
        {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"},{'1', ".----"},
        {'2', "..---"},{'3', "...--"},{'4', "....-"},{'5', "....."},
        {'6', "-...."},{'7', "--..."},{'8', "---.."},{'9', "----."},
    };
    return t;
}

// Text -> on/off keying envelope at the sample rate.
std::vector<bool> keying(const QString& text, int wpm, double rate)
{
    const double dit = 1.2 / wpm;
    const auto samples = [&](double s) { return size_t(s * rate); };
    std::vector<bool> k;
    for (const QChar ch : text) {
        if (ch == ' ') {
            k.insert(k.end(), samples(7 * dit), false);
            continue;
        }
        const QString pat = morse().value(ch.toUpper());
        for (const QChar e : pat) {
            k.insert(k.end(), samples(e == '.' ? dit : 3 * dit), true);
            k.insert(k.end(), samples(dit), false);
        }
        k.insert(k.end(), samples(2 * dit), false);
    }
    return k;
}

struct Station {
    QString call;
    qint64 hz;
    int wpm;
    float amp;
    QString text;
    std::vector<bool> key;
    size_t pos = 0;
    std::complex<double> lo{1.0, 0.0}, step;
};

// A synthetic band: stations keying around an LO, plus noise.
struct Band {
    double rate;
    qint64 loAbs;
    std::vector<Station> st;
    std::mt19937 rng{7};
    std::normal_distribution<float> noise{0.0f, 0.004f};

    void prepare()
    {
        for (auto& s : st) {
            s.key = keying(s.text.isEmpty() ? QString("CQ CQ DE %1 %1 K  ").arg(s.call)
                                            : s.text,
                           s.wpm, rate);
            const double inc = 2.0 * M_PI * double(s.hz - loAbs) / rate;
            s.step = {std::cos(inc), std::sin(inc)};
        }
    }
    void fill(std::vector<std::complex<float>>& iq)
    {
        for (auto& v : iq) {
            std::complex<double> acc(noise(rng), noise(rng));
            for (auto& s : st) {
                if (s.key[s.pos]) acc += double(s.amp) * s.lo;
                s.lo *= s.step;
                if (++s.pos >= s.key.size()) s.pos = 0;
            }
            v = std::complex<float>(float(acc.real()), float(acc.imag()));
        }
        for (auto& s : st) s.lo /= std::abs(s.lo);
    }
    // The averaged spectrum the host would hold: flat floor + peaks.
    std::vector<float> spectrum(int n) const
    {
        const double binHz = rate / n;
        std::vector<float> db(size_t(n), -120.0f);
        for (const auto& s : st) {
            const int bin = n / 2 + int(std::lround(double(s.hz - loAbs) / binHz));
            for (int k = -2; k <= 2; ++k) {
                db[size_t(bin + k)] = std::max(db[size_t(bin + k)],
                    -75.0f + 20.0f * std::log10(s.amp / 0.2f) - 6.0f * std::abs(k));
            }
        }
        return db;
    }
};

void testEngine()
{
    std::printf("-- the engine (the console's skimtest)\n");
    constexpr double kRate = 500000.0;
    constexpr qint64 kDial = 14030000;
    constexpr int kLoOff = 60000;
    Band band{kRate, kDial + kLoOff, {
        {"W1AW",  14012000, 22, 0.20f},
        {"DL1ABC",14025500, 28, 0.12f},
        {"JA2XYZ",14040100, 17, 0.06f},
        {"VE3KI", 14055700, 25, 0.10f},
        {"TEST",  14070300, 20, 0.15f},   // not a callsign: never spotted
        {"K1TTT", 14003200, 45, 0.15f},
        {"N8TT",  14008700, 33, 0.12f, "CQ TEST N8TT N8TT  "},
        {"VE7CC", 14018700, 30, 0.08f},
        {"DL8AAM",14033300, 24, 0.10f},
        {"JA7QQQ",14047900, 20, 0.07f},
        {"W9RE",  14061100, 35, 0.12f},
    }};
    band.prepare();

    SkimmerEngine eng(kRate, 24);
    eng.setCallValidator([](const QString& c) {
        for (const char* p : {"W1", "W9", "DL", "JA", "VE", "K1", "N8"})
            if (c.startsWith(p)) return true;
        return false;
    });
    eng.setEnabled(true);
    QHash<QString, qint64> found;
    QObject::connect(&eng, &SkimmerEngine::spotFound, &eng,
                     [&](const QString& call, qint64 hz, int) { found[call] = hz; });

    constexpr int kN = 8192;
    const std::vector<float> db = band.spectrum(kN);
    std::vector<std::complex<float>> iq(16384);
    size_t sinceSpec = 0;
    for (size_t done = 0; done < size_t(45.0 * kRate); done += iq.size()) {
        band.fill(iq);
        eng.processIq(iq.data(), iq.size());
        sinceSpec += iq.size();
        if (sinceSpec >= size_t(2.0 * kRate)) {
            sinceSpec = 0;
            eng.updateFromSpectrum(db, int(kRate), kDial, kLoOff);
        }
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();

    bool allFound = true, onFrequency = true, phantoms = false;
    for (const auto& s : band.st) {
        if (s.call == QLatin1String("TEST")) continue;
        if (!found.contains(s.call)) {
            std::printf("    never spotted: %s\n", qPrintable(s.call));
            allFound = false;
        } else if (std::llabs(found[s.call] - s.hz) > 60) {
            onFrequency = false;
        }
    }
    for (auto it = found.constBegin(); it != found.constEnd(); ++it) {
        bool real = false;
        for (const auto& s : band.st)
            if (s.call != QLatin1String("TEST") && s.call == it.key()) real = true;
        if (!real) {
            std::printf("    phantom: %s\n", qPrintable(it.key()));
            phantoms = true;
        }
    }
    check(allFound, "every real station is spotted");
    check(onFrequency, "each spot is within 60 Hz of the station");
    check(!found.contains(QStringLiteral("TEST")) && !phantoms, "no bogus or phantom spots");
}

void testServer()
{
    std::printf("-- the DX-cluster telnet server (the console's skimsrvtest)\n");
    skim::SkimServer srv;
    srv.setSpotterCall(QStringLiteral("N8EM"));
    check(srv.start(0), "listens on localhost");
    QTcpSocket cli;
    QString rx;
    QObject::connect(&cli, &QTcpSocket::readyRead, &cli,
                     [&] { rx += QString::fromLatin1(cli.readAll()); });
    cli.connectToHost(QStringLiteral("127.0.0.1"), srv.port());
    const auto pump = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    };
    pump(400);
    cli.write("N8EM-TEST\r\n");
    pump(400);
    srv.announce(QStringLiteral("W1AW"), 14012000, 25);
    srv.announce(QStringLiteral("W1AW"), 14012000, 25);   // throttled
    srv.announce(QStringLiteral("DL1ABC"), 14025500, 30);
    pump(600);
    check(rx.contains(QStringLiteral("enter your call")), "login prompt");
    check(rx.contains(QStringLiteral("N8EM-TEST de N8EM-#")), "login acknowledged");
    static const QRegularExpression spotRe(
        QStringLiteral("DX de N8EM-#:\\s+14012\\.0\\s+W1AW\\s+CW 25 WPM\\s+\\d{4}Z"));
    check(spotRe.match(rx).hasMatch(), "spot line in DX-cluster format");
    check(rx.count(QStringLiteral("W1AW")) == 1, "a call re-announced at once is throttled");
    check(rx.contains(QStringLiteral("DL1ABC")), "a second call goes out");
    srv.stop();
}

void testGlue(const QString& ctyPath)
{
    std::printf("-- AetherSDR glue: OrionSkimmer, its feed thread, cty.dat\n");
    constexpr double kRate = 500000.0;
    constexpr qint64 kDial = 7020000;
    constexpr qint64 kLo = 7080000;
    Band band{kRate, kLo, {
        {"W1AW", 7012000, 22, 0.20f},
        {"DL1ABC", 7025500, 28, 0.12f},
        {"VE3KI", 7041000, 25, 0.10f},
    }};
    band.prepare();

    OrionSkimmer::Settings s;
    s.channels = 8;
    s.telnetPort = 0;
    s.ctyPath = ctyPath;
    s.masterScpPath = QStringLiteral("/nonexistent");   // every call needs 3 sightings
    OrionSkimmer sk(kRate, s);
    check(sk.callValidatorLoaded(), "AetherSDR's cty.dat loads as the call validator");
    QHash<QString, qint64> found;
    QObject::connect(&sk, &OrionSkimmer::spotFound, &sk,
                     [&](const QString& call, qint64 hz, int) { found[call] = hz; });

    std::vector<std::complex<float>> iq(16384);
    // Not active yet: nothing passes the feed.
    band.fill(iq);
    sk.feed()->push(iq.data(), iq.size(), kLo);
    QCoreApplication::processEvents();
    check(sk.feed()->processedSamples() == 0, "inactive: no samples reach the decoders");

    sk.setActive(true);
    const std::vector<float> db = band.spectrum(8192);
    // Blocks at an LO the channels were not laid out for are dropped.
    band.fill(iq);
    sk.feed()->push(iq.data(), iq.size(), kLo);
    QCoreApplication::processEvents();
    check(sk.feed()->processedSamples() == 0, "before the first layout: nothing decoded");

    sk.onFullSpectrum(db, int(kRate), kLo, kDial);
    check(sk.activeChannels() == 3, "three channels laid out on the three stations");
    size_t sinceSpec = 0;
    QElapsedTimer wall;
    wall.start();
    for (size_t done = 0; done < size_t(45.0 * kRate); done += iq.size()) {
        band.fill(iq);
        sk.feed()->push(iq.data(), iq.size(), kLo);
        sinceSpec += iq.size();
        if (sinceSpec >= size_t(2.0 * kRate)) {
            sinceSpec = 0;
            sk.onFullSpectrum(db, int(kRate), kLo, kDial);
        }
        // Do not outrun the feed's thread (the real stream is paced).
        while (sk.feed()->processedSamples() + OrionSkimmerFeed::kRingSamples / 2 < done) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        QCoreApplication::processEvents();
    }
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    check(found.contains(QStringLiteral("W1AW")) && found.contains(QStringLiteral("DL1ABC"))
              && found.contains(QStringLiteral("VE3KI")),
          "all three calls spotted through the feed thread");
    check(found.size() == 3, "and nothing else");
    check(sk.droppedSamples() == 0, "no IQ dropped");

    // A pan move: the LO changes; old-LO blocks stop until the next layout.
    const quint64 before = sk.feed()->processedSamples();
    band.fill(iq);
    sk.feed()->push(iq.data(), iq.size(), kLo + 50000);
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    check(sk.feed()->processedSamples() - before < iq.size(),
          "a block at a new LO waits for the channels to be re-laid");
    sk.setActive(false);
    check(sk.activeChannels() == 0, "off frees every channel");
}

#ifdef AETHER_ORION_SDRPLAY
void testStft()
{
    std::printf("-- the SKIM waterfall's STFT (stage 2)\n");
    constexpr double kRate = 1'000'000.0;
    skim::SkimStft stft(kRate);
    check(std::abs(stft.binHz() - 125000.0 / skim::SkimStft::kFft) < 1.0,
          "~125 ksps after decimation, ~30 Hz bins");
    // A steady carrier 3 kHz above the slice centre, the slice 40 kHz
    // below the LO.
    const double offset = -40000.0;
    const double carrier = offset + 3000.0;
    std::vector<std::complex<float>> iq(16384);
    double ph = 0.0;
    const auto fill = [&] {
        for (auto& v : iq) {
            v = {0.5f * float(std::cos(ph)), 0.5f * float(std::sin(ph))};
            ph += 2.0 * M_PI * carrier / kRate;
        }
    };
    fill();
    stft.processIq(iq.data(), iq.size());
    check(stft.takeColumns().empty(), "disabled: no columns");
    stft.setEnabled(true);
    const uint32_t gen = stft.setViewOffset(offset);
    for (int i = 0; i < 20; ++i) {   // ~330 ms
        fill();
        stft.processIq(iq.data(), iq.size());
    }
    const auto cols = stft.takeColumns();
    check(cols.size() > 20 && cols.size() < 60, "columns every ~8 ms");
    bool onBin = !cols.empty();
    for (const auto& c : cols) {
        if (c.gen != gen) { onBin = false; break; }
        const auto peak = std::max_element(c.db.begin(), c.db.end()) - c.db.begin();
        const int want = skim::SkimStft::kFft / 2 + int(std::lround(3000.0 / stft.binHz()));
        if (std::abs(int(peak) - want) > 1) { onBin = false; break; }
    }
    check(onBin, "the carrier sits 3 kHz above the slice centre, in every column");
    const uint32_t gen2 = stft.setViewOffset(offset + 10000.0);
    fill();
    stft.processIq(iq.data(), iq.size());
    bool fresh = true;
    for (const auto& c : stft.takeColumns()) {
        if (c.gen != gen2) fresh = false;
    }
    check(fresh && gen2 != gen, "a new view offset drops the old columns");
}

void testViewFeed(const QString& ctyPath)
{
    std::printf("-- the waterfall's own feed (stage 2)\n");
    OrionSkimmer::Settings s;
    s.channels = 4;
    s.telnetPort = 0;
    s.ctyPath = ctyPath;
    OrionSkimmer sk(1'000'000.0, s);
    check(sk.stft() != nullptr && sk.viewFeed() != nullptr, "the skimmer carries an STFT and its feed");
    std::vector<std::complex<float>> iq(16384, {0.1f, 0.0f});
    sk.viewFeed()->push(iq.data(), iq.size(), 7'080'000);
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 200) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    check(sk.viewFeed()->processedSamples() == 0, "window closed: the feed is idle");
    sk.setViewActive(true);
    sk.stft()->setViewOffset(-40000.0);
    for (int i = 0; i < 10; ++i) sk.viewFeed()->push(iq.data(), iq.size(), 7'080'000 + i * 1000);
    t.restart();
    while (t.elapsed() < 1500 && sk.viewFeed()->processedSamples() < 100'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    check(sk.viewFeed()->processedSamples() >= 100'000,
          "window open: every block reaches the STFT, whatever the LO");
    check(!sk.stft()->takeColumns().empty(), "and columns come out");
    sk.setViewActive(false);
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testEngine();
    testServer();
    testGlue(QStringLiteral(ORION_TEST_CTY_DAT));
#ifdef AETHER_ORION_SDRPLAY
    testStft();
    testViewFeed(QStringLiteral(ORION_TEST_CTY_DAT));
#endif
    if (g_failures == 0) {
        std::printf("orion_skimmer_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_skimmer_test: %d failure(s)\n", g_failures);
    return 1;
}
