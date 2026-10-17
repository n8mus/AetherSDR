#include "core/backends/orion/OrionPollScheduler.h"

#include "core/backends/orion/OrionCat.h"

namespace AetherSDR::orion {

void OrionPollScheduler::reset()
{
    *this = OrionPollScheduler{};
}

QList<QByteArray> OrionPollScheduler::mainFrequency(qint64 nowMs)
{
    if (m_mainFrequencyInFlight && nowMs - m_mainFrequencySentMs < kFrequencyRearmMs) {
        return {};
    }
    m_mainFrequencyInFlight = true;
    m_mainFrequencySentMs = nowMs;
    return {queryFrequency(Rx::Main)};
}

QList<QByteArray> OrionPollScheduler::next(qint64 nowMs)
{
    const int tick = m_tick++;
    const int phase = tick % 5;

    if (phase == 0 || phase == 4) {
        return mainFrequency(nowMs);
    }

    if (phase == 2) {
        const int item = m_fastWheel++ % 5;
        switch (item) {
        case 0:
            return {queryMode(Rx::Main)};
        case 1:
            return {queryBandwidth(Rx::Main), queryPbt(Rx::Main)};
        case 2:
            return {queryFrequency(Rx::Sub)};
        case 3:
            return {queryMode(Rx::Sub)};
        default:
            return {queryBandwidth(Rx::Sub), queryPbt(Rx::Sub)};
        }
    }

    // Phases 1 and 3: the S-meter slots.
    if (nowMs - m_lastDialMoveMs < kBurstWindowMs) {
        return mainFrequency(nowMs);
    }
    if (tick % kSlowWheelEveryTicks == 3) {
        const int item = m_slowWheel++ % 6;
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
        default:
            return {queryPreamp(Rx::Main)};
        }
    }
    return {querySMeter()};
}

}  // namespace AetherSDR::orion
