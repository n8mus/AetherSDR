#pragma once

// K1EL WinKeyer (WK2/WK3 protocol) for the Orion backend (step 7). The
// station's CW keyer, operator decision: AetherSDR's CW text and cwdaemon
// (cqrlog) both key through it; the Orion's own keyer is never used.
//
// 1200 8N2, DTR on, RTS off. The keyer hardware owns element timing, the
// paddle (which interrupts buffered text by itself) and the speed pot.
// Open sequence from the reference program (K1EL datasheet, cqrlog,
// fldigi), done with timers instead of sleeps so the owner thread never
// blocks: wait 800 ms after DTR, three 0x13 nulls, 300 ms, echo test
// 00 04 14 (wait for 0x14), host open 00 02 (answers the firmware major
// revision; 3x = WK3). Host open leaves the keyer in WK1 mode.
//
// This fork adopts NONE of the keyer's shaping parameters and never writes
// the mode register: the keyer keeps what its owner stored. Only speed is
// sent (the AetherSDR CW speed control).

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

class QSerialPort;

namespace AetherSDR::orion {

class OrionWinKeyer : public QObject {
    Q_OBJECT

public:
    // The station's WinKeyer: an FTDI adapter, found by serial number.
    static constexpr const char* kFtdiSerial = "A904QF5Z";
    static constexpr int kDtrSettleMs = 800;
    static constexpr int kNullSettleMs = 300;
    static constexpr int kEchoTimeoutMs = 3000;
    static constexpr int kHostOpenWaitMs = 600;
    static constexpr int kPotSettleMs = 250;
    static constexpr int kMinWpm = 5;
    static constexpr int kMaxWpm = 99;

    explicit OrionWinKeyer(QObject* parent = nullptr);
    ~OrionWinKeyer() override;

    // The /dev/serial/by-id path for kFtdiSerial, or empty when absent.
    [[nodiscard]] static QString findStationKeyer();

    // Starts the open sequence; ready() or failed() follows.
    void open(const QString& device);
    void close();   // host close: the keyer goes back to standalone
    [[nodiscard]] bool isReady() const { return m_state == State::Ready; }
    [[nodiscard]] int firmwareRevision() const { return m_firmware; }
    [[nodiscard]] bool isWk3() const { return m_firmware >= 30; }
    [[nodiscard]] QString device() const { return m_device; }
    [[nodiscard]] bool busy() const { return m_busy; }

    void setSpeed(int wpm);
    [[nodiscard]] int speed() const { return m_wpm; }

    // Buffered ASCII, filtered to the keyer's own table (WK3 datasheet
    // p17): characters it would map to null are dropped here, not sent.
    // Returns the bytes actually queued (empty = nothing to send).
    QByteArray sendText(const QString& text);
    // 0x1B merge: the next two characters as one prosign.
    void sendProsign(char a, char b);
    void stop();   // 0x0A: clear the buffer, key up

    // The keyer's own character filter, exposed for tests and for the
    // capability's allowed-character list.
    [[nodiscard]] static QByteArray filterText(const QString& text, bool wk3);
    [[nodiscard]] static QString allowedCharacters(bool wk3);

signals:
    void ready(int firmwareRevision);
    void failed(const QString& reason);
    void busyChanged(bool busy);
    void breakIn();              // the paddle interrupted buffered text
    void speedPotChanged(int wpm);

private:
    enum class State { Closed, WaitDtr, WaitNulls, WaitEcho, WaitHostOpen, Ready };
    void step();
    void fail(const QString& reason);
    void onReadyRead();
    void write(const QByteArray& bytes);

    std::unique_ptr<QSerialPort> m_port;
    QString m_device;
    State m_state = State::Closed;
    QTimer m_stepTimer;
    QTimer m_potTimer;
    QByteArray m_rx;
    int m_firmware = -1;
    int m_wpm = 25;
    int m_potMin = 7;
    int m_potPending = -1;
    int m_potEmitted = -1;
    bool m_busy = false;
};

}  // namespace AetherSDR::orion
