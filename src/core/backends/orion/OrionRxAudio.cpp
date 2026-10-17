#include "core/backends/orion/OrionRxAudio.h"

#include <QLoggingCategory>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

Q_DECLARE_LOGGING_CATEGORY(lcOrion)

namespace AetherSDR::orion {

namespace {

// WDSP's RXA expects the opposite spectral handedness to the RSP2's IQ
// (the reference program conjugates before WDSP, and HL2 found the same
// for its own wire). Pinned by orion_rxaudio_test: a tone above the dial
// must be heard in USB and not in LSB.
constexpr bool kConjugateForWdsp = true;

// Blackman-windowed sinc at the x12 rate, split into its 12 phases.
std::vector<float> polyphasePrototype(int interpolate, int tapsPerPhase, double cutoffHz,
                                      double inputRateHz)
{
    const int n = interpolate * tapsPerPhase;
    const double fc = cutoffHz / (inputRateHz * interpolate);
    const double mid = (n - 1) / 2.0;
    std::vector<double> h(static_cast<std::size_t>(n));
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double k = i - mid;
        const double sinc = std::fabs(k) < 1e-9
            ? 2.0 * fc
            : std::sin(2.0 * std::numbers::pi * fc * k) / (std::numbers::pi * k);
        const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (n - 1))
                         + 0.08 * std::cos(4.0 * std::numbers::pi * i / (n - 1));
        h[static_cast<std::size_t>(i)] = sinc * w;
        sum += sinc * w;
    }
    // Unity passband gain after interpolation: the taps sum to `interpolate`.
    std::vector<float> phases(static_cast<std::size_t>(n));
    for (int p = 0; p < interpolate; ++p) {
        for (int k = 0; k < tapsPerPhase; ++k) {
            phases[static_cast<std::size_t>(p * tapsPerPhase + k)] =
                static_cast<float>(h[static_cast<std::size_t>(p + k * interpolate)] * interpolate / sum);
        }
    }
    return phases;
}

}  // namespace

OrionRxAudio::OrionRxAudio(QObject* parent)
    : QObject(parent)
{
}

OrionRxAudio::~OrionRxAudio()
{
    stop();
}

qint64 OrionRxAudio::centreHz(const Tuning& t)
{
    switch (t.mode) {
    case Mode::Cw:
        return t.dialHz - t.cwPitchHz;
    case Mode::Cwl:
        return t.dialHz + t.cwPitchHz;
    default:
        return t.dialHz;
    }
}

std::pair<double, double> OrionRxAudio::wdspPassband(const Tuning& t)
{
    double low = t.lowHz;
    double high = t.highHz;
    if (t.mode == Mode::Cw) {
        low += t.cwPitchHz;
        high += t.cwPitchHz;
    } else if (t.mode == Mode::Cwl) {
        low -= t.cwPitchHz;
        high -= t.cwPitchHz;
    }
    if (high - low < 50.0) {
        high = low + 50.0;
    }
    return {low, high};
}

WdspChannel::Mode OrionRxAudio::wdspMode(Mode mode)
{
    switch (mode) {
    case Mode::Usb: return WdspChannel::Mode::Usb;
    case Mode::Lsb: return WdspChannel::Mode::Lsb;
    case Mode::Cw:  return WdspChannel::Mode::Cwu;
    case Mode::Cwl: return WdspChannel::Mode::Cwl;
    case Mode::Am:  return WdspChannel::Mode::Am;
    case Mode::Fm:  return WdspChannel::Mode::Fm;
    }
    return WdspChannel::Mode::Usb;
}

