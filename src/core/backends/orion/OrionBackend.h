#pragma once

// IRadioBackend for a Ten-Tec Orion 565/566 on serial CAT (fork family "orion").
//
// Step 1 of docs/architecture/orion-backend-design.md: CAT only. Two slices
// (main RX = VFO A = slice 0, sub RX = VFO B = slice 1), tuning, mode,
// filter, AGC, the main receiver's front end and both S-meters. No SDR, no
// audio and NO TRANSMIT: capabilities().canTransmit is false and setKeying()
// is a no-op. The radio is authoritative (Constitution II/III): this backend
// declares no client settings domains and is never pushed a restored state.
//
// Threading: everything lives on the owner thread. QSerialPort is already
// asynchronous, so there is no worker; every seam signal is emitted from a
// serial readyRead or timer slot on this thread.

#include "core/backends/IRadioBackend.h"
#include "core/backends/orion/OrionCat.h"
#include "core/backends/orion/OrionPollScheduler.h"
#include "core/backends/orion/OrionSpectrum.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <array>
#include <vector>
#include <memory>
#include <optional>

class QSerialPort;

namespace AetherSDR::orion {

class Rsp2Receiver;
class OrionSkimmer;
class OrionRxAudio;
class OrionTxAudio;
class OrionWinKeyer;
class OrionRotor;
class OrionRotctld;
class OrionCwDaemon;
class OrionPatMonitor;

class OrionBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit OrionBackend(QObject* parent = nullptr);
    ~OrionBackend() override;

    static QString familyName() { return QStringLiteral("orion"); }
    // The device used when the connect request names none: the udev name for
    // the Orion's motherboard RS-232 port. Never a raw ttyS*.
    static QString defaultDevice() { return QStringLiteral("/dev/orion"); }
    // "orion:/dev/orion" -> "/dev/orion"; anything without the prefix is a path.
    static QString deviceFromSerial(const QString& serial);

    // Echo guards and timing, in ms (values from the reference program's
    // on-air experience; see the design doc).
    static constexpr int kConnectTimeoutMs = 3000;
    static constexpr int kSilenceTimeoutMs = 5000;
    // A poll tick this late means the owner thread stalled, not the radio.
    static constexpr int kStallToleranceMs = 1000;
    static constexpr int kWriteCoalesceMs = 40;
    static constexpr int kModeAfterRetuneMs = 120;
    static constexpr int kFilterRecallMs = 400;
    static constexpr int kFrequencyEchoGuardMs = 1500;
    static constexpr int kFilterEchoGuardMs = 2000;
    static constexpr qint64 kLargeJumpHz = 100000;
    static constexpr int kLinkTickMs = 1000;
    // The operator's split habit: B parks this far above A when TX moves to it.
    static constexpr qint64 kSplitParkCwHz = 1000;
    static constexpr qint64 kSplitParkVoiceHz = 5000;
    static constexpr int kRoutingReadbackMs = 300;

    // ---- transmit (step 6) ----
    // The two TX audio paths, as the Phone applet's mic-source list names them.
    static constexpr const char* kMicInputRadio = "MIC";   // the radio's front jack
    static constexpr const char* kMicInputPc = "PC";       // AetherSDR via the SignaLink
    static constexpr int kMaxKeyMs = 180'000;      // failsafe: 3 minutes keyed
    static constexpr int kTxAudioLeadMs = 300;     // key, then audio (reference program)
    static constexpr int kTxAudioTailMs = 250;     // SignaLink + radio audio path
    static constexpr int kTxHangMs = 1500;         // RX-form ?S replies inside a CW over
    // TUNE: a carrier never outlives 15 s (reference program); mode and power
    // readback is held this long after, while the restore lands.
    static constexpr int kMaxTuneMs = 15'000;
    static constexpr int kTuneSettleMs = 1500;
    // ATU: the internal tuner's own cycle (*TTT). It must start transmitting
    // soon, and never runs longer than this (then *TU).
    // On air the Orion took 2.9 s to start its carrier after *TTT.
    static constexpr int kAtuStartMs = 6000;
    static constexpr int kMaxAtuMs = 30'000;
    static constexpr double kAtuMatchedSwr = 2.0;
    static constexpr int kTxGuardHangMs = 1500;    // RSP2 stays guarded after unkey
    static constexpr int kDefaultRadioMic = 51;    // reference program's voice defaults
    static constexpr int kDefaultRadioProcessor = 2;
    // Without an RSP2 (not installed, in use, or turned off) a flat trace at
    // the floor keeps the pan and waterfall alive: AetherSDR's connect
    // overlay only clears on the first spectrum frame.
    static constexpr int kPlaceholderSpectrumMs = 250;
    static constexpr int kPlaceholderSpectrumBins = 512;
    static constexpr float kPlaceholderFloorDbm = -127.0f;
    // RSP2 retunes during a pan drag are coalesced to this.
    static constexpr int kLoRetuneMs = 50;
    // Delivery below this share of 1 MS/s is reported (a Debug build or a
    // busy machine makes the SDRplay service drop transfers silently).
    static constexpr double kMinDeliveryRatio = 0.95;
    static constexpr int kSdrSilentWarningMs = 3000;
    static constexpr int kSdrDeliveryWindowMs = 5000;
    // A receiver this far ahead of the other in the speaker mix (0.2 s of
    // 24 kHz float stereo) plays alone rather than waiting.
    static constexpr qsizetype kMixStarvationBytes = 24000 / 5 * 2 * 4;

