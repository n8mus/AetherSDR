#pragma once

// What to ask the Orion next. Pure logic over an injected clock, so the
// cadence is unit-testable without a serial port or timers.
//
// The Orion services its UART about every 100 ms; round trips are bimodal
// (about 17 ms median, 110 ms tail). One query per 200 ms tick keeps the
// link well inside that budget:
//
//   phase 0, 4  the ACTIVE slice's dial (?AF or ?BF; one in flight at a
//               time, re-armed after 600 ms)
//   phase 1, 3  ?S       both S-meters (2 Hz)
//   phase 2     fast wheel: main mode, main filter, the other dial, sub
//               mode, sub filter, B lock, A lock
//
// Every 25th tick one S-meter slot is lent to a slow wheel (routing, AGC,
// front end), about one item every 5 s. Within 800 ms of a dial move the
// S-meter slots go to that dial so the display tracks the knob, whichever
// VFO it is (on-air finding, 2026-10-17: B lagged when only A had this).

#include "core/backends/orion/OrionCat.h"

#include <QByteArray>
#include <QList>

namespace AetherSDR::orion {

class OrionPollScheduler {
public:
    static constexpr int kTickMs = 200;
    static constexpr int kFrequencyRearmMs = 600;
    static constexpr int kBurstWindowMs = 800;
    static constexpr int kSlowWheelEveryTicks = 25;

    // The queries to send on this tick (often one, a filter read is two).
    QList<QByteArray> next(qint64 nowMs);

    // The receiver whose dial gets the fast slots: the active slice's.
    void setActiveReceiver(Rx rx) { m_activeRx = rx; }
    [[nodiscard]] Rx activeReceiver() const { return m_activeRx; }
    // A dial moved (ours or the radio's knob): favour it for a while.
    void noteDialMoved(Rx rx, qint64 nowMs) { dial(rx).lastMoveMs = nowMs; }
    // A frequency reply arrived: that dial's next query may go out at once.
    void noteFrequencyReply(Rx rx) { dial(rx).inFlight = false; }

    void reset();

private:
    struct Dial {
        bool inFlight = false;
        qint64 sentMs = 0;
        qint64 lastMoveMs = -1'000'000;
    };
    Dial& dial(Rx rx) { return rx == Rx::Main ? m_main : m_sub; }
    QList<QByteArray> frequency(Rx rx, qint64 nowMs);

    int m_tick = 0;
    int m_fastWheel = 0;
    int m_slowWheel = 0;
    Rx m_activeRx = Rx::Main;
    Dial m_main;
    Dial m_sub;
};

}  // namespace AetherSDR::orion