bool OrionRxAudio::start(const Tuning& tuning, QString* error)
{
    stop();

    WdspChannel::Config c;
    c.direction = WdspChannel::Direction::Receive;
    c.inputBlockSize = kWdspBlock;
    // One WDSP pass per input block: dsp_size covers the same time at 48 kHz.
    c.dspBlockSize = static_cast<std::size_t>(kWdspBlock) * kWdspDspRateHz / kWdspInputRateHz;
    c.inputSampleRate = kWdspInputRateHz;
    c.dspSampleRate = kWdspDspRateHz;
    c.outputSampleRate = kAudioRateHz;
    c.mode = wdspMode(tuning.mode);
    const auto [low, high] = wdspPassband(tuning);
    c.filterLowHz = low;
    c.filterHighHz = high;
    c.agcMode = tuning.agcMode;
    c.maximumAgcGainDb = tuning.agcMaxGainDb;
    // This thread exists only to feed WDSP, so it may wait for each output
    // block. Non-blocking, a burst of input (the SDRplay API delivers in
    // bursts) can underrun the two-slot output ring and leave it out of step
    // for good (WdspChannel.h, outputReady()); measured here as a 30 dB loss
    // of sideband rejection.
    c.blockForOutput = true;

    Q_UNUSED(error);   // a WDSP failure is reported later, by failed()
    m_config = c;

    m_phases = polyphasePrototype(kInterpolate, kTapsPerPhase, kFilterCutoffHz, kInputRateHz);
    m_history.assign(static_cast<std::size_t>(2 * kTapsPerPhase), {});
    m_historyPos = 0;
    m_upPosition = 0;
    m_ncoPhase = 0.0;
    m_wdspI.clear();
    m_wdspQ.clear();
    m_dcX[0] = m_dcX[1] = m_dcY[0] = m_dcY[1] = 0.0f;
    // 20 Hz DC-blocker corner at 24 kHz: removes AM's carrier pedestal.
    m_dcR = static_cast<float>(1.0 - 2.0 * std::numbers::pi * 20.0 / kAudioRateHz);

    {
        std::lock_guard lock(m_tuningLock);
        m_requested = tuning;
        m_applied = tuning;
        m_tuningDirty = false;
    }
    {
        std::lock_guard lock(m_queueLock);
        m_queue.clear();
        m_queuedSamples = 0;
    }
    m_dropped = 0;
    ++m_generation;
    m_stop = false;
    m_ready = false;
    // WDSP is built ON the audio thread: a cold open plans FFTW filters and
    // can take seconds (measured: 12 s on this station). Built on the owner
    // thread it froze the GUI and the CAT watchdog then declared the radio
    // dead. Audio starts when the channel is ready; until then push() drops.
    m_worker = std::thread([this] { workerLoop(); });
    return true;
}

void OrionRxAudio::stop()
{
    if (m_worker.joinable()) {
        m_stop = true;
        m_queueWake.notify_all();
        m_worker.join();
    }
    ++m_generation;   // drop audio already queued to the owner
    m_ready = false;
    // processIq() is no longer called: the channel may be destroyed here.
    m_channel.reset();
}

void OrionRxAudio::setTuning(const Tuning& tuning)
{
    std::lock_guard lock(m_tuningLock);
    if (tuning == m_requested) {
        return;
    }
    m_requested = tuning;
    m_tuningDirty = true;
}

std::size_t OrionRxAudio::queuedSamples()
{
    std::lock_guard lock(m_queueLock);
    return m_queuedSamples;
}

