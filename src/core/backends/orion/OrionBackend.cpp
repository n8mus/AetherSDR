#include "core/backends/orion/OrionBackend.h"

#include "core/WaterfallRate.h"

#ifdef AETHER_ORION_SDRPLAY
#include "core/backends/orion/OrionRxAudio.h"
#include "core/backends/orion/Rsp2Receiver.h"
#else
namespace AetherSDR::orion {
// Built without the SDRplay API headers: no RSP2, the pan shows the
// placeholder trace. A complete type so the unique_ptr member compiles.
class Rsp2Receiver {};
class OrionRxAudio {};
}  // namespace AetherSDR::orion
#endif

#include "core/backends/orion/OrionCwDaemon.h"
#include "core/backends/orion/OrionPatMonitor.h"
#include "core/backends/orion/OrionRotor.h"
#include "core/backends/orion/OrionTxAudio.h"
#include "core/backends/orion/OrionSkimmer.h"
#include "core/backends/orion/OrionSkimmerFeed.h"
#include "core/backends/orion/OrionWinKeyer.h"
#include "core/AppSettings.h"
#include "core/dsp/SpectrumTemporalAverage.h"

#include <QLoggingCategory>
#include <QSerialPort>

#include <algorithm>
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

    // A pan drag moves the view at mouse rate; the RSP2 retunes at most
    // every 50 ms.
    m_loRetuneTimer.setSingleShot(true);
    m_loRetuneTimer.setInterval(kLoRetuneMs);
    connect(&m_loRetuneTimer, &QTimer::timeout, this, &OrionBackend::retuneSdr);

    // A transmission nobody releases is stopped here, whatever above the seam
    // thinks (the reference program's MOX failsafe).
    m_txFailsafeTimer.setSingleShot(true);
    m_txFailsafeTimer.setInterval(kMaxKeyMs);
    connect(&m_txFailsafeTimer, &QTimer::timeout, this, &OrionBackend::onTxFailsafe);

    m_tuneTimer.setSingleShot(true);
    m_tuneTimer.setInterval(kMaxTuneMs);
    connect(&m_tuneTimer, &QTimer::timeout, this, [this] { endTune(/*timedOut=*/true); });

    m_atuTimer.setSingleShot(true);
    m_atuTimer.setInterval(kMaxAtuMs);
    connect(&m_atuTimer, &QTimer::timeout, this, [this] {
        if (m_atuCycle) {
            send(orion::setPtt(false));
            finishAtuCycle("TUNE_ABORTED",
                           tr("ATU tune cycle stopped after %1 s.").arg(kMaxAtuMs / 1000));
        }
    });

    m_txGuardReleaseTimer.setSingleShot(true);
    m_txGuardReleaseTimer.setInterval(kTxGuardHangMs);
    connect(&m_txGuardReleaseTimer, &QTimer::timeout, this, [this] {
        if (!m_keyIntent && !m_radioTx) {
            applyTxGuard(false);
        }
    });
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
    m_loRetuneTimer.stop();
    m_txFailsafeTimer.stop();
    m_txGuardReleaseTimer.stop();
    m_tuneTimer.stop();
    m_atuTimer.stop();
    // Never leave the radio keyed, and leave it usable stand-alone.
    leaveRadioSafe();
    stopPeripherals();
    stopSdr();
    m_txAudio.reset();
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
    // The RSP2 spectrum is computed here, with AetherSDR's shared time-based
    // averager (as the RTL backend): FFT AVG and Wt Avg reach it, and the
    // client keeps both between sessions. Wording as the RTL backend's.
    c.backendPanAveraging = BackendPanAveraging{SpectrumTemporalAverage::kMsPerStep, true,
        tr("Spectrum averaging time: 10 ms per step; 0 is off and 100 is one second. Higher "
           "values smooth changes but blur short signals. The decay time stays the same when "
           "FPS changes."),
        tr("Off averages signal power. On averages dB levels for a smoother trace, but varying "
           "signals and noise read lower. Both modes use the FFT AVG time setting.")};
    // The waterfall gets its OWN copy of every frame and its own average
    // (Display -> WF AVG), so FFT AVG no longer smears the waterfall with the
    // trace — they used to be built from the same averaged frames.
    c.backendPanWaterfallAveraging = BackendPanAveraging{SpectrumTemporalAverage::kMsPerStep, true,
        tr("Waterfall averaging time: 10 ms per step; 0 is off and 100 is one second. It shapes "
           "the waterfall only — FFT AVG shapes the trace."),
        QString()};

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

    // TRANSMIT (step 6). PTT by CAT; the RADIO modulates. TX audio reaches
    // it by one of two paths the operator picks in the Phone applet: MIC (the
    // radio's own front jack; AetherSDR sends no audio) or PC (AetherSDR's mic
    // chain, TCI/DAX audio, to the SignaLink). The radio's ?S reply turns
    // into a TX meter while it transmits: that is the PTT readback.
    c.canTransmit = true;
    c.txPowerMaxWatts = 100.0;
    c.hasRadioPttReadback = true;
    c.radioPttObservation = std::nullopt;
    c.hostModulates = false;
    c.takesTxAudioOverSeam = true;
    c.hasSelectableMicInputs = true;
    c.extensions.insert(QStringLiteral("micInputs"),
                        QStringList{QString::fromLatin1(kMicInputRadio),
                                    QString::fromLatin1(kMicInputPc)});
    // *TP is read back from the radio, so the drive is confirmed state.
    c.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
        SliceFrequencyControl::Authority::Radio, /*tunePowerAppliesLive=*/true};
    c.speechProcessorControl = std::nullopt;
    c.voxControl = std::nullopt;
    // MON is the radio's own TX monitor (*TO); level 0 is off.
    c.txMonitorControl = RadioCapabilities::TxMonitorControl{};
    c.hasTxFilterControls = false;
    // The internal tuner (*TT): in, bypass and the radio's own tune cycle.
    // PC Audio's state arrives on this namespace (audio.pc, audio.pc.state).
    c.extensionNamespaces << QStringLiteral("orion");
    c.hasTuner = true;
    c.hasTunerMemories = false;
    // TUNE switches the radio to FM for its carrier, so it works from CW too.
    c.hasCwTune = true;
    // CW is the WinKeyer's job (operator decision), never the radio's keyer.
    // AetherSDR's CW text keyer is offered once the WinKeyer has answered.
    const bool keyer = m_keyer && m_keyer->isReady();
    c.hasRadioSideCwKeyer = keyer;
    c.cwTextKeyerName = QStringLiteral("WinKeyer");
    c.cwTextMinWpm = OrionWinKeyer::kMinWpm;
    c.cwTextMaxWpm = OrionWinKeyer::kMaxWpm;
    c.cwSpeedMinWpm = OrionWinKeyer::kMinWpm;
    c.cwSpeedMaxWpm = OrionWinKeyer::kMaxWpm;
    // The sidetone range this radio actually has (setCwPitch clamps to it).
    // Undeclared, it fell back to the generic 100-6000 Hz, and a 5900 Hz
    // span across a ~150 px slider is about 40 Hz per pixel — the operator
    // could land on 540 or 580 but never on the 550 he uses. 1 kHz over the
    // same slider is a few Hz per pixel, and the step keeps it on tens.
    c.cwPitchMinHz = 200;
    c.cwPitchMaxHz = 1200;
    c.cwPitchStepHz = 10;
    c.cwTextMaxMessageChars = 0;
    c.cwTextAllowedCharacters = OrionWinKeyer::allowedCharacters(keyer && m_keyer->isWk3());
    c.cwTextHasProgress = false;
    c.cwTextHasStoredMacros = false;
    c.cwTextSupportsLive = false;
    c.cwTextSupportsSpeedModifiers = false;
    c.hasVoiceKeyer = false;
    c.hasFullDuplex = false;
    c.twoToneGenerator = std::nullopt;

    // The RSP2's spectrum is computed on this host and the scale is ours, not
    // the radio's.
    c.radioOwnsDbmScale = false;
    // What the vertical axis means here (as the RTL backend declares it).
    // Without this record the panadapter treated the Orion as "no backend has
    // been read": the noise-floor auto-adjust was gated off, so the trace sat
    // wherever the saved range left it — a flat floor jammed against the
    // bottom of the pane with 60 dB of empty sky above it.
    PanAmplitudeModel amplitude;
    // Levels are relative: the RSP2's gain reduction and LNA state are known,
    // but nothing here is calibrated to the antenna, so a level from this
    // receiver is never published as a spot level or held against a report.
    amplitude.calibratedDbm = false;
    // Bins are absolute host-side levels (SpectrumProcessor: 10*log10 of
    // window-normalised power, plus the fixed gain offset), so they do not
    // move when the display reference moves and the auto-adjust converges.
    amplitude.binsAbsolute = true;
    // The operator's dB range is remembered here, per radio and pan: the radio
    // echoes none of it.
    amplitude.clientPersistsDbmRange = true;
    c.panAmplitude = amplitude;
    c.hasDaxStreams = false;
    c.hasDdcPanEdgeRolloff = false;
    c.panZoomModes = std::nullopt;
    // This engine paces its own frames and rows, and the radio echoes
    // neither, so the client owns both and keeps them between sessions.
    c.panFrameRateShaping = PanFrameRateShaping{/*clientPersistsFrameRate*/ true};
    c.panWaterfallRateShaping = PanWaterfallRateShaping{/*clientPersistsWaterfallRate*/ true};
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
    m_connectParams = request.params;
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
    m_view = {};
    m_viewKnown = false;
    m_txVfo.reset();
    m_pendingTxVfo.reset();
    m_txPath = TxPath::RadioMic;
    m_keyIntent = false;
    m_radioTx = false;
    m_lastMicReading = -1;
    m_lastAuxReading = -1;
    m_repairedProfile = false;
    m_vfoLocked = {};
    m_kvMain = 'A';
    m_kvSub = 'B';
    m_activeSlice = 0;
    m_rxBuffer.clear();
    m_scheduler.reset();
    m_rxBytes = 0;
    m_txBytes = 0;
    m_repliesDecoded = 0;
    m_linesIgnored = 0;
    m_lastReplyMs = -1;
    m_lastPollMs = -1;
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
    // Unkey and give the radio back its own mic BEFORE the port closes.
    leaveRadioSafe();
    m_txFailsafeTimer.stop();
    m_txGuardReleaseTimer.stop();
    m_tuneTimer.stop();
    m_atuTimer.stop();
    if (m_txAudio) {
        m_txAudio->close();
    }
    stopPeripherals();
    m_keyIntent = false;
    m_radioTx = false;
    ++m_generation;
    m_connectTimer.stop();
    m_pollTimer.stop();
    m_linkTimer.stop();
    m_writeTimer.stop();
    m_placeholderSpectrumTimer.stop();
    m_loRetuneTimer.stop();
    stopSdr();
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
            // TUNE's borrowed FM is not the operator's mode.
            if (!(r.rx == Rx::Main && tuneHoldsReadback())) {
                applyMode(r.rx, r.mode);
            }
        } else if constexpr (std::is_same_v<T, BandwidthReply>) {
            applyBandwidth(r.rx, r.hz);
        } else if constexpr (std::is_same_v<T, PbtReply>) {
            applyPbt(r.rx, r.hz);
        } else if constexpr (std::is_same_v<T, AgcReply>) {
            if (const auto neutral = neutralFromAgcLetter(r.letter)) {
                SliceDelta d;
                d.agcMode = *neutral;
                emit sliceChanged(sliceForRx(r.rx), d);
                // The host demodulator follows the radio's AGC speed.
                const int wdsp = r.letter == 'O' ? 0 : r.letter == 'S' ? 2
                                 : r.letter == 'F' ? 4 : 3;
                m_wdspAgcMode[r.rx == Rx::Main ? 0 : 1] = wdsp;
                pushRxTuning();
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
            // In CW the radio answers in RX form between elements, so TX is
            // only over after a hang without TX replies.
            if (m_radioTx && nowMs() - m_lastTxMeterMs > kTxHangMs) {
                setRadioTx(false);
            }
            emit meterUpdate(QStringLiteral("SLC:LEVEL"), rawToDbm(r.mainRaw));
            emit meterUpdate(QStringLiteral("SLC1:LEVEL"), rawToDbm(r.subRaw));
        } else if constexpr (std::is_same_v<T, TxMeterReply>) {
            // The radio is transmitting: by us or from its own front panel.
            // Either way that is the radio's state, and it is reported.
            m_lastTxMeterMs = nowMs();
            if (!m_radioTx) {
                setRadioTx(true);
            }
            emit meterUpdate(QStringLiteral("TX:FWDPWR"), r.forwardWatts);
            emit meterUpdate(QStringLiteral("TX:REFPWR"), r.reflectedWatts);
            emit meterUpdate(QStringLiteral("TX:SWR"), r.swr);
            if (m_atuCycle) {
                m_atuLastSwr = r.swr;
            }
        } else if constexpr (std::is_same_v<T, AudioRoutingReply>) {
            m_audioRouting = std::array<char, 3>{r.left, r.right, r.speaker};
        } else if constexpr (std::is_same_v<T, AfVolumeReply>) {
            for (Rx rx : {Rx::Main, Rx::Sub}) {
                const std::optional<int> pct = rx == Rx::Main ? r.mainPercent : r.subPercent;
                const auto i = static_cast<std::size_t>(sliceForRx(rx));
                // Muted, the radio reads 0 by our doing: keep the level.
                if (!pct || m_radioMuted[i] || *pct == m_radioVolume[i]) {
                    continue;
                }
                m_radioVolume[i] = *pct;
                if (!m_pcAudio) {
                    publishSliceAudio(rx);   // the radio's own knob moved
                }
            }
        } else if constexpr (std::is_same_v<T, orion::CwRigReply>) {
            using What = orion::CwRigReply::What;
            switch (r.what) {
            case What::Pitch: m_cwRigPitchHz = r.value; break;
            case What::QskDelay: m_cwRigQsk = r.value; break;
            case What::AttackDecay: m_cwRigAttackMs = r.value; break;
            case What::KeyerEnabled: m_cwRigKeyerOn = r.value; break;
            case What::KeyerSpeed: m_cwRigKeyerWpm = r.value; break;
            case What::KeyerWeight: m_cwRigKeyerWeight = r.value; break;
            }
            emit extensionStatus(QStringLiteral("orion"), QStringLiteral("cw.rig"),
                                 cwRigState());
        } else if constexpr (std::is_same_v<T, SidetoneReply>) {
            // One knob on the radio: 0 is sidetone off, and the level is kept.
            TransmitDelta d;
            m_cwSidetoneOn = r.volume > 0;
            d.cwSidetone = m_cwSidetoneOn;
            if (r.volume > 0) {
                m_cwSidetoneLevel = r.volume;
                d.monGainCw = r.volume;
            }
            emit transmitChanged(d);
        } else if constexpr (std::is_same_v<T, TunerReply>) {
            if (m_tunerIn != r.in) {
                m_tunerIn = r.in;
                TransmitDelta d;
                d.atuEnabled = r.in;
                emit transmitChanged(d);
            }
        } else if constexpr (std::is_same_v<T, TxLevelReply>) {
            TransmitDelta d;
            switch (r.kind) {
            case TxLevelReply::Kind::Power:
                // Nor is TUNE's borrowed drive the operator's RF power.
                if (!tuneHoldsReadback()) {
                    m_rfPowerPercent = r.value;
                    d.rfPower = r.value;
                }
                break;
            case TxLevelReply::Kind::Mic:
                m_lastMicReading = r.value;
                if (m_txPath == TxPath::RadioMic && r.value > 0) {
                    m_radioMicLevel = r.value;
                    d.micLevel = r.value;
                }
                break;
            case TxLevelReply::Kind::Processor:
                if (m_txPath == TxPath::RadioMic) {
                    m_radioProcessorLevel = r.value;
                }
                break;
            case TxLevelReply::Kind::Aux:
                m_lastAuxReading = r.value;
                break;
            case TxLevelReply::Kind::Monitor:
                // The radio has one knob: 0 is MON off, and the level is kept.
                d.sbMonitor = r.value > 0;
                if (r.value > 0) {
                    d.monGainSb = r.value;
                }
                break;
            }
            // A radio left in the PC profile (mic 0, aux up) by an earlier
            // session that did not exit cleanly: give it its mic back.
            if (m_txPath == TxPath::RadioMic && m_lastMicReading == 0 && m_lastAuxReading > 0
                && !m_repairedProfile) {
                m_repairedProfile = true;
                qCInfo(lcOrion) << "Orion: radio was left in the PC audio profile; restoring its mic";
                applyRadioMicProfile();
            }
            emit transmitChanged(d);
        } else if constexpr (std::is_same_v<T, VfoAssignmentReply>) {
            m_kvMain = r.mainRx;
            m_kvSub = r.subRx;
            // A routing change we asked for is held until the radio agrees,
            // so a reply already in flight cannot flip the badge back.
            if (m_pendingTxVfo && m_sinceRoutingEdit.isValid()
                && m_sinceRoutingEdit.elapsed() < kFrequencyEchoGuardMs
                && r.tx != *m_pendingTxVfo) {
                return;
            }
            m_pendingTxVfo.reset();
            publishTxVfo(r.tx);
        } else if constexpr (std::is_same_v<T, VfoLockReply>) {
            auto& known = m_vfoLocked[r.rx == Rx::Main ? 0 : 1];
            if (known != r.locked) {
                known = r.locked;
                SliceDelta d;
                d.locked = r.locked;
                emit sliceChanged(sliceForRx(r.rx), d);
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
    openTxAudio(m_connectParams);
    startPeripherals(m_connectParams);
    startSdr(m_connectParams);
    if (!sdrRunning()) {
        m_placeholderSpectrumTimer.start();
        onPlaceholderSpectrumTick();
    }
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
    emit panBandwidthLimitsChanged(panId(), static_cast<double>(spectrum::kMinViewHz) / 1e6,
                                   static_cast<double>(spectrum::kMaxViewHz) / 1e6);

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

    TransmitDelta tx;
    tx.micSelection = QString::fromLatin1(kMicInputRadio);
    tx.mox = false;
    emit transmitChanged(tx);

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
    // The radio's own TX meter (@STF): forward and reflected watts, SWR.
    const struct { int index; const char* name; const char* unit; double low; double high; } tx[] = {
        {10, "FWDPWR", "Watts", 0.0, 120.0},
        {11, "REFPWR", "Watts", 0.0, 120.0},
        {12, "SWR", "SWR", 1.0, 6.4},
    };
    for (const auto& m : tx) {
        MeterDef d;
        d.index = m.index;
        d.source = QStringLiteral("TX");
        d.name = QString::fromLatin1(m.name);
        d.unit = QString::fromLatin1(m.unit);
        d.low = m.low;
        d.high = m.high;
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
    send(queryVfoLock(Rx::Main));
    send(queryVfoLock(Rx::Sub));
    send(queryTxPower());
    send(queryTuner());
    send(queryMonitor());
    send(queryCwSidetoneVolume());
    // The radio's other CW settings, so the CW window opens showing what
    // the rig actually has rather than blanks.
    send(queryCwSidetonePitch());
    send(queryCwQskDelay());
    send(queryCwAttackDecay());
    send(queryCwKeyerEnabled());
    send(queryCwKeyerSpeed());
    send(queryCwKeyerWeight());
    send(queryAfVolume(Rx::Main));
    send(queryAfVolume(Rx::Sub));
    send(queryAudioRouting());
    send(queryMicGain());
    send(querySpeechProcessor());
    send(queryAuxGain());
    send(queryRfGain(Rx::Main));
    send(queryAttenuator(Rx::Main));
    send(queryPreamp(Rx::Main));
}

void OrionBackend::onPollTick()
{
    if (!m_connected) {
        return;
    }
    // A late tick means OUR thread stalled (a slow plan, a busy machine):
    // nothing could have been read meanwhile, so the radio is not to blame.
    // Restart the silence clock instead of declaring the link dead.
    const qint64 now = nowMs();
    if (m_lastPollMs >= 0 && now - m_lastPollMs > kStallToleranceMs) {
        qCInfo(lcOrion) << "Orion: event loop stalled" << (now - m_lastPollMs)
                        << "ms - not counted against the radio";
        m_lastReplyMs = now;
    }
    m_lastPollMs = now;
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
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        // Delivery check: the SDRplay service drops transfers silently when
        // the host cannot keep up (a Debug build manages about 40%).
        // Measured over 5 s windows: the API delivers in bursts, so a 1 s
        // count reads anywhere from about 94% to 104% on a healthy stream.
        const qint64 now = nowMs();
        const qint64 samples = static_cast<qint64>(m_sdr->samplesReceived());
        const qint64 dt = now - m_sdrTickMs;
        if (dt >= kSdrDeliveryWindowMs) {
            m_sdrDeliveryRatio = static_cast<double>(samples - m_sdrSamplesAtTick)
                                 / (static_cast<double>(spectrum::kCaptureHz) * dt / 1000.0);
            m_sdrSamplesAtTick = samples;
            m_sdrTickMs = now;
            m_sdrLowWindows = m_sdrDeliveryRatio < kMinDeliveryRatio ? m_sdrLowWindows + 1 : 0;
        }
        const bool settled = now - m_sdrStartedMs > kSdrSilentWarningMs;
        if (settled && samples == 0 && !m_sdrDeliveryWarned) {
            m_sdrDeliveryWarned = true;
            emit configurationWarning(
                tr("The RSP2 opened but sends no samples. After an unclean exit the "
                   "SDRplay service can wedge: disconnect and connect again."));
        } else if (samples > 0 && m_sdrLowWindows >= 2 && !m_sdrDeliveryWarned) {
            m_sdrDeliveryWarned = true;
            emit configurationWarning(
                tr("The RSP2 is delivering %1% of its samples. Use an optimised "
                   "(RelWithDebInfo or Release) build and check the machine is not busy.")
                    .arg(qRound(m_sdrDeliveryRatio * 100.0)));
        }
    }
#endif
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
    pushRxTuning();   // both receivers have a host demodulator (step 4)
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
        updateSkimmerGate();          // the skimmer pauses outside CW
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
    pushRxTuning();   // the mode and the passband travel together
}

void OrionBackend::publishPan()
{
    // The view keeps the centre the operator or the recenter policy chose,
    // and follows dial A only when A leaves it, so the main slice stays in
    // frame. With an RSP2 the LO follows the view (OrionSpectrum.h).
    if (!m_rx[0].hz) {
        emit panCenterBandwidthChanged(panId(), 0.0, 0.0);
        return;
    }
    const qint64 dial = *m_rx[0].hz;
    const qint64 half = m_view.spanHz / 2;
    if (!m_viewKnown || dial < m_view.centreHz - half || dial > m_view.centreHz + half) {
        m_view.centreHz = dial;
        m_viewCentreEchoHz = static_cast<double>(m_view.centreHz);
        m_viewKnown = true;
    }
    emit panCenterBandwidthChanged(panId(), static_cast<double>(m_view.centreHz) / 1e6,
                                   static_cast<double>(m_view.spanHz) / 1e6);
    applyViewToSdr();
}

bool OrionBackend::sdrRunning() const
{
#ifdef AETHER_ORION_SDRPLAY
    return m_sdr && m_sdr->isRunning();
#else
    return false;
#endif
}

void OrionBackend::applyViewToSdr()
{
#ifdef AETHER_ORION_SDRPLAY
    if (!sdrRunning() || !m_viewKnown) {
        return;
    }
    m_sdr->setView(m_view, m_panPoints);
    if (!spectrum::viewFits(m_view, m_sdr->lo()) && !m_loRetuneTimer.isActive()) {
        m_loRetuneTimer.start();
    }
#endif
}

void OrionBackend::retuneSdr()
{
#ifdef AETHER_ORION_SDRPLAY
    if (sdrRunning() && m_viewKnown) {
        m_sdr->setLo(spectrum::loForView(m_view));
        pushRxTuning();   // a receiver may have moved in or out of the capture
    }
#endif
}

void OrionBackend::startSdr(const QVariantMap& params)
{
#ifdef AETHER_ORION_SDRPLAY
    if (!params.value(QStringLiteral("orion.sdr"), true).toBool()) {
        m_sdrStatus = tr("off for this connection");
        return;
    }
    Rsp2Receiver::Settings settings;
    settings.gainReductionDb = params.value(QStringLiteral("orion.sdr.gRdB"),
                                            settings.gainReductionDb).toInt();
    settings.lnaState = params.value(QStringLiteral("orion.sdr.lna"), settings.lnaState).toInt();
    settings.antennaB = params.value(QStringLiteral("orion.sdr.antenna")).toString()
                        == QLatin1String("B");

    if (!m_viewKnown) {
        m_view.centreHz = m_rx[0].hz.value_or(14'025'000);
        m_viewCentreEchoHz = static_cast<double>(m_view.centreHz);
        m_viewKnown = true;
    }
    // Receive audio first: the RSP2's stream callback feeds both chains from
    // the first block, so they must exist before the device starts. One per
    // receiver: main (slice 0) and sub (slice 1).
    for (int slice = 0; slice < 2; ++slice) {
        auto audio = std::make_unique<OrionRxAudio>();
        connect(audio.get(), &OrionRxAudio::audioReady, this,
                [this, slice](const QByteArray& pcm) { onRxAudio(slice, pcm); });
        connect(audio.get(), &OrionRxAudio::failed, this, [this](const QString& reason) {
            qCWarning(lcOrion) << "RX audio:" << reason;
            emit configurationWarning(tr("Receive audio: %1").arg(reason));
        });
        QString audioError;
        OrionRxAudio::Tuning t;
        t.dialHz = m_rx[static_cast<std::size_t>(slice)].hz.value_or(14'025'000);
        if (!audio->start(t, &audioError)) {
            qCWarning(lcOrion) << "RX audio:" << audioError;
            emit configurationWarning(tr("Receive audio: %1").arg(audioError));
            audio.reset();
        }
        m_rxAudio[static_cast<std::size_t>(slice)] = std::move(audio);
    }
    m_mixPending = {};
    m_sdr = std::make_unique<Rsp2Receiver>();
    m_sdr->setAudioSinks(m_rxAudio[0].get(), m_rxAudio[1].get());
    // The skimmer's feed, like the audio chains, must exist before the first
    // block (step 8).
    startSkimmer(params);
    if (m_skimmer) {
        m_sdr->setSkimmerSink(m_skimmer->feed(), m_skimmer->viewFeed());
        connect(m_sdr.get(), &Rsp2Receiver::fullSpectrumReady, this,
                [this](const std::vector<float>& db, qint64 lo) {
            if (m_skimmer) {
                m_skimmer->onFullSpectrum(db, static_cast<int>(spectrum::kCaptureHz), lo,
                                          m_rx[0].hz.value_or(0));
            }
        });
    }
    // The waterfall's copy rides as same-observation coverage: RadioModel
    // builds its rows from it, the trace from the frame (fork rule 2).
    connect(m_sdr.get(), &Rsp2Receiver::frameReady, this,
            [this](const QByteArray& frame, const QByteArray& waterfall, qint64 lowHz,
                   qint64 highHz) {
        if (m_connected) {
            SpectrumCoverage coverage;
            coverage.bins = waterfall;
            coverage.lowMhz = static_cast<double>(lowHz) / 1e6;
            coverage.highMhz = static_cast<double>(highHz) / 1e6;
            emit spectrumFrameReady(0, frame, coverage);
        }
    });
    m_sdr->setWaterfallHoldMs(m_waterfallHoldMs);
    m_sdr->setWaterfallAverage(m_waterfallAverage);
    // The display settings the client restored before the receiver existed.
    m_sdr->setAverage(m_panAverage);
    m_sdr->setWeightedAverage(m_panWeightedAverage);
    m_sdr->setFrameRate(m_panFps);
    connect(m_sdr.get(), &Rsp2Receiver::deviceLost, this, [this](const QString& reason) {
        stopSdr();
        m_sdrStatus = reason;
        emit configurationWarning(tr("Panadapter: %1.").arg(reason));
        if (m_connected) {
            m_placeholderSpectrumTimer.start();
        }
    });
    QString error;
    if (!m_sdr->start(spectrum::loForView(m_view), settings, &error)) {
        m_sdr.reset();
        m_rxAudio = {};
        m_sdrStatus = error;
        qCWarning(lcOrion) << "RSP2:" << error;
        emit configurationWarning(
            tr("Panadapter: %1 The radio still works; the pan shows no spectrum.").arg(error));
        return;
    }
    m_sdrStatus = tr("streaming");
    updateSkimmerGate();
    m_sdr->setView(m_view, m_panPoints);
    pushRxTuning();
    m_sdrStartedMs = nowMs();
    m_sdrTickMs = m_sdrStartedMs;
    m_sdrSamplesAtTick = 0;
    m_sdrDeliveryRatio = -1.0;
    m_sdrLowWindows = 0;
    m_sdrDeliveryWarned = false;
    m_placeholderSpectrumTimer.stop();
#else
    Q_UNUSED(params);
    m_sdrStatus = tr("this build has no SDRplay support");
#endif
}

void OrionBackend::pushRxTuning()
{
#ifdef AETHER_ORION_SDRPLAY
    for (int slice = 0; slice < 2; ++slice) {
        OrionRxAudio* audio = m_rxAudio[static_cast<std::size_t>(slice)].get();
        if (!audio) {
            continue;
        }
        const Receiver& r = m_rx[static_cast<std::size_t>(slice)];
        // The sub receiver runs the main's mode (the radio copies it) until
        // it reports its own.
        const Mode mode = r.mode.value_or(m_rx[0].mode.value_or(Mode::Usb));
        OrionRxAudio::Tuning t;
        t.dialHz = r.hz.value_or(0);
        switch (mode) {
        case Mode::Usb: t.mode = OrionRxAudio::Mode::Usb; break;
        case Mode::Lsb: t.mode = OrionRxAudio::Mode::Lsb; break;
        case Mode::Cw:  t.mode = OrionRxAudio::Mode::Cw; break;
        case Mode::Cwl: t.mode = OrionRxAudio::Mode::Cwl; break;
        case Mode::Am:  t.mode = OrionRxAudio::Mode::Am; break;
        case Mode::Fm:  t.mode = OrionRxAudio::Mode::Fm; break;
        }
        const Passband p = passbandFromFilter(mode, r.bandwidthHz.value_or(2400),
                                              r.pbtHz.value_or(0));
        t.lowHz = p.lowHz;
        t.highHz = p.highHz;
        t.cwPitchHz = m_cwPitchHz;
        t.agcMode = m_wdspAgcMode[static_cast<std::size_t>(slice)];
        if (t.dialHz <= 0) {
            continue;
        }
        audio->setTuning(t);

        // Whether the RSP2 can hear this receiver at all. A slice outside
        // the capture is silent (operator decision) and shown as such.
        const bool heard = sdrRunning() && OrionRxAudio::inCapture(t, m_sdr->lo());
        auto& known = m_inCapture[static_cast<std::size_t>(slice)];
        if (m_connected && known != heard) {
            known = heard;
            SliceDelta d;
            d.inCapture = heard;
            emit sliceChanged(slice, d);
        }
    }
#endif
}

void OrionBackend::onRxAudio(int slice, const QByteArray& stereoFloat)
{
#ifndef AETHER_ORION_SDRPLAY
    Q_UNUSED(slice);
    Q_UNUSED(stereoFloat);
#else
    if (!m_connected || slice < 0 || slice > 1) {
        return;
    }
    // While the Orion transmits the RSP2 hears our own signal: silence, not
    // the operator's own hash (and no feedback into a PC microphone).
    if (m_keyIntent || m_radioTx) {
        m_mixPending = {};
        return;
    }
    // This slice's own feed: pre-mute, pre-gain (TCI, decoders, recorder).
    publishLegacySliceAudio(slice, stereoFloat);

    // The speaker mix. Both chains are clocked by the same RSP2 stream and
    // produce equal blocks, so they are summed sample-aligned; a chain that
    // falls far behind is mixed as silence rather than holding the other.
    auto& mine = m_mixPending[static_cast<std::size_t>(slice)];
    mine.append(stereoFloat);
    const int other = 1 - slice;
    const bool otherRunning = m_rxAudio[static_cast<std::size_t>(other)]
                              && m_rxAudio[static_cast<std::size_t>(other)]->isReady();
    auto& theirs = m_mixPending[static_cast<std::size_t>(other)];
    qsizetype bytes = otherRunning ? std::min(mine.size(), theirs.size()) : mine.size();
    if (bytes == 0) {
        if (mine.size() < kMixStarvationBytes) {
            return;   // wait for the other receiver's block
        }
        bytes = mine.size();   // the other has stalled: play this one alone
    }
    bytes -= bytes % static_cast<qsizetype>(2 * sizeof(float));   // whole stereo frames
    if (bytes <= 0) {
        return;
    }

    const qsizetype n = bytes / static_cast<qsizetype>(sizeof(float));
    m_mixBuffer.fill('\0', bytes);
    auto* out = reinterpret_cast<float*>(m_mixBuffer.data());
    for (int s = 0; s < 2; ++s) {
        auto& pending = m_mixPending[static_cast<std::size_t>(s)];
        const qsizetype take = std::min(pending.size(), bytes);
        if (take > 0 && !m_sliceMuted[static_cast<std::size_t>(s)]) {
            // A balance, not a constant-power pan: centre is unity both sides.
            const float g = m_sliceGain[static_cast<std::size_t>(s)];
            const int pan = m_slicePan[static_cast<std::size_t>(s)];
            const float lw = g * (pan <= 50 ? 1.0f : (100 - pan) / 50.0f);
            const float rw = g * (pan >= 50 ? 1.0f : pan / 50.0f);
            const auto* in = reinterpret_cast<const float*>(pending.constData());
            const qsizetype m = take / static_cast<qsizetype>(sizeof(float));
            for (qsizetype k = 0; k + 1 < m; k += 2) {
                out[k] += in[k] * lw;
                out[k + 1] += in[k + 1] * rw;
            }
        }
        pending.remove(0, take);
    }
    Q_UNUSED(n);
    publishLegacyAudio(m_mixBuffer);
#endif
}

ReceiveDispatch OrionBackend::requestSliceAudio(int sliceId, const SliceAudioRequest& request)
{
    // AetherSDR's per-slice volume, mute and balance arrive here (refusing
    // it left every fader dead: "Backend refused operation receive audio").
    if (!request.valid() || request.origin != SliceAudioRequest::Origin::Operator
        || !rxForSlice(sliceId)) {
        return ReceiveDispatch::Unsupported;
    }
    switch (request.field) {
    case SliceAudioRequest::Field::Gain:
        setSliceAudioGain(sliceId, request.value);
        break;
    case SliceAudioRequest::Field::Mute:
        setSliceAudioMute(sliceId, request.value != 0);
        break;
    case SliceAudioRequest::Field::Pan:
        setSliceAudioPan(sliceId, request.value);
        break;
    }
    return ReceiveDispatch::Dispatched;
}

void OrionBackend::setSliceAudioMute(int sliceId, bool mute)
{
    const std::optional<Rx> rx = rxForSlice(sliceId);
    if (!rx) {
        return;
    }
    const auto i = static_cast<std::size_t>(sliceId);
    if (m_pcAudio) {
        m_sliceMuted[i] = mute;
        return;
    }
    // The radio has no mute: 0, and the level is kept to come back to.
    m_radioMuted[i] = mute;
    sendRadioVolume(*rx);
}

void OrionBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    const std::optional<Rx> rx = rxForSlice(sliceId);
    if (!rx) {
        return;
    }
    const auto i = static_cast<std::size_t>(sliceId);
    if (m_pcAudio) {
        m_sliceGain[i] = std::clamp(gainPercent, 0, 100) / 100.0f;
        return;
    }
    m_radioVolume[i] = std::clamp(gainPercent, 0, 100);
    if (!m_radioMuted[i]) {
        sendRadioVolume(*rx);
    }
}

void OrionBackend::sendRadioVolume(Rx rx)
{
    if (!m_connected) {
        return;
    }
    const auto i = static_cast<std::size_t>(sliceForRx(rx));
    const int level = m_radioVolume[i] >= 0 ? m_radioVolume[i] : kRadioVolumeFallback;
    send(orion::setAfVolume(rx, m_radioMuted[i] ? 0 : level));
}

void OrionBackend::setPcAudio(bool on)
{
    if (on == m_pcAudio) {
        return;
    }
    m_pcAudio = on;
    qCInfo(lcOrion) << "Orion: slice volume and mute now drive"
                    << (on ? "the RSP2 audio (PC Audio on)" : "the radio's own AF (PC Audio off)");
    // Each place keeps its own level; the sliders show the one now in use.
    for (Rx rx : {Rx::Main, Rx::Sub}) {
        publishSliceAudio(rx);
    }
    if (!on && m_connected) {
        send(queryAfVolume(Rx::Main));
        send(queryAfVolume(Rx::Sub));
    }
}

void OrionBackend::publishSliceAudio(Rx rx)
{
    const auto i = static_cast<std::size_t>(sliceForRx(rx));
    SliceDelta d;
    if (m_pcAudio) {
        d.audioGain = std::round(m_sliceGain[i] * 100.0f);
        d.audioMute = m_sliceMuted[i];
    } else {
        if (m_radioVolume[i] >= 0) {
            d.audioGain = m_radioVolume[i];
        }
        d.audioMute = m_radioMuted[i];
    }
    emit sliceChanged(sliceForRx(rx), d);
}

void OrionBackend::setSliceAudioPan(int sliceId, int panPercent)
{
    if (rxForSlice(sliceId)) {
        m_slicePan[static_cast<std::size_t>(sliceId)] = std::clamp(panPercent, 0, 100);
    }
}

void OrionBackend::setCwPitch(int hz)
{
    m_cwPitchHz = std::clamp(hz, 200, 1200);
    pushRxTuning();
}

void OrionBackend::stopSdr()
{
    // Device first (its callback feeds the audio chain), then the audio
    // thread. Both join before returning (IRadioBackend rule 5), so nothing
    // reaches a seam signal afterwards.
    m_sdr.reset();
    // After the device: its callback fed the skimmer's ring.
    m_skimmer.reset();
    m_rxAudio = {};
    m_mixPending = {};
    m_inCapture = {};
}

void OrionBackend::startSkimmer(const QVariantMap& params)
{
    // The operator's switches are AetherSDR settings; "none" turns the
    // skimmer off for a session (tests, and a second instance).
    auto& settings = AppSettings::instance();
    m_skimmerEnabled = settings.value(QStringLiteral("OrionSkimmerEnabled"), false).toBool();
    m_skimmerCwOnly = settings.value(QStringLiteral("OrionSkimmerCwOnly"), true).toBool();
    if (params.value(QStringLiteral("orion.skimmer")).toString() == QLatin1String("none")) {
        m_skimmerStatus = tr("off for this connection");
        return;
    }
    OrionSkimmer::Settings s;
    s.channels = settings.value(QStringLiteral("OrionSkimmerChannels"), s.channels).toInt();
    s.telnetPort = settings.value(QStringLiteral("OrionSkimmerPort"), s.telnetPort).toInt();
    s.bayes = settings.value(QStringLiteral("OrionSkimmerBayes"), false).toBool();
    s.spotterCall = settings.value(QStringLiteral("StationCallsign")).toString().trimmed().toUpper();
    m_skimmer = std::make_unique<OrionSkimmer>(static_cast<double>(spectrum::kCaptureHz), s);
    QString error;
    if (!m_skimmer->startServer(&error)) {
        qCWarning(lcOrion) << "Skimmer:" << error;
        emit configurationWarning(tr("Skimmer telnet feed: %1").arg(error));
    }
    connect(m_skimmer.get(), &OrionSkimmer::spotFound, this,
            [this](const QString& call, qint64 hz, int wpm) {
        qCInfo(lcOrion).noquote() << "Skimmer:" << call << "on" << hz / 1000.0 << "kHz"
                                  << (wpm > 0 ? QStringLiteral("%1 WPM").arg(wpm) : QString());
        QVariantMap f;
        f.insert(QStringLiteral("call"), call);
        f.insert(QStringLiteral("freqHz"), hz);
        f.insert(QStringLiteral("wpm"), wpm);
        emit extensionStatus(QStringLiteral("orion"), QStringLiteral("skimmer.spot"), f);
    });
    qCInfo(lcOrion) << "Skimmer:" << m_skimmer->channelCount() << "channels, telnet port"
                    << m_skimmer->serverPort() << ", cty.dat"
                    << (m_skimmer->callValidatorLoaded() ? "loaded" : "missing")
                    << ", MASTER.SCP" << m_skimmer->knownCalls() << "calls";
}

void OrionBackend::updateSkimmerGate()
{
    if (!m_skimmer) {
        if (m_skimmerStatus.isEmpty()) {
            m_skimmerStatus = tr("off");
        }
        return;
    }
    const std::optional<Mode> mode = m_rx[0].mode;
    const bool cw = mode && (*mode == Mode::Cw || *mode == Mode::Cwl);
    const bool active = m_skimmerEnabled && !m_radioTx && (cw || !m_skimmerCwOnly);
    m_skimmer->setActive(active);
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        // The channel search reads the whole capture every 2.5 s (console).
        m_sdr->setFullSpectrumInterval(active ? 2500 : 0);
    }
#endif
    m_skimmerStatus = !m_skimmerEnabled ? tr("off")
        : m_radioTx ? tr("paused while transmitting")
        : !active ? tr("paused outside CW")
        : tr("decoding");
}

