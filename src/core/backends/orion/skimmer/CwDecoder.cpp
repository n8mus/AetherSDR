// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/CwDecoder.cpp), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
#include "core/backends/orion/skimmer/CwDecoder.h"

#include <QHash>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace AetherSDR::orion::skim {

namespace {
// ITU Morse, plus the prosigns that matter on the air.
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
        {"---...", ':'}, {"-.-.-.", ';'}, {".-..-.", '"'}, {"..--.-", '_'},
        {"-.--.", '('}, {"-.--.-", ')'}, {"...-..-", '$'}, {".-...", '&'},
    };
    return t;
}
} // namespace

CwDecoder::CwDecoder(double inputRate, double offsetHz, QObject* parent)
    : QObject(parent), inputRate_(inputRate) {
    decim_ = static_cast<int>(inputRate / 2000.0);   // -> 2 ksps envelope
    tickMs_ = 1000.0 * decim_ / inputRate;
    if (std::getenv("TTC_CWENGINE")) brain_ = Brain::Engine; // test hooks
    if (std::getenv("TTC_CWBAYES"))  brain_ = Brain::Bayes;
    if (std::getenv("TTC_CWLEGACY")) brain_ = Brain::Legacy;
    if (std::getenv("TTC_CWDEEP"))
        deep_.store(true, std::memory_order_relaxed);
    retune(offsetHz);
}

void CwDecoder::setEnabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
}

void CwDecoder::retune(double offsetHz) {
    pendingOffset_.store(offsetHz, std::memory_order_relaxed);
    retunePending_.store(true, std::memory_order_release);
}

void CwDecoder::setInputRate(double rate) {
    inputRate_ = rate;
    decim_ = static_cast<int>(rate / 2000.0);    // envelope stays 2 ksps
    tickMs_ = 1000.0 * decim_ / rate;
    retune(pendingOffset_.load(std::memory_order_relaxed));
}