    // ---- IRadioBackend ----
    RadioCapabilities capabilities() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override { return m_connected; }
    // Slice A is demodulated on this host from the RSP2 (step 3).
    bool ownsRxAudio() const override { return true; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    void setPanWaterfallRate(const QString& panId, int rate) override;
    void setPanAverage(const QString& panId, int average) override;
    void setPanWaterfallAverage(const QString& panId, int average) override;
    void setPanWeightedAverage(const QString& panId, bool on) override;
    void setPanPixelWidth(const QString& panId, int pixels) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanPreamp(const QString& panId, int step) override;
    void setPanAttenuator(const QString& panId, int step) override;
    void setActiveSlice(int sliceId) override;
    void setTxSlice(int sliceId) override;
    ReceiveDispatch requestSliceControl(int sliceId, const SliceControlRequest& request) override;
    ReceiveDispatch requestSliceAudio(int sliceId, const SliceAudioRequest& request) override;
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;
    void setCwPitch(int hz) override;

    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation,
                   const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    ReceiveDispatch requestTransmitControl(const TransmitControlRequest& request,
                                           TunePowerContext context = TunePowerContext::Deferred) override;
    void setTune(bool on, int tunePowerPercent, const AetherSDR::TxCoordinator::Operation& operation,
                 const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void setTunePower(int percent) override;
    void setAtu(bool start, const AetherSDR::TxCoordinator::Operation& operation,
                const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void setTxPower(int percent) override;
    void setMicGain(int level) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz, TxAudioSource source,
                       const TxCoordinator::Context& context) override;
    int finishTxAudio(const TxCoordinator::Context& context) override;
    QString sendCwText(const QString& text, const TxCoordinator::Operation& operation,
                       const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void abortCwText(const TxCoordinator::Operation& operation,
                     const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void setCwSpeed(int wpm) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    HealthSnapshot healthSnapshot() const override;

    // For the fork's skimmer windows (GUI thread): the skimmer while the
    // RSP2 runs (else null), and the dial (VFO A) and RSP2 LO it works at.
    [[nodiscard]] OrionSkimmer* skimmer() const { return m_skimmer.get(); }
    void skimmerTuning(qint64& dialHz, qint64& loHz) const;
    LinkStats linkStats() const override;

private:
    struct Receiver {
        // Last value the radio confirmed (or we optimistically sent).
        std::optional<qint64> hz;
        std::optional<Mode> mode;
        std::optional<int> bandwidthHz;
        std::optional<int> pbtHz;
        // Pending coalesced writes.
        std::optional<qint64> pendingHz;
        std::optional<FilterSetting> pendingFilter;
        // Echo guards.
        QElapsedTimer sinceFrequencyEdit;
        QElapsedTimer sinceFilterEdit;
        QElapsedTimer sinceModeEdit;
        // A large jump or a mode change needs two agreeing reports.
        std::optional<qint64> candidateHz;
        std::optional<Mode> candidateMode;
    };

    void openLink();
    void closeLink();
    void failLink(const QString& reason);
    void onReadyRead();
    void onLine(const QByteArray& line);
    void onFirstReply();
    void onPollTick();
    void onLinkTick();
    void onPlaceholderSpectrumTick();
    void flushWrites();
    void send(const QByteArray& command);

    void applyFrequency(Rx rx, qint64 hz);
    void applyMode(Rx rx, Mode mode);
    void applyBandwidth(Rx rx, int hz);
    void applyPbt(Rx rx, int hz);
    void publishPassband(Rx rx);
    void publishPan();
    void publishTxVfo(char tx);

    enum class TxPath { RadioMic, Pc };
    void keyRadio();
    void unkey();
    void endTune(bool timedOut);
    void restoreAfterTune();
    [[nodiscard]] bool tuneHoldsReadback() const;
    void finishAtuCycle(const char* status, const QString& warning = {});
    void publishAtu(const char* status);
    void setPcAudio(bool on);
    void publishSliceAudio(Rx rx);
    void sendRadioVolume(Rx rx);
    void leaveRadioSafe();
    void updateTxAudioGate();
    void onTxFailsafe();
    void applyTxGuard(bool on);
    void applyRadioMicProfile();
    void applyPcProfile();
    void setTxPath(TxPath path);
    void setRadioTx(bool on);
    void startSkimmer(const QVariantMap& params);
    void updateSkimmerGate();
    [[nodiscard]] QVariantMap rotorState() const;
    [[nodiscard]] QVariantMap cwRigState() const;
    [[nodiscard]] QVariantMap skimmerState() const;
    void openTxAudio(const QVariantMap& params);

    // Station peripherals (step 7), owned with the session: one owner per
    // port while AetherSDR runs, none when it does not (fork rule 7).
    void startPeripherals(const QVariantMap& params);
    void stopPeripherals();
    void finishCwCompletions();
    void applyViewToSdr();
    void startSdr(const QVariantMap& params);
    void stopSdr();
    void retuneSdr();
    [[nodiscard]] bool sdrRunning() const;
    void pushRxTuning();
    void onRxAudio(int slice, const QByteArray& stereoFloat);
    void publishMeterDefs();
    void publishInitialState();
    void startupQueries();

    [[nodiscard]] static std::optional<Rx> rxForSlice(int sliceId);
    [[nodiscard]] static int sliceForRx(Rx rx) { return rx == Rx::Main ? 0 : 1; }
    [[nodiscard]] Receiver& receiver(Rx rx) { return m_rx[rx == Rx::Main ? 0 : 1]; }
    [[nodiscard]] qint64 nowMs() const { return m_clock.elapsed(); }

    static QString panId() { return QStringLiteral("0"); }

    QString m_device;
    std::unique_ptr<QSerialPort> m_port;
    QByteArray m_rxBuffer;
    bool m_connected = false;
    bool m_connecting = false;
    // Bumped on every open/close so queued single-shots from an old session
    // drop themselves.
    quint64 m_generation = 0;

    std::array<Receiver, 2> m_rx;
    std::optional<char> m_txVfo;
    std::optional<char> m_pendingTxVfo;
    QElapsedTimer m_sinceRoutingEdit;
    char m_kvMain = 'A';
    char m_kvSub = 'B';
    std::array<std::optional<bool>, 2> m_vfoLocked;

    // Transmit state. m_keyIntent: WE keyed it. m_radioTx: the radio says it
    // is transmitting (its ?S reply), whoever keyed it.
    TxPath m_txPath = TxPath::RadioMic;
    bool m_keyIntent = false;
    bool m_radioTx = false;
    qint64 m_keyedAtMs = 0;
    qint64 m_lastTxMeterMs = 0;
    int m_radioMicLevel = kDefaultRadioMic;
    int m_radioProcessorLevel = kDefaultRadioProcessor;
    int m_pcMicLevel = 50;
    int m_lastMicReading = -1;
    int m_lastAuxReading = -1;
    bool m_repairedProfile = false;
    std::unique_ptr<OrionTxAudio> m_txAudio;
    QString m_txAudioError;
    QTimer m_txFailsafeTimer;
    QTimer m_txGuardReleaseTimer;
    // TUNE borrows the drive and the main receiver's mode (FM gives the steady
    // carrier, as in the reference program); both go back after the unkey.
    bool m_tuning = false;
    int m_rfPowerPercent = -1;          // the operator's drive, from @TP
    int m_preTuneRfPower = -1;
    std::optional<Mode> m_preTuneMode;
    qint64 m_tuneEndedMs = -1;
    QTimer m_tuneTimer;
    // ATU: the radio keys itself for its tune cycle; we watch its readback.
    bool m_atuCycle = false;
    bool m_atuSawTx = false;
    double m_atuLastSwr = -1.0;
    std::optional<bool> m_tunerIn;      // @TT, the radio's own state
    // CW sidetone (*CV): the level the slider asks for, and the button.
    int m_cwSidetoneLevel = 50;
    bool m_cwSidetoneOn = true;
    QTimer m_atuTimer;

    std::unique_ptr<OrionWinKeyer> m_keyer;
    std::vector<TxCoordinator::Completion> m_cwCompletions;
    QString m_keyerStatus;
    std::unique_ptr<OrionRotor> m_rotor;
    std::unique_ptr<OrionRotctld> m_rotctld;
    QString m_rotorStatus;
    std::unique_ptr<OrionCwDaemon> m_cwDaemon;
    QString m_cwDaemonStatus;
    std::unique_ptr<OrionPatMonitor> m_pat;
    QString m_patStatus;
    spectrum::View m_view;
    int m_waterfallHoldMs = 0;   // the waterfall's row interval, see setPanWaterfallRate
    int m_waterfallAverage = 0;  // the waterfall's own WF AVG (0 = off)
    // The radio's own CW settings, as it last reported them (-1 = unasked).
    int m_cwRigPitchHz = -1;
    int m_cwRigQsk = -1;
    int m_cwRigAttackMs = -1;
    int m_cwRigKeyerOn = -1;
    int m_cwRigKeyerWpm = -1;
    int m_cwRigKeyerWeight = -1;
    int m_panAverage = 0;        // FFT AVG, held until the RSP2 starts
    int m_panFps = 25;           // FFT FPS, likewise
    bool m_panWeightedAverage = false;
    bool m_viewKnown = false;
    double m_viewCentreEchoHz = 0.0;   // the centre exactly as last requested
    int m_panPoints = 1024;
    int m_activeSlice = 0;

    OrionPollScheduler m_scheduler;
    QElapsedTimer m_clock;
    QTimer m_connectTimer;
    QTimer m_pollTimer;
    QTimer m_linkTimer;
    QTimer m_writeTimer;
    QTimer m_placeholderSpectrumTimer;
    QTimer m_loRetuneTimer;
    QVariantMap m_connectParams;
    std::unique_ptr<Rsp2Receiver> m_sdr;
    std::array<std::unique_ptr<OrionRxAudio>, 2> m_rxAudio;
    // CW skimmer (step 8): runs on the RSP2's IQ, so it lives with m_sdr
    // (created before it starts, destroyed after it stops).
    std::unique_ptr<OrionSkimmer> m_skimmer;
    bool m_skimmerEnabled = false;      // the operator's switch (persisted)
    bool m_skimmerCwOnly = true;        // pause outside CW (console default)
    QString m_skimmerStatus;
    // Per-slice monitor controls, applied in the speaker mix only (the
    // per-slice feed stays pre-mute, pre-gain, for TCI and decoders).
    std::array<bool, 2> m_sliceMuted{false, false};
    std::array<float, 2> m_sliceGain{1.0f, 1.0f};
    std::array<int, 2> m_slicePan{50, 50};
    // PC Audio decides where a slice's volume and mute act: on, the RSP2
    // audio here (the three arrays above); off, the radio's own AF volume
    // (*UM / *US), so the operator hears the Orion. AetherSDR reports the
    // button through the "orion" extension (audio.pc / audio.pc.state).
    bool m_pcAudio = true;
    std::array<int, 2> m_radioVolume{-1, -1};      // percent; the unmuted level
    std::array<bool, 2> m_radioMuted{false, false};
    static constexpr int kRadioVolumeFallback = 40;   // ~ the radio's *U100
    // *UC, the radio's own: phones left, phones right, speaker ('M'/'S'/'B').
    std::optional<std::array<char, 3>> m_audioRouting;
    std::array<QByteArray, 2> m_mixPending;
    std::array<std::optional<bool>, 2> m_inCapture;
    int m_cwPitchHz = 600;
    std::array<int, 2> m_wdspAgcMode{3, 3};
    QByteArray m_mixBuffer;
    QString m_sdrStatus;
    qint64 m_sdrSamplesAtTick = 0;
    qint64 m_sdrTickMs = 0;
    double m_sdrDeliveryRatio = -1.0;
    int m_sdrLowWindows = 0;
    bool m_sdrDeliveryWarned = false;
    qint64 m_sdrStartedMs = -1;
    qint64 m_lastFrequencyWriteMs = -1'000'000;

    // Link accounting (owner thread only).
    qint64 m_rxBytes = 0;
    qint64 m_txBytes = 0;
    quint64 m_repliesDecoded = 0;
    quint64 m_linesIgnored = 0;
    qint64 m_lastReplyMs = -1;
    qint64 m_lastPollMs = -1;
    bool m_trafficSinceTick = false;
    qint64 m_frequencyQuerySentMs = -1;
    int m_rttMs = -1;
};

}  // namespace AetherSDR::orion
