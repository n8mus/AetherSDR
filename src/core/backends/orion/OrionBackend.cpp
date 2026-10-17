#include "core/backends/orion/OrionBackend.h"

#include <QLoggingCategory>
#include <QSerialPort>

#include <cmath>
#include <cstdlib>
#include <type_traits>

#if defined(Q_OS_UNIX)
#include <sys/ioctl.h>
#include <termios.h>
#endif

Q_LOGGING_CATEGORY(lcOrion, "aether.orion")

namespace AetherSDR::orion {

namespace {
// QSerialPort opens the tty in exclusive mode (TIOCEXCL) and does not clear
// it on close. The kernel clears it on the tty's LAST close, so a real port
// is fine, but anything else holding the device open (a pty bridge, the fake
// radio) would leave every later open failing with "busy". Release it here.
void closeSerial(QSerialPort& port)
{
#if defined(Q_OS_UNIX) && defined(TIOCNXCL)
    if (port.isOpen() && port.handle() >= 0) {
        ::ioctl(port.handle(), TIOCNXCL);
    }
#endif
    port.close();
}
}  // namespace

OrionBackend::OrionBackend(QObject* parent)
    : IRadioBackend(parent)
{
    m_clock.start();

    m_connectTimer.setSingleShot(true);
    m_connectTimer.setInterval(kConnectTimeoutMs);
    connect(&m_connectTimer, &QTimer::timeout, this, [this] {
        failLink(tr("No reply from the Orion on %1. Check the radio is on, the "
                    "cable is connected, and no other program has the port open.")
                     .arg(m_device));
    });

    m_pollTimer.setInterval(OrionPollScheduler::kTickMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &OrionBackend::onPollTick);

    m_linkTimer.setInterval(kLinkTickMs);
    connect(&m_linkTimer, &QTimer::timeout, this, &OrionBackend::onLinkTick);

    // Drags arrive at mouse rate; the Orion takes about 25 writes a second.
    m_writeTimer.setSingleShot(true);
    m_writeTimer.setInterval(kWriteCoalesceMs);
    connect(&m_writeTimer, &QTimer::timeout, this, &OrionBackend::flushWrites);

    m_placeholderSpectrumTimer.setInterval(kPlaceholderSpectrumMs);
    connect(&m_placeholderSpectrumTimer, &QTimer::timeout, this,
            &OrionBackend::onPlaceholderSpectrumTick);
}

OrionBackend::~OrionBackend()
{
    // Same drain as disconnectRadio(), without signals: nothing is listening
    // any more and RadioModel has already torn down its wiring.
    ++m_generation;
    m_connectTimer.stop();
    m_pollTimer.stop();
    m_linkTimer.stop();
    m_writeTimer.stop();
    m_placeholderSpectrumTimer.stop();
    if (m_port) {
        m_port->disconnect(this);
        closeSerial(*m_port);
    }
}

QString OrionBackend::deviceFromSerial(const QString& serial)
{
    const QString trimmed = serial.trimmed();
    const QString prefix = familyName() + QLatin1Char(':');
    if (trimmed.startsWith(prefix, Qt::CaseInsensitive)) {
        return trimmed.mid(prefix.size()).trimmed();
    }
    return trimmed;
}

std::optional<Rx> OrionBackend::rxForSlice(int sliceId)
{
    if (sliceId == 0) {
        return Rx::Main;
    }
    if (sliceId == 1) {
        return Rx::Sub;
    }
    return std::nullopt;
}

RadioCapabilities OrionBackend::capabilities() const
{
    RadioCapabilities c;
    c.family = familyName();
    c.manufacturer = QStringLiteral("Ten-Tec");
    c.model = QStringLiteral("Orion 565/566");

    // Two fixed receivers; slices are neither created nor removed.
    c.canCreateSlices = false;
    c.maxSlices = 2;
    c.maxPanadapters = 1;
    c.tuningMinHz = static_cast<double>(kMinHz);
    c.tuningMaxHz = static_cast<double>(kMaxHz);
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Radio, kMinHz, kMaxHz};
    c.receiveModeControl = std::nullopt;
    c.receiveFilterControl = std::nullopt;
    c.receiveAudioControl = std::nullopt;
    c.receiveAudioExport = std::nullopt;
    c.receivePanCenterControl = std::nullopt;
    c.receivePanBandwidthControl = std::nullopt;
    c.broadcastFmReceive = std::nullopt;

