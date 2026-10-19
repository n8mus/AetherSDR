#include "core/backends/orion/Rsp2Receiver.h"

#include "core/backends/orion/OrionSkimmerFeed.h"

#include "core/backends/orion/OrionRxAudio.h"
#include "core/backends/orion/SdrPlayApi.h"

#include <QLoggingCategory>
#include <QMetaObject>

#include <algorithm>
#include <chrono>

Q_DECLARE_LOGGING_CATEGORY(lcOrion)

namespace AetherSDR::orion {

namespace {
// About half a second of IQ: the worker only ever wants the newest FFT's
// worth, so anything older is simply overwritten.
constexpr std::size_t kRingSamples = 1u << 19;
}  // namespace

Rsp2Receiver::Rsp2Receiver(QObject* parent)
    : QObject(parent)
{
}

Rsp2Receiver::~Rsp2Receiver()
{
    stop();
}

double Rsp2Receiver::levelOffsetDb() const
{
    const int lna = std::clamp(m_settings.lnaState, 0, 8);
    return m_settings.gainReductionDb + kLnaReductionDb[lna] + kLevelOffsetDb;
}

bool Rsp2Receiver::start(qint64 loHz, const Settings& settings, QString* error)
{
    stop();
    m_settings = settings;
    m_settings.gainReductionDb = std::clamp(m_settings.gainReductionDb, 20, 59);
    m_settings.lnaState = std::clamp(m_settings.lnaState, 0, 8);

    QString why;
    m_api = SdrPlayApi::load(&why);
    if (!m_api) {
        *error = why;
        return false;
    }
    sdrplay_api_ErrT e = m_api->open();
    if (e != sdrplay_api_Success) {
        *error = tr("cannot reach the SDRplay API service (%1). Is sdrplay.service running?")
                     .arg(m_api->errorText(e));
        return false;
    }
    m_apiOpen = true;

    const auto fail = [this, error](const QString& reason) {
        *error = reason;
        if (m_apiOpen) {
            m_api->close();
            m_apiOpen = false;
        }
        return false;
    };

    if ((e = m_api->lockDeviceApi()) != sdrplay_api_Success) {
        return fail(tr("SDRplay LockDeviceApi: %1").arg(m_api->errorText(e)));
    }
    sdrplay_api_DeviceT devices[8];
    unsigned int count = 0;
    e = m_api->getDevices(devices, &count, 8);
    if (e != sdrplay_api_Success) {
        m_api->unlockDeviceApi();
        return fail(tr("SDRplay GetDevices: %1").arg(m_api->errorText(e)));
    }
    int index = -1;
    for (unsigned int i = 0; i < count; ++i) {
        if (devices[i].hwVer == SDRPLAY_RSP2_ID) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) {
        m_api->unlockDeviceApi();
        return fail(count == 0 ? tr("no RSP2 found. Is it plugged in, and is no other "
                                    "program (the Orion console) using it?")
                               : tr("the SDRplay device found is not an RSP2"));
    }
    m_device = devices[index];
    m_device.tuner = sdrplay_api_Tuner_A;
    if ((e = m_api->selectDevice(&m_device)) != sdrplay_api_Success) {
        m_api->unlockDeviceApi();
        return fail(tr("the RSP2 is in use (%1). Close the Orion console or any other "
                       "SDR program first.").arg(m_api->errorText(e)));
    }
    m_api->unlockDeviceApi();

    if ((e = m_api->getDeviceParams(m_device.dev, &m_params)) != sdrplay_api_Success
        || !m_params || !m_params->rxChannelA) {
        m_api->releaseDevice(&m_device);
        return fail(tr("SDRplay GetDeviceParams: %1").arg(m_api->errorText(e)));
    }

    m_params->devParams->fsFreq.fsHz = static_cast<double>(spectrum::kSampleRateHz);
    sdrplay_api_RxChannelParamsT* rx = m_params->rxChannelA;
    rx->tunerParams.rfFreq.rfHz = static_cast<double>(loHz);
    rx->tunerParams.ifType = sdrplay_api_IF_Zero;
    rx->tunerParams.bwType = sdrplay_api_BW_1_536;
    rx->tunerParams.gain.gRdB = m_settings.gainReductionDb;
    rx->tunerParams.gain.LNAstate = static_cast<unsigned char>(m_settings.lnaState);
    rx->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    rx->ctrlParams.dcOffset.DCenable = 1;
    rx->ctrlParams.dcOffset.IQenable = 1;
    rx->ctrlParams.decimation.enable = 1;
    rx->ctrlParams.decimation.decimationFactor = spectrum::kHardwareDecimation;
    rx->rsp2TunerParams.antennaSel = m_settings.antennaB ? sdrplay_api_Rsp2_ANTENNA_B
                                                         : sdrplay_api_Rsp2_ANTENNA_A;
    rx->rsp2TunerParams.rfNotchEnable = 0;

    {
        std::lock_guard lock(m_ringLock);
        m_ring.assign(kRingSamples, {});
        m_writeIndex = 0;
        m_samplesSinceRetune = 0;
        m_resetAverage = true;
    }
    m_requestedLo = loHz;
    m_pendingLo = loHz;
    m_activeLo = loHz;
    m_samples = 0;
    m_overloads = 0;
    ++m_generation;

    sdrplay_api_CallbackFnsT callbacks{};
    callbacks.StreamACbFn = &Rsp2Receiver::streamCallback;
    callbacks.StreamBCbFn = nullptr;
    callbacks.EventCbFn = &Rsp2Receiver::eventCallback;
    if ((e = m_api->init(m_device.dev, &callbacks, this)) != sdrplay_api_Success) {
        m_api->releaseDevice(&m_device);
        return fail(tr("SDRplay Init: %1").arg(m_api->errorText(e)));
    }
    m_running = true;

    m_stopWorker = false;
    m_worker = std::thread([this] { workerLoop(); });
    qCInfo(lcOrion) << "RSP2: streaming, LO" << loHz << "Hz, antenna"
                    << (m_settings.antennaB ? "B" : "A") << "gRdB" << m_settings.gainReductionDb
                    << "LNA" << m_settings.lnaState;
    return true;
}

void Rsp2Receiver::stop()
{
    // Worker first: it may be mid-FFT, and it reads nothing the API owns.
    if (m_worker.joinable()) {
        m_stopWorker = true;
        m_ringWake.notify_all();
        m_worker.join();
    }
    ++m_generation;   // drop frames already queued to the owner
    if (m_running) {
        // Uninit returns only once the stream and event callbacks are done.
        m_api->uninit(m_device.dev);
        m_api->releaseDevice(&m_device);
        m_running = false;
    }
    m_params = nullptr;
    if (m_apiOpen) {
        m_api->close();
        m_apiOpen = false;
    }
}

void Rsp2Receiver::setLo(qint64 loHz)
{
    if (!m_running || !m_params || loHz == m_requestedLo) {
        return;
    }
    m_requestedLo = loHz;
    m_pendingLo = loHz;
    m_params->rxChannelA->tunerParams.rfFreq.rfHz = static_cast<double>(loHz);
    const sdrplay_api_ErrT e = m_api->update(m_device.dev, m_device.tuner,
                                             sdrplay_api_Update_Tuner_Frf,
                                             sdrplay_api_Update_Ext1_None);
    if (e != sdrplay_api_Success) {
        qCWarning(lcOrion) << "RSP2: retune to" << loHz << "failed:" << m_api->errorText(e);
    }
}

void Rsp2Receiver::setTxGuard(bool on)
{
    if (!m_running || !m_params || m_txGuard == on) {
        return;
    }
    m_txGuard = on;
    auto& gain = m_params->rxChannelA->tunerParams.gain;
    gain.gRdB = on ? 59 : m_settings.gainReductionDb;
    gain.LNAstate = static_cast<unsigned char>(on ? 8 : m_settings.lnaState);
    const sdrplay_api_ErrT e = m_api->update(m_device.dev, m_device.tuner,
                                             sdrplay_api_Update_Tuner_Gr,
                                             sdrplay_api_Update_Ext1_None);
    if (e != sdrplay_api_Success) {
        qCWarning(lcOrion) << "RSP2: TX guard" << on << "failed:" << m_api->errorText(e);
    }
}

void Rsp2Receiver::setView(const spectrum::View& view, int points)
{
    std::lock_guard lock(m_viewLock);
    m_view = view;
    m_points = std::clamp(points, 64, 8192);
}

void Rsp2Receiver::setFrameRate(int fps)
{
    std::lock_guard lock(m_viewLock);
    m_frameIntervalMs = 1000 / std::clamp(fps, 1, 60);
}

void Rsp2Receiver::setWaterfallHoldMs(int ms)
{
    std::lock_guard lock(m_viewLock);
    m_waterfallHoldMs = std::clamp(ms, 0, 10000);
}

void Rsp2Receiver::setWaterfallAverage(int percent)
{
    std::lock_guard lock(m_viewLock);
    m_waterfallAverage = std::clamp(percent, 0, 100);
}

void Rsp2Receiver::setAverage(int percent)
{
    std::lock_guard lock(m_viewLock);
    m_average = std::clamp(percent, 0, 100);
}

void Rsp2Receiver::setWeightedAverage(bool on)
{
    std::lock_guard lock(m_viewLock);
    m_weightedAverage = on;
}

void Rsp2Receiver::streamCallback(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                                  unsigned int numSamples, unsigned int reset, void* context)
{
    auto* self = static_cast<Rsp2Receiver*>(context);
    if (!self) {
        return;
    }
    const bool rfChanged = (params && params->rfChanged) || reset;
    self->onStream(xi, xq, numSamples, rfChanged);
}

void Rsp2Receiver::onStream(const short* xi, const short* xq, unsigned int n, bool rfChanged)
{
    constexpr float kNorm = 1.0f / 32768.0f;
    if (m_scratch.size() < n) {
        m_scratch.resize(n);
    }
    for (unsigned int i = 0; i < n; ++i) {
        m_scratch[i] = {xi[i] * kNorm, xq[i] * kNorm};
    }
    qint64 lo = 0;
    {
        std::lock_guard lock(m_ringLock);
        if (m_ring.empty()) {
            return;
        }
        if (rfChanged) {
            // Samples from here on belong to the new LO; the spectrum must
            // not mix the two, so the averaging restarts too.
            m_activeLo = m_pendingLo.load();
            m_samplesSinceRetune = 0;
            m_resetAverage = true;
        }
        for (unsigned int i = 0; i < n; ++i) {
            m_ring[m_writeIndex] = m_scratch[i];
            m_writeIndex = (m_writeIndex + 1) & (kRingSamples - 1);
        }
        m_samplesSinceRetune += n;
        lo = m_activeLo.load();
    }
    m_samples.fetch_add(n, std::memory_order_relaxed);
    for (OrionRxAudio* sink : m_audioSinks) {
        if (sink) {
            sink->push(m_scratch.data(), n, lo);
        }
    }
    for (OrionSkimmerFeed* feed : m_skimmerSinks) {
        if (feed) {
            feed->push(m_scratch.data(), n, lo);
        }
    }
}

void Rsp2Receiver::eventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                                 sdrplay_api_EventParamsT* params, void* context)
{
    Q_UNUSED(params);
    auto* self = static_cast<Rsp2Receiver*>(context);
    if (!self || !self->m_api) {
        return;
    }
    if (eventId == sdrplay_api_PowerOverloadChange) {
        // The API repeats the event until it is acknowledged.
        self->m_overloads.fetch_add(1, std::memory_order_relaxed);
        self->m_api->update(self->m_device.dev, tuner, sdrplay_api_Update_Ctrl_OverloadMsgAck,
                            sdrplay_api_Update_Ext1_None);
    } else if (eventId == sdrplay_api_DeviceRemoved) {
        const quint64 generation = self->m_generation;
        QMetaObject::invokeMethod(self, [self, generation] {
            if (generation == self->m_generation) {
                emit self->deviceLost(tr("the RSP2 was unplugged"));
            }
        }, Qt::QueuedConnection);
    }
}

