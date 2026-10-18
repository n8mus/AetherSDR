// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/SkimmerEngine.cpp), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
#include "core/backends/orion/skimmer/SkimmerEngine.h"
#include "core/backends/orion/skimmer/CwDecoder.h"

#include <QDateTime>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace AetherSDR::orion::skim {

namespace {
// Practical US CW windows (where straight CW actually happens — RBN
// convention), each stopping BELOW the band's FT8/digital hotspot. The
// first table used the full license allocations (40 m up to 7.125 etc.)
// and the skimmer parked channels on the 7.074 FT8 blob all evening
// (operator report: "on 40 m it is going way higher — wasted channels").
struct Seg { qint64 lo, hi; };
const Seg kCwSegs[] = {
    {1800000, 1840000},   // 160 m
    {3500000, 3570000},   // 80 m   (FT8 3.573)
    {7000000, 7070000},   // 40 m   (FT8 7.074)
    {10100000, 10130000}, // 30 m   (FT8 10.136)
    {14000000, 14070000}, // 20 m   (FT8 14.074)
    {18068000, 18095000}, // 17 m   (FT8 18.100)
    {21000000, 21070000}, // 15 m   (FT8 21.074)
    {24890000, 24912000}, // 12 m   (FT8 24.915)
    {28000000, 28070000}, // 10 m   (FT8 28.074)
    {50050000, 50100000}, // 6 m
};

// Callsign shape: 1-2 letter (or letter-digit) prefix, area digit(s),
// 1-4 letter suffix. Deliberately loose — the cty.dat validator and the
// seen-twice/after-DE rule do the real filtering.
const QRegularExpression& callRe() {
    static const QRegularExpression re(
        QStringLiteral("^(?:[A-Z]{1,2}|[0-9][A-Z]|[A-Z][0-9])[0-9]{1,2}"
                       "[A-Z]{1,4}$"));
    return re;
}
} // namespace

SkimmerEngine::SkimmerEngine(double inputRate, int channels, QObject* parent)
    : QObject(parent), inputRate_(inputRate) {
    ch_.resize(std::clamp(channels, 1, 64));
    for (int i = 0; i < int(ch_.size()); ++i) {
        ch_[i].dec = new CwDecoder(inputRate, 0.0, this);
        // SOM stays OFF on skimmer channels: the fuzzy matcher never
        // outputs '*' — every noise burst becomes a plausible letter, and
        // a quiet band's noise ridges assembled themselves into a
        // cty-valid phantom that passed confirmation 107 times in one
        // replay (UT3TUA, 2026-07-16 capture). The call miner wants
        // honest junk markers; the CW window keeps SOM for human eyes.
        ch_[i].dec->setSom(false);
        connect(ch_[i].dec, &CwDecoder::textDecoded, this,
                [this, i](const QString& t) { onText(i, t); },
                Qt::QueuedConnection);
        // Display twin: the SOM best-guess stream reads like the CW
        // window; the miner above keeps its honest '*' markers.
        connect(ch_[i].dec, &CwDecoder::textLoose, this,
                [this, i](const QString& t) {
                    Chan& c = ch_[i];
                    if (!c.active) return;
                    c.show += t;
                    if (c.show.size() > 160) c.show = c.show.right(120);
                },
                Qt::QueuedConnection);
        connect(ch_[i].dec, &CwDecoder::wpmEstimated, this,
                [this, i](int wpm) {
                    Chan& c = ch_[i];
                    if (!c.active) return;        // late queued delivery
                    // Free+reclaim inside one spectrum pass re-arms active
                    // before the OLD occupant's queued estimate lands; a
                    // real estimate needs ~1 s of copy, so anything this
                    // early belongs to the previous tenancy. Drop it.
                    if (QDateTime::currentMSecsSinceEpoch() - c.assignedMs
                        < 300) return;
                    c.wpm = wpm;
                },
                Qt::QueuedConnection);
    }
}