    // STEP 1 IS RECEIVE-ONLY. Transmit, PTT and TX audio arrive in step 6,
    // operator-attended (fork rule 3).
    c.canTransmit = false;
    c.txPowerMaxWatts = 0.0;
    c.hasRadioPttReadback = false;
    c.radioPttObservation = std::nullopt;
    c.hostModulates = false;
    c.takesTxAudioOverSeam = false;
    c.transmitDriveControl = std::nullopt;
    c.speechProcessorControl = std::nullopt;
    c.voxControl = std::nullopt;
    c.txMonitorControl = std::nullopt;
    c.hasTxFilterControls = false;
    c.hasTuner = false;
    c.hasTunerMemories = false;
    c.hasCwTune = false;
    // CW is the WinKeyer's job (operator decision), never the radio's keyer.
    c.hasRadioSideCwKeyer = false;
    c.cwTextHasProgress = false;
    c.cwTextHasStoredMacros = false;
    c.cwTextSupportsLive = false;
    c.cwTextSupportsSpeedModifiers = false;
    c.hasVoiceKeyer = false;
    c.hasFullDuplex = false;
    c.twoToneGenerator = std::nullopt;

    // No spectrum yet (the RSP2 arrives in step 2), and when it does the
    // scale is ours, not the radio's.
    c.radioOwnsDbmScale = false;
    c.hasDaxStreams = false;
    c.hasDdcPanEdgeRolloff = false;
    c.panZoomModes = std::nullopt;
    c.panFrameRateShaping = std::nullopt;
    c.widebandConverterView = std::nullopt;

    // The radio's DSP (NR/NB/notch) exists but is not wired in step 1.
    c.hasRadioSideDsp = false;
    c.hasLmsNoiseFilters = false;
    c.hasAudioPeakingFilter = false;
    c.hasHostNoiseBlanker = false;
    c.hasManualNotch = false;
    c.maxNotchFilters = 0;
    c.hasExtendedDsp = false;
    c.hasFmRepeaterOffset = false;
    c.fmTonePresentation = FmTonePresentation::Hidden;

    // AGC letters F/M/S/O; the programmable 'P' has no neutral name.
    c.hasAgcThreshold = false;
    c.agcModes = {QStringLiteral("off"), QStringLiteral("slow"), QStringLiteral("med"),
                  QStringLiteral("fast")};

    // Continuous filter, 1 Hz resolution. The ceiling depends on the main
    // receiver's mode (AM 9000, else 6000).
    const Mode mode = m_rx[0].mode.value_or(Mode::Usb);
    c.rxFilterControl.minimumWidthHz = kMinBandwidthHz;
    c.rxFilterControl.maximumWidthHz = mode == Mode::Am ? 2 * maxBandwidthHz(mode)
                                                        : maxBandwidthHz(mode);
    c.rxFilterControl.widthStepHz = 50;
    switch (mode) {
    case Mode::Cw:
    case Mode::Cwl:
        c.rxFilterWidthsHz = {250, 500, 1000, 1800};
        break;
    case Mode::Am:
        c.rxFilterWidthsHz = {6000, 9000, 12000};
        break;
    case Mode::Fm:
        c.rxFilterWidthsHz = {6000};
        break;
    default:
        c.rxFilterWidthsHz = {1800, 2100, 2400, 2700, 3000};
        break;
    }

    c.hasProfiles = false;
    c.hasWaveforms = false;
    c.hasMultiClientSessions = false;
    c.alwaysUseClientSideSpots = true;
    c.hasRadioSideWaterfallAutoBlack = false;
    c.persistsMemories = false;
    c.canWriteMemories = false;
    c.canApplyMemories = false;
    c.canRefreshMemories = false;
    c.canReboot = false;
    c.hasRemoteOnControl = false;
    c.canUpgradeFirmware = false;
    c.hasSmartLink = false;
    c.hasLicenseInfo = false;
    c.hasClientNetworkConfig = false;
    c.hasFlexControlIntegration = false;
    c.hasAudioCompression = false;
    c.hasSharpFilters = false;
    c.usesVita49Transport = false;
    c.hasNetworkConfigurationReadback = false;
    c.hasPrivateIpConnectionPolicy = false;
    c.hasRadioDialLock = false;
    c.hasTransmitFrequencyCheck = false;
    c.hasSelectableMicInputs = false;
    c.hasDownwardExpander = false;
    c.hasAmCarrierLevel = false;
    c.hasModeIndependentSquelch = false;
    c.hasSupplyVoltageTelemetry = false;
    c.hasPaTemperatureTelemetry = false;
    c.hasPaCurrentTelemetry = false;
    c.hasMainFanTelemetry = false;
    c.hasGpsLocation = false;
    c.hasGpsSatelliteTelemetry = false;
    c.hasGpsFrequencyReference = false;
    c.hasGpsTimeConfiguration = false;
    c.hasGpsHardware = false;
    c.gpsHardwareRequiresPresence = false;
    c.firmwareUpdateSource = std::nullopt;
    c.lineoutControl = std::nullopt;
    c.txWaterfallClientFlag = std::nullopt;