void OrionBackend::skimmerTuning(qint64& dialHz, qint64& loHz) const
{
    dialHz = m_rx[0].hz.value_or(0);
    loHz = 0;
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        loHz = m_sdr->lo();
    }
#endif
}

// What the rotor is doing, for rotor.get and for every rotor.* answer:
// whether it is there, where it says it is pointing, and where it was last
// asked to go. Azimuth is -1 when it has not reported yet.
// What the radio says its own CW settings are. -1 where it has not
// answered yet, so a UI can tell "not known" from a real value.
QVariantMap OrionBackend::cwRigState() const
{
    QVariantMap m;
    m.insert(QStringLiteral("volume"), m_cwSidetoneLevel);
    m.insert(QStringLiteral("pitch"), m_cwRigPitchHz);
    m.insert(QStringLiteral("qsk"), m_cwRigQsk);
    m.insert(QStringLiteral("attack"), m_cwRigAttackMs);
    m.insert(QStringLiteral("keyerOn"), m_cwRigKeyerOn);
    m.insert(QStringLiteral("keyerWpm"), m_cwRigKeyerWpm);
    m.insert(QStringLiteral("keyerWeight"), m_cwRigKeyerWeight);
    return m;
}

QVariantMap OrionBackend::rotorState() const
{
    QVariantMap m;
    m.insert(QStringLiteral("open"), m_rotor && m_rotor->isOpen());
    m.insert(QStringLiteral("alive"), m_rotor && m_rotor->isAlive());
    m.insert(QStringLiteral("status"), m_rotorStatus);
    m.insert(QStringLiteral("azimuth"), m_rotor ? m_rotor->azimuth() : -1.0);
    return m;
}

