#pragma once

// Hy-Gain DCU-3 rotor controller and a rotctld server for it (step 7).
//
// DCU-3 on RS-232 (/dev/portS4, 4800 8N1; its internal jumper on R, which
// disables its USB port). AI1; asks the heading, AP1ddd; sets a target and
// AM1; executes it, 500 ms later or the controller drops the execute
// (hamlib's post_write_delay; bench-verified). While turning it STREAMS
// "ddd;" frames, some truncated, which is why hamlib's DCU backend freezes:
// this reads it as a stream and keeps the last whole frame.
//
// OrionRotctld re-serves the rotctld protocol on loopback :4533 so cqrlog
// and Not1MM keep pointing where they always have. The retired
// rotctld.service must stay retired: one owner per serial port.

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTimer>

#include <memory>

class QSerialPort;
class QTcpSocket;

namespace AetherSDR::orion {

class OrionRotor : public QObject {
    Q_OBJECT

public:
    static constexpr const char* kStationDevice = "/dev/portS4";
    static constexpr int kPollMs = 800;
    static constexpr qint64 kDeadMs = 2500;
    static constexpr int kExecuteDelayMs = 500;
    static constexpr int kMaxBuffer = 512;

    explicit OrionRotor(QObject* parent = nullptr);
    ~OrionRotor() override;

    bool open(const QString& device, QString* error);
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] bool isAlive() const { return m_alive; }
    // Degrees 0..359, or -1 when no frame has been read (or the link died).
    [[nodiscard]] double azimuth() const { return m_azimuth; }

    bool turnTo(double degrees);
    bool stop();

    // The last whole "ddd;" frame in buf (consumed up to it), or -1. A
    // stream with damage keeps only what could still start a frame.
    [[nodiscard]] static double parseFrames(QByteArray& buf);

signals:
    void azimuthChanged(double degrees);
    void aliveChanged(bool alive);

private:
    void poll();
    void onReadyRead();
    void setAlive(bool alive);

    std::unique_ptr<QSerialPort> m_port;
    QTimer m_pollTimer;
    QByteArray m_buffer;
    double m_azimuth = -1.0;
    qint64 m_lastFrameMs = 0;
    bool m_alive = false;
    quint64 m_generation = 0;
};

class OrionRotctld : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 4533;

    explicit OrionRotctld(OrionRotor* rotor, QObject* parent = nullptr);
    // Loopback only: the station's own rotor is not a network service.
    bool listen(quint16 port, QString* error);
    void close();
    [[nodiscard]] quint16 port() const { return m_server.serverPort(); }

    // One protocol line -> the reply text (empty when none is due).
    // Public so the protocol is testable without a socket.
    [[nodiscard]] QString handleLine(const QString& line, bool* quit = nullptr);

private:
    OrionRotor* m_rotor;
    QTcpServer m_server;
};

}  // namespace AetherSDR::orion
