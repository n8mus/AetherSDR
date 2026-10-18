// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from the N8EM Orion console (src/cw/FldigiCwEngine.h), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
//
// CW decode engine ported from fldigi's src/cw/cw.cxx (fldigi 4.2.12),
// Copyright (C) 2006-2021 Dave Freese W1HKJ and the fldigi contributors;
// speed-tracking idea by Lawrence Glaister VE7IT; SOM fuzzy character
// matching by Mauri Niininen AG1LE. Adapted for this console by feeding it
// magnitude samples from the SDR IQ chain (2 ksps) instead of fldigi's
// audio path (500/s post-decimation) — all time constants are scaled
// accordingly, the decode logic itself is a faithful port.
//
// This file is deliberately self-contained so it can be removed cleanly:
// delete it and switch CwDecoder back to its legacy tick() path (env
// TTC_CWLEGACY=1 selects legacy at runtime for A/B testing).
#pragma once
#include <QString>
#include <string>
#include <vector>

namespace AetherSDR::orion::skim {

class FldigiCwEngine {
public:
    FldigiCwEngine();

    void reset();                          // retune/idle: forget everything

    // One magnitude sample at 2 ksps. Returns decoded text (usually empty,
    // sometimes one character or a space).
    QString process(float mag);

    // Companion best-guess stream for DISPLAY: the SOM (fuzzy) winner
    // for the same character events process() just returned. The
    // skimmer's call miner needs honest '*' markers (SOM never prints
    // junk, and a quiet band's noise once assembled a cty-valid phantom
    // through it), but the human-facing waterfall/menu text wants the
    // closest letter — the way the CW window reads. One decode, two
    // streams; drain after each process() call.
    QString takeLoose() {
        QString t;
        t.swap(loose_);
        return t;
    }

    // fldigi's CW config knobs, same value ranges.
    void setSom(bool on) { useSom_ = on; }
    void setAttack(int idx);               // 0 slow, 1 normal, 2 fast
    void setDecay(int idx);
    // Squelch on the fldigi signal metric (0..100); keying events are
    // ignored below it. fldigi leaves this to a UI slider; the skimmer's
    // hard "dead channel stays quiet" rule needs a sane default here.
    void setSquelch(double s) { squelch_ = s; }

    bool   inTone() const { return state_ == State::InTone; }
    double dotMs() const { return twoDots_ / 2.0; }
    int    wpm() const { return int(1200.0 / (twoDots_ / 2.0) + 0.5); }
    double metric() const { return metric_; }

private:
    enum class State { Idle, InTone, AfterTone };

    QString handleQuery();                 // silence timing -> char / space
    void    markEnded(double ms);          // classify + track speed
    QString lookup() const;                // hard table lookup of repBuf_
    QString somWinner() const;             // fuzzy duration-vector match

    // trackers (fldigi decode_stream)
    double sigAvg_ = 0.0, noiseFloor_ = 0.0, agcPeak_ = 0.0;
    double metric_ = 0.0;
    double squelch_ = 12.0;   // metric gate; operator-adjustable (SQL slider)
    int    attack_ = 800, decay_ = 4000;   // decayavg weights at 2 ksps
    bool   useSom_ = true;

    // bit filter: moving average over ~1/3 dot (fldigi's bitfilter)
    std::vector<float> bitBuf_;
    size_t bitPtr_ = 0;
    double bitSum_ = 0.0;
    void   setBitLen(int n);

    // element state machine (fldigi handle_event)
    State  state_ = State::Idle;
    double tMs_ = 0.0;                     // running clock
    double toneStart_ = 0.0, toneEnd_ = 0.0;
    double lastElement_ = 0.0;
    bool   spaceSent_ = true;

    // speed tracking: two_dots through a 16-tap moving average of valid
    // dot/dash pairs (VE7IT's 3:1 ratio rule)
    double twoDots_ = 2.0 * 1200.0 / 18.0; // start at 18 WPM, like fldigi
    std::vector<double> track_;
    size_t trackPtr_ = 0;

    // per-character element buffers
    std::string repBuf_;                   // '.'/'-' representation
    std::vector<double> durBuf_;           // element durations, ms (SOM)
    QString loose_;                        // display stream (see takeLoose)
};

} // namespace AetherSDR::orion::skim