void OrionRxAudio::push(const std::complex<float>* iq, std::size_t n, qint64 loHz)
{
    if (n == 0 || !m_ready.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard lock(m_queueLock);
        while (m_queuedSamples + n > kMaxQueuedSamples && !m_queue.empty()) {
            m_queuedSamples -= m_queue.front().iq.size();
            m_queue.pop_front();
            m_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        Block b;
        b.iq.assign(iq, iq + n);
        b.loHz = loHz;
        m_queuedSamples += n;
        m_queue.push_back(std::move(b));
    }
    m_queueWake.notify_one();
}

void OrionRxAudio::workerLoop()
{
    std::string why;
    m_channel = WdspChannel::create(m_config, &why);
    if (!m_channel) {
        const quint64 generation = m_generation;
        const QString reason = tr("cannot build the WDSP receive channel: %1")
                                   .arg(QString::fromStdString(why));
        QMetaObject::invokeMethod(this, [this, reason, generation] {
            if (generation == m_generation) {
                emit failed(reason);
            }
        }, Qt::QueuedConnection);
        return;
    }
    if (!m_channel->isRunning()) {
        (void)m_channel->setRunning(true);
    }
    m_left.assign(m_channel->outputBlockSize(), 0.0f);
    m_right.assign(m_channel->outputBlockSize(), 0.0f);
    m_ready = true;

    while (!m_stop) {
        Block block;
        {
            std::unique_lock lock(m_queueLock);
            m_queueWake.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
            if (m_stop) {
                break;
            }
            block = std::move(m_queue.front());
            m_queue.pop_front();
            m_queuedSamples -= block.iq.size();
        }
        processBlock(block);
    }
}

void OrionRxAudio::applyTuningIfChanged()
{
    Tuning want;
    {
        std::lock_guard lock(m_tuningLock);
        if (!m_tuningDirty) {
            return;
        }
        want = m_requested;
        m_tuningDirty = false;
    }
    // Control operations, made on the only thread that calls processIq(),
    // between blocks, so no callback of ours is in flight.
    bool ok = true;
    if (want.mode != m_applied.mode) {
        ok = m_channel->setMode(wdspMode(want.mode)) && ok;
    }
    const auto [low, high] = wdspPassband(want);
    const auto [oldLow, oldHigh] = wdspPassband(m_applied);
    if (want.mode != m_applied.mode || low != oldLow || high != oldHigh) {
        ok = m_channel->setFilter(low, high) && ok;
    }
    if (want.agcMode != m_applied.agcMode || want.agcMaxGainDb != m_applied.agcMaxGainDb) {
        ok = m_channel->setAgc(want.agcMode, want.agcMaxGainDb) && ok;
    }
    if (ok) {
        m_applied = want;
        return;
    }
    // A refused control operation is retried on the next block. The dial is
    // ours, not WDSP's, so the mixer follows it at once regardless.
    m_applied.dialHz = want.dialHz;
    std::lock_guard lock(m_tuningLock);
    m_tuningDirty = true;
}

void OrionRxAudio::processBlock(const Block& block)
{
    if (!m_channel) {
        return;
    }
    applyTuningIfChanged();

    // Mix the wanted frequency down to 0 Hz. Outside the capture there is
    // nothing to hear: WDSP is still clocked, with silence, so its latency
    // stays constant and the receiver comes back cleanly.
    const bool audible = inCapture(m_applied, block.loHz);
    const double offsetHz = static_cast<double>(centreHz(m_applied) - block.loHz);
    const double step = -2.0 * std::numbers::pi * offsetHz / kInputRateHz;
    const std::size_t taps = kTapsPerPhase;
    for (const std::complex<float>& s : block.iq) {
        const std::complex<float> rot(static_cast<float>(std::cos(m_ncoPhase)),
                                      static_cast<float>(std::sin(m_ncoPhase)));
        m_ncoPhase += step;
        if (m_ncoPhase > std::numbers::pi || m_ncoPhase < -std::numbers::pi) {
            m_ncoPhase = std::remainder(m_ncoPhase, 2.0 * std::numbers::pi);
        }
        // Doubled delay line, newest first: window[0] is this sample.
        m_historyPos = (m_historyPos + taps - 1) % taps;
        const std::complex<float> x = audible ? s * rot : std::complex<float>{};
        m_history[m_historyPos] = x;
        m_history[m_historyPos + taps] = x;
        // At most one output per input (decimate > interpolate).
        if (m_upPosition < kInterpolate) {
            const float* h = &m_phases[static_cast<std::size_t>(m_upPosition) * taps];
            const std::complex<float>* window = &m_history[m_historyPos];
            std::complex<float> acc{};
            for (std::size_t k = 0; k < taps; ++k) {
                acc += window[k] * h[k];
            }
            if (kConjugateForWdsp) {
                acc = std::conj(acc);
            }
            m_wdspI.push_back(acc.real());
            m_wdspQ.push_back(acc.imag());
            m_upPosition += kDecimate;
        }
        m_upPosition -= kInterpolate;
    }

    std::size_t consumed = 0;
    while (m_wdspI.size() - consumed >= static_cast<std::size_t>(kWdspBlock)) {
        const auto res = m_channel->processIq(
            std::span<const float>(m_wdspI.data() + consumed, kWdspBlock),
            std::span<const float>(m_wdspQ.data() + consumed, kWdspBlock),
            std::span<float>(m_left), std::span<float>(m_right));
        consumed += kWdspBlock;
        if (res != WdspChannel::ProcessResult::Ok) {
            continue;   // Underrun while the pipeline fills: no output yet
        }
        QByteArray out(static_cast<qsizetype>(m_left.size() * 2 * sizeof(float)), Qt::Uninitialized);
        auto* o = reinterpret_cast<float*>(out.data());
        for (std::size_t k = 0; k < m_left.size(); ++k) {
            const float in[2] = {m_left[k], m_right[k]};
            for (int ch = 0; ch < 2; ++ch) {
                const float y = in[ch] - m_dcX[ch] + m_dcR * m_dcY[ch];
                m_dcX[ch] = in[ch];
                m_dcY[ch] = std::fabs(y) > 1e-20f ? y : 0.0f;
                o[2 * k + ch] = m_dcY[ch];
            }
        }
        const quint64 generation = m_generation;
        QMetaObject::invokeMethod(this, [this, out, generation] {
            if (generation == m_generation) {
                emit audioReady(out);
            }
        }, Qt::QueuedConnection);
    }
    if (consumed > 0) {
        m_wdspI.erase(m_wdspI.begin(), m_wdspI.begin() + static_cast<std::ptrdiff_t>(consumed));
        m_wdspQ.erase(m_wdspQ.begin(), m_wdspQ.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}

}  // namespace AetherSDR::orion