QVariantMap OrionBackend::skimmerState() const
{
    QVariantMap m;
    m.insert(QStringLiteral("enabled"), m_skimmerEnabled);
    m.insert(QStringLiteral("cwOnly"), m_skimmerCwOnly);
    auto& settings = AppSettings::instance();
    m.insert(QStringLiteral("bayes"), settings.value(QStringLiteral("OrionSkimmerBayes"), false).toBool());
    m.insert(QStringLiteral("channels"), settings.value(QStringLiteral("OrionSkimmerChannels"), 24).toInt());
    m.insert(QStringLiteral("port"), settings.value(QStringLiteral("OrionSkimmerPort"), 7300).toInt());
    m.insert(QStringLiteral("status"), m_skimmerStatus);
    m.insert(QStringLiteral("available"), m_skimmer != nullptr);
    if (m_skimmer) {
        m.insert(QStringLiteral("activeChannels"), m_skimmer->activeChannels());
        m.insert(QStringLiteral("serverPort"), m_skimmer->serverPort());
        m.insert(QStringLiteral("serverClients"), m_skimmer->serverClients());
    }
    return m;
}

// ---- intents down ------------------------------------------------------------

void OrionBackend::setSliceFrequency(int sliceId, double hz)
{
    const std::optional<Rx> rx = rxForSlice(sliceId);
    const qint64 rounded = std::llround(hz);
    if (!rx || !m_connected || rounded < kMinHz || rounded > kMaxHz) {
        return;
    }
    // The radio's lock only freezes its knob; CAT would still tune. Honour
    // it here, as the reference program does, and put the slice back.
    if (m_vfoLocked[static_cast<std::size_t>(sliceId)].value_or(false)) {
        if (const auto hz = receiver(*rx).hz) {
            SliceDelta d;
            d.frequency = static_cast<double>(*hz) / 1e6;
            emit sliceChanged(sliceId, d);
        }
        return;
    }
    Receiver& r = receiver(*rx);
    r.pendingHz = rounded;
    r.sinceFrequencyEdit.start();
    if (!m_writeTimer.isActive()) {
        m_writeTimer.start();
    }
}

