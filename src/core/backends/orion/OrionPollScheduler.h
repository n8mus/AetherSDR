#pragma once

// What to ask the Orion next. Pure logic over an injected clock, so the
// cadence is unit-testable without a serial port or timers.
//
// The Orion services its UART about every 100 ms; round trips are bimodal
// (about 17 ms median, 110 ms tail). One query per 200 ms tick keeps the
// link well inside that budget:
//
//   phase 0, 4  ?AF      dial A (one in flight at a time, re-armed after 600 ms)
//   phase 1, 3  ?S       both S-meters (2 Hz)
//   phase 2     fast wheel: main mode, main filter, ?BF, sub mode, sub filter
//
// Every 25th tick one S-meter slot is lent to a slow wheel (routing, AGC,
// front end), about one item every 5 s. Within 800 ms of a dial move the
// S-meter slots go to ?AF so the display tracks the knob.

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

    // The operator moved a dial (ours or the radio's): favour ?AF for a while.
    void noteDialMoved(qint64 nowMs) { m_lastDialMoveMs = nowMs; }
    // A dial-A reply arrived: the next ?AF may go out immediately.
    void noteMainFrequencyReply() { m_mainFrequencyInFlight = false; }

    void reset();

private:
    QList<QByteArray> mainFrequency(qint64 nowMs);

    int m_tick = 0;
    int m_fastWheel = 0;
    int m_slowWheel = 0;
    bool m_mainFrequencyInFlight = false;
    qint64 m_mainFrequencySentMs = 0;
    qint64 m_lastDialMoveMs = -1'000'000;
};

}  // namespace AetherSDR::orion
