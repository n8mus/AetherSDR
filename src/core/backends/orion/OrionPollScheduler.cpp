#include "core/backends/orion/OrionPollScheduler.h"

#include "core/backends/orion/OrionCat.h"

namespace AetherSDR::orion {

void OrionPollScheduler::reset()
{
    *this = OrionPollScheduler{};
}

QList<QByteArray> OrionPollScheduler::frequency(Rx rx, qint64 nowMs)
{
    Dial& d = dial(rx);
    if (d.inFlight && nowMs - d.sentMs < kFrequencyRearmMs) {
        return {};
    }
    d.inFlight = true;
    d.sentMs = nowMs;
    return {queryFrequency(rx)};
}

QList<QByteArray> OrionPollScheduler::next(qint64 nowMs)
{
    const int tick = m_tick++;
    const int phase = tick % 5;
    const Rx other = m_activeRx == Rx::Main ? Rx::Sub : Rx::Main;

    // The dial the operator is working gets the fast slots.
    if (phase == 0 || phase == 4) {
        return frequency(m_activeRx, nowMs);
    }

    if (phase == 2) {
        const int item = m_fastWheel++ % 7;
        switch (item) {
        case 0:
            return {queryMode(Rx::Main)};
        case 1:
            return {queryBandwidth(Rx::Main), queryPbt(Rx::Main)};
        case 2:
            return frequency(other, nowMs);
        case 3:
            return {queryMode(Rx::Sub)};
        case 4:
            return {queryBandwidth(Rx::Sub), queryPbt(Rx::Sub)};
        case 5:
            // The locks gate the split park and CAT tuning, so they are
            // read every lap (~7 s), not in the slow wheel.
            return {queryVfoLock(Rx::Sub)};
        default:
            return {queryVfoLock(Rx::Main)};
        }
    }

    // Phases 1 and 3: the S-meter slots, lent to whichever dial is moving.
    const bool mainMoving = nowMs - m_main.lastMoveMs < kBurstWindowMs;
    const bool subMoving = nowMs - m_sub.lastMoveMs < kBurstWindowMs;
    if (mainMoving && subMoving) {
        return frequency(phase == 1 ? Rx::Main : Rx::Sub, nowMs);
    }
    if (mainMoving || subMoving) {
        return frequency(mainMoving ? Rx::Main : Rx::Sub, nowMs);
    }
    if (tick % kSlowWheelEveryTicks == 3) {
        const int item = m_slowWheel++ % 7;
        switch (item) {
        case 0:
            return {queryVfoAssignment()};
        case 1:
            return {queryAgc(Rx::Main)};
        case 2:
            return {queryAgc(Rx::Sub)};
        case 3:
            return {queryRfGain(Rx::Main)};
        case 4:
            return {queryAttenuator(Rx::Main)};
        case 5:
            return {queryPreamp(Rx::Main)};
        default:
            // The front panel can switch the tuner in or out.
            return {queryTuner()};
        }
    }
    return {querySMeter()};
}

}  // namespace AetherSDR::orion