ReceiveDispatch OrionBackend::requestSliceControl(int sliceId, const SliceControlRequest& request)
{
    using Field = SliceControlRequest::Field;
    if (!request.valid() || !rxForSlice(sliceId)) {
        return ReceiveDispatch::Unsupported;
    }
    switch (request.field) {
    case Field::TxSlice:
        if (!m_connected) {
            return ReceiveDispatch::Unsupported;
        }
        if (std::get<bool>(request.value)) {
            setTxSlice(sliceId);
        }
        return ReceiveDispatch::Dispatched;
    case Field::ActiveSlice:
        if (std::get<bool>(request.value)) {
            setActiveSlice(sliceId);
        }
        return ReceiveDispatch::LocalOnly;
    default:
        return IRadioBackend::requestSliceControl(sliceId, request);
    }
}

void OrionBackend::setTxSlice(int sliceId)
{
    // Routing only: which VFO the transmitter WOULD use. Nothing here keys.
    const std::optional<Rx> rx = rxForSlice(sliceId);
    if (!rx || !m_connected) {
        return;
    }
    const char tx = *rx == Rx::Main ? 'A' : 'B';
    if (m_txVfo == tx && !m_pendingTxVfo) {
        return;
    }
    // Snapshot BEFORE the radio call: the routing reply may arrive before
    // this returns (the Omni VII reports synchronously, live-found).
    const bool newlyOnB = tx == 'B' && m_txVfo != 'B';

    // The operator's split habit: TX onto B parks B just above A (1 kHz in
    // CW, 5 kHz in voice) so he can tune it onto a clear spot; unless B is
    // locked. No mode is sent: the radio's menu copies it to B.
    if (newlyOnB && !m_vfoLocked[1].value_or(false) && m_rx[0].hz) {
        const Mode mode = m_rx[0].mode.value_or(Mode::Usb);
        const qint64 offset = (mode == Mode::Cw || mode == Mode::Cwl) ? kSplitParkCwHz
                                                                      : kSplitParkVoiceHz;
        const qint64 target = *m_rx[0].hz + offset;
        if (target <= kMaxHz) {
            Receiver& b = m_rx[1];
            b.pendingHz = target;
            b.sinceFrequencyEdit.start();
            flushWrites();
        }
    }

    send(setVfoAssignment(m_kvMain, m_kvSub, tx));
    m_pendingTxVfo = tx;
    m_sinceRoutingEdit.start();
    publishTxVfo(tx);
    // Read the routing back once the radio has applied it.
    const quint64 generation = m_generation;
    QTimer::singleShot(kRoutingReadbackMs, this, [this, generation] {
        if (generation == m_generation && m_connected) {
            send(queryVfoAssignment());
        }
    });
}

