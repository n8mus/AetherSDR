// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from the N8EM Orion console (src/cw/FldigiCwEngine.cpp), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
// Ported from fldigi src/cw/cw.cxx — see FldigiCwEngine.h for provenance.
#include "core/backends/orion/skimmer/FldigiCwEngine.h"

#include <QHash>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace AetherSDR::orion::skim {

namespace {
constexpr double kRateHz = 2000.0;         // our sample cadence into process()
constexpr double kTickMs = 1000.0 / kRateHz;
constexpr int    kMaxElements = 6;         // fldigi MAX_MORSE_ELEMENTS
constexpr int    kTrackLen = 16;           // fldigi TRACKING_FILTER_SIZE
constexpr double kMinDotMs = 6.0;          // fldigi KWPM/200 (200 WPM)
constexpr double kMaxDashMs = 720.0;       // fldigi 3 dots at 5 WPM

// fldigi's decayavg: avg += (value - avg) / weight.
inline double decayavg(double avg, double value, int weight) {
    return avg + (value - avg) / std::max(1, weight);
}

const QHash<QString, QChar>& morseTable() {
    static const QHash<QString, QChar> t = {
        {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'}, {"..", 'I'}, {".---", 'J'},
        {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'}, {"-.", 'N'}, {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'},
        {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
        {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
        {"---..", '8'}, {"----.", '9'},
        {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'},
        {"-...-", '='}, {".-.-.", '+'}, {"-....-", '-'}, {".--.-.", '@'},
    };
    return t;
}

// SOM codebook generated from the same table: dit weight 0.33, dah 1.0,
// zero-padded to 6 elements (fldigi's som_table holds the same values as
// literals; generating them keeps one source of truth).
struct SomEntry { QChar ch; float wgt[kMaxElements]; int len; };
const std::vector<SomEntry>& somTable() {
    static const std::vector<SomEntry> t = [] {
        std::vector<SomEntry> v;
        for (auto it = morseTable().constBegin(); it != morseTable().constEnd();
             ++it) {
            const QString& rep = it.key();
            if (rep.size() > kMaxElements) continue;
            SomEntry e{};
            e.ch = it.value();
            e.len = rep.size();
            for (int i = 0; i < kMaxElements; ++i)
                e.wgt[i] = i < rep.size() ? (rep[i] == '-' ? 1.0f : 0.33f)
                                          : 0.0f;
            v.push_back(e);
        }
        return v;
    }();
    return t;
}
} // namespace

FldigiCwEngine::FldigiCwEngine() {
    track_.assign(kTrackLen, twoDots_);
    setBitLen(int(twoDots_ / 2.0 / 3.0 / kTickMs));
    reset();
}

void FldigiCwEngine::setAttack(int idx) {
    // fldigi: {400,200,100} at 500/s -> x4 at our 2 ksps cadence.
    attack_ = idx == 0 ? 1600 : idx == 2 ? 400 : 800;
}

void FldigiCwEngine::setDecay(int idx) {
    decay_ = idx == 0 ? 8000 : idx == 2 ? 2000 : 4000;
}

void FldigiCwEngine::setBitLen(int n) {
    n = std::max(1, n);
    if (int(bitBuf_.size()) == n) return;
    bitBuf_.assign(n, 0.0f);
    bitPtr_ = 0;
    bitSum_ = 0.0;
}

void FldigiCwEngine::reset() {
    sigAvg_ = noiseFloor_ = agcPeak_ = 0.0;
    metric_ = 0.0;
    state_ = State::Idle;
    tMs_ = toneStart_ = toneEnd_ = 0.0;
    lastElement_ = 0.0;
    spaceSent_ = true;
    repBuf_.clear();
    durBuf_.clear();
    loose_.clear();
    std::fill(bitBuf_.begin(), bitBuf_.end(), 0.0f);
    bitSum_ = 0.0;
}

QString FldigiCwEngine::lookup() const {
    const auto it = morseTable().constFind(QString::fromStdString(repBuf_));
    return it != morseTable().constEnd() ? QString(it.value())
                                         : QStringLiteral("*");
}

QString FldigiCwEngine::somWinner() const {
    // fldigi cw::normalize + find_winner: scale the duration vector so the
    // longest element maps to 1.0 if it's dah-length (> two_dots), else to
    // 0.33; nearest codebook entry by Euclidean distance wins. The whole
    // character is judged at once — one smeared element can't bust it.
    if (durBuf_.empty()) return QStringLiteral(" ");
    float v[kMaxElements] = {};
    double maxDur = 0.0;
    for (size_t i = 0; i < durBuf_.size() && i < kMaxElements; ++i)
        maxDur = std::max(maxDur, durBuf_[i]);
    if (maxDur <= 0.0) return QStringLiteral(" ");
    const double ratio = (maxDur > twoDots_ ? 1.0 : 0.33) / maxDur;
    for (size_t i = 0; i < durBuf_.size() && i < kMaxElements; ++i)
        v[i] = float(durBuf_[i] * ratio);
    double bestD = 1e30;
    QChar best;
    for (const auto& e : somTable()) {
        double d = 0.0;
        for (int i = 0; i < kMaxElements; ++i) {
            const double diff = v[i] - e.wgt[i];
            d += diff * diff;
            if (d > bestD) break;
        }
        if (d < bestD) { bestD = d; best = e.ch; }
    }
    return best.isNull() ? QStringLiteral("*") : QString(best);
}

void FldigiCwEngine::markEnded(double ms) {
    // fldigi CW_KEYUP_EVENT: spike gate, speed tracking on 3:1 pairs,
    // dit/dah split at two_dots.
    const double noiseSpike = twoDots_ / 4.0;   // dot/2
    if (ms < noiseSpike) {
        state_ = State::Idle;
        return;
    }
    if (lastElement_ > 0.0) {
        double d1 = 0.0, d2 = 0.0;
        if (ms > 2.0 * lastElement_ && ms < 4.0 * lastElement_) {
            d1 = lastElement_; d2 = ms;
        } else if (lastElement_ > 2.0 * ms && lastElement_ < 4.0 * ms) {
            d1 = ms; d2 = lastElement_;
        }
        if (d1 > kMinDotMs && d2 < kMaxDashMs && d2 > 0.0) {
            track_[trackPtr_++ % kTrackLen] = (d1 + d2) / 2.0;
            double s = 0.0;
            for (double v : track_) s += v;
            twoDots_ = s / kTrackLen;
            setBitLen(int(twoDots_ / 2.0 / 3.0 / kTickMs));
        }
    }
    lastElement_ = ms;
    repBuf_ += ms <= twoDots_ ? '.' : '-';
    durBuf_.push_back(ms);
    if (int(repBuf_.size()) > kMaxElements) {   // noise flood: start over
        state_ = State::Idle;
        repBuf_.clear();
        durBuf_.clear();
        return;
    }
    state_ = State::AfterTone;
}

QString FldigiCwEngine::handleQuery() {
    // fldigi CW_QUERY_EVENT: 2..4 dots of silence closes the character,
    // longer than 4 dots emits one word space. The in-tone guard is
    // fldigi's first line and it is LOAD-BEARING: without it the stale
    // silence clock keeps counting through a long dash and fires a word
    // space mid-tone — probe signature: a space before every character
    // that starts with a dash ("C Q", "N 8EM").
    if (state_ == State::InTone) return {};
    const double dot = twoDots_ / 2.0;
    const double silence = tMs_ - toneEnd_;
    if (silence < 2.0 * dot) return {};
    if (silence <= 4.0 * dot && state_ == State::AfterTone) {
        const QString som = somWinner();
        const QString sc = useSom_ ? som : lookup();
        loose_ += som;                     // display always gets the guess
        repBuf_.clear();
        durBuf_.clear();
        state_ = State::Idle;
        spaceSent_ = false;
        return sc;
    }
    if (silence > 4.0 * dot && !spaceSent_) {
        spaceSent_ = true;
        loose_ += QLatin1Char(' ');
        return QStringLiteral(" ");
    }
    return {};
}

QString FldigiCwEngine::process(float mag) {
    tMs_ += kTickMs;
    // bitfilter: moving average over ~1/3 dot (fldigi runs this on the
    // rectified filter output before the trackers).
    bitSum_ += mag - bitBuf_[bitPtr_];
    bitBuf_[bitPtr_] = mag;
    if (++bitPtr_ >= bitBuf_.size()) bitPtr_ = 0;
    const double value = std::max(0.0, bitSum_ / double(bitBuf_.size()));

    // decode_stream trackers, verbatim semantics.
    sigAvg_ = decayavg(sigAvg_, 0.5 * value,
                       value > sigAvg_ ? attack_ : decay_);
    if (value < sigAvg_) {
        noiseFloor_ = decayavg(noiseFloor_, value,
                               value < noiseFloor_ ? attack_ : decay_);
    }
    if (value > sigAvg_) {
        agcPeak_ = decayavg(agcPeak_, value,
                            value > agcPeak_ ? attack_ : decay_);
    }
    double normValue = noiseFloor_, normSig = noiseFloor_,
           normNoise = noiseFloor_;
    if (agcPeak_ > 0.0) {
        normValue = value / agcPeak_;
        normSig = sigAvg_ / agcPeak_;
        normNoise = noiseFloor_ / agcPeak_;
    }
    metric_ *= 0.8;
    if (normNoise > 1e-4 && noiseFloor_ < sigAvg_)
        metric_ += 0.2 * std::clamp(2.5 * 20.0 * std::log10(normSig / normNoise),
                                    0.0, 100.0);

    // hysteresis detector around the signal average (fldigi: ±5%).
    // Dominance gate (our divergence from the port, sweepable via
    // TTC_DOMGATE while it earns its keep): a tone may only START above
    // this fraction of the AGC peak — i.e. of the STRONG station's
    // level. A co-channel interferer 6-9 dB down keys the stock
    // detector in the strong station's gaps (live-found: a practice
    // along-sender zero-beat under W1AW shredded 27 dB copy); normValue
    // is already value/agcPeak_, so the gate is a direct fraction, and
    // a lone weak station still normalizes to ~1 and passes.
    static const double kDom = [] {
        const char* v = std::getenv("TTC_DOMGATE");
        return v ? atof(v) : 0.0;
    }();
    const double upper = 1.05 * normSig, lower = 0.95 * normSig;
    const double startThr = std::max(upper, kDom);
    if (metric_ > squelch_) {
        if (normValue > startThr && state_ != State::InTone) {
            if (state_ == State::Idle) {
                repBuf_.clear();
                durBuf_.clear();
            }
            toneStart_ = tMs_;
            state_ = State::InTone;
        } else if (normValue < lower && state_ == State::InTone) {
            toneEnd_ = tMs_;
            markEnded(toneEnd_ - toneStart_);
        }
    }
    return handleQuery();
}

} // namespace AetherSDR::orion::skim