void CwDecoder::processIq(const std::complex<float>* d, size_t n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (retunePending_.exchange(false, std::memory_order_acquire)) {
        // Mix the carrier at offsetHz (negative = below the LO) up to DC,
        // and forget everything learned about the previous station.
        const double inc =
            -2.0 * M_PI * pendingOffset_.load(std::memory_order_relaxed)
            / inputRate_;
        loStep_ = {std::cos(inc), std::sin(inc)};
        lo_ = {1.0, 0.0};
        acc_ = {0.0, 0.0};
        accN_ = 0;
        lp1_ = lp2_ = {0.0f, 0.0f};
        afcPrevZ_ = {0.0f, 0.0f};
        afcErrAvg_ = 0.0;
        afcTicks_ = 0;
        afcTotal_.store(0.0, std::memory_order_relaxed);
        env_ = 0.0f;
        peak_ = 0.0f;
        floor_ = 1e-3f;
        key_ = false;
        pendKey_ = false;
        pendTicks_ = 0;
        runMs_ = 0.0;
        ditMs_ = 60.0;
        gapMin_ = 60.0;
        lastSpaceMs_ = 0.0;
        sym_.clear();
        wordGapSent_ = true;
        lastWpm_ = 0;
        for (auto& m : markHist_) m = 0.0f;
        markHistN_ = 0;
        locked_ = false;
        eng_.reset();
        bayes_.reset();
        engWpm_ = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        acc_ += std::complex<double>(d[i]) * lo_;
        lo_ *= loStep_;
        if (++renorm_ >= 4096) {           // keep the phasor on the unit circle
            renorm_ = 0;
            lo_ /= std::abs(lo_);
        }
        if (++accN_ < decim_) continue;
        // one decimated complex sample
        std::complex<float> z(float(acc_.real() / decim_),
                              float(acc_.imag() / decim_));
        acc_ = {0.0, 0.0};
        accN_ = 0;
        // Matched-filter bandwidth from the sender's speed: keying
        // fundamental is 1000/(2*dit) Hz; pass ~2.5x that, each pole at
        // fc, cascaded for ~12 dB/oct skirts. The engine path takes the
        // dit from the fldigi tracker; DEEP narrows the clamp range for
        // weak-signal work (latency and soft edges traded for SNR).
        const bool legacy = brain_ == Brain::Legacy;
        const bool bayes = brain_ == Brain::Bayes;
        const float ditEst = legacy ? float(gapMin_)
            : float(bayes ? bayes_.dotMs() : eng_.dotMs());
        const bool deep = deep_.load(std::memory_order_relaxed);
        static const float kFirLo = [] {   // sweepable floor (TTC_FIRLO)
            const char* v = std::getenv("TTC_FIRLO");
            return v ? float(atof(v)) : 40.0f;
        }();
        const float fc = deep
            ? std::clamp(2400.0f / ditEst, 15.0f, 60.0f)
            : std::clamp(2400.0f / ditEst, kFirLo, 150.0f);
        std::complex<float> s;
        if (legacy) {
            const float a =
                1.0f - std::exp(-2.0f * float(M_PI) * fc / 2000.0f);
            lp1_ += a * (z - lp1_);
            lp2_ += a * (lp1_ - lp2_);
            s = lp2_;
        } else {
            // Engine path: proper windowed-sinc FIR lowpass instead of the
            // one-pole pair. fldigi's front end is a 512-tap FIR brick
            // wall (plus the radio's own crystal filter feeding it); the
            // shallow IIR skirts let the neighborhood leak in and both
            // decode paths busted in the same places on the W1AW replay —
            // the gap to real fldigi was the EARS, not the brain. Taps
            // are recomputed only when the speed-tracked cutoff moves
            // >10% (cheap: ~1/s worst case, never in steady copy).
            if (firTaps_.empty() || fc < firFc_ * 0.9f || fc > firFc_ * 1.1f) {
                firFc_ = fc;
                const int n = 128;
                firTaps_.resize(n);
                float sum = 0.0f;
                for (int k = 0; k < n; ++k) {
                    const float t = k - (n - 1) / 2.0f;
                    const float x = 2.0f * float(M_PI) * fc / 2000.0f * t;
                    const float sinc = std::abs(x) < 1e-6f
                        ? 1.0f : std::sin(x) / x;
                    const float w = 0.54f - 0.46f * std::cos(
                        2.0f * float(M_PI) * k / (n - 1));
                    firTaps_[k] = sinc * w;
                    sum += firTaps_[k];
                }
                for (float& t : firTaps_) t /= sum;
                if (firBuf_.size() != size_t(n))
                    firBuf_.assign(n, {0.0f, 0.0f});
            }
            firBuf_[firPos_] = z;
            std::complex<float> acc(0.0f, 0.0f);
            size_t idx = firPos_;
            for (const float t : firTaps_) {
                acc += t * firBuf_[idx];
                idx = idx == 0 ? firBuf_.size() - 1 : idx - 1;
            }
            firPos_ = (firPos_ + 1) % firBuf_.size();
            s = acc;
        }
        // AFC: while the key is down the filtered sample is a carrier;
        // successive-sample phase advance IS the residual frequency error.
        static const bool kNoAfc = std::getenv("TTC_NOAFC") != nullptr;
        if (!kNoAfc
            && (legacy ? key_ : (bayes ? bayes_.inTone() : eng_.inTone()))) {
            const std::complex<float> d = s * std::conj(afcPrevZ_);
            if (std::abs(d) > 1e-9f) {
                const double errHz =
                    std::arg(d) * (inputRate_ / decim_) / (2.0 * M_PI);
                if (std::abs(errHz) < 150.0)       // in-band only
                    afcErrAvg_ += 0.05 * (errHz - afcErrAvg_);
            }
            if (++afcTicks_ >= 200) {              // nudge every ~100 ms
                afcTicks_ = 0;
                double total =
                    afcTotal_.load(std::memory_order_relaxed);
                double step = std::clamp(afcErrAvg_, -20.0, 20.0);
                step = std::clamp(step, -80.0 - total, 80.0 - total);
                if (std::abs(step) > 0.5) {
                    const double inc = -2.0 * M_PI * step / inputRate_;
                    loStep_ *= std::complex<double>(std::cos(inc),
                                                    std::sin(inc));
                    afcTotal_.store(total + step,
                                    std::memory_order_relaxed);
                    afcErrAvg_ = 0.0;
                }
            }
        }
        afcPrevZ_ = s;
        if (legacy) {
            tick(std::abs(s));
        } else {
            // Receiver-AGC equivalent (the stage real fldigi gets for free
            // from the radio): fast-attack, slow-release peak follower
            // normalizing the envelope, so selective-fading flutter can't
            // dip an element through the slicer mid-character. Replay
            // evidence: both decode paths busted the same words in the
            // same places on tonight's W1AW capture until this was added
            // — the errors tracked the flutter, not the algorithms.
            const float mag = std::abs(s);
            // Release rate is sweepable (TTC_AGCDN) while the Farnsworth
            // fix is being tuned on live W1AW captures.
            static const float kAgcDn = [] {
                const char* v = std::getenv("TTC_AGCDN");
                return v ? float(atof(v)) : 0.0017f;
            }();
            if (mag > agcPk_) agcPk_ += 0.5f * (mag - agcPk_);   // ~1 ms up
            else              agcPk_ += kAgcDn * (mag - agcPk_); // ~300 ms dn
            const float norm = agcPk_ > 1e-6f ? mag / agcPk_ : 0.0f;
            if (std::getenv("TTC_CWENV")) {   // engine-path envelope dump
                static int k = 0;
                std::fprintf(stderr, "[env] %d %.6f %.6f\n", k++, mag, norm);
            }
            const QString out =
                bayes ? bayes_.process(norm) : eng_.process(norm);
            if (!out.isEmpty()) emit textDecoded(out);
            const QString loose =
                bayes ? bayes_.takeLoose() : eng_.takeLoose();
            if (!loose.isEmpty()) emit textLoose(loose);
            const int wpm = bayes ? bayes_.wpm() : eng_.wpm();
            const double metric = bayes ? bayes_.metric() : eng_.metric();
            if (wpm != engWpm_ && wpm >= 5 && wpm <= 60 && metric > 10.0
                && (!bayes || bayes_.locked())) {
                engWpm_ = wpm;
                emit wpmEstimated(wpm);
            }
        }
    }
}