void OrionBackend::publishTxVfo(char tx)
{
    if (m_txVfo == tx) {
        return;
    }
    m_txVfo = tx;
    SliceDelta a;
    a.txSlice = tx == 'A';
    emit sliceChanged(0, a);
    SliceDelta b;
    b.txSlice = tx == 'B';
    emit sliceChanged(1, b);
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
    if (*rx == Rx::Main && m_tuning) {
        m_preTuneMode = *mode;    // applied when the carrier ends
        publishPassband(*rx);
        return;
    }
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
        updateSkimmerGate();
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
            pushRxTuning();
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
    // The pan is an independent window (the RSP2), never the radio's dial:
    // a drag or a recenter moves the view and, through it, the RSP2's LO.
    if (!m_connected || hz < static_cast<double>(kMinHz) || hz > static_cast<double>(kMaxHz)) {
        return;
    }
    // Echo the centre EXACTLY as asked. The view is kept in whole hertz, and
    // Center Lock compares the echo with its (fractional, after Zero Beat)
    // target: a rounded echo never matched, so it asked again from inside this
    // signal, forever, until the stack overflowed (2026-10-18 crash). And an
    // unchanged view echoes nothing, so a re-ask cannot loop either.
    const qint64 centreHz = std::llround(hz);
    if (m_viewKnown && centreHz == m_view.centreHz && hz == m_viewCentreEchoHz) {
        return;
    }
    m_view.centreHz = centreHz;
    m_viewCentreEchoHz = hz;
    m_viewKnown = true;
    emit panCenterBandwidthChanged(this->panId(), hz / 1e6,
                                   static_cast<double>(m_view.spanHz) / 1e6);
    applyViewToSdr();
}

void OrionBackend::setPanBandwidth(const QString& panId, double hz)
{
    Q_UNUSED(panId);
    if (!m_connected) {
        return;
    }
    // A request: the span is capped by the DC-free half of the capture.
    m_view = spectrum::clampView({m_view.centreHz, std::llround(hz)});
    emit panCenterBandwidthChanged(this->panId(), static_cast<double>(m_view.centreHz) / 1e6,
                                   static_cast<double>(m_view.spanHz) / 1e6);
    applyViewToSdr();
}

void OrionBackend::setPanFrameRate(const QString& panId, int fps)
{
    Q_UNUSED(panId);
    m_panFps = std::clamp(fps, 1, 60);
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        m_sdr->setFrameRate(m_panFps);
    }
#endif
}

void OrionBackend::setPanWaterfallRate(const QString& panId, int rate)
{
    Q_UNUSED(panId);
    // Peak-hold the waterfall's copy over one row interval, so every frame
    // between two rows shows in the later one.
    m_waterfallHoldMs = WaterfallRate::localRowIntervalMs(rate);
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        m_sdr->setWaterfallHoldMs(m_waterfallHoldMs);
    }
#endif
}

// Remembered, not just forwarded: the operator's saved FFT AVG arrives
// while the client is wiring the pan, which is BEFORE the RSP2 has
// started. Forwarding only when m_sdr exists dropped it on the floor —
// the slider kept the number and the panadapter ran unaveraged. startSdr()
// applies whatever is held here. Same for Wt Avg below.
void OrionBackend::setPanAverage(const QString& panId, int average)
{
    Q_UNUSED(panId);
    m_panAverage = std::clamp(average, 0, 100);
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        m_sdr->setAverage(m_panAverage);
    }
#endif
}

void OrionBackend::setPanWaterfallAverage(const QString& panId, int average)
{
    Q_UNUSED(panId);
    m_waterfallAverage = std::clamp(average, 0, 100);
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        m_sdr->setWaterfallAverage(m_waterfallAverage);
    }
#endif
}

void OrionBackend::setPanWeightedAverage(const QString& panId, bool on)
{
    Q_UNUSED(panId);
    m_panWeightedAverage = on;
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        m_sdr->setWeightedAverage(on);
    }
#else
    Q_UNUSED(on);
#endif
}

void OrionBackend::setPanPixelWidth(const QString& panId, int pixels)
{
    Q_UNUSED(panId);
    m_panPoints = std::clamp(pixels, 64, 8192);
    applyViewToSdr();
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
    // The decision is made above the seam (Constitution VI); this only
    // translates it. A key needs an admitted operation; an UNKEY is never
    // refused, whatever state the link or the operation is in.
    if (key && !TxCoordinator::Command{operation, key}.permitsDispatch(TxCoordinator::monotonicMs())) {
        completion.finish();
        return;
    }
    if (key) {
        if (!m_connected) {
            completion.finish();
            return;
        }
        if (m_txPath == TxPath::Pc && !(m_txAudio && m_txAudio->isOpen())) {
            emit configurationWarning(
                tr("PC audio is selected but the SignaLink is not available (%1). The radio "
                   "is keyed with no audio; select MIC to use the radio's microphone.")
                    .arg(m_txAudioError));
        }
        qCInfo(lcOrion) << "Orion: KEY (*TK) on the"
                        << (m_txPath == TxPath::Pc ? "PC" : "MIC") << "path";
        keyRadio();
    } else {
        unkey();
    }
    completion.finish();
}

void OrionBackend::keyRadio()
{
    send(orion::setPtt(true));
    m_keyIntent = true;
    m_keyedAtMs = nowMs();
    m_txFailsafeTimer.start();
    applyTxGuard(true);
    // Audio waits for the radio to report TX (or the lead-in fallback),
    // so nothing reaches the SignaLink before the radio is transmitting.
    const quint64 generation = m_generation;
    QTimer::singleShot(kTxAudioLeadMs, this, [this, generation] {
        if (generation == m_generation) {
            updateTxAudioGate();
        }
    });
}

void OrionBackend::setTune(bool on, int tunePowerPercent,
                           const AetherSDR::TxCoordinator::Operation& operation,
                           const AetherSDR::TxCoordinator::Completion& completion)
{
    // As setKeying: the carrier needs an admitted operation; stopping it is
    // never refused.
    if (!on) {
        endTune(/*timedOut=*/false);
        completion.finish();
        return;
    }
    if (!TxCoordinator::Command{operation, true}.permitsDispatch(TxCoordinator::monotonicMs())
        || !m_connected) {
        completion.finish();
        return;
    }
    const int power = std::clamp(tunePowerPercent, 0, 100);
    if (m_tuning) {
        send(orion::setTxPower(power));
        completion.finish();
        return;
    }
    // The reference program's manual tune: the TUNE drive, FM for a steady
    // carrier at the dial, key by CAT; the carrier stops itself after 15 s.
    m_tuning = true;
    m_preTuneRfPower = m_rfPowerPercent;
    m_preTuneMode = receiver(Rx::Main).mode;
    qCInfo(lcOrion) << "Orion: TUNE carrier at" << power << "% (FM, *TK); was"
                    << m_preTuneRfPower << "%";
    send(orion::setTxPower(power));
    if (m_preTuneMode != Mode::Fm) {
        send(orion::setMode(Rx::Main, Mode::Fm));
    }
    keyRadio();
    m_tuneTimer.start();
    TransmitDelta d;
    d.tune = true;
    emit transmitChanged(d);
    completion.finish();
}

void OrionBackend::setTunePower(int percent)
{
    if (m_tuning && m_connected) {
        send(orion::setTxPower(std::clamp(percent, 0, 100)));
    }
}

void OrionBackend::endTune(bool timedOut)
{
    if (!m_tuning) {
        unkey();
        return;
    }
    m_tuning = false;    // first: unkey() below must not come back here
    m_tuneTimer.stop();
    unkey();             // the carrier drops BEFORE mode and power go back
    restoreAfterTune();
    qCInfo(lcOrion) << "Orion: TUNE carrier off" << (timedOut ? "(15 s limit)" : "")
                    << "- mode and power restored";
    TransmitDelta d;
    d.tune = false;
    emit transmitChanged(d);
    if (timedOut) {
        emit configurationWarning(tr("TUNE stopped after %1 s.").arg(kMaxTuneMs / 1000));
    }
}

void OrionBackend::restoreAfterTune()
{
    m_tuneEndedMs = nowMs();
    if (m_preTuneMode && *m_preTuneMode != Mode::Fm) {
        send(orion::setMode(Rx::Main, *m_preTuneMode));
        // The mode change recalls the radio's per-mode filter.
        const quint64 generation = m_generation;
        QTimer::singleShot(kFilterRecallMs, this, [this, generation] {
            if (generation == m_generation && m_port) {
                send(queryBandwidth(Rx::Main));
                send(queryPbt(Rx::Main));
            }
        });
    }
    if (m_preTuneRfPower >= 0) {
        // The readback is held a moment, so this is the drive from now on.
        m_rfPowerPercent = m_preTuneRfPower;
        send(orion::setTxPower(m_preTuneRfPower));
    }
    send(queryTxPower());
    m_preTuneMode.reset();
    m_preTuneRfPower = -1;
}

bool OrionBackend::tuneHoldsReadback() const
{
    return m_tuning || (m_tuneEndedMs >= 0 && nowMs() - m_tuneEndedMs < kTuneSettleMs);
}

void OrionBackend::setAtu(bool start, const AetherSDR::TxCoordinator::Operation& operation,
                          const AetherSDR::TxCoordinator::Completion& completion)
{
    if (!start) {
        // Bypass: never refused. A cycle in progress is stopped first.
        if (m_atuCycle && m_port) {
            send(orion::setPtt(false));
            finishAtuCycle("TUNE_ABORTED");
        }
        if (m_connected) {
            qCInfo(lcOrion) << "Orion: ATU bypassed (*TT0)";
            send(orion::setTuner(false));
            send(queryTuner());
            m_tunerIn = false;    // the @TT readback confirms it
            publishAtu("TUNE_BYPASS");
        }
        completion.finish();
        return;
    }
    // The cycle keys the transmitter: it needs an admitted operation, and
    // never runs on top of our own key or TUNE carrier.
    if (!TxCoordinator::Command{operation, true}.permitsDispatch(TxCoordinator::monotonicMs())
        || !m_connected || m_atuCycle || m_keyIntent || m_tuning) {
        completion.finish();
        return;
    }
    // The reference program's tune with the internal tuner: tuner in, then
    // *TTT; the radio makes its own carrier and drops it when matched.
    qCInfo(lcOrion) << "Orion: ATU tune cycle (*TT1, *TTT)";
    m_atuCycle = true;
    m_atuSawTx = false;
    m_atuLastSwr = -1.0;
    send(orion::setTuner(true));
    send(orion::startTunerCycle());
    m_tunerIn = true;
    m_atuTimer.start();
    publishAtu("TUNE_IN_PROGRESS");
    const quint64 generation = m_generation;
    QTimer::singleShot(kAtuStartMs, this, [this, generation] {
        if (generation == m_generation && m_atuCycle && !m_atuSawTx) {
            finishAtuCycle("TUNE_FAIL",
                           tr("The Orion did not start a tune cycle. Is its internal tuner "
                              "fitted and enabled?"));
        }
    });
    completion.finish();
}

