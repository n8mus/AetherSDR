// Bench tool (not a ctest test): the same CW audio through AetherSDR's
// ggmorse reader and the fork's Orion reader (fldigi / Bayes / legacy
// brains), scored by character error rate against the text that was sent.
//
//   orion_cw_bench                 generated signals, the full table
//   orion_cw_bench --wav FILE      a recording (mono or stereo s16 WAV at
//                                  24 kHz); prints each reader's copy
//   --show                         also print each reader's copy per trial
//
// Both readers get identical float audio at 24 kHz, the way each gets it in
// its own program: ggmorse as s16 mono (src/core/CwDecoder.cpp), the Orion
// reader as complex(x, 0) at the CW pitch (the console's AudioCwSource).
// Every signal passes a 500 Hz CW filter first, as the radio's would.

#include "core/backends/orion/skimmer/CwDecoder.h"
#include "ggmorse/ggmorse.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <unistd.h>
#include <vector>

using AetherSDR::orion::skim::CwDecoder;

namespace {

constexpr double kRate = 24000.0;
double kPitch = 600.0;   // --pitch for a recording

const QHash<QChar, QString>& morse()
{
    static const QHash<QChar, QString> m = {
        {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."},
        {'F', "..-."}, {'G', "--."}, {'H', "...."}, {'I', ".."}, {'J', ".---"},
        {'K', "-.-"}, {'L', ".-.."}, {'M', "--"}, {'N', "-."}, {'O', "---"},
        {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."}, {'S', "..."}, {'T', "-"},
        {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"}, {'Y', "-.--"},
        {'Z', "--.."}, {'0', "-----"}, {'1', ".----"}, {'2', "..---"},
        {'3', "...--"}, {'4', "....-"}, {'5', "....."}, {'6', "-...."},
        {'7', "--..."}, {'8', "---.."}, {'9', "----."}, {'/', "-..-."},
        {'?', "..--.."}, {'=', "-...-"},
    };
    return m;
}

QString randomCall(std::mt19937& rng)
{
    static const QStringList prefixes = {"K", "W", "N", "AA", "KC", "KE", "KD", "VE",
                                         "G", "DL", "JA", "EA", "OH", "I", "F", "PY",
                                         "LU", "VK", "ZL", "OK", "SP", "WB", "KB"};
    std::uniform_int_distribution<int> p(0, int(prefixes.size()) - 1);
    std::uniform_int_distribution<int> digit(0, 9);
    std::uniform_int_distribution<int> len(1, 3);
    std::uniform_int_distribution<int> letter(0, 25);
    QString call = prefixes[p(rng)] + QString::number(digit(rng));
    const int n = len(rng);
    for (int i = 0; i < n; ++i) {
        call += QChar('A' + letter(rng));
    }
    return call;
}

struct Qso {
    QString text;
    QStringList calls;
};

Qso randomQso(std::mt19937& rng)
{
    static const QStringList names = {"BOB", "JON", "ANN", "TOM", "SUE", "MIKE", "JIM", "PAT"};
    static const QStringList qths = {"OHIO", "TEXAS", "MAINE", "IOWA", "UTAH", "OREGON", "KANSAS"};
    static const QStringList rsts = {"599", "579", "559", "449", "5NN"};
    std::uniform_int_distribution<int> n(0, int(names.size()) - 1);
    std::uniform_int_distribution<int> q(0, int(qths.size()) - 1);
    std::uniform_int_distribution<int> r(0, int(rsts.size()) - 1);
    const QString a = randomCall(rng);
    const QString b = randomCall(rng);
    const QString rst = rsts[r(rng)];
    const QString name = names[n(rng)];
    const QString qth = qths[q(rng)];
    Qso qso;
    qso.calls = {a, b};
    qso.text = QStringLiteral("CQ CQ DE %1 %1 K %1 DE %2 %2 K %2 DE %1 GM TNX FER CALL "
                              "UR RST %3 %3 NAME %4 %4 QTH %5 %5 HW? %2 DE %1 K")
                   .arg(a, b, rst, name, qth);
    return qso;
}

struct Condition {
    QString name;
    double wpm = 25.0;
    double snrDb = 10.0;      // in a 2500 Hz reference bandwidth
    bool handSent = false;    // timing jitter and loose spacing
    bool fading = false;      // 15 dB slow QSB
    double driftHz = 0.0;     // pitch drift over the whole message
    bool neighbour = false;   // a second station 180 Hz up, same strength
};

// Key-down envelope for one message at the given speed, 4 ms raised edges.
std::vector<float> keyEnvelope(const QString& text, double wpm, bool handSent,
                               std::mt19937& rng, double leadSec, double tailSec)
{
    const double dit = 1.2 / wpm;
    std::normal_distribution<double> jit(1.0, handSent ? 0.12 : 0.0);
    std::uniform_real_distribution<double> loose(1.0, handSent ? 1.5 : 1.0);
    std::vector<std::pair<bool, double>> runs;
    runs.push_back({false, leadSec});
    for (const QChar ch : text) {
        if (ch == ' ') {
            runs.push_back({false, 4.0 * dit * loose(rng)});   // 3 already after the char
            continue;
        }
        const QString code = morse().value(ch);
        for (int i = 0; i < code.size(); ++i) {
            const double units = code[i] == '.' ? 1.0 : 3.0;
            runs.push_back({true, std::max(0.4, jit(rng)) * units * dit});
            runs.push_back({false, std::max(0.4, jit(rng)) * dit});
        }
        runs.push_back({false, 2.0 * dit * loose(rng)});
    }
    runs.push_back({false, tailSec});

    std::vector<float> key;
    for (const auto& [on, sec] : runs) {
        key.insert(key.end(), size_t(std::lround(sec * kRate)), on ? 1.0f : 0.0f);
    }
    const int ramp = int(0.004 * kRate);
    std::vector<float> win(size_t(ramp), 0.0f);
    float sum = 0.0f;
    for (int i = 0; i < ramp; ++i) {
        win[size_t(i)] = 0.5f - 0.5f * float(std::cos(2.0 * M_PI * (i + 0.5) / ramp));
        sum += win[size_t(i)];
    }
    std::vector<float> env(key.size(), 0.0f);
    for (size_t i = 0; i < key.size(); ++i) {
        float acc = 0.0f;
        for (int k = 0; k < ramp && k <= int(i); ++k) {
            acc += win[size_t(k)] * key[i - size_t(k)];
        }
        env[i] = acc / sum;
    }
    return env;
}

// The radio's 500 Hz CW filter around the pitch (windowed-sinc band-pass).
std::vector<float> cwFilter(const std::vector<float>& x)
{
    constexpr int kTaps = 511;
    const double lo = (kPitch - 250.0) / kRate;
    const double hi = (kPitch + 250.0) / kRate;
    std::vector<float> h(kTaps);
    for (int i = 0; i < kTaps; ++i) {
        const double n = i - (kTaps - 1) / 2.0;
        const double ideal = n == 0.0
            ? 2.0 * (hi - lo)
            : (std::sin(2.0 * M_PI * hi * n) - std::sin(2.0 * M_PI * lo * n)) / (M_PI * n);
        const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * i / (kTaps - 1))
            + 0.08 * std::cos(4.0 * M_PI * i / (kTaps - 1));
        h[size_t(i)] = float(ideal * w);
    }
    std::vector<float> y(x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); ++i) {
        float acc = 0.0f;
        const int kmax = std::min(kTaps, int(i) + 1);
        for (int k = 0; k < kmax; ++k) {
            acc += h[size_t(k)] * x[i - size_t(k)];
        }
        y[i] = acc;
    }
    return y;
}

void addTone(std::vector<float>& out, const std::vector<float>& env, double pitch,
             double driftHz, bool fading, std::mt19937& rng)
{
    std::uniform_real_distribution<double> phase0(0.0, 2.0 * M_PI);
    const double fadePhase = phase0(rng);
    double phase = phase0(rng);
    const double total = double(env.size());
    if (out.size() < env.size()) {
        out.resize(env.size(), 0.0f);
    }
    for (size_t i = 0; i < env.size(); ++i) {
        const double t = double(i) / kRate;
        const double f = pitch + driftHz * (double(i) / total - 0.5);
        phase += 2.0 * M_PI * f / kRate;
        double gain = 1.0;
        if (fading) {
            const double depthDb = 15.0 * (0.5 + 0.5 * std::sin(2.0 * M_PI * 0.12 * t + fadePhase));
            gain = std::pow(10.0, -depthDb / 20.0);
        }
        out[i] += float(gain * env[i] * std::sin(phase));
    }
}

std::vector<float> synth(const Qso& qso, const Condition& c, std::mt19937& rng)
{
    const std::vector<float> env = keyEnvelope(qso.text, c.wpm, c.handSent, rng, 1.5, 2.0);
    std::vector<float> x;
    addTone(x, env, kPitch, c.driftHz, c.fading, rng);
    if (c.neighbour) {
        const Qso other = randomQso(rng);
        std::vector<float> envB = keyEnvelope(other.text, 22.0, false, rng, 0.7, 0.0);
        envB.resize(env.size(), 0.0f);
        addTone(x, envB, kPitch + 180.0, 0.0, false, rng);
    }
    // White noise; the carrier (amplitude 1) has power 0.5.
    const double sigma =
        std::sqrt(0.5 / std::pow(10.0, c.snrDb / 10.0) * (kRate / 2.0) / 2500.0);
    std::normal_distribution<float> noise(0.0f, float(sigma));
    for (float& s : x) {
        s += noise(rng);
    }
    std::vector<float> y = cwFilter(x);
    // A receiver's AGC: bring the passband to a fixed level.
    double peak = 0.0;
    for (const float s : y) {
        peak = std::max(peak, double(std::abs(s)));
    }
    const float scale = peak > 0.0 ? float(0.5 / peak) : 1.0f;
    for (float& s : y) {
        s *= scale;
    }
    return y;
}

// ---- the readers ----------------------------------------------------------

struct GgChunk {
    QString text;
    float cost;
};

// AetherSDR's reader: CwDecoder::decodeLoop's ggmorse set-up and frame loop,
// run synchronously.
std::vector<GgChunk> runGgmorse(const std::vector<float>& audio)
{
    GGMorse::Parameters params;
    params.sampleRateInp = 24000.0f;
    params.sampleRateOut = 24000.0f;
    params.samplesPerFrame = GGMorse::kDefaultSamplesPerFrame;
    params.sampleFormatInp = GGMORSE_SAMPLE_FORMAT_I16;
    params.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_I16;
    GGMorse ggmorse(params);

    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = -1.0f;
    dp.speed_wpm = -1.0f;
    dp.frequencyRangeMin_hz = 200.0f;
    dp.frequencyRangeMax_hz = 1200.0f;
    dp.speedRangeMin_wpm = -1.0f;
    dp.speedRangeMax_wpm = -1.0f;
    ggmorse.setParametersDecode(dp);

    std::vector<int16_t> pcm(audio.size());
    for (size_t i = 0; i < audio.size(); ++i) {
        pcm[i] = int16_t(std::clamp(double(audio[i]) * 32768.0, -32768.0, 32767.0));
    }
    const int resample = int(params.sampleRateInp / GGMorse::kBaseSampleRate);
    const uint32_t bytesPerFrame =
        uint32_t(params.samplesPerFrame * resample * int(sizeof(int16_t)));

    std::vector<GgChunk> out;
    size_t pos = 0;   // bytes
    const size_t total = pcm.size() * sizeof(int16_t);
    while (total - pos >= bytesPerFrame) {
        int frames = 0;
        ggmorse.decode([&](void* data, uint32_t nMaxBytes) -> uint32_t {
            if (frames > 0 || total - pos < nMaxBytes) {
                return 0;
            }
            std::memcpy(data, reinterpret_cast<const char*>(pcm.data()) + pos, nMaxBytes);
            pos += nMaxBytes;
            ++frames;
            return nMaxBytes;
        });
        if (frames == 0) {
            break;
        }
        GGMorse::TxRx rx;
        const float cost = ggmorse.getStatistics().costFunction;
        if (ggmorse.takeRxData(rx) > 0 && cost < 1.0f) {
            out.push_back({QString::fromLatin1(reinterpret_cast<const char*>(rx.data()),
                                               int(rx.size())),
                           cost});
        }
    }
    return out;
}

QString ggText(const std::vector<GgChunk>& chunks, float threshold)
{
    QString s;
    for (const GgChunk& c : chunks) {
        if (c.cost < threshold) {
            s += c.text;
        }
    }
    return s;
}

struct OrionCopy {
    QString shown;   // textDecoded: what the console's CW window prints
    QString loose;   // textLoose: the best-guess twin
};

OrionCopy runOrion(const std::vector<float>& audio, CwDecoder::Brain brain, bool som, bool deep,
                   double squelch = 12.0)
{
    CwDecoder dec(kRate, kPitch);
    dec.setBrain(brain);
    dec.setSom(som);
    dec.setDeep(deep);
    dec.setSquelch(squelch);
    dec.setAttack(1);
    dec.setDecay(1);
    dec.setEnabled(true);
    OrionCopy copy;
    QObject::connect(&dec, &CwDecoder::textDecoded, [&](const QString& t) { copy.shown += t; });
    QObject::connect(&dec, &CwDecoder::textLoose, [&](const QString& t) { copy.loose += t; });
    constexpr size_t kBlock = 2400;
    std::vector<std::complex<float>> buf;
    for (size_t i = 0; i < audio.size(); i += kBlock) {
        const size_t n = std::min(kBlock, audio.size() - i);
        buf.resize(n);
        for (size_t k = 0; k < n; ++k) {
            buf[k] = {audio[i + k], 0.0f};
        }
        dec.processIq(buf.data(), buf.size());
    }
    return copy;
}

// ---- scoring --------------------------------------------------------------

QString normalise(QString s)
{
    s = s.toUpper();
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return s.simplified();
}

int editDistance(const QString& a, const QString& b)
{
    std::vector<int> prev(size_t(b.size()) + 1), cur(size_t(b.size()) + 1);
    for (int j = 0; j <= b.size(); ++j) {
        prev[size_t(j)] = j;
    }
    for (int i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int sub = prev[size_t(j - 1)] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[size_t(j)] = std::min({sub, prev[size_t(j)] + 1, cur[size_t(j - 1)] + 1});
        }
        std::swap(prev, cur);
    }
    return prev[size_t(b.size())];
}

struct Score {
    double cer = 0.0;     // character error rate, 0..1+ (can exceed 1 with junk)
    double calls = 0.0;   // fraction of the callsign appearances copied exactly
};

Score score(const Qso& qso, const QString& copy)
{
    const QString ref = normalise(qso.text);
    const QString hyp = normalise(copy);
    Score s;
    s.cer = double(editDistance(ref, hyp)) / double(ref.size());
    const QStringList refWords = ref.split(QLatin1Char(' '));
    const QStringList hypWords = hyp.split(QLatin1Char(' '));
    int wanted = 0;
    int got = 0;
    // Each appearance of a call in the sent text, matched against the copy.
    QHash<QString, int> hypCount;
    for (const QString& w : hypWords) {
        ++hypCount[w];
    }
    for (const QString& w : refWords) {
        if (qso.calls.contains(w)) {
            ++wanted;
            if (hypCount.value(w) > 0) {
                --hypCount[w];
                ++got;
            }
        }
    }
    s.calls = wanted ? double(got) / wanted : 0.0;
    return s;
}

struct Reader {
    QString name;
    std::function<QString(const std::vector<float>&)> run;
};

std::vector<Reader> readers()
{
    // AetherSDR's panel drops chunks whose ggmorse cost is at or above
    // 1 - sens/100*0.9; the default Sens of 30 gives 0.73.
    return {
        {"Aether ggmorse (Sens 30)",
         [](const std::vector<float>& a) { return ggText(runGgmorse(a), 0.73f); }},
        {"Aether ggmorse (Sens 100)",
         [](const std::vector<float>& a) { return ggText(runGgmorse(a), 1.0f); }},
        {"Orion fldigi + SOM",
         [](const std::vector<float>& a) {
             return runOrion(a, CwDecoder::Brain::Engine, true, false).shown;
         }},
        {"Orion fldigi + SOM + DEEP",
         [](const std::vector<float>& a) {
             return runOrion(a, CwDecoder::Brain::Engine, true, true).shown;
         }},
        {"Orion fldigi + SOM, SQL 0",
         [](const std::vector<float>& a) {
             return runOrion(a, CwDecoder::Brain::Engine, true, false, 0.0).shown;
         }},
        {"Orion Bayes",
         [](const std::vector<float>& a) {
             return runOrion(a, CwDecoder::Brain::Bayes, true, false).shown;
         }},
        {"Orion legacy",
         [](const std::vector<float>& a) {
             return runOrion(a, CwDecoder::Brain::Legacy, false, false).shown;
         }},
    };
}

std::vector<float> readWav(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray all = f.readAll();
    if (all.size() < 44 || !all.startsWith("RIFF")) {
        return {};
    }
    int channels = 1;
    int bits = 16;
    int rate = 0;
    qsizetype pos = 12;
    while (pos + 8 <= all.size()) {
        const QByteArray id = all.mid(pos, 4);
        quint32 len = 0;
        std::memcpy(&len, all.constData() + pos + 4, 4);
        const qsizetype body = pos + 8;
        if (id == "fmt ") {
            quint16 ch = 0, bps = 0;
            quint32 sr = 0;
            std::memcpy(&ch, all.constData() + body + 2, 2);
            std::memcpy(&sr, all.constData() + body + 4, 4);
            std::memcpy(&bps, all.constData() + body + 14, 2);
            channels = ch;
            rate = int(sr);
            bits = bps;
        } else if (id == "data") {
            if (bits != 16 || rate != int(kRate)) {
                std::fprintf(stderr, "need 16-bit %d Hz WAV (got %d-bit %d Hz)\n",
                             int(kRate), bits, rate);
                return {};
            }
            const qsizetype n = std::min<qsizetype>(len, all.size() - body) / (2 * channels);
            std::vector<float> out(static_cast<size_t>(n));
            for (qsizetype i = 0; i < n; ++i) {
                int16_t s = 0;
                std::memcpy(&s, all.constData() + body + i * 2 * channels, 2);
                out[size_t(i)] = float(s) / 32768.0f;
            }
            return out;
        }
        pos = body + len + (len & 1);
    }
    return {};
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    for (const char* v : {"TTC_CWLEGACY", "TTC_CWBAYES", "TTC_CWENGINE", "TTC_CWENV"}) {
        qunsetenv(v);
    }
    const QStringList args = app.arguments();
    const bool show = args.contains("--show");
    const int pitchAt = int(args.indexOf("--pitch"));
    if (pitchAt > 0 && pitchAt + 1 < args.size()) {
        kPitch = args[pitchAt + 1].toDouble();
    }
    // ggmorse prints every character it reads to stdout; keep that out of
    // the results, which go to the real stdout through `out`.
    FILE* out = fdopen(dup(1), "w");
    if (!out || !std::freopen("/dev/null", "w", stdout)) {
        return 1;
    }
    const auto rs = readers();

    const int wavAt = int(args.indexOf("--wav"));
    if (wavAt > 0 && wavAt + 1 < args.size()) {
        const std::vector<float> audio = readWav(args[wavAt + 1]);
        if (audio.empty()) {
            std::fprintf(stderr, "could not read %s\n", qPrintable(args[wavAt + 1]));
            return 1;
        }
        std::fprintf(out, "%s: %.0f s\n\n", qPrintable(args[wavAt + 1]), audio.size() / kRate);
        for (const Reader& r : rs) {
            std::fprintf(out, "== %s\n%s\n\n", qPrintable(r.name), qPrintable(normalise(r.run(audio))));
        }
        return 0;
    }

    std::vector<Condition> conds;
    for (const double wpm : {15.0, 25.0, 35.0}) {
        for (const double snr : {10.0, 0.0, -6.0, -10.0, -14.0}) {
            Condition c;
            c.wpm = wpm;
            c.snrDb = snr;
            c.name = QStringLiteral("machine %1 wpm %2 dB").arg(int(wpm)).arg(int(snr));
            conds.push_back(c);
        }
    }
    for (const double snr : {10.0, 0.0, -6.0}) {
        Condition c;
        c.wpm = 20.0;
        c.snrDb = snr;
        c.handSent = true;
        c.name = QStringLiteral("hand fist 20 wpm %1 dB").arg(int(snr));
        conds.push_back(c);
        c.handSent = false;
        c.wpm = 25.0;
        c.fading = true;
        c.driftHz = 30.0;
        c.name = QStringLiteral("QSB + 30 Hz drift 25 wpm %1 dB").arg(int(snr));
        conds.push_back(c);
        c.fading = false;
        c.driftHz = 0.0;
        c.neighbour = true;
        c.name = QStringLiteral("neighbour 180 Hz 25 wpm %1 dB").arg(int(snr));
        conds.push_back(c);
    }

    constexpr int kTrials = 3;
    std::vector<double> sumCer(rs.size(), 0.0), sumCalls(rs.size(), 0.0);
    int rows = 0;

    std::fprintf(out, "Character error rate %% (lower is better) / callsigns copied %% "
                "(higher is better), %d messages per row\n\n", kTrials);
    std::fprintf(out, "%-34s", "condition");
    for (const Reader& r : rs) {
        std::fprintf(out, " | %-15.15s", qPrintable(r.name.mid(r.name.indexOf(' ') + 1)));
    }
    std::fprintf(out, "\n");

    const int onlyAt = int(args.indexOf("--only"));
    const QString only = onlyAt > 0 && onlyAt + 1 < args.size() ? args[onlyAt + 1] : QString();
    for (const Condition& c : conds) {
        if (!only.isEmpty() && !c.name.contains(only)) {
            continue;
        }
        std::vector<double> cer(rs.size(), 0.0), calls(rs.size(), 0.0);
        for (int t = 0; t < kTrials; ++t) {
            std::mt19937 rng(uint32_t(1000 * t + int(c.wpm) * 7 + int(c.snrDb + 50) * 13
                                      + (c.handSent ? 1 : 0) + (c.fading ? 2 : 0)
                                      + (c.neighbour ? 4 : 0)));
            const Qso qso = randomQso(rng);
            const std::vector<float> audio = synth(qso, c, rng);
            for (size_t r = 0; r < rs.size(); ++r) {
                const QString copy = rs[r].run(audio);
                const Score s = score(qso, copy);
                cer[r] += s.cer;
                calls[r] += s.calls;
                if (show) {
                    std::fprintf(out, "   [%s] %s\n      sent: %s\n      copy: %s\n",
                                qPrintable(rs[r].name), qPrintable(c.name),
                                qPrintable(normalise(qso.text)), qPrintable(normalise(copy)));
                }
            }
        }
        std::fprintf(out, "%-34s", qPrintable(c.name));
        for (size_t r = 0; r < rs.size(); ++r) {
            std::fprintf(out, " | %5.0f / %5.0f  ", 100.0 * cer[r] / kTrials, 100.0 * calls[r] / kTrials);
            sumCer[r] += cer[r] / kTrials;
            sumCalls[r] += calls[r] / kTrials;
        }
        std::fprintf(out, "\n");
        std::fflush(out);
        ++rows;
    }
    std::fprintf(out, "%-34s", "AVERAGE");
    for (size_t r = 0; r < rs.size(); ++r) {
        std::fprintf(out, " | %5.0f / %5.0f  ", 100.0 * sumCer[r] / rows, 100.0 * sumCalls[r] / rows);
    }
    std::fprintf(out, "\n\nNoise only (60 s, nothing sent): characters printed\n");

    std::mt19937 rng(42);
    const double sigma = std::sqrt(0.5 / std::pow(10.0, 0.0 / 10.0) * (kRate / 2.0) / 2500.0);
    std::normal_distribution<float> noise(0.0f, float(sigma));
    std::vector<float> x(size_t(60 * kRate));
    for (float& s : x) {
        s = noise(rng);
    }
    std::vector<float> y = cwFilter(x);
    double peak = 0.0;
    for (const float s : y) {
        peak = std::max(peak, double(std::abs(s)));
    }
    for (float& s : y) {
        s *= float(0.5 / peak);
    }
    for (const Reader& r : rs) {
        const QString copy = normalise(r.run(y));
        std::fprintf(out, "  %-28s %4d  %s\n", qPrintable(r.name), int(QString(copy).remove(' ').size()),
                    qPrintable(copy.left(60)));
    }
    return 0;
}
