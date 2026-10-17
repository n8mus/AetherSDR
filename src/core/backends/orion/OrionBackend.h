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

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <array>
#include <memory>
#include <optional>

class QSerialPort;

namespace AetherSDR::orion {

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
    static constexpr int kWriteCoalesceMs = 40;
    static constexpr int kModeAfterRetuneMs = 120;
    static constexpr int kFilterRecallMs = 400;
    static constexpr int kFrequencyEchoGuardMs = 1500;
    static constexpr int kFilterEchoGuardMs = 2000;
    static constexpr qint64 kLargeJumpHz = 100000;
    static constexpr int kLinkTickMs = 1000;
    // The pan's width until the RSP2 supplies a real capture (step 2).
    static constexpr double kPanViewMhz = 0.2;
    // Until the RSP2 supplies real spectrum, a flat trace at the floor keeps
    // the pan and waterfall alive (AetherSDR's connect overlay only clears on
    // the first spectrum frame).
    static constexpr int kPlaceholderSpectrumMs = 250;
    static constexpr int kPlaceholderSpectrumBins = 512;
    static constexpr float kPlaceholderFloorDbm = -127.0f;

    // ---- IRadioBackend ----
    RadioCapabilities capabilities() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override { return m_connected; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanPreamp(const QString& panId, int step) override;
    void setPanAttenuator(const QString& panId, int step) override;
    void setActiveSlice(int sliceId) override;

    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation,
                   const AetherSDR::TxCoordinator::Completion& completion = {}) override;
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
    double m_panCentreMhz = 0.0;
    int m_activeSlice = 0;

    OrionPollScheduler m_scheduler;
    QElapsedTimer m_clock;
    QTimer m_connectTimer;
    QTimer m_pollTimer;
    QTimer m_linkTimer;
    QTimer m_writeTimer;
    QTimer m_placeholderSpectrumTimer;
    qint64 m_lastFrequencyWriteMs = -1'000'000;

    // Link accounting (owner thread only).
    qint64 m_rxBytes = 0;
    qint64 m_txBytes = 0;
    quint64 m_repliesDecoded = 0;
    quint64 m_linesIgnored = 0;
    qint64 m_lastReplyMs = -1;
    bool m_trafficSinceTick = false;
    qint64 m_frequencyQuerySentMs = -1;
    int m_rttMs = -1;
};

}  // namespace AetherSDR::orion