void OrionBackend::finishAtuCycle(const char* status, const QString& warning)
{
    if (!m_atuCycle) {
        return;
    }
    m_atuCycle = false;
    m_atuTimer.stop();
    qCInfo(lcOrion) << "Orion: ATU tune cycle ended:" << status << "last SWR" << m_atuLastSwr;
    publishAtu(status);
    send(queryTuner());
    if (!warning.isEmpty()) {
        emit configurationWarning(warning);
    }
}

void OrionBackend::publishAtu(const char* status)
{
    TransmitDelta d;
    d.atuStatusRaw = QString::fromLatin1(status);
    if (m_tunerIn) {
        d.atuEnabled = *m_tunerIn;
    }
    emit transmitChanged(d);
}

void OrionBackend::unkey()
{
    if (m_tuning) {
        endTune(/*timedOut=*/false);    // it unkeys, then restores
        return;
    }
    // Gate first: no more audio into the SignaLink, which could key the
    // radio by itself.
    if (m_keyIntent) {
        qCInfo(lcOrion) << "Orion: UNKEY (*TU) after" << (nowMs() - m_keyedAtMs) << "ms";
    }
    m_keyIntent = false;
    updateTxAudioGate();
    m_txFailsafeTimer.stop();
    if (m_port) {
        send(orion::setPtt(false));
    }
    // The RSP2 stays desensitised a moment: the radio is still ringing down.
    m_txGuardReleaseTimer.start();
}

void OrionBackend::leaveRadioSafe()
{
    if (!m_port || !m_port->isOpen()) {
        return;
    }
    if (m_keyIntent) {
        m_keyIntent = false;
        if (m_txAudio) {
            m_txAudio->setGate(false);
        }
        send(orion::setPtt(false));
    }
    // An ATU cycle keys the radio by itself: stop it before we let go.
    if (m_atuCycle) {
        m_atuCycle = false;
        send(orion::setPtt(false));
    }
    // A receiver we muted (volume 0) gets its level back: the radio must not
    // be left silent for the front panel (stand-alone rule).
    for (Rx rx : {Rx::Main, Rx::Sub}) {
        const auto i = static_cast<std::size_t>(sliceForRx(rx));
        if (m_radioMuted[i]) {
            m_radioMuted[i] = false;
            send(orion::setAfVolume(rx, m_radioVolume[i] >= 0 ? m_radioVolume[i]
                                                              : kRadioVolumeFallback));
        }
    }
    // A carrier cut short by a disconnect still gives back mode and power.
    if (m_tuning) {
        m_tuning = false;
        m_tuneTimer.stop();
        restoreAfterTune();
    }
    // Stand-alone rule: the radio must work from its front panel with the PC
    // off, so the radio-mic profile goes back before we let go.
    if (m_txPath == TxPath::Pc) {
        applyRadioMicProfile();
        m_txPath = TxPath::RadioMic;
    }
    m_port->flush();
    m_port->waitForBytesWritten(200);
}

void OrionBackend::setRadioTx(bool on)
{
    qCInfo(lcOrion) << "Orion: radio reports" << (on ? "TRANSMIT" : "RECEIVE")
                    << (m_atuCycle ? "(its ATU tune cycle)"
                        : on && !m_keyIntent ? "(keyed from the radio, not by AetherSDR)" : "");
    m_radioTx = on;
    updateSkimmerGate();
    if (m_atuCycle) {
        if (on) {
            m_atuSawTx = true;
        } else if (m_atuSawTx) {
            // The radio dropped its own carrier: the cycle is over.
            const bool matched = m_atuLastSwr > 0.0 && m_atuLastSwr <= kAtuMatchedSwr;
            finishAtuCycle(m_atuLastSwr <= 0.0 ? "TUNE_OK"
                           : matched ? "TUNE_SUCCESSFUL" : "TUNE_FAIL");
        }
    }
    TransmitDelta d;
    d.mox = on;
    emit transmitChanged(d);
    // The radio's own answer, by the ?S reply's form (its PTT readback).
    emit keyingStateConfirmed(on);
    updateTxAudioGate();
    if (on) {
        applyTxGuard(true);
    } else {
        emit meterUpdate(QStringLiteral("TX:FWDPWR"), 0.0);
        emit meterUpdate(QStringLiteral("TX:REFPWR"), 0.0);
        if (!m_keyIntent) {
            m_txGuardReleaseTimer.start();
        }
    }
}

void OrionBackend::openTxAudio(const QVariantMap& params)
{
    // The SignaLink output, open for the whole session but silent: the gate
    // stays shut unless we keyed the radio on the PC path.
    m_txAudio = std::make_unique<OrionTxAudio>();
    m_txAudioError.clear();
    // Tests shorten the failsafe; the operator's build keeps 3 minutes.
    const int maxKeyMs = params.value(QStringLiteral("orion.tx.maxKeyMs"), kMaxKeyMs).toInt();
    m_txFailsafeTimer.setInterval(std::clamp(maxKeyMs, 500, kMaxKeyMs));
    const int maxTuneMs = params.value(QStringLiteral("orion.tx.maxTuneMs"), kMaxTuneMs).toInt();
    m_tuneTimer.setInterval(std::clamp(maxTuneMs, 500, kMaxTuneMs));
    const QString device = params.value(QStringLiteral("orion.tx.audioDevice")).toString();
    if (!m_txAudio->open(device, &m_txAudioError)) {
        qCWarning(lcOrion) << "TX audio:" << m_txAudioError;
        m_txAudio.reset();
        return;
    }
    qCInfo(lcOrion) << "TX audio: PC path to" << m_txAudio->deviceName();
}

void OrionBackend::updateTxAudioGate()
{
    if (!m_txAudio) {
        return;
    }
    const bool leadDone = m_keyIntent && nowMs() - m_keyedAtMs >= kTxAudioLeadMs;
    // A TUNE carrier is clean: no audio into the SignaLink while it is up.
    m_txAudio->setGate(m_keyIntent && !m_tuning && m_txPath == TxPath::Pc
                       && (m_radioTx || leadDone));
}

void OrionBackend::onTxFailsafe()
{
    if (!m_keyIntent) {
        return;
    }
    qCWarning(lcOrion) << "Orion: transmit failsafe - unkeyed after" << kMaxKeyMs / 1000 << "s";
    unkey();
    emit configurationWarning(tr("Transmit stopped: keyed for %1 minutes without release.")
                                  .arg(kMaxKeyMs / 60000));
}

void OrionBackend::applyTxGuard(bool on)
{
#ifdef AETHER_ORION_SDRPLAY
    // The RSP2 shares the antenna field: while the Orion transmits, park it
    // at minimum gain so its front end is not driven into overload.
    if (m_sdr) {
        m_sdr->setTxGuard(on);
    }
#else
    Q_UNUSED(on);
#endif
}

void OrionBackend::applyRadioMicProfile()
{
    // The radio's front-panel microphone: aux (SignaLink) off, the operator's
    // own mic gain and processor back.
    send(orion::setAuxGain(0));
    send(orion::setMicGain(m_radioMicLevel));
    send(orion::setSpeechProcessor(m_radioProcessorLevel));
}

void OrionBackend::applyPcProfile()
{
    // AetherSDR's audio through the SignaLink: the radio's mic and processor
    // off (AetherSDR's own chain does the shaping), aux full.
    send(orion::setMicGain(0));
    send(orion::setSpeechProcessor(0));
    send(orion::setAuxGain(100));
}

void OrionBackend::setTxPath(TxPath path)
{
    if (path == m_txPath) {
        return;
    }
    qCInfo(lcOrion) << "Orion: TX audio path ->" << (path == TxPath::Pc ? "PC (SignaLink)" : "MIC (radio jack)");
    if (path == TxPath::Pc) {
        applyPcProfile();
    } else {
        applyRadioMicProfile();
    }
    m_txPath = path;
    updateTxAudioGate();
    TransmitDelta d;
    d.micSelection = QString::fromLatin1(path == TxPath::Pc ? kMicInputPc : kMicInputRadio);
    d.micLevel = path == TxPath::Pc ? m_pcMicLevel : m_radioMicLevel;
    emit transmitChanged(d);
}

ReceiveDispatch OrionBackend::requestTransmitControl(const TransmitControlRequest& request,
                                                     TunePowerContext context)
{
    Q_UNUSED(context);
    using Field = TransmitControlRequest::Field;
    if (!request.valid()) {
        return ReceiveDispatch::Unsupported;
    }
    switch (request.field) {
    case Field::MicInput: {
        const QString input = std::get<QString>(request.value).trimmed().toUpper();
        if (input == QLatin1String(kMicInputPc)) {
            setTxPath(TxPath::Pc);
        } else if (input == QLatin1String(kMicInputRadio)) {
            setTxPath(TxPath::RadioMic);
        } else {
            return ReceiveDispatch::Unsupported;
        }
        return ReceiveDispatch::Dispatched;
    }
    case Field::RfPower:
        setTxPower(std::get<int>(request.value));
        return ReceiveDispatch::Dispatched;
    case Field::MicGain:
        setMicGain(std::get<int>(request.value));
        return ReceiveDispatch::Dispatched;
    // CW speed is the WinKeyer's (the CWX panel and the CW applet both land
    // here); the Orion's own keyer speed is left to the radio.
    case Field::CwSpeed: {
        if (!m_keyer || !m_keyer->isReady()) {
            return ReceiveDispatch::Unsupported;
        }
        setCwSpeed(std::get<int>(request.value));
        TransmitDelta d;
        d.cwSpeed = m_keyer->speed();
        emit transmitChanged(d);
        return ReceiveDispatch::Dispatched;
    }
    // The CW tab's Sidetone button and level: the radio's own sidetone (*CV),
    // which is what the operator hears from the Orion while the WinKeyer keys.
    case Field::CwSidetone: {
        m_cwSidetoneOn = std::get<bool>(request.value);
        send(orion::setCwSidetoneVolume(m_cwSidetoneOn ? m_cwSidetoneLevel : 0));
        send(orion::queryCwSidetoneVolume());
        return ReceiveDispatch::Dispatched;
    }
    case Field::CwMonitorGain: {
        m_cwSidetoneLevel = std::clamp(std::get<int>(request.value), 0, 100);
        if (m_cwSidetoneOn) {
            send(orion::setCwSidetoneVolume(m_cwSidetoneLevel));
            send(orion::queryCwSidetoneVolume());
        }
        return ReceiveDispatch::Dispatched;
    }
    case Field::MonitorEnabled: case Field::MonitorLevel: {
        const MonitorSetting setting = std::get<MonitorSetting>(request.value);
        send(orion::setMonitor(setting.enabled ? setting.level : 0));
        send(orion::queryMonitor());
        return ReceiveDispatch::Dispatched;
    }
    // AetherSDR's own TX chain shapes PC audio: these stay on the host.
    case Field::ProcessorEnabled: case Field::ProcessorLevel:
        return ReceiveDispatch::LocalOnly;
    default:
        return ReceiveDispatch::Unsupported;
    }
}

void OrionBackend::setTxPower(int percent)
{
    if (!m_connected) {
        return;
    }
    if (m_tuning) {
        // TUNE has the drive: this is what goes back after the carrier.
        m_preTuneRfPower = std::clamp(percent, 0, 100);
        return;
    }
    m_rfPowerPercent = std::clamp(percent, 0, 100);
    send(orion::setTxPower(percent));
    send(queryTxPower());
}

void OrionBackend::setMicGain(int level)
{
    level = std::clamp(level, 0, 100);
    if (m_txPath == TxPath::Pc) {
        // The PC path's level is ours: it scales the audio to the SignaLink.
        m_pcMicLevel = level;
    } else if (m_connected) {
        m_radioMicLevel = level;
        send(orion::setMicGain(level));
    }
}

