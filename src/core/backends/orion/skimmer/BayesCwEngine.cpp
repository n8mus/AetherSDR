// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/BayesCwEngine.cpp), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
#include "core/backends/orion/skimmer/BayesCwEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace AetherSDR::orion::skim {

namespace {
constexpr double kTickMs = 0.5;            // 2 ksps envelope cadence

// Level-model spreads (fractions of the level; floors keep the Gaussians
// honest when a tracker sits near zero).
constexpr double kSig0 = 0.40, kSig0Min = 0.03;
constexpr double kSig1 = 0.30, kSig1Min = 0.08;

// Duration-likelihood spreads (log-domain): marks are what the sender
// keys, gaps wobble more in human sending.
constexpr double kSigMark = 0.25;
constexpr double kSigGap  = 0.40;

// Alignment-op prices (log-likelihood units, tuned on the cwtest matrix).
constexpr double kDelBase   = 2.5;   // drop an observed mark (noise blip)
constexpr double kMissBase  = 3.0;   // pattern element never observed
constexpr double kSplitBase = 2.0;   // fade split one element in two
constexpr double kMergeBase = 2.5;   // two elements smeared into one mark
constexpr double kPunctPrior = 1.5;  // punctuation is rarer than letters
constexpr double kGarbagePerMark = 4.5;   // above this average: print '*'

struct PatternDef { const char* rep; char ch; };
const PatternDef kDefs[] = {
    {".-", 'A'},   {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
    {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
    {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
    {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
    {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
    {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
    {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
    {"---..", '8'}, {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'},
    {"-...-", '='},  {".-.-.", '+'},  {"-....-", '-'}, {".--.-.", '@'},
};
} // namespace

struct BayesCwEngine::Pattern {
    char ch;
    int  n;
    double w[7];                           // 1 = dit, 3 = dah
    double prior;                          // added cost
};

namespace {
const std::vector<BayesCwEngine::Pattern>& codebook() {
    static const std::vector<BayesCwEngine::Pattern> t = [] {
        std::vector<BayesCwEngine::Pattern> v;
        for (const auto& d : kDefs) {
            BayesCwEngine::Pattern p{};
            p.ch = d.ch;
            p.n = int(std::char_traits<char>::length(d.rep));
            for (int i = 0; i < p.n; ++i)
                p.w[i] = d.rep[i] == '-' ? 3.0 : 1.0;
            const bool word = (d.ch >= 'A' && d.ch <= 'Z')
                || (d.ch >= '0' && d.ch <= '9');
            p.prior = word ? 0.0 : kPunctPrior;
            v.push_back(p);
        }
        return v;
    }();
    return t;
}
} // namespace

// Level-tracker rate and posterior hysteresis: winners of the 8-point
// replay sweep on the W1AW bulletin capture (2026-08-06) — slower mu1
// and looser/tighter hysteresis all read worse on real air.
constexpr double kMu1Rate = 0.020;
constexpr double kHystOn  = 0.70;
constexpr double kHystOff = 0.30;

BayesCwEngine::BayesCwEngine() { reset(); }
// Tried and rejected (2026-08-06): fldigi's 1/3-dit bitfilter ahead of
// the HMM. The matched filter upstream already band-limits; the extra
// boxcar smeared clean edges ("DE" read "E" on the clean matrix rows)
// and shortened real-capture copy. The HMM wants the crisp envelope.

void BayesCwEngine::reset() {
    pOn_ = 0.0;
    amb_ = 0.5;
    mu0_ = 0.15;
    mu1_ = 0.85;
    key_ = false;
    runMs_ = 0.0;
    metric_ = 0.0;
    ditMs_ = 60.0;
    gapMin_ = 60.0;
    gapInnerMs_ = 60.0;
    for (auto& m : markHist_) m = 0.0f;
    markHistN_ = 0;
    locked_ = false;
    marks_.clear();
    gaps_.clear();
    loose_.clear();
    wordGapSent_ = true;
    lastSpaceMs_ = 0.0;
    lastWpm_ = 0;
}

QString BayesCwEngine::process(float mag) {
    const double x = mag;

    // Two-state forward filter. The persistence prior scales with the dit
    // clock: at 20 WPM an element lasts ~120 ticks, so a state change
    // needs sustained evidence — this IS the debounce.
    const double ps =
        std::clamp(kTickMs / std::max(10.0, ditMs_), 0.001, 0.05);
    const double s0 = std::max(kSig0Min, kSig0 * mu0_);
    const double s1 = std::max(kSig1Min, kSig1 * mu1_);
    const double e0 = (x - mu0_) / s0, e1 = (x - mu1_) / s1;
    // Cap the exponents: a static crash 10 sigma above BOTH models must
    // not produce a likelihood ratio that instantly saturates the state.
    const double l0 = std::exp(-0.5 * std::min(e0 * e0, 40.0)) / s0;
    const double l1 = std::exp(-0.5 * std::min(e1 * e1, 40.0)) / s1;
    const double prOn = pOn_ * (1.0 - ps) + (1.0 - pOn_) * ps;
    const double num = l1 * prOn;
    const double den = num + l0 * (1.0 - prOn);
    pOn_ = den > 1e-300 ? num / den : 0.5;

    // Level learning, each model from its own posterior weight. The
    // signal tracker re-trains fast for the same reason the legacy peak
    // does: stations sharing a frequency alternate at different levels.
    mu0_ += 0.005 * (1.0 - pOn_) * (x - mu0_);
    // Signal tracker: fast enough to re-train on a new station sharing
    // the frequency, slow enough that ionospheric flutter mid-element
    // doesn't drag the model down and chatter the state (replay-found:
    // at 0.020 the W1AW bulletin shredded during fades).
    mu1_ += kMu1Rate * pOn_ * (x - mu1_);
    if (mu0_ < 1e-4) mu0_ = 1e-4;
    if (mu1_ < mu0_ * 1.2) mu1_ = mu0_ * 1.2;

    // SNR metric, calibrated so DEAD-CHANNEL noise reads ~0: the
    // posterior-weighted trackers separate to ~2.2:1 on pure normalized
    // noise (mu1 learns the envelope's upper tail), so that ratio is the
    // zero point, not 1:1. A real station — even the matrix's weakest —
    // sits far above it. Squelch default 12 ≈ a 3.5:1 separation.
    const double inst = std::clamp(
        50.0 * std::log10(std::max(1e-6, mu1_ / (2.0 * mu0_))), 0.0, 100.0);
    metric_ += 0.005 * (inst - metric_);

    // Keying-structure evidence: real CW snaps the posterior between 0
    // and 1 (the level models are far apart), noise leaves it loitering
    // in the middle. The occupancy of that middle band is the cleanest
    // dead-channel discriminator found on the matrix.
    amb_ += 0.002 * ((pOn_ > 0.2 && pOn_ < 0.8 ? 1.0 : 0.0) - amb_);

    // Posterior hysteresis -> runs.
    bool key = key_;
    if (!key_ && pOn_ > kHystOn) key = true;
    if (key_ && pOn_ < kHystOff) key = false;

    QString out;
    if (key != key_) {
        const double ms = runMs_;
        runMs_ = 0.0;
        key_ = key;
        edge(key, ms);
        // Character boundary, decided at the down-edge when the gap's
        // full length is known. The MAP rule between an element gap and
        // a 3x character gap is their geometric mean (sqrt 3) — but
        // measured against the LEARNED element gap, not a 1-dit ideal:
        // real flutter erodes marks and stretches gaps (replay-found on
        // the W1AW bulletin capture: a fixed 1.75-dit boundary shredded
        // solid copy into E/T mush), while a hard 2.5 threshold merged
        // the .20-jitter row into CQQ / DEW / WAW. The tracker follows
        // whichever bias the band is serving tonight.
        if (key && !marks_.empty() && lastSpaceMs_ >= charGapMs())
            out += closeChar();
    } else {
        runMs_ += kTickMs;
        if (!key_) {
            // Late closers, for the tail of a transmission: the down-edge
            // rule above does the work mid-copy. Word gap at the 3-vs-7
            // MAP boundary, sqrt(21) ~ 4.6.
            if (!marks_.empty() && runMs_ > 1.5 * charGapMs())
                out += closeChar();
            if (!wordGapSent_ && runMs_ > 2.65 * charGapMs()) {
                wordGapSent_ = true;
                out += QLatin1Char(' ');
                loose_ += QLatin1Char(' ');
            }
        }
    }
    return out;
}

void BayesCwEngine::edge(bool down, double runMs) {
    // The clock learns ONLY in the presence of a plausible signal —
    // otherwise a dead channel's noise runs walk the dit estimate down
    // until random durations fit the rhythm check and the lock opens
    // (the legacy brain learned the same rule from a live feedback loop).
    const bool credible = mu1_ > 3.0 * mu0_;
    if (down) {
        // A space ended. Short spaces are element gaps = one dit of the
        // sender's clock — the same bootstrap that breaks the high-speed
        // acquisition deadlock in the legacy brain.
        lastSpaceMs_ = runMs;
        if (!marks_.empty() && runMs < charGapMs()) {
            gaps_.push_back(runMs);
            if (credible && runMs > 0.4 * ditMs_ && runMs < 1.6 * ditMs_)
                ditMs_ += 0.10 * (runMs - ditMs_);
            if (credible) {
                gapInnerMs_ += 0.10 * (runMs - gapInnerMs_);
                gapInnerMs_ = std::clamp(gapInnerMs_, 0.6 * ditMs_,
                                         1.8 * ditMs_);
            }
        }
        if (runMs > 16.0 && credible)
            gapMin_ = std::min(runMs, gapMin_ + 0.03 * (120.0 - gapMin_));
        gapMin_ = std::clamp(gapMin_, 20.0, 120.0);
        if (ditMs_ > 2.2 * gapMin_) ditMs_ = gapMin_;
        ditMs_ = std::clamp(ditMs_, 20.0, 240.0);
        return;
    }
    // A mark ended.
    if (runMs > 12.0 * ditMs_) {           // stuck carrier, not keying
        marks_.clear();
        gaps_.clear();
        return;
    }
    // QRN spike gate (fldigi's noiseSpike + legacy's blip-resume), only
    // while the SMOOTHED metric says a real station is present: a
    // sub-half-dit mark inside real copy is a static crash, not keying
    // — drop it and mend the space it interrupted so the character
    // still closes on time. The metric (not the lock) is the right key:
    // it rides stale trackers through fades, where the absorption is
    // needed most, and reads ~0 on a dead channel, where absorbing
    // junk marks would keep phantom locks alive (matrix-found: the
    // lock-keyed version leaked 9 garbage chars). The pop keeps the
    // marks/gaps invariant: the crash's down-edge already pushed the
    // interrupted gap, and the mended silence will push again at the
    // next real mark (sweep-found: without it alignCost's gap indexing
    // skews).
    if (metric_ > squelch_ && runMs < 0.5 * ditMs_) {
        // The rhythm history still sees the crash: pre-filtering the
        // lock's evidence stream made dead-channel noise look rhythmic
        // enough to lock (matrix-found on both gate variants) — the
        // absorption protects the CHARACTER, never the lock's honesty.
        if (runMs >= 0.3 * ditMs_)
            markHist_[markHistN_++ & 7] = float(runMs);
        if (!gaps_.empty() && gaps_.size() >= marks_.size())
            gaps_.pop_back();
        runMs_ = lastSpaceMs_ + runMs;
        key_ = false;
        return;
    }
    if (runMs < 0.30 * ditMs_ && marks_.empty() && !locked_)
        return;                            // pre-lock noise pip
    marks_.push_back(runMs);
    if (marks_.size() > 10) {              // noise flood: start over
        marks_.clear();
        gaps_.clear();
        return;
    }
    // Rhythm lock: durations must cluster on {1,3} x dit — random noise
    // runs don't. Lock acquisition additionally requires real level
    // separation, or a dead channel's occasional rhythmic noise could
    // open the gate (the babble rule has two on-air scars). Sub-element
    // pips stay OUT of the history — the alignment DP prices them at
    // decode time, but two AGC-release pips after a word gap must not
    // poison the rhythm record (matrix-found: a clean W suppressed).
    // The hold gate rides the SMOOTHED metric, not the instant ratio:
    // the level separation legitimately dips during long silences while
    // the front-end AGC releases and normalized noise floats up.
    if (runMs >= 0.3 * ditMs_) markHist_[markHistN_++ & 7] = float(runMs);
    const int hn = std::min(markHistN_, 8);
    if (hn >= 4) {
        int fits = 0;
        for (int k = 0; k < hn; ++k) {
            const float m = markHist_[k];
            if (std::abs(m - ditMs_) < 0.35 * ditMs_) ++fits;
            else if (std::abs(m - 3.0 * ditMs_) < 1.05 * ditMs_) ++fits;
        }
        locked_ = locked_
            ? (fits > hn / 2 && metric_ > squelch_ && amb_ < 0.40)
            : (fits >= (3 * hn) / 4 && mu1_ > 3.5 * mu0_ && amb_ < 0.20);
    }
    // Clock from the classified element (credible signals only). The
    // clamp is physical: 60 WPM is a 20 ms dit, 5 WPM is 240 — a clock
    // outside that range means the tracker is chasing noise, and on a
    // dead channel it walked all the way down to 2 ms before the bound
    // existed (matrix-found via the TTC_BAYESDBG dump).
    // Slow updates: under .20-lognormal jitter a fast EMA swings the
    // clock ±20% and the segmentation thresholds swing with it — the
    // engine survives that row on the stability of its 16-element
    // average, so match that time constant. Once LOCKED, only marks
    // that already fit the clock may move it — QRN crashes are strong
    // enough to pass 'credible' but their durations are random, and on
    // a stormy night they walked a 22 WPM clock to 60 (live-found).
    // Unlocked keeps the coarse dit/dah split: that's what converges
    // from a cold start (sweep-found: fit-gating acquisition starves
    // the tracker and busts the .20-jitter row).
    if (credible) {
        if (locked_) {
            if (std::abs(runMs - ditMs_) < 0.35 * ditMs_)
                ditMs_ += 0.10 * (runMs - ditMs_);
            else if (std::abs(runMs - 3.0 * ditMs_) < 1.05 * ditMs_)
                ditMs_ += 0.10 * (runMs / 3.0 - ditMs_);
        } else if (runMs < 2.0 * ditMs_) {
            if (runMs > 0.4 * ditMs_) ditMs_ += 0.10 * (runMs - ditMs_);
        } else {
            ditMs_ += 0.10 * (runMs / 3.0 - ditMs_);
        }
        ditMs_ = std::clamp(ditMs_, 20.0, 240.0);
    }
}

double BayesCwEngine::alignCost(const Pattern& p) const {
    const double d = ditMs_;
    const int k = int(marks_.size()), n = p.n;
    const auto mark = [&](double m, double w) {
        const double r = std::log(std::max(1.0, m) / (w * d)) / kSigMark;
        return 0.5 * r * r;
    };
    const auto gap = [&](double g) {
        const double r = std::log(std::max(1.0, g) / d) / kSigGap;
        return 0.5 * r * r;
    };
    const auto del = [&](double m) {
        const double over = std::max(0.0, m / d - 0.7);
        return kDelBase + 2.0 * over * over;
    };
    // D[i][j]: first i observed marks explained by first j pattern
    // elements. Ops: match, delete-obs (blip), miss-pattern (truncated),
    // split (one element arrived as two marks), merge (two elements in
    // one mark, the swallowed gap counted as one dit).
    constexpr double kInf = 1e18;
    double D[11][8];
    D[0][0] = 0.0;
    for (int i = 1; i <= k; ++i) D[i][0] = D[i - 1][0] + del(marks_[i - 1]);
    for (int j = 1; j <= n; ++j) D[0][j] = D[0][j - 1] + kMissBase;
    for (int i = 1; i <= k; ++i) {
        for (int j = 1; j <= n; ++j) {
            const double gBefore = i > 1 ? gap(gaps_[i - 2]) : 0.0;
            double best = D[i - 1][j - 1] + mark(marks_[i - 1], p.w[j - 1])
                + gBefore;
            best = std::min(best, D[i - 1][j] + del(marks_[i - 1]));
            best = std::min(best, D[i][j - 1] + kMissBase);
            if (i >= 2) {
                const double g = gaps_[i - 2];
                const double whole = marks_[i - 2] + g + marks_[i - 1];
                const double c = D[i - 2][j - 1]
                    + mark(whole, p.w[j - 1]) + kSplitBase
                    + 2.0 * (g / d) * (g / d);
                best = std::min(best, c);
            }
            if (j >= 2) {
                const double c = D[i - 1][j - 2]
                    + mark(marks_[i - 1], p.w[j - 2] + p.w[j - 1] + 1.0)
                    + kMergeBase + gBefore;
                best = std::min(best, c);
            }
            D[i][j] = best;
        }
    }
    return D[k][n] + p.prior;
}

QString BayesCwEngine::closeChar() {
    const int k = int(marks_.size());
    QString out;
    if (std::getenv("TTC_BAYESDBG") && k > 0 && (!locked_ || metric_ <= squelch_)) {
        std::fprintf(stderr, "[bayes] SUPPRESSED k=%d lock=%d metric %.1f:",
                     k, int(locked_), metric_);
        for (double m : marks_) std::fprintf(stderr, " %.0f", m);
        std::fprintf(stderr, "\n");
    }
    if (k > 0 && locked_ && metric_ > squelch_) {
        double bestC = 1e18;
        char best = 0;
        for (const auto& p : codebook()) {
            if (std::abs(p.n - k) > 2) continue;   // alignment can't bridge
            const double c = alignCost(p);
            if (c < bestC) {
                bestC = c;
                best = p.ch;
            }
        }
        if (best != 0 && bestC <= kGarbagePerMark * k)
            out = QChar::fromLatin1(best);
        else
            out = QStringLiteral("*");
        // Display stream: the closest character regardless of the
        // garbage threshold — the honest '*' is for the call miner.
        loose_ += best != 0 ? QChar::fromLatin1(best) : QChar('*');
        if (std::getenv("TTC_BAYESDBG"))
            std::fprintf(stderr,
                         "[bayes] '%s' cost %.1f k=%d ratio %.2f metric "
                         "%.1f dit %.0f amb %.2f\n",
                         qPrintable(out), bestC, k, mu1_ / mu0_, metric_,
                         ditMs_, amb_);
        wordGapSent_ = false;
        const int wpm = this->wpm();
        if (wpm != lastWpm_ && wpm >= 5 && wpm <= 60) lastWpm_ = wpm;
    }
    marks_.clear();
    gaps_.clear();
    return out;
}

} // namespace AetherSDR::orion::skim