    // Radio-authoritative: restore nothing (Constitution II/III).
    c.clientSettingsDomains = {};
    return c;
}

// ---- connection ------------------------------------------------------------

void OrionBackend::connectRadio(const RadioConnectRequest& request)
{
    if (m_connected || m_connecting) {
        disconnectRadio();
    }
    m_device = deviceFromSerial(request.serial);
    if (m_device.isEmpty()) {
        m_device = defaultDevice();
    }
    openLink();
}

void OrionBackend::openLink()
{
    ++m_generation;
    m_rx = {};
    m_panCentreMhz = 0.0;
    m_txVfo.reset();
    m_activeSlice = 0;
    m_rxBuffer.clear();
    m_scheduler.reset();
    m_rxBytes = 0;
    m_txBytes = 0;
    m_repliesDecoded = 0;
    m_linesIgnored = 0;
    m_lastReplyMs = -1;
    m_rttMs = -1;
    m_frequencyQuerySentMs = -1;

    m_port = std::make_unique<QSerialPort>();
    m_port->setPortName(m_device);
    m_port->setBaudRate(57600);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        const QString reason = m_port->errorString();
        m_port.reset();
        emit connectionError(tr("Cannot open the Orion's serial port %1: %2")
                                 .arg(m_device, reason));
        return;
    }
    // The Orion's RS-232 side goes silent with DTR low (live-found). On a
    // pty these ioctls fail harmlessly, so their result is not checked.
    m_port->setDataTerminalReady(true);
    m_port->setRequestToSend(true);

    connect(m_port.get(), &QSerialPort::readyRead, this, &OrionBackend::onReadyRead);
    connect(m_port.get(), &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError error) {
                // Only a lost device ends the session; a refused modem-line
                // ioctl (UnsupportedOperation on a pty) does not.
                if (error == QSerialPort::ResourceError
                    || error == QSerialPort::PermissionError
                    || error == QSerialPort::DeviceNotFoundError) {
                    failLink(tr("Lost the Orion's serial port %1: %2")
                                 .arg(m_device, m_port ? m_port->errorString() : QString()));
                }
            });

    m_connecting = true;
    qCInfo(lcOrion) << "Orion: opened" << m_device << "- waiting for the radio to answer";
    m_connectTimer.start();
    send(queryFrequency(Rx::Main));
    m_frequencyQuerySentMs = nowMs();
}

void OrionBackend::closeLink()
{
    ++m_generation;
    m_connectTimer.stop();
    m_pollTimer.stop();
    m_linkTimer.stop();
    m_writeTimer.stop();
    m_placeholderSpectrumTimer.stop();
    for (Receiver& r : m_rx) {
        r.pendingHz.reset();
        r.pendingFilter.reset();
    }
    if (m_port) {
        m_port->disconnect(this);
        closeSerial(*m_port);
        m_port.reset();
    }
    m_connecting = false;
}

void OrionBackend::failLink(const QString& reason)
{
    const bool wasConnected = m_connected;
    qCWarning(lcOrion) << "Orion:" << reason;
    closeLink();
    m_connected = false;
    if (wasConnected) {
        emit disconnected();
    }
    emit connectionError(reason);
}

void OrionBackend::disconnectRadio()
{
    const bool wasConnected = m_connected;
    closeLink();
    m_connected = false;
    if (wasConnected) {
        emit disconnected();
    }
}

// ---- wire ------------------------------------------------------------------

void OrionBackend::send(const QByteArray& command)
{
    if (!m_port || command.isEmpty()) {
        return;
    }
    QByteArray out = command;
    out.append('\r');   // CR terminates every Orion command
    m_txBytes += m_port->write(out);
}