void OrionBackend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                                 TxAudioSource source, const TxCoordinator::Context& context)
{
    Q_UNUSED(source);
    Q_UNUSED(context);
    // Only the PC path carries host audio, and the gate (keyed by us, radio
    // in TX) decides whether any of it reaches the SignaLink.
    if (!m_txAudio || m_txPath != TxPath::Pc) {
        return;
    }
    // 50 is unity on the PC mic slider, 100 is +6 dB.
    m_txAudio->submit(int16Stereo, sampleRateHz, static_cast<float>(m_pcMicLevel) / 50.0f);
}

int OrionBackend::finishTxAudio(const TxCoordinator::Context& context)
{
    Q_UNUSED(context);
    if (!m_txAudio || m_txPath != TxPath::Pc) {
        return 0;
    }
    // What is still queued for the codec, plus the SignaLink and the radio's
    // own audio path (the reference program drained 250 ms before unkey).
    return m_txAudio->queuedMs() + kTxAudioTailMs;
}

void OrionBackend::startPeripherals(const QVariantMap& params)
{
    const auto setting = [&params](const char* key, const QString& fallback) {
        const QString v = params.value(QString::fromLatin1(key)).toString();
        return v.isEmpty() ? fallback : v;
    };
    const auto off = [](const QString& v) { return v == QLatin1String("none"); };

    // WinKeyer: the station's FTDI by serial number, never a raw ttyUSB.
    const QString keyerDevice = setting("orion.winkeyer.device", OrionWinKeyer::findStationKeyer());
    if (off(keyerDevice)) {
        m_keyerStatus = tr("off");
    } else if (keyerDevice.isEmpty()) {
        m_keyerStatus = tr("not found (FTDI %1)").arg(QString::fromLatin1(OrionWinKeyer::kFtdiSerial));
    } else {
        m_keyer = std::make_unique<OrionWinKeyer>();
        m_keyerStatus = tr("opening %1").arg(keyerDevice);
        connect(m_keyer.get(), &OrionWinKeyer::ready, this, [this](int firmware) {
            m_keyerStatus = tr("ready, firmware %1%2").arg(firmware)
                                .arg(m_keyer && m_keyer->isWk3() ? tr(" (WK3)") : QString());
            emit capabilitiesChanged();
        });
        connect(m_keyer.get(), &OrionWinKeyer::failed, this, [this](const QString& reason) {
            m_keyerStatus = reason;
            finishCwCompletions();
            emit configurationWarning(tr("WinKeyer: %1").arg(reason));
            emit capabilitiesChanged();
        });
        connect(m_keyer.get(), &OrionWinKeyer::busyChanged, this, [this](bool busy) {
            if (!busy) {
                finishCwCompletions();   // the keyer has sent everything queued
            }
        });
        connect(m_keyer.get(), &OrionWinKeyer::breakIn, this, [this] {
            // The paddle interrupted buffered text: it is not going out.
            finishCwCompletions();
        });
        connect(m_keyer.get(), &OrionWinKeyer::speedPotChanged, this, [this](int wpm) {
            TransmitDelta d;
            d.cwSpeed = wpm;
            emit transmitChanged(d);
        });
        m_keyer->open(keyerDevice);
    }

    // DCU-3 rotor and rotctld on :4533.
    const QString rotorDevice = setting("orion.rotor.device", QString::fromLatin1(OrionRotor::kStationDevice));
    if (off(rotorDevice)) {
        m_rotorStatus = tr("off");
    } else {
        m_rotor = std::make_unique<OrionRotor>();
        QString error;
        if (!m_rotor->open(rotorDevice, &error)) {
            m_rotorStatus = error;
            emit configurationWarning(tr("Rotor: %1").arg(error));
        } else {
            m_rotorStatus = tr("open on %1").arg(rotorDevice);
            connect(m_rotor.get(), &OrionRotor::azimuthChanged, this, [this](double az) {
                QVariantMap f;
                f.insert(QStringLiteral("azimuth"), az);
                emit extensionStatus(QStringLiteral("orion"), QStringLiteral("rotor"), f);
            });
        }
        // The server runs even if the rotor did not open: clients get a
        // clean "no reading" (RPRT -5), not a refused connection.
        m_rotctld = std::make_unique<OrionRotctld>(m_rotor.get());
        const quint16 port = static_cast<quint16>(
            params.value(QStringLiteral("orion.rotor.port"), OrionRotctld::kDefaultPort).toInt());
        if (!m_rotctld->listen(port, &error)) {
            m_rotorStatus += QStringLiteral("; rotctld: ") + error;
            emit configurationWarning(tr("rotctld: %1").arg(error));
        }
    }

    // cwdaemon on :6789, into the WinKeyer.
    const quint16 cwPort = static_cast<quint16>(
        params.value(QStringLiteral("orion.cwdaemon.port"), OrionCwDaemon::kDefaultPort).toInt());
    if (params.value(QStringLiteral("orion.cwdaemon.port")).toString() == QLatin1String("none")) {
        m_cwDaemonStatus = tr("off");
    } else {
        m_cwDaemon = std::make_unique<OrionCwDaemon>();
        QString error;
        if (!m_cwDaemon->listen(cwPort, &error)) {
            m_cwDaemonStatus = error;
            emit configurationWarning(tr("cwdaemon: %1").arg(error));
            m_cwDaemon.reset();
        } else {
            m_cwDaemonStatus = tr("listening on localhost:%1").arg(m_cwDaemon->port());
            // cqrlog's CW goes straight to the WinKeyer, whose key line keys
            // the radio, as it did through the reference program. It does
            // not pass AetherSDR's TX gate, so every send is logged.
            connect(m_cwDaemon.get(), &OrionCwDaemon::textRequested, this, [this](const QString& t) {
                if (!m_keyer || !m_keyer->isReady()) {
                    qCWarning(lcOrion) << "cwdaemon: text dropped, the WinKeyer is not ready";
                    return;
                }
                const QByteArray sent = m_keyer->sendText(t);
                qCInfo(lcOrion) << "cwdaemon: CW to the WinKeyer:" << sent.size() << "characters";
            });
            connect(m_cwDaemon.get(), &OrionCwDaemon::prosignRequested, this, [this](char a, char b) {
                if (m_keyer && m_keyer->isReady()) {
                    qCInfo(lcOrion) << "cwdaemon: prosign" << a << b;
                    m_keyer->sendProsign(a, b);
                }
            });
            connect(m_cwDaemon.get(), &OrionCwDaemon::stopRequested, this, [this] {
                if (m_keyer) {
                    m_keyer->stop();
                }
                finishCwCompletions();
            });
            connect(m_cwDaemon.get(), &OrionCwDaemon::speedRequested, this, [this](int wpm) {
                setCwSpeed(wpm);
                TransmitDelta d;
                d.cwSpeed = wpm;
                emit transmitChanged(d);
            });
        }
    }

    // Pat (Winlink): WARN ONLY (operator decision); never blocks keying.
    const QString patUrl = setting("orion.pat.url", QString::fromLatin1(OrionPatMonitor::kDefaultUrl));
    if (off(patUrl)) {
        m_patStatus = tr("off");
    } else {
        m_pat = std::make_unique<OrionPatMonitor>();
        connect(m_pat.get(), &OrionPatMonitor::patUpChanged, this, [this](bool up) {
            m_patStatus = up ? tr("watching Pat") : tr("Pat not running");
        });
        connect(m_pat.get(), &OrionPatMonitor::sessionChanged, this,
                [this](bool active, const QString& remote) {
            qCInfo(lcOrion) << "Pat: Winlink session" << (active ? "started" : "ended") << remote;
            QVariantMap f;
            f.insert(QStringLiteral("active"), active);
            f.insert(QStringLiteral("remote"), remote);
            emit extensionStatus(QStringLiteral("orion"), QStringLiteral("winlink"), f);
            if (active) {
                emit configurationWarning(
                    tr("Winlink session active%1: Pat is using the radio. Don't key over it.")
                        .arg(remote.isEmpty() ? QString() : tr(" with %1").arg(remote)));
            }
        });
        m_patStatus = m_pat->start(QUrl(patUrl)) ? tr("Pat not running")
                                                 : tr("this build has no websocket support");
    }
}

void OrionBackend::stopPeripherals()
{
    finishCwCompletions();
    m_pat.reset();
    m_cwDaemon.reset();
    m_rotctld.reset();
    m_rotor.reset();
    m_keyer.reset();   // stops anything buffered, then host close
}

void OrionBackend::finishCwCompletions()
{
    auto pending = std::move(m_cwCompletions);
    m_cwCompletions.clear();
    for (const auto& c : pending) {
        c.finish();
    }
}

QString OrionBackend::sendCwText(const QString& text, const TxCoordinator::Operation& operation,
                                 const AetherSDR::TxCoordinator::Completion& completion)
{
    // CW text keys the radio through the WinKeyer's key line: it needs the
    // TX coordinator's admission like any other key.
    if (!TxCoordinator::Command{operation, true}.permitsDispatch(TxCoordinator::monotonicMs())) {
        completion.finish();
        return tr("transmit is not permitted right now");
    }
    if (!m_keyer || !m_keyer->isReady()) {
        completion.finish();
        return tr("the WinKeyer is not connected (%1)").arg(m_keyerStatus);
    }
    const QByteArray sent = m_keyer->sendText(text);
    if (sent.isEmpty()) {
        completion.finish();
        return tr("nothing the WinKeyer can send in that text");
    }
    qCInfo(lcOrion) << "CW text to the WinKeyer:" << sent.size() << "characters";
    m_cwCompletions.push_back(completion);
    return {};
}

void OrionBackend::abortCwText(const TxCoordinator::Operation& operation,
                               const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(operation);   // stopping is always allowed
    if (m_keyer) {
        m_keyer->stop();
    }
    finishCwCompletions();
    completion.finish();
}

void OrionBackend::setCwSpeed(int wpm)
{
    if (m_keyer) {
        m_keyer->setSpeed(wpm);
    }
}