void SkimmerEngine::setEnabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
    if (!on)
        for (auto& c : ch_) freeChannel(c);
}

void SkimmerEngine::setInputRate(double rate) {
    inputRate_ = rate;
    for (auto& c : ch_) c.dec->setInputRate(rate);
}

void SkimmerEngine::setBayes(bool on) {
    for (auto& c : ch_)
        c.dec->setBrain(on ? CwDecoder::Brain::Bayes
                           : CwDecoder::Brain::Engine);
}

void SkimmerEngine::processIq(const std::complex<float>* d, size_t n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    // Channels are independent (own mixer, own brain, queued signals
    // out) — spread the bank across cores. A 24-channel bank costs more
    // than one core at 1 MS/s, and a single feed thread fell to 40%
    // throughput (live-found 2026-08-07: the ring overflowed and the
    // decoders got spliced time). Without OpenMP the pragma is a no-op
    // and the loop runs serial — tests don't need the flag.
#pragma omp parallel for schedule(static) num_threads(4)
    for (int i = 0; i < int(ch_.size()); ++i)
        ch_[i].dec->processIq(d, n);              // each gated by its atomic
}

bool SkimmerEngine::cwSegment(qint64 dialHz, qint64& lo, qint64& hi) {
    for (const Seg& s : kCwSegs)
        if (dialHz >= s.lo - 50000 && dialHz <= s.hi + 50000) {
            lo = s.lo;
            hi = s.hi;
            return true;
        }
    return false;
}