void OrionBackend::onReadyRead()
{
    if (!m_port) {
        return;
    }
    const QByteArray chunk = m_port->readAll();
    m_rxBytes += chunk.size();
    m_trafficSinceTick = true;
    m_rxBuffer.append(chunk);
    // A link that never sends CR must not grow the buffer without bound.
    if (m_rxBuffer.size() > 4096) {
        m_rxBuffer = m_rxBuffer.right(256);
    }
    for (;;) {
        const qsizetype cr = m_rxBuffer.indexOf('\r');
        if (cr < 0) {
            break;
        }
        QByteArray line = m_rxBuffer.left(cr).trimmed();
        m_rxBuffer.remove(0, cr + 1);
        if (!line.isEmpty()) {
            onLine(line);
            if (!m_port) {
                return;   // the line ended the session
            }
        }
    }
}

void OrionBackend::onLine(const QByteArray& line)
{
    const std::optional<Reply> reply = decodeReply(line);
    if (!reply) {
        ++m_linesIgnored;
        return;
    }
    ++m_repliesDecoded;
    m_lastReplyMs = nowMs();
    if (m_connecting && !m_connected) {
        onFirstReply();
    }

    std::visit([this](const auto& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, FrequencyReply>) {
            m_scheduler.noteFrequencyReply(r.rx);
            if (r.rx == Rx::Main) {
                if (m_frequencyQuerySentMs >= 0) {
                    m_rttMs = static_cast<int>(nowMs() - m_frequencyQuerySentMs);
                    m_frequencyQuerySentMs = -1;
                }
            }
            applyFrequency(r.rx, r.hz);
        } else if constexpr (std::is_same_v<T, ModeReply>) {
            applyMode(r.rx, r.mode);
        } else if constexpr (std::is_same_v<T, BandwidthReply>) {
            applyBandwidth(r.rx, r.hz);
        } else if constexpr (std::is_same_v<T, PbtReply>) {
            applyPbt(r.rx, r.hz);
        } else if constexpr (std::is_same_v<T, AgcReply>) {
            if (const auto neutral = neutralFromAgcLetter(r.letter)) {
                SliceDelta d;
                d.agcMode = *neutral;
                emit sliceChanged(sliceForRx(r.rx), d);
            }
        } else if constexpr (std::is_same_v<T, RfGainReply>) {
            if (r.rx == Rx::Main) {
                emit panRfGainChanged(panId(), r.percent);
            }
        } else if constexpr (std::is_same_v<T, AttenuatorReply>) {
            if (r.rx == Rx::Main) {
                emit panAttenuatorChanged(panId(), r.step);
            }
        } else if constexpr (std::is_same_v<T, PreampReply>) {
            if (r.rx == Rx::Main) {
                emit panPreampChanged(panId(), r.on ? 1 : 0);
            }
        } else if constexpr (std::is_same_v<T, SMeterReply>) {
            emit meterUpdate(QStringLiteral("SLC:LEVEL"), rawToDbm(r.mainRaw));
            emit meterUpdate(QStringLiteral("SLC1:LEVEL"), rawToDbm(r.subRaw));
        } else if constexpr (std::is_same_v<T, TxMeterReply>) {
            // Step 1 never keys; a TX reply means the operator keyed from the
            // front panel. Not published until the TX step defines its meters.
        } else if constexpr (std::is_same_v<T, VfoAssignmentReply>) {
            if (m_txVfo != r.tx) {
                m_txVfo = r.tx;
                SliceDelta a;
                a.txSlice = r.tx == 'A';
                emit sliceChanged(0, a);
                SliceDelta b;
                b.txSlice = r.tx == 'B';
                emit sliceChanged(1, b);
            }
        }
    }, *reply);
}

void OrionBackend::onFirstReply()
{
    m_connectTimer.stop();
    m_connecting = false;
    m_connected = true;
    qCInfo(lcOrion) << "Orion: radio answered on" << m_device;

    // CONNECTED FIRST, then the state: RadioModel clears its slices on the
    // connect edge, so a slice published before it would be swept away.
    emit connected();
    emit capabilitiesChanged();
    publishInitialState();
    startupQueries();
    m_pollTimer.start();
    m_linkTimer.start();
    m_placeholderSpectrumTimer.start();
    onPlaceholderSpectrumTick();
}