void Rsp2Receiver::workerLoop()
{
    // Up to this many FFT blocks per frame: every sample since the previous
    // frame at 25 fps (2 MS/s / 16384 / 25 = 4.9), with headroom for 5 fps.
    constexpr int kMaxBlocks = 32;
    spectrum::SpectrumProcessor processor;
    const std::size_t n = static_cast<std::size_t>(processor.size());
    std::vector<std::complex<float>> blocks(n * kMaxBlocks);
    spectrum::WaterfallHold waterfall;
    const quint64 generation = m_generation;
    quint64 lastSamples = 0;
    // The average is a time constant, so it needs the time between frames.
    auto lastFrame = std::chrono::steady_clock::now();
    auto lastFullSpectrum = lastFrame;
    const auto started = lastFrame;

    while (!m_stopWorker) {
        int intervalMs = 40;
        int holdMs = 0;
        spectrum::View view;
        int points = 1024;
        int average = 0;
        int wfAverage = 0;
        bool weighted = false;
        {
            std::lock_guard lock(m_viewLock);
            intervalMs = m_frameIntervalMs;
            holdMs = m_waterfallHoldMs;
            view = m_view;
            points = m_points;
            average = m_average;
            wfAverage = m_waterfallAverage;
            weighted = m_weightedAverage;
        }
        qint64 lo = 0;
        int count = 0;
        bool resetAverage = false;
        {
            std::unique_lock lock(m_ringLock);
            m_ringWake.wait_for(lock, std::chrono::milliseconds(intervalMs),
                                [this] { return m_stopWorker.load(); });
            if (m_stopWorker) {
                break;
            }
            const quint64 total = m_samples.load(std::memory_order_relaxed);
            // Whole FFT blocks of samples all taken at the current LO, and new
            // ones since the last frame (a stalled stream draws nothing).
            if (m_samplesSinceRetune >= n && total != lastSamples) {
                const quint64 fresh = std::min<quint64>(
                    {total - lastSamples, m_samplesSinceRetune, kRingSamples / 2});
                lastSamples = total;
                count = std::clamp(static_cast<int>(fresh / n), 1, kMaxBlocks);
                const std::size_t want = n * static_cast<std::size_t>(count);
                std::size_t start = (m_writeIndex + kRingSamples - want) & (kRingSamples - 1);
                for (std::size_t i = 0; i < want; ++i) {
                    blocks[i] = m_ring[start];
                    start = (start + 1) & (kRingSamples - 1);
                }
                lo = m_activeLo.load();
                resetAverage = m_resetAverage;
                m_resetAverage = false;
            }
        }
        if (count == 0) {
            continue;
        }
        processor.setAverage(average);
        processor.setWaterfallAverage(wfAverage);
        processor.setWeightedAverage(weighted);
        if (resetAverage) {
            processor.resetAverage();
            waterfall.reset();
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastFrame).count();
        lastFrame = now;
        const std::vector<float>& db = processor.process(blocks.data(), count, elapsed, lo);
        // The skimmer's channel search reads the whole capture, not the view.
        const int fullMs = m_fullSpectrumMs.load(std::memory_order_relaxed);
        if (fullMs > 0 && now - lastFullSpectrum >= std::chrono::milliseconds(fullMs)) {
            lastFullSpectrum = now;
            std::vector<float> full = db;
            QMetaObject::invokeMethod(this, [this, full = std::move(full), lo, generation] {
                if (generation == m_generation) {
                    emit fullSpectrumReady(full, lo);
                }
            }, Qt::QueuedConnection);
        }
        const float offset = static_cast<float>(levelOffsetDb());
        std::vector<float> frame = spectrum::cropToView(db, lo, view, points);
        std::vector<float> raw = spectrum::cropToView(processor.waterfallDb(), lo, view, points);
        for (float& v : frame) {
            v += offset;
        }
        for (float& v : raw) {
            v += offset;
        }
        waterfall.setHoldMs(holdMs);
        const std::vector<float>& held = waterfall.push(
            raw, std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        QByteArray bytes(reinterpret_cast<const char*>(frame.data()),
                         static_cast<qsizetype>(frame.size() * sizeof(float)));
        QByteArray wf(reinterpret_cast<const char*>(held.data()),
                      static_cast<qsizetype>(held.size() * sizeof(float)));
        const qint64 lowHz = view.centreHz - view.spanHz / 2;
        const qint64 highHz = view.centreHz + view.spanHz / 2;
        QMetaObject::invokeMethod(this, [this, bytes, wf, lowHz, highHz, generation] {
            if (generation == m_generation) {
                emit frameReady(bytes, wf, lowHz, highHz);
            }
        }, Qt::QueuedConnection);
    }
}

}  // namespace AetherSDR::orion