void SkimmerEngine::updateFromSpectrum(const std::vector<float>& db,
                                       int spanHz, qint64 dialHz,
                                       int loOffsetHz) {
    if (!enabled()) return;
    const int n = static_cast<int>(db.size());
    if (n < 64 || spanHz <= 0) return;
    const double binHz = double(spanHz) / n;
    const qint64 loAbs = dialHz + loOffsetHz;      // bin n/2

    // LO moved: re-express every surviving channel relative to the new LO;
    // channels that slid out of the capture are freed. (AetherSDR fork: the
    // console's LO rode a fixed offset above the dial, so it watched the
    // dial; here the LO follows the panadapter view and can move with the
    // dial still, so the LO itself is what is watched.)
    if (loAbs != lastDial_) {
        lastDial_ = loAbs;
        for (auto& c : ch_) {
            if (!c.active) continue;
            const qint64 off = c.hz - loAbs;
            if (std::llabs(off) > spanHz / 2 - 2000) freeChannel(c);
            else c.dec->retune(double(off));
        }
    }

    qint64 segLo = 0, segHi = 0;
    if (!cwSegment(dialHz, segLo, segHi)) {
        // Not parked on a CW segment — let existing channels finish but
        // assign nothing new.
        segLo = segHi = 0;
    }

    // Expire channels: 90 s without a decoded character means the station
    // is gone (or was never real). A channel still trickling junk after
    // 45 s (mostly E/T/*, no call) is parked on noise the consistency
    // squelch couldn't fully silence — free it for a real peak.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto& c : ch_) {
        if (!c.active) continue;
        // Outside the CW window (parked on FT8 under the old wider table,
        // or the dial hopped bands): reclaim immediately.
        if (segLo != 0 && (c.hz < segLo || c.hz > segHi)) {
            freeChannel(c);
            continue;
        }
        const qint64 idleSince = std::max(c.lastCharMs, c.assignedMs);
        if (nowMs - idleSince > 90000) { freeChannel(c); continue; }
        if (c.call.isEmpty() && nowMs - c.assignedMs > 45000
            && c.text.size() > 8) {
            int junk = 0;
            for (const QChar ch : c.text)
                if (ch == 'E' || ch == 'T' || ch == ' ' || ch == '*'
                    || ch == 'I')
                    ++junk;
            if (junk * 10 >= c.text.size() * 8) freeChannel(c);
        }
    }

    if (segLo == 0) return;
    // Bin range covering (CW segment) ∩ (capture, minus edge margins).
    const auto binOf = [&](qint64 absHz) {
        return n / 2 + int(std::lround(double(absHz - loAbs) / binHz));
    };
    int i0 = std::max(binOf(segLo), 32);
    int i1 = std::min(binOf(segHi), n - 33);
    if (i1 - i0 < 32) return;

    // Average the spectrum across assignment passes: single-frame noise
    // spikes look exactly like weak carriers (replay-found: half the bank
    // parked on QRM), a 3-frame EMA keeps only what persists.
    if (int(specAvg_.size()) != n) specAvg_.assign(db.begin(), db.end());
    else
        for (int i = 0; i < n; ++i)
            specAvg_[i] += 0.4f * (db[i] - specAvg_[i]);

    // Noise floor: median of the segment.
    std::vector<float> sorted(specAvg_.begin() + i0,
                              specAvg_.begin() + i1 + 1);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2,
                     sorted.end());
    const float floorDb = sorted[sorted.size() / 2];

    // Peaks: local max over ±3 bins, well above the floor, away from the
    // LO's DC artifact.
    struct Peak { int bin; float db; };
    std::vector<Peak> peaks;
    const std::vector<float>& sp = specAvg_;
    for (int i = i0; i <= i1; ++i) {
        if (sp[i] < floorDb + 12.0f) continue;
        if (std::abs(i - n / 2) * binHz < 1500.0) continue;   // DC hump
        bool best = true;
        for (int k = -3; k <= 3 && best; ++k)
            if (k != 0 && sp[i + k] > sp[i]) best = false;
        if (best) peaks.push_back({i, sp[i]});
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const Peak& a, const Peak& b) { return a.db > b.db; });

    for (const Peak& pk : peaks) {
        // Parabolic interpolation on the peak and its neighbors: the true
        // carrier is rarely at a bin center, and ±30 Hz of assignment
        // error is exactly the kind of thing AFC then has to claw back.
        const double ym = specAvg_[pk.bin - 1], y0 = specAvg_[pk.bin],
                     yp = specAvg_[pk.bin + 1];
        const double den = ym - 2.0 * y0 + yp;
        const double d =
            (std::abs(den) > 1e-9) ? std::clamp(0.5 * (ym - yp) / den,
                                                -0.5, 0.5) : 0.0;
        const qint64 hz = loAbs
            + qint64(std::lround((pk.bin - n / 2 + d) * binHz));
        // Already covered? (Assigned channels hold their frequency; CW
        // stations don't wander.)
        bool covered = false;
        for (const auto& c : ch_)
            if (c.active && std::llabs(c.hz - hz) < 150) covered = true;
        if (covered) continue;
        Chan* free = nullptr;
        for (auto& c : ch_)
            if (!c.active) { free = &c; break; }
        if (!free) break;                          // bank is full
        free->active = true;
        free->hz = hz;
        free->wpm = 0;
        free->call.clear();
        free->text.clear();
        free->lastCharMs = 0;
        free->assignedMs = nowMs;
        free->dec->retune(double(hz - loAbs));
        free->dec->setEnabled(true);
        if (std::getenv("TTC_SKIMDBG"))
            fprintf(stderr, "[skim] ch -> %.1f kHz (%.0f dB peak)\n",
                    hz / 1000.0, double(pk.db));
    }
}

void SkimmerEngine::freeChannel(Chan& c) {
    c.dec->setEnabled(false);
    c.active = false;
    c.call.clear();
    c.candidate.clear();
    c.candCount = 0;
    c.candWpm[0] = c.candWpm[1] = c.candWpm[2] = 0;
    c.candWpmN = 0;
    c.text.clear();
    c.show.clear();
    c.wpm = 0;
}