void OrionBackend::publishInitialState()
{
    RadioDelta r;
    r.model = QStringLiteral("Orion");
    r.nickname = QStringLiteral("Ten-Tec Orion");
    emit radioChanged(r);

    // The pan is announced before the slices that name it. Geometry is
    // provisional until dial A answers.
    publishPan();

    for (int slice = 0; slice < 2; ++slice) {
        SliceDelta s;
        s.panId = panId();
        s.inUse = true;
        s.active = slice == m_activeSlice;
        s.modeList = neutralModeList();
        s.txSlice = slice == 0;
        emit sliceChanged(slice, s);
    }

    publishMeterDefs();

    emit panRfGainInfoChanged(panId(), 0, 100, 1, QStringLiteral("%"));
    emit panPreampInfoChanged(panId(), {tr("OFF"), tr("ON")});
    emit panAttenuatorInfoChanged(panId(), {QStringLiteral("0 dB"), QStringLiteral("6 dB"),
                                            QStringLiteral("12 dB"), QStringLiteral("18 dB")});

    // The reply that connected us carried dial A; publish it now that the
    // slice exists.
    for (Rx rx : {Rx::Main, Rx::Sub}) {
        if (const auto hz = receiver(rx).hz) {
            SliceDelta d;
            d.frequency = static_cast<double>(*hz) / 1e6;
            emit sliceChanged(sliceForRx(rx), d);
        }
    }
}

void OrionBackend::publishMeterDefs()
{
    // The first receiver keeps the bare "SLC:LEVEL" every single-receiver
    // consumer looks for; the second carries its sourceIndex as a suffix.
    for (int slice = 0; slice < 2; ++slice) {
        MeterDef d;
        d.index = 1 + slice;
        d.source = QStringLiteral("SLC");
        d.sourceIndex = slice;
        d.name = QStringLiteral("LEVEL");
        d.unit = QStringLiteral("dBm");
        d.low = -140.0;
        d.high = -10.0;
        d.description = slice == 0 ? tr("Main receiver signal level")
                                   : tr("Sub receiver signal level");
        emit meterDefined(d);
    }
}

void OrionBackend::startupQueries()
{
    // A burst sized well inside the UART budget. The slow wheel picks up the
    // rest within a lap.
    for (Rx rx : {Rx::Main, Rx::Sub}) {
        send(queryFrequency(rx));
        send(queryMode(rx));
        send(queryBandwidth(rx));
        send(queryPbt(rx));
        send(queryAgc(rx));
    }
    send(queryVfoAssignment());
    send(queryRfGain(Rx::Main));
    send(queryAttenuator(Rx::Main));
    send(queryPreamp(Rx::Main));
}

void OrionBackend::onPollTick()
{
    if (!m_connected) {
        return;
    }
    if (m_lastReplyMs >= 0 && nowMs() - m_lastReplyMs > kSilenceTimeoutMs) {
        failLink(tr("The Orion stopped answering on %1.").arg(m_device));
        return;
    }
    const QList<QByteArray> queries = m_scheduler.next(nowMs());
    for (const QByteArray& q : queries) {
        if (q == queryFrequency(Rx::Main)) {
            m_frequencyQuerySentMs = nowMs();
        }
        send(q);
    }
}