void OrionBackend::invokeExtension(const QString& ns, const QString& verb,
                                   quint64 requestId, const QVariant& arg)
{
    // The radio's phones/speaker routing (*UC), for the Radio Setup page.
    // Each of left/right/speaker is "M", "S" or "B".
    if (ns == QLatin1String("orion") && verb == QLatin1String("audio.routing.get")) {
        QVariantMap m;
        if (m_audioRouting) {
            m.insert(QStringLiteral("left"), QString(QLatin1Char((*m_audioRouting)[0])));
            m.insert(QStringLiteral("right"), QString(QLatin1Char((*m_audioRouting)[1])));
            m.insert(QStringLiteral("speaker"), QString(QLatin1Char((*m_audioRouting)[2])));
        }
        if (requestId != 0) {
            emit extensionResult(requestId, m);
        }
        return;
    }
    if (ns == QLatin1String("orion") && verb == QLatin1String("audio.routing.set")) {
        const QVariantMap m = arg.toMap();
        const auto letter = [&](const char* key, int index) {
            const QString v = m.value(QLatin1String(key)).toString();
            if (v.size() == 1 && isRoutingLetter(v.at(0).toLatin1())) {
                return v.at(0).toLatin1();
            }
            return m_audioRouting ? (*m_audioRouting)[static_cast<std::size_t>(index)] : 'B';
        };
        const QByteArray cmd = setAudioRouting(letter("left", 0), letter("right", 1),
                                               letter("speaker", 2));
        if (!m_connected || cmd.isEmpty()) {
            if (requestId != 0) {
                emit extensionError(requestId, tr("The Orion is not connected."));
            }
            return;
        }
        qCInfo(lcOrion) << "Orion: audio routing" << cmd;
        m_audioRouting = std::array<char, 3>{cmd[3], cmd[4], cmd[5]};
        send(cmd);
        send(queryAudioRouting());
        if (requestId != 0) {
            emit extensionResult(requestId, true);
        }
        return;
    }
    // The radio's own CW settings (*CT pitch, *CQ QSK delay, *CD attack and
    // decay), plus its internal keyer read-only. These are rig-side: the
    // Orion makes the sidetone and shapes the CW envelope whenever it
    // transmits CW, whatever did the keying, so they apply with the
    // WinKeyer sending. cw.rig.get answers the lot; cw.rig.set takes any of
    // pitch / qsk / attack and asks the radio back.
    if (ns == QLatin1String("orion") && verb == QLatin1String("cw.rig.get")) {
        // Also as a status event, so a caller that asked without a request
        // id still hears the answer — the CW window opens long after the
        // connect-time queries have been answered, and would otherwise show
        // blanks until the operator touched something.
        emit extensionStatus(QStringLiteral("orion"), QStringLiteral("cw.rig"), cwRigState());
        if (requestId != 0) {
            emit extensionResult(requestId, cwRigState());
        }
        return;
    }
    if (ns == QLatin1String("orion") && verb == QLatin1String("cw.rig.set")) {
        const QVariantMap m = arg.toMap();
        if (m.contains(QStringLiteral("pitch"))) {
            send(orion::setCwSidetonePitch(m.value(QStringLiteral("pitch")).toInt()));
            send(orion::queryCwSidetonePitch());
        }
        if (m.contains(QStringLiteral("qsk"))) {
            send(orion::setCwQskDelay(m.value(QStringLiteral("qsk")).toInt()));
            send(orion::queryCwQskDelay());
        }
        if (m.contains(QStringLiteral("attack"))) {
            send(orion::setCwAttackDecay(m.value(QStringLiteral("attack")).toInt()));
            send(orion::queryCwAttackDecay());
        }
        emit extensionStatus(QStringLiteral("orion"), QStringLiteral("cw.rig"), cwRigState());
        if (requestId != 0) {
            emit extensionResult(requestId, cwRigState());
        }
        return;
    }

    // The DCU-3 rotor. rotor.get answers the state; rotor.turn takes a
    // bearing in degrees and rotor.stop halts it where it is. Choosing a
    // bearing and sending it are separate gestures in every UI that uses
    // these, and this is the ONLY path that can move the antenna.
    if (ns == QLatin1String("orion") && verb == QLatin1String("rotor.get")) {
        if (requestId != 0) {
            emit extensionResult(requestId, rotorState());
        }
        return;
    }
    if (ns == QLatin1String("orion") && verb == QLatin1String("rotor.turn")) {
        const double degrees = arg.toMap().value(QStringLiteral("azimuth"), -1.0).toDouble();
        QVariantMap m = rotorState();
        if (!m_rotor || !m_rotor->isOpen()) {
            m.insert(QStringLiteral("error"), tr("the rotor is not open"));
        } else if (!(degrees >= 0.0 && degrees <= 360.0)) {
            m.insert(QStringLiteral("error"), tr("%1 is not a bearing").arg(degrees));
        } else if (!m_rotor->turnTo(degrees)) {
            m.insert(QStringLiteral("error"), tr("the rotor refused the turn"));
        } else {
            qCInfo(lcOrion) << "Rotor: turning to" << degrees;
            m.insert(QStringLiteral("asked"), degrees);
        }
        if (requestId != 0) {
            emit extensionResult(requestId, m);
        }
        return;
    }
    if (ns == QLatin1String("orion") && verb == QLatin1String("rotor.stop")) {
        QVariantMap m = rotorState();
        if (!m_rotor || !m_rotor->isOpen()) {
            m.insert(QStringLiteral("error"), tr("the rotor is not open"));
        } else if (!m_rotor->stop()) {
            m.insert(QStringLiteral("error"), tr("the rotor refused the stop"));
        } else {
            qCInfo(lcOrion) << "Rotor: stop";
        }
        if (requestId != 0) {
            emit extensionResult(requestId, m);
        }
        return;
    }

    // The CW skimmer's switches (Radio Setup -> Orion): skimmer.get answers the
    // state; skimmer.set takes any of enabled / cwOnly / bayes (live) and
    // channels / port (applied at the next connect), and persists them.
    if (ns == QLatin1String("orion") && verb == QLatin1String("skimmer.get")) {
        if (requestId != 0) {
            emit extensionResult(requestId, skimmerState());
        }
        return;
    }
    if (ns == QLatin1String("orion") && verb == QLatin1String("skimmer.set")) {
        const QVariantMap m = arg.toMap();
        auto& settings = AppSettings::instance();
        if (m.contains(QStringLiteral("enabled"))) {
            m_skimmerEnabled = m.value(QStringLiteral("enabled")).toBool();
            settings.setValue(QStringLiteral("OrionSkimmerEnabled"), m_skimmerEnabled);
        }
        if (m.contains(QStringLiteral("cwOnly"))) {
            m_skimmerCwOnly = m.value(QStringLiteral("cwOnly")).toBool();
            settings.setValue(QStringLiteral("OrionSkimmerCwOnly"), m_skimmerCwOnly);
        }
        if (m.contains(QStringLiteral("bayes"))) {
            settings.setValue(QStringLiteral("OrionSkimmerBayes"),
                              m.value(QStringLiteral("bayes")).toBool());
        }
        if (m.contains(QStringLiteral("channels"))) {
            settings.setValue(QStringLiteral("OrionSkimmerChannels"),
                              std::clamp(m.value(QStringLiteral("channels")).toInt(), 4, 64));
        }
        if (m.contains(QStringLiteral("port"))) {
            settings.setValue(QStringLiteral("OrionSkimmerPort"),
                              std::clamp(m.value(QStringLiteral("port")).toInt(), 0, 65535));
        }
        settings.save();
        qCInfo(lcOrion) << "Skimmer settings:" << m;
        updateSkimmerGate();
        if (requestId != 0) {
            emit extensionResult(requestId, skimmerState());
        }
        return;
    }
    // PC Audio: the click (audio.pc) and the state at connect (audio.pc.state).
    if (ns == QLatin1String("orion")
        && (verb == QLatin1String("audio.pc") || verb == QLatin1String("audio.pc.state"))) {
        setPcAudio(arg.toBool());
        return;
    }
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
    h.values.insert(QStringLiteral("txPath"),
                    QString::fromLatin1(m_txPath == TxPath::Pc ? kMicInputPc : kMicInputRadio));
    h.values.insert(QStringLiteral("txAudioDevice"),
                    m_txAudio ? m_txAudio->deviceName() : m_txAudioError);
    if (m_txAudio) {
        h.values.insert(QStringLiteral("txAudioWritten"), QVariant::fromValue(m_txAudio->samplesWritten()));
        h.values.insert(QStringLiteral("txAudioDropped"), QVariant::fromValue(m_txAudio->samplesDropped()));
    }
    h.values.insert(QStringLiteral("keyerStatus"), m_keyerStatus);
    h.values.insert(QStringLiteral("rotorStatus"), m_rotorStatus);
    if (m_rotor && m_rotor->azimuth() >= 0.0) {
        h.values.insert(QStringLiteral("rotorAzimuth"), m_rotor->azimuth());
    }
    if (m_rotctld) {
        h.values.insert(QStringLiteral("rotctldPort"), m_rotctld->port());
    }
    h.values.insert(QStringLiteral("cwDaemonStatus"), m_cwDaemonStatus);
    h.values.insert(QStringLiteral("patStatus"), m_patStatus);
    h.values.insert(QStringLiteral("skimmerStatus"), m_skimmerStatus);
    if (m_skimmer) {
        h.values.insert(QStringLiteral("skimmerChannels"),
                        QStringLiteral("%1 of %2").arg(m_skimmer->activeChannels())
                            .arg(m_skimmer->channelCount()));
        h.values.insert(QStringLiteral("skimmerServer"),
                        m_skimmer->serverPort() != 0
                            ? tr("localhost:%1, %2 client(s)").arg(m_skimmer->serverPort())
                                  .arg(m_skimmer->serverClients())
                            : tr("off"));
        h.values.insert(QStringLiteral("skimmerDropped"), m_skimmer->droppedSamples());
    }
    h.values.insert(QStringLiteral("sdrStatus"), m_sdrStatus);
#ifdef AETHER_ORION_SDRPLAY
    if (m_sdr) {
        h.values.insert(QStringLiteral("sdrLoHz"), m_sdr->lo());
        h.values.insert(QStringLiteral("sdrOverloads"), m_sdr->overloads());
        if (m_sdrDeliveryRatio >= 0.0) {
            h.values.insert(QStringLiteral("sdrDeliveryPercent"),
                            qRound(m_sdrDeliveryRatio * 100.0));
        }
    }
#endif
    h.order = {QStringLiteral("device"), QStringLiteral("repliesDecoded"),
               QStringLiteral("linesIgnored"), QStringLiteral("lastReplyAgeMs"),
               QStringLiteral("frequencyRoundTripMs"), QStringLiteral("txPath"),
               QStringLiteral("txAudioDevice"), QStringLiteral("txAudioWritten"),
               QStringLiteral("txAudioDropped"), QStringLiteral("keyerStatus"),
               QStringLiteral("cwDaemonStatus"), QStringLiteral("rotorStatus"),
               QStringLiteral("rotorAzimuth"), QStringLiteral("rotctldPort"),
               QStringLiteral("patStatus"), QStringLiteral("skimmerStatus"),
               QStringLiteral("skimmerChannels"), QStringLiteral("skimmerServer"),
               QStringLiteral("skimmerDropped"), QStringLiteral("sdrStatus"),
               QStringLiteral("sdrLoHz"), QStringLiteral("sdrDeliveryPercent"),
               QStringLiteral("sdrOverloads")};
    h.sections.insert(QStringLiteral("txPath"), tr("Transmit"));
    h.labels.insert(QStringLiteral("txPath"), tr("TX audio path"));
    h.labels.insert(QStringLiteral("txAudioDevice"), tr("PC audio output"));
    h.labels.insert(QStringLiteral("txAudioWritten"), tr("TX audio samples sent"));
    h.labels.insert(QStringLiteral("txAudioDropped"), tr("TX audio samples held back"));
    h.sections.insert(QStringLiteral("keyerStatus"), tr("Station"));
    h.labels.insert(QStringLiteral("keyerStatus"), tr("WinKeyer"));
    h.labels.insert(QStringLiteral("cwDaemonStatus"), tr("cwdaemon"));
    h.labels.insert(QStringLiteral("rotorStatus"), tr("Rotor (DCU-3)"));
    h.labels.insert(QStringLiteral("rotorAzimuth"), tr("Rotor heading (deg)"));
    h.labels.insert(QStringLiteral("rotctldPort"), tr("rotctld port"));
    h.labels.insert(QStringLiteral("patStatus"), tr("Pat (Winlink)"));
    h.sections.insert(QStringLiteral("skimmerStatus"), tr("CW skimmer"));
    h.labels.insert(QStringLiteral("skimmerStatus"), tr("Status"));
    h.labels.insert(QStringLiteral("skimmerChannels"), tr("Channels decoding"));
    h.labels.insert(QStringLiteral("skimmerServer"), tr("Telnet feed"));
    h.labels.insert(QStringLiteral("skimmerDropped"), tr("IQ samples dropped"));
    h.sections.insert(QStringLiteral("sdrStatus"), tr("RSP2 panadapter"));
    h.labels.insert(QStringLiteral("sdrStatus"), tr("Status"));
    h.labels.insert(QStringLiteral("sdrLoHz"), tr("LO (Hz)"));
    h.labels.insert(QStringLiteral("sdrDeliveryPercent"), tr("Sample delivery (%)"));
    h.labels.insert(QStringLiteral("sdrOverloads"), tr("ADC overload events"));
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
