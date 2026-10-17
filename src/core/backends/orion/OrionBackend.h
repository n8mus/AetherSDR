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
#include <memory>
#include <optional>

class QSerialPort;

namespace AetherSDR::orion {

class Rsp2Receiver;
class OrionRxAudio;
class OrionTxAudio;

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
    void setPanAverage(const QString& panId, int average) override;
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
    void setTxPower(int percent) override;
    void setMicGain(int level) override;
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz, TxAudioSource source,
                       const TxCoordinator::Context& context) override;
    int finishTxAudio(const TxCoordinator::Context& context) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    HealthSnapshot healthSnapshot() const override;
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
    void unkey();
    void leaveRadioSafe();
    void updateTxAudioGate();
    void onTxFailsafe();
    void applyTxGuard(bool on);
    void applyRadioMicProfile();
    void applyPcProfile();
    void setTxPath(TxPath path);
    void setRadioTx(bool on);
    void openTxAudio(const QVariantMap& params);
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
    spectrum::View m_view;
    bool m_viewKnown = false;
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
    // Per-slice monitor controls, applied in the speaker mix only (the
    // per-slice feed stays pre-mute, pre-gain, for TCI and decoders).
    std::array<bool, 2> m_sliceMuted{false, false};
    std::array<float, 2> m_sliceGain{1.0f, 1.0f};
    std::array<int, 2> m_slicePan{50, 50};
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