void SkimmerEngine::onText(int idx, const QString& t) {
    Chan& c = ch_[idx];
    if (!c.active) return;                         // late queued delivery
    c.text += t;
    if (c.text.size() > 160) c.text = c.text.right(120);
    c.lastCharMs = QDateTime::currentMSecsSinceEpoch();
    if (std::getenv("TTC_SKIMDBG"))
        fprintf(stderr, "[skim] %.1f kHz: %s\n", c.hz / 1000.0,
                qPrintable(c.text.right(40)));
    extractCall(c, t.contains(' '));
}

void SkimmerEngine::extractCall(Chan& c, bool wordDone) {
    // Mine the rolling buffer: a token is a call if it has callsign shape,
    // survives the cty.dat check, and either follows "DE " (CQ CQ DE X1YZ)
    // or shows up twice — one clean copy could still be a decode artifact.
    // Only COMPLETED words count: the token still being received is a
    // prefix of the real call ("W1A" mid-"W1AW") and would spot as a
    // phantom, so it waits until its word gap arrives.
    QStringList toks = c.text.split(' ', Qt::SkipEmptyParts);
    if (!c.text.endsWith(' ') && !toks.isEmpty()) toks.removeLast();
    // Two shape stats of the recent honest copy, computed once per scan
    // (they depend only on the buffer; 2026-08-08 max review found them
    // recomputed per token). junkFrac counts characters in E/T/I/'*'-ONLY
    // tokens plus the inter-token gaps: dit/dah junk decodes to bare
    // E/T/I runs and isolated letters, while a real WORD merely containing
    // those letters stays clean — the old per-character count scored
    // "CQ TEST K1TTT" at 0.65 on perfect copy and would have blocked a
    // T-heavy contest runner the moment sighting counts became honest.
    // The stronger signal stays single-char TOKENS: junk decodes to
    // isolated letters where real copy is words (measured: K3KY 0.12 vs
    // junk 0.46).
    const QString recent = c.text.right(40);
    double junkFrac = 0.0, singleFrac = 0.0;
    if (recent.size() >= 16) {
        int junk = 0, singles = 0, tokChars = 0;
        const QStringList w = recent.split(' ', Qt::SkipEmptyParts);
        for (const QString& tk : w) {
            tokChars += tk.size();
            int stars = 0;
            bool junkTok = true;
            for (const QChar ch : tk) {
                if (ch == '*') { ++stars; continue; }   // unreadable marker:
                if (ch != 'E' && ch != 'T' && ch != 'I')//  junk ANYWHERE, even
                    junkTok = false;                    //  inside a real word
            }
            junk += junkTok ? tk.size() : stars;
            if (tk.size() == 1) ++singles;
        }
        junk += int(recent.size()) - tokChars;         // the gaps themselves
        junkFrac = double(junk) / recent.size();
        if (w.size() >= 4) singleFrac = double(singles) / w.size();
    }
    for (int i = toks.size() - 1; i >= 0; --i) {
        QString tok = toks[i];
        bool fullToken = true;                     // vs fished from a merge
        if (tok.contains('*')) continue;           // unreadable char inside
        if (tok.size() < 3) continue;
        if (!callRe().match(tok).hasMatch()) {
            fullToken = false;
            // Copy often merges neighbors ("RICKWB4IT") — fish a valid
            // call out of the tail of a long token.
            QString found;
            for (int st = 1; st + 3 <= tok.size() && st <= 6; ++st) {
                const QString sub = tok.mid(st);
                if (sub.size() > 8) continue;
                if (callRe().match(sub).hasMatch()
                    && (!validate_ || validate_(sub))) {
                    found = sub;
                    break;
                }
            }
            if (found.isEmpty()) continue;
            tok = found;
        }
        if (tok.size() > 8) continue;
        if (validate_ && !validate_(tok)) continue;
        const bool afterDe = i > 0 && toks[i - 1] == QLatin1String("DE");
        // Sighting bookkeeping: a sighting counts only when this token is
        // the freshest completed word AND this very event carried the word
        // gap that completed it (wordDone). Freshness alone was not enough:
        // the trailing partial is stripped each scan, so the candidate
        // stayed "freshest" and re-counted on EVERY following character —
        // one physical copy plus one keystroke of the next word met the
        // listed threshold (max-review find 2026-08-08; it also explains
        // how one-copy phantoms "passed the sighting count" on 08-07).
        // Candidacy persists across buffer scroll — ragchewers ID once per
        // over, minutes apart (ground truth: WB4IT copied cleanly once per
        // 45 s window and never spotted before this). Only a clean full
        // word of 4+ chars may OPEN a candidacy; fragments/merges confirm.
        const bool fresh = (i == toks.size() - 1) && wordDone;
        if (tok == c.candidate) {
            if (fresh) {
                ++c.candCount;
                if (c.wpm > 0) {           // window of the newest 3 clocks
                    c.candWpm[2] = c.candWpm[1];
                    c.candWpm[1] = c.candWpm[0];
                    c.candWpm[0] = c.wpm;
                    if (c.candWpmN < 3) ++c.candWpmN;
                }
            }
        } else if (fresh && fullToken && tok.size() >= 4) {
            c.candidate = tok;
            c.candCount = 1;
            c.candWpm[0] = c.wpm > 0 ? c.wpm : 0;
            c.candWpm[1] = c.candWpm[2] = 0;
            c.candWpmN = c.wpm > 0 ? 1 : 0;
        }
        // Confidence: DE spots at once; MASTER.SCP calls need 2 distinct
        // sightings; unlisted calls need 3 (recurring garbage can shape
        // itself into a plausible call — NE3INE/TS9BL were spotted from
        // busts of weak real QSOs before this tier).
        const bool listed = known_.isEmpty() || known_.contains(tok);
        if (!afterDe && c.candCount < (listed ? 2 : 3)) continue;
        if (tok != c.candidate && !afterDe) continue;

        // Phantom-junk gates (2026-08-07): a keyer tester and a
        // starved-stream evening minted cty-valid calls (XS6MB, E3MT,
        // S6TUU, A5OJ) from dit-string junk — recurring enough to pass
        // the sighting count. Two fingerprints separate a real weak
        // station from junk with a call-shaped hole in it; DE-adjacency
        // is trusted evidence that clears them.
        {
            // Env-tunable ceilings, parsed locale-proof: after QApplication
            // adopts the environment locale, atof("0.55") is 0.0 under a
            // comma-decimal locale (proven on this Qt), and a 0.0 ceiling
            // silently blocks EVERY non-DE spot. QByteArray::toDouble is
            // locale-fixed; garbage or non-positive values keep the
            // shipped default instead of poisoning the gate.
            static const auto envD = [](const char* name, double def) {
                const char* v = std::getenv(name);
                if (!v || !*v) return def;
                bool ok = false;
                const double d = QByteArray(v).toDouble(&ok);
                return ok && d > 0.0 ? d : def;
            };
            static const double kJunkMax  = envD("TTC_SKIMJUNK", 0.55);
            static const double kJunkHold = envD("TTC_SKIMHOLD", 0.70);
            static const double kWpmSwing = envD("TTC_SKIMWPM",  1.6);
            static const double kSingleMax = envD("TTC_SKIMSGL", 0.40);
            const bool refresh = (tok == c.call);
            bool blocked = false;
            if (!afterDe) {
                if (refresh) {
                    // The held call is only re-announced while its channel
                    // still carries real copy; once it decays to noise the
                    // station is gone and the spot ages out (10 min) — this
                    // is what stops a phantom re-announcing 140x after one
                    // lucky confirmation (the pre-hardening leak).
                    if (junkFrac >= kJunkHold
                        || singleFrac >= kSingleMax + 0.15) blocked = true;
                } else {
                    if (junkFrac >= kJunkMax || singleFrac >= kSingleMax)
                        blocked = true;
                    // Clock stability: a real sender re-sighted 3+ times
                    // holds its WPM; junk swings (SN1N replayed 28-48).
                    // Judged over the newest three sightings only, so a
                    // warm-up transient ages out instead of latching (a
                    // 2-sighting listed call hasn't the samples to judge,
                    // so it's exempt).
                    if (c.candWpmN >= 3) {
                        const int lo = std::min({c.candWpm[0], c.candWpm[1],
                                                 c.candWpm[2]});
                        const int hi = std::max({c.candWpm[0], c.candWpm[1],
                                                 c.candWpm[2]});
                        if (lo > 0 && hi > kWpmSwing * lo) blocked = true;
                    }
                }
            }
            if (std::getenv("TTC_SKIMDBG"))
                fprintf(stderr,
                        "[gate] %-8s %s junk %.2f sgl %.2f wpm %d/%d/%d n%d -> %s\n",
                        qPrintable(tok), afterDe ? "DE" : refresh ? "rf" : "  ",
                        junkFrac, singleFrac, c.candWpm[0], c.candWpm[1],
                        c.candWpm[2], c.candCount, blocked ? "BLOCK" : "pass");
            if (blocked) continue;
        }
        // Duplicate listener: a strong station's keying sidebands raise
        // peaks of their own, and several channels end up copying the SAME
        // station (replay-found: one CQ, three channels, spot frequency
        // thrashing between them). Channels are assigned strongest-first,
        // so the first holder has the true carrier — later ones fold.
        for (auto& other : ch_)
            if (&other != &c && other.active && other.call == tok) {
                freeChannel(c);
                return;
            }
        if (tok == c.call) {                       // same station, refresh
            for (auto& s : spots_)
                if (s.call == tok) {
                    s.atSecs = QDateTime::currentSecsSinceEpoch();
                    s.wpm = c.wpm;
                }
            // Re-announce so downstream consumers (telnet clients, the
            // band map's age column) see the station is still active;
            // they throttle their own output.
            emit spotFound(tok, c.hz, c.wpm);
            emit spotsChanged();
            return;
        }
        // One spot per channel: a marginal station's wobbling decode can
        // self-confirm a NEW junk variant every few minutes (ground truth:
        // one station at 7029.7 accumulated T4EE, AM4I, M4FG and W4FT).
        // When this channel's confirmed call changes, its previous spot —
        // its own earlier bust — goes with it (unless another channel
        // legitimately holds that call).
        if (!c.call.isEmpty() && c.call != tok) {
            bool held = false;
            for (const auto& other : ch_)
                if (&other != &c && other.active && other.call == c.call)
                    held = true;
            if (!held) {
                const QString old = c.call;
                spots_.erase(std::remove_if(spots_.begin(), spots_.end(),
                                            [&old](const SkimSpot& s) {
                                                return s.call == old;
                                            }),
                             spots_.end());
            }
        }
        c.call = tok;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        bool known = false;
        for (auto& s : spots_)
            if (s.call == tok) {
                s.hz = c.hz;
                s.atSecs = now;
                s.wpm = c.wpm;
                known = true;
            }
        if (!known) spots_.push_back({tok, c.hz, now, c.wpm});
        emit spotFound(tok, c.hz, c.wpm);
        emit spotsChanged();
        return;
    }
}

const QVector<SkimmerEngine::SkimSpot>& SkimmerEngine::spots() {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    spots_.erase(std::remove_if(spots_.begin(), spots_.end(),
                                [now](const SkimSpot& s) {
                                    return now - s.atSecs > 600;
                                }),
                 spots_.end());
    return spots_;
}

QVector<SkimmerEngine::ChanInfo> SkimmerEngine::channelInfo() const {
    QVector<ChanInfo> out;
    out.reserve(int(ch_.size()));
    for (const auto& c : ch_)
        out.push_back({c.active, c.hz, c.wpm, c.call,
                       (c.show.isEmpty() ? c.text : c.show).right(28)});
    return out;
}

} // namespace AetherSDR::orion::skim