void OrionBackend::onPlaceholderSpectrumTick()
{
    // Step 1 has no spectrum source. A flat floor says "no data" honestly and
    // lets the pan finish connecting; step 2 replaces it with the RSP2.
    if (!m_connected) {
        return;
    }
    static const QByteArray kFrame = [] {
        QByteArray frame(kPlaceholderSpectrumBins * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        auto* bins = reinterpret_cast<float*>(frame.data());
        for (int i = 0; i < kPlaceholderSpectrumBins; ++i) {
            bins[i] = kPlaceholderFloorDbm;
        }
        return frame;
    }();
    emit spectrumFrameReady(0, kFrame);
}

void OrionBackend::onLinkTick()
{
    if (!m_connected) {
        return;
    }
    emit linkStatsUpdated(linkStats());
    m_trafficSinceTick = false;
}

// ---- decode → model ----------------------------------------------------------

void OrionBackend::applyFrequency(Rx rx, qint64 hz)
{
    Receiver& r = receiver(rx);
    // ECHO GUARD: right after we tuned, an older reply still in flight would
    // snap the slice back. Only our own value is accepted in that window.
    if (r.sinceFrequencyEdit.isValid()
        && r.sinceFrequencyEdit.elapsed() < kFrequencyEchoGuardMs) {
        const qint64 expected = r.pendingHz.value_or(r.hz.value_or(hz));
        if (hz != expected) {
            return;
        }
    }
    if (r.hz && *r.hz == hz) {
        r.candidateHz.reset();
        return;
    }
    // A jump of more than 100 kHz needs two identical reads: a corrupted
    // reply must not throw the slice across the band.
    if (r.hz && std::llabs(hz - *r.hz) > kLargeJumpHz) {
        if (r.candidateHz != hz) {
            r.candidateHz = hz;
            return;
        }
    }
    r.candidateHz.reset();
    const bool knobMoved = r.hz.has_value();
    r.hz = hz;
    if (knobMoved) {
        m_scheduler.noteDialMoved(rx, nowMs());
    }
    if (!m_connected) {
        return;   // published once the slices exist (publishInitialState)
    }
    SliceDelta d;
    d.frequency = static_cast<double>(hz) / 1e6;
    emit sliceChanged(sliceForRx(rx), d);
    if (rx == Rx::Main) {
        publishPan();
    }
}

void OrionBackend::applyMode(Rx rx, Mode mode)
{
    Receiver& r = receiver(rx);
    if (r.sinceModeEdit.isValid() && r.sinceModeEdit.elapsed() < kFrequencyEchoGuardMs
        && r.mode && *r.mode != mode) {
        return;
    }
    if (r.mode && *r.mode == mode) {
        r.candidateMode.reset();
        return;
    }
    // A polled mode change needs two agreeing reports.
    if (r.mode && r.candidateMode != mode) {
        r.candidateMode = mode;
        return;
    }
    r.candidateMode.reset();
    r.mode = mode;
    SliceDelta d;
    d.mode = neutralMode(mode);
    emit sliceChanged(sliceForRx(rx), d);
    publishPassband(rx);
    if (rx == Rx::Main) {
        emit capabilitiesChanged();   // the filter ceiling follows the mode
    }
}

void OrionBackend::applyBandwidth(Rx rx, int hz)
{
    Receiver& r = receiver(rx);
    if (r.sinceFilterEdit.isValid() && r.sinceFilterEdit.elapsed() < kFilterEchoGuardMs
        && r.bandwidthHz && *r.bandwidthHz != hz) {
        return;
    }
    if (r.bandwidthHz == hz) {
        return;
    }
    r.bandwidthHz = hz;
    publishPassband(rx);
}

void OrionBackend::applyPbt(Rx rx, int hz)
{
    Receiver& r = receiver(rx);
    if (r.sinceFilterEdit.isValid() && r.sinceFilterEdit.elapsed() < kFilterEchoGuardMs
        && r.pbtHz && *r.pbtHz != hz) {
        return;
    }
    if (r.pbtHz == hz) {
        return;
    }
    r.pbtHz = hz;
    publishPassband(rx);
}

void OrionBackend::publishPassband(Rx rx)
{
    const Receiver& r = receiver(rx);
    // The sub receiver runs the main's mode (the radio copies it), so its
    // sideband placement falls back to the main mode until it reports.
    const std::optional<Mode> mode = r.mode ? r.mode : m_rx[0].mode;
    if (!m_connected || !mode || !r.bandwidthHz || !r.pbtHz) {
        return;
    }
    const Passband p = passbandFromFilter(*mode, *r.bandwidthHz, *r.pbtHz);
    SliceDelta d;
    d.filterLow = p.lowHz;
    d.filterHigh = p.highHz;
    emit sliceChanged(sliceForRx(rx), d);
}

void OrionBackend::publishPan()
{
    // No spectrum until the RSP2 arrives (step 2), so the pan is only a view.
    // It keeps the centre the operator or the recenter policy chose, and
    // follows dial A only when A leaves it, so the main slice stays in frame.
    if (!m_rx[0].hz) {
        emit panCenterBandwidthChanged(panId(), 0.0, 0.0);
        return;
    }
    const double dialMhz = static_cast<double>(*m_rx[0].hz) / 1e6;
    const double halfMhz = kPanViewMhz / 2.0;
    if (m_panCentreMhz <= 0.0 || std::fabs(dialMhz - m_panCentreMhz) > halfMhz) {
        m_panCentreMhz = dialMhz;
    }
    emit panCenterBandwidthChanged(panId(), m_panCentreMhz, kPanViewMhz);
}

// ---- intents down ------------------------------------------------------------

void OrionBackend::setSliceFrequency(int sliceId, double hz)
{
    const std::optional<Rx> rx = rxForSlice(sliceId);
    const qint64 rounded = std::llround(hz);
    if (!rx || !m_connected || rounded < kMinHz || rounded > kMaxHz) {
        return;
    }
    Receiver& r = receiver(*rx);
    r.pendingHz = rounded;
    r.sinceFrequencyEdit.start();
    if (!m_writeTimer.isActive()) {
        m_writeTimer.start();
    }
}

void OrionBackend::setSliceMode(int sliceId, const QString& neutral)
{
    const std::optional<Rx> rx = rxForSlice(sliceId);
    const std::optional<Mode> mode = modeFromNeutral(neutral);
    if (!rx || !mode || !m_connected) {
        return;
    }
    Receiver& r = receiver(*rx);
    r.mode = *mode;
    r.candidateMode.reset();
    r.sinceModeEdit.start();
    SliceDelta d;
    d.mode = neutralMode(*mode);
    emit sliceChanged(sliceId, d);
    const QByteArray command = orion::setMode(*rx, *mode);
    // The Orion drops commands while it is busy retuning: a mode change
    // trails a frequency write by 120 ms.
    const qint64 sinceRetune = nowMs() - m_lastFrequencyWriteMs;
    const quint64 generation = m_generation;
    const Rx which = *rx;
    const auto sendModeAndRecall = [this, generation, command, which] {
        if (generation != m_generation) {
            return;
        }
        send(command);
        // The Orion recalls its per-mode stored filter on a mode change;
        // fetch it once the firmware has settled.
        QTimer::singleShot(kFilterRecallMs, this, [this, generation, which] {
            if (generation != m_generation) {
                return;
            }
            send(queryBandwidth(which));
            send(queryPbt(which));
        });
    };
    if (sinceRetune < kModeAfterRetuneMs || m_writeTimer.isActive()) {
        QTimer::singleShot(kModeAfterRetuneMs, this, sendModeAndRecall);
    } else {
        sendModeAndRecall();
    }
    publishPassband(*rx);
    if (*rx == Rx::Main) {
        emit capabilitiesChanged();
    }
}

void OrionBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    const std::optional<Rx> rx = rxForSlice(sliceId);
    if (!rx || !m_connected) {
        return;
    }
    Receiver& r = receiver(*rx);
    const std::optional<Mode> mode = r.mode ? r.mode : m_rx[0].mode;
    if (!mode) {
        return;   // no placement rule until the radio has told us its mode
    }
    const FilterSetting f = filterFromPassband(*mode, lowHz, highHz);
    r.pendingFilter = f;
    r.bandwidthHz = f.bandwidthHz;
    r.pbtHz = f.pbtHz;
    r.sinceFilterEdit.start();
    publishPassband(*rx);
    if (!m_writeTimer.isActive()) {
        m_writeTimer.start();
    }
}

