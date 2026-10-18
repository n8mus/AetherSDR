// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/SkimmerEngine.h), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
#pragma once
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace AetherSDR::orion::skim {

class CwDecoder;

// CW skimmer: a bank of CwDecoder channels parked on the strongest peaks
// inside the band's CW segment, decoding all of them at once and mining
// the text for callsigns. Found calls surface as panadapter spots (kind
// 'S'), colored by worked-before status like any cluster spot.
//
// Threading: processIq runs on the SDR streaming thread and only touches
// the decoders (each gated by its own atomic). Everything else — peak
// assignment from the averaged spectrum, decoded-text bookkeeping,
// callsign extraction — happens on the GUI thread (decoder signals cross
// over as queued connections). Channel retunes go through CwDecoder's
// thread-safe retune().
class SkimmerEngine : public QObject {
    Q_OBJECT
public:
    // channels: size of the decoder bank. Each channel costs one complex
    // multiply per input sample (the recurrence-phasor mixer), so even a
    // few dozen are cheap at 500 ksps.
    explicit SkimmerEngine(double inputRate, int channels = 8,
                           QObject* parent = nullptr);

    int channelCount() const { return int(ch_.size()); }

    void setEnabled(bool on);              // gates the whole bank
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Rescale the whole bank for a different stream rate (capture-file
    // replay of the old 500 ksps era). Before streaming starts only.
    void setInputRate(double rate);

    // Flip every channel between the fldigi engine (the default brain)
    // and the Bayesian one — the SKIM menu's experimental toggle, so the
    // operator can A/B on the air without env vars.
    void setBayes(bool on);

    // The band's practical CW window (RBN convention, stops below the FT8
    // hotspot); false when the dial isn't near one. Shared with the SKIM
    // view so both agree on what "the CW segment" means.
    static bool cwSegment(qint64 dialHz, qint64& lo, qint64& hi);

    // Optional callsign sanity check (the console wires this to cty.dat:
    // a "call" whose prefix maps to no country is a decode artifact).
    void setCallValidator(std::function<bool(const QString&)> v) {
        validate_ = std::move(v);
    }

    // Optional known-call list (MASTER.SCP): listed calls confirm on 2
    // sightings, unlisted need 3 — casual operators aren't in a contest
    // super-check file, so this is a confidence tier, not a whitelist.
    void setKnownCalls(QSet<QString> calls) { known_ = std::move(calls); }

    // SDR streaming thread: fan the block out to the active channels.
    void processIq(const std::complex<float>* d, size_t n);

    // GUI thread, ~every few seconds: (re)assign channels from the latest
    // averaged spectrum. dialHz is VFO A; loOffsetHz is the capture LO's
    // offset above the dial (bin n/2 sits at dial+loOffsetHz).
    void updateFromSpectrum(const std::vector<float>& db, int spanHz,
                            qint64 dialHz, int loOffsetHz);

    // Live snapshot for the SKIM menu readout and the waterfall labels.
    // text is the DISPLAY stream (best-guess characters, reads like the
    // CW window); the call miner works on the honest stream internally.
    struct ChanInfo {
        bool    active = false;
        qint64  hz = 0;                    // absolute carrier frequency
        int     wpm = 0;
        QString call;                      // empty until extracted
        QString text;                      // recent decoded tail
    };
    QVector<ChanInfo> channelInfo() const;

    // Skimmer spots for the panadapter (pruned to the last 10 minutes).
    struct SkimSpot {
        QString call;
        qint64  hz = 0;
        qint64  atSecs = 0;
        int     wpm = 0;
    };
    const QVector<SkimSpot>& spots();

signals:
    void spotFound(const QString& call, qint64 hz, int wpm);
    void spotsChanged();

private:
    struct Chan {
        CwDecoder* dec = nullptr;
        bool    active = false;
        qint64  hz = 0;                    // absolute assigned frequency
        int     wpm = 0;
        QString call;
        QString candidate;                 // current unconfirmed call
        int     candCount = 0;             // distinct sightings of it
        QString text;                      // rolling decode buffer (honest)
        QString show;                      // rolling display buffer (SOM)
        qint64  lastCharMs = 0;            // activity clock (ms epoch)
        qint64  assignedMs = 0;
        // WPM samples at the last few COUNTED sightings of the candidate.
        // A real sender holds a near-constant clock across re-sends; junk
        // swings (2026-08-07: phantom E3MT re-announced 42/60/58/42 WPM;
        // SN1N replayed 28-48). A WINDOW of the newest three, not a
        // lifetime min/max — the lifetime version latched on one decoder
        // warm-up transient and then blocked a real unlisted station for
        // as long as it kept transmitting (max-review find, 2026-08-08).
        int     candWpm[3] = {0, 0, 0};    // newest first
        quint8  candWpmN = 0;              // valid samples (saturates at 3)
    };

    void onText(int idx, const QString& t);
    void extractCall(Chan& c, bool wordDone);
    void freeChannel(Chan& c);

    std::atomic<bool> enabled_{false};
    double inputRate_;
    std::vector<Chan> ch_;                 // fixed size after construction
    QVector<SkimSpot> spots_;
    std::function<bool(const QString&)> validate_;
    QSet<QString> known_;                  // MASTER.SCP (empty = all known)
    std::vector<float> specAvg_;           // EMA across assignment passes
    qint64 lastDial_ = 0;
};

} // namespace AetherSDR::orion::skim
