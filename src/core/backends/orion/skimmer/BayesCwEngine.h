// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/BayesCwEngine.h), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
//
// Bayesian CW decode engine — the third brain (legacy slicer and the
// ported fldigi engine are the other two; see CwDecoder.h). Two ideas,
// both borrowed from how CW Skimmer reads a waterfall instead of a
// comparator:
//
//  1. Soft keying: a two-state HMM forward filter over the envelope.
//     Each 0.5 ms sample updates P(key-down) from adaptive signal/noise
//     level models; the state flips on posterior, not on a threshold
//     crossing, so a flutter dip has to hold long enough to out-argue
//     the persistence prior before it splits an element. Debounce,
//     hysteresis and blip rejection stop being tuned constants and
//     become one probability.
//
//  2. Whole-character inference: at each character boundary the observed
//     mark AND gap durations are scored against every Morse pattern with
//     a small alignment DP — an element may be split by a fade (two
//     short marks re-merge), a noise blip may be deleted, two elements
//     smeared together may be re-split — each op priced in log-
//     likelihood. The winner is the maximum-posterior character over
//     everything that was heard, not a hard classify-then-lookup of
//     each element on its own. The gaps carrying the sender's dit clock
//     is also why this tracks sloppy sending.
//
// Emission is gated like the other brains have learned the hard way:
// a rhythm lock plus an SNR squelch, and unreadable characters print
// '*' (the skimmer's call miner needs honest junk markers). Dead
// channels must stay quiet — that rule has two on-air scars.
#pragma once
#include <QString>
#include <algorithm>
#include <string>
#include <vector>

namespace AetherSDR::orion::skim {

class BayesCwEngine {
public:
    BayesCwEngine();

    void reset();                          // retune/idle: forget everything

    // One magnitude sample at 2 ksps (normalized by the caller's AGC).
    // Returns decoded text (usually empty, sometimes a char or a space).
    QString process(float mag);

    void   setSquelch(double s) { squelch_ = s; }
    // Display stream: best-guess characters without the garbage
    // threshold (see FldigiCwEngine::takeLoose — same contract).
    QString takeLoose() {
        QString t;
        t.swap(loose_);
        return t;
    }
    bool   inTone() const { return key_; }
    double dotMs() const { return ditMs_; }
    int    wpm() const { return int(1200.0 / ditMs_ + 0.5); }
    double metric() const { return metric_; }
    // Rhythm lock: WPM readouts are only meaningful when this is up —
    // credibility flickers on a noise channel walk the clock to its
    // clamp and would report a phantom 60 WPM (replay-found).
    bool   locked() const { return locked_; }

    struct Pattern;                        // codebook entry (see .cpp)

private:
    void    edge(bool down, double runMs); // a run just ended
    QString closeChar();                   // decode marks_/gaps_ -> char
    double  alignCost(const Pattern& p) const;
    // Character-gap threshold: sqrt(3) above the learned element gap.
    double  charGapMs() const {
        return 1.75 * std::max(ditMs_, gapInnerMs_);
    }

    // --- soft keying -------------------------------------------------
    double pOn_ = 0.0;                     // forward posterior of key-down
    double mu0_ = 0.15, mu1_ = 0.85;       // adaptive noise/signal levels
    bool   key_ = false;                   // hysteresis on the posterior
    double runMs_ = 0.0;                   // current run length
    double metric_ = 0.0;                  // 0..100 SNR-ish, fldigi scale
    double squelch_ = 12.0;
    double amb_ = 0.5;                     // posterior-ambiguity occupancy

    // --- clock + rhythm lock ------------------------------------------
    double ditMs_ = 60.0;                  // adaptive dit clock
    double gapMin_ = 60.0;                 // decaying min of credible gaps
    double gapInnerMs_ = 60.0;             // learned element-gap length
    float  markHist_[8] = {};
    int    markHistN_ = 0;
    bool   locked_ = false;

    // --- per-character observation ------------------------------------
    std::vector<double> marks_;            // element durations, ms
    std::vector<double> gaps_;             // gaps BETWEEN those marks, ms
    QString loose_;                        // display stream (takeLoose)
    bool   wordGapSent_ = true;
    double lastSpaceMs_ = 0.0;
    int    lastWpm_ = 0;
};

} // namespace AetherSDR::orion::skim