void OrionBackend::flushWrites()
{
    for (Rx rx : {Rx::Main, Rx::Sub}) {
        Receiver& r = receiver(rx);
        if (r.pendingHz) {
            send(setFrequency(rx, *r.pendingHz));
            r.hz = *r.pendingHz;
            r.pendingHz.reset();
            m_lastFrequencyWriteMs = nowMs();
            SliceDelta d;
            d.frequency = static_cast<double>(*r.hz) / 1e6;
            emit sliceChanged(sliceForRx(rx), d);
            m_scheduler.noteDialMoved(rx, nowMs());
            if (rx == Rx::Main) {
                publishPan();
            }
        }
        if (r.pendingFilter) {
            const Mode mode = r.mode.value_or(m_rx[0].mode.value_or(Mode::Usb));
            send(setBandwidth(rx, mode, r.pendingFilter->bandwidthHz));
            send(setPbt(rx, r.pendingFilter->pbtHz));
            r.pendingFilter.reset();
        }
    }
}

void OrionBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    Q_UNUSED(thresholdDb);   // hasAgcThreshold is false
    const std::optional<Rx> rx = rxForSlice(sliceId);
    const std::optional<char> letter = agcLetterFromNeutral(mode);
    if (!rx || !letter || !m_connected) {
        return;
    }
    send(setAgc(*rx, *letter));
    send(queryAgc(*rx));
}

void OrionBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(panId);
    Q_UNUSED(intent);
    // No receive window to move until the RSP2 (step 2): take the view centre
    // as asked and report it, so a drag or a recenter sticks.
    if (!m_connected || hz < static_cast<double>(kMinHz) || hz > static_cast<double>(kMaxHz)) {
        return;
    }
    m_panCentreMhz = hz / 1e6;
    emit panCenterBandwidthChanged(this->panId(), m_panCentreMhz, kPanViewMhz);
}

void OrionBackend::setPanRfGain(const QString& panId, int gainDb)
{
    Q_UNUSED(panId);
    if (!m_connected) {
        return;
    }
    // The advertised range is 0..100 percent, the Orion's own RF gain scale.
    send(setRfGain(Rx::Main, gainDb));
    send(queryRfGain(Rx::Main));
}

void OrionBackend::setPanPreamp(const QString& panId, int step)
{
    Q_UNUSED(panId);
    if (!m_connected) {
        return;
    }
    send(setPreamp(Rx::Main, step > 0));
    send(queryPreamp(Rx::Main));
}

void OrionBackend::setPanAttenuator(const QString& panId, int step)
{
    Q_UNUSED(panId);
    if (!m_connected) {
        return;
    }
    send(setAttenuator(Rx::Main, step));
    send(queryAttenuator(Rx::Main));
}

void OrionBackend::setActiveSlice(int sliceId)
{
    if (!rxForSlice(sliceId) || sliceId == m_activeSlice) {
        return;
    }
    // The radio has no "active" notion to echo, so clear the old one here.
    const int previous = m_activeSlice;
    m_activeSlice = sliceId;
    m_scheduler.setActiveReceiver(*rxForSlice(sliceId));
    if (!m_connected) {
        return;
    }
    SliceDelta off;
    off.active = false;
    emit sliceChanged(previous, off);
    SliceDelta on;
    on.active = true;
    emit sliceChanged(sliceId, on);
}

void OrionBackend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation,
                             const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(key);
    Q_UNUSED(operation);
    Q_UNUSED(completion);
    // Step 1 is receive-only: canTransmit is false, so this is never
    // authorized. Deliberately no wire command (fork rule 3).
}

void OrionBackend::invokeExtension(const QString& ns, const QString& verb,
                                   quint64 requestId, const QVariant& arg)
{
    Q_UNUSED(arg);
    if (requestId != 0) {
        emit extensionError(requestId, tr("Orion backend has no extension %1.%2")
                                           .arg(ns, verb));
    }
}

// ---- diagnostics -------------------------------------------------------------

IRadioBackend::HealthSnapshot OrionBackend::healthSnapshot() const
{
    HealthSnapshot h;
    h.values.insert(QStringLiteral("device"), m_device);
    h.values.insert(QStringLiteral("repliesDecoded"), QVariant::fromValue(m_repliesDecoded));
    h.values.insert(QStringLiteral("linesIgnored"), QVariant::fromValue(m_linesIgnored));
    if (m_lastReplyMs >= 0) {
        h.values.insert(QStringLiteral("lastReplyAgeMs"), nowMs() - m_lastReplyMs);
    }
    if (m_rttMs >= 0) {
        h.values.insert(QStringLiteral("frequencyRoundTripMs"), m_rttMs);
    }
    h.order = {QStringLiteral("device"), QStringLiteral("repliesDecoded"),
               QStringLiteral("linesIgnored"), QStringLiteral("lastReplyAgeMs"),
               QStringLiteral("frequencyRoundTripMs")};
    h.sections.insert(QStringLiteral("device"), tr("CAT link"));
    h.labels.insert(QStringLiteral("device"), tr("Serial device"));
    h.labels.insert(QStringLiteral("repliesDecoded"), tr("Replies decoded"));
    h.labels.insert(QStringLiteral("linesIgnored"), tr("Lines ignored"));
    h.labels.insert(QStringLiteral("lastReplyAgeMs"), tr("Last reply (ms ago)"));
    h.labels.insert(QStringLiteral("frequencyRoundTripMs"), tr("?AF round trip (ms)"));
    return h;
}

IRadioBackend::LinkStats OrionBackend::linkStats() const
{
    LinkStats s;
    if (!m_connected) {
        return s;
    }
    s.reported = true;
    s.alive = m_trafficSinceTick;
    s.rxBytes = m_rxBytes;
    s.txBytes = m_txBytes;
    s.rxPackets = m_repliesDecoded;
    s.rttMs = m_rttMs;
    s.localEndpoint = m_device;
    return s;
}

}  // namespace AetherSDR::orion