void CwDecoder::tick(float mag) {
    // Envelope smoothing: ~4 ms at 20 WPM, proportionally faster at high
    // speed so 27 ms elements keep crisp edges. Driven by the gap
    // bootstrap, not the dit clock — the clock is exactly what's stuck
    // when a fast station starts up.
    const float envA =
        std::clamp(0.12f * float(60.0 / gapMin_), 0.12f, 0.45f);
    env_ += envA * (mag - env_);
    if (std::getenv("TTC_CWENV")) {        // debug: envelope/slicer state dump
        static int k = 0;
        if (++k % 100 == 0)
            std::fprintf(stderr, "[env] mag %.5f env %.5f peak %.5f floor %.5f key %d\n",
                         mag, env_, peak_, floor_, int(key_));
    }
    // Track key-down level and noise floor — each learns ONLY during its
    // own state. Letting the floor rise during a long dah collapses the
    // SNR gate mid-element and chops it (found via envelope dump: the
    // floor climbed to half the signal within 300 ms of key-down).
    if (key_) {
        if (env_ > peak_) peak_ += 0.20f * (env_ - peak_);
        else              peak_ += 0.04f * (env_ - peak_);   // re-train fast:
        // two stations sharing a frequency alternate at different levels,
        // and thresholds trained on the loud one mush the quiet one into
        // E/T (ground truth: the W0 sign-off both the operator and fldigi
        // struggled with).
    } else {
        if (env_ < floor_) floor_ += 0.20f * (env_ - floor_);
        else               floor_ += 0.01f * (env_ - floor_);
        // A plausible signal onset must charge the peak FAST or the very
        // first element gets bitten off waiting for the SNR gate (C decoded
        // as R, every time). Plausible = well clear of the noise floor —
        // otherwise ordinary noise wiggle pumps the peak up and the gate
        // opens on a dead frequency (live test: an empty spot babbled E's).
        if (env_ > peak_ && env_ > 5.0f * floor_)
            peak_ += 0.20f * (env_ - peak_);
        else
            peak_ += 0.002f * (env_ - peak_);  // QSB/handover: stale peaks let go
    }
    if (floor_ < 1e-6f) floor_ = 1e-6f;

    bool key = key_;
    if (peak_ > 5.0f * floor_) {           // usable SNR only
        const float span = peak_ - floor_;
        const float on  = floor_ + 0.55f * span;
        // Deep off-hysteresis: ionospheric flutter dips the envelope
        // mid-element on real signals (replay: dahs split into dit-pairs)
        // — the off threshold sits well below on.
        const float off = floor_ + 0.28f * span;
        if (!key_ && env_ > on)  key = true;
        if (key_  && env_ < off) key = false;
    } else {
        key = false;
    }
    // QSB return: a signal well clear of the noise floor keys on even if
    // the peak tracker is still holding the pre-fade level (the relative
    // threshold would wait out the whole recovery otherwise). Only when
    // the envelope stays BELOW that stale peak though — an excursion past
    // it is a static crash, not a returning station, and keying on QRN
    // impulses is exactly the babble v1 was cured of (live-found: every
    // hunting skimmer channel "decoding" 30-55 WPM noise).
    // (env must also reach a fair fraction of the known signal level —
    // 8x a clean channel's tiny noise floor is ~2% of the signal, and
    // that fired on every element's rise/decay tail, chattering against
    // the hysteresis slicer at each edge. The blip guard used to mask it;
    // the debounce exposed it as 0% on the whole matrix.)
    if (!key_ && env_ > 8.0f * floor_ && env_ > 0.25f * peak_
        && env_ < 1.1f * peak_ && peak_ > 8.0f * floor_)
        key = true;

    // Debounce: the tentative state must hold ~6 ms (but never more than
    // a third of a dit) before it becomes an edge. Chatter pips and dips
    // shorter than that are absorbed into the run they interrupted.
    if (key != key_) {
        if (key != pendKey_) { pendKey_ = key; pendTicks_ = 0; }
        const int need = std::max(
            4, std::min(12, int(0.3 * ditMs_ / tickMs_)));
        if (++pendTicks_ < need) key = key_;   // not yet: hold state
    } else {
        pendKey_ = key_;
        pendTicks_ = 0;
    }

    if (key == key_) {
        runMs_ += tickMs_;
        // Long silence closes out the pending character and word.
        if (!key_ && !sym_.isEmpty() && runMs_ > 2.5 * ditMs_) emitSymbol();
        if (!key_ && !wordGapSent_ && runMs_ > 6.0 * ditMs_) {
            wordGapSent_ = true;
            emit textDecoded(" ");
        }
        return;
    }
    // Edge: classify the run that just ended.
    const double ms = runMs_;
    runMs_ = 0.0;
    key_ = key;
    if (key) {
        // A space ended. Short spaces are element gaps = exactly one dit —
        // they carry the sender's dit clock even when the marks are being
        // misclassified, which is what breaks the 35 WPM acquisition trap
        // (dahs captured as dits keep the estimate high; the 34 ms gaps
        // pull it right back down).
        if (ms > 0.4 * ditMs_ && ms < 2.0 * ditMs_)
            ditMs_ += 0.35 * (ms - ditMs_);
        // Gap bootstrap: snap down to any credible shorter gap, relax
        // slowly upward so a speed drop is followed too. Learns ONLY in
        // the presence of a real signal — noise chatter making the gaps
        // look short would switch in the wide fast filters and admit
        // even more noise (live-found feedback loop on empty channels).
        if (ms > 8.0 && peak_ > 6.0f * floor_)
            gapMin_ = std::min(ms, gapMin_ + 0.03 * (120.0 - gapMin_));
        // Merged-mark pollution escape: if the dit clock sits way above
        // the observed element gap, the marks it learned from were
        // smeared pairs — resync to the sender's real clock.
        if (ditMs_ > 2.2 * gapMin_) ditMs_ = gapMin_;
        lastSpaceMs_ = ms;
        return;
    }
    // A mark ended: dit or dah, and adapt the dit clock.
    if (ms < 0.35 * ditMs_) {
        // Impulse blip, not an element — and it must not SPLIT the gap it
        // landed in, or characters never close in noise: resume the space
        // as if the blip never happened.
        runMs_ = lastSpaceMs_ + ms;
        return;
    }
    // Consistency squelch bookkeeping (see header). Lock acquires at 3/4
    // of recent marks fitting the clock and only lets go below half —
    // without the hysteresis, borderline real signals (flutter, QSB)
    // dropped characters mid-copy (replay: "K4MW" fragmenting to "4MW").
    markHist_[markHistN_++ & 7] = float(ms);
    const int hn = std::min(markHistN_, 8);
    if (hn >= 4) {
        int fits = 0;
        for (int k = 0; k < hn; ++k) {
            const float m = markHist_[k];
            if (std::abs(m - ditMs_) < 0.35 * ditMs_) ++fits;
            else if (std::abs(m - 3.0 * ditMs_) < 1.05 * ditMs_) ++fits;
        }
        locked_ = locked_ ? (fits > hn / 2) : (fits >= (3 * hn) / 4);
    }
    if (ms < 2.0 * ditMs_) {
        sym_ += '.';
        if (ms > 0.4 * ditMs_) {           // too short to trust for timing
            ditMs_ += 0.35 * (ms - ditMs_);
            const int wpm = int(std::lround(1200.0 / ditMs_));
            if (wpm != lastWpm_ && wpm >= 5 && wpm <= 60) {
                lastWpm_ = wpm;
                emit wpmEstimated(wpm);
            }
        }
    } else {
        sym_ += '-';
        ditMs_ += 0.35 * (ms / 3.0 - ditMs_);
    }
    if (sym_.size() > 8) sym_.clear();     // garbage guard
}

void CwDecoder::emitSymbol() {
    const auto it = morseTable().constFind(sym_);
    sym_.clear();
    if (!locked_) return;                  // consistency squelch closed
    wordGapSent_ = false;
    if (it != morseTable().constEnd())
        emit textDecoded(QString(it.value()));
    else
        emit textDecoded("*");             // unreadable character
}

} // namespace AetherSDR::orion::skim
