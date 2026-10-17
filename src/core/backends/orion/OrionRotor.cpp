#include "core/backends/orion/OrionRotor.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QSerialPort>
#include <QTcpSocket>

#if defined(Q_OS_UNIX)
#include <sys/ioctl.h>
#include <termios.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(lcOrion)

namespace AetherSDR::orion {

OrionRotor::OrionRotor(QObject* parent)
    : QObject(parent)
{
    m_pollTimer.setInterval(kPollMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &OrionRotor::poll);
}

OrionRotor::~OrionRotor()
{
    close();
}

bool OrionRotor::open(const QString& device, QString* error)
{
    close();
    m_port = std::make_unique<QSerialPort>();
    m_port->setPortName(device);
    m_port->setBaudRate(4800);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port->open(QIODevice::ReadWrite)) {
        if (error) {
            *error = tr("cannot open %1: %2").arg(device, m_port->errorString());
        }
        m_port.reset();
        return false;
    }
    connect(m_port.get(), &QSerialPort::readyRead, this, &OrionRotor::onReadyRead);
    ++m_generation;
    m_buffer.clear();
    m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
    m_pollTimer.start();
    poll();
    return true;
}

void OrionRotor::close()
{
    ++m_generation;
    m_pollTimer.stop();
    if (m_port) {
        m_port->disconnect(this);
#if defined(Q_OS_UNIX) && defined(TIOCNXCL)
        if (m_port->isOpen()) {
            ::ioctl(m_port->handle(), TIOCNXCL);
        }
#endif
        m_port->close();
        m_port.reset();
    }
    if (m_azimuth >= 0.0) {
        m_azimuth = -1.0;
        emit azimuthChanged(-1.0);
    }
    setAlive(false);
}

bool OrionRotor::isOpen() const
{
    return m_port && m_port->isOpen();
}

void OrionRotor::setAlive(bool alive)
{
    if (alive == m_alive) {
        return;
    }
    m_alive = alive;
    emit aliveChanged(alive);
}

void OrionRotor::poll()
{
    if (!isOpen()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_alive && now - m_lastFrameMs > kDeadMs) {
        m_azimuth = -1.0;
        setAlive(false);
        emit azimuthChanged(-1.0);
    }
    m_port->write(QByteArrayLiteral("AI1;"));
}

bool OrionRotor::turnTo(double degrees)
{
    while (degrees < 0.0) {
        degrees += 360.0;
    }
    while (degrees >= 360.0) {
        degrees -= 360.0;
    }
    if (!isOpen()) {
        return false;
    }
    // Target, a gap, then execute: the controller drops an execute that
    // arrives hard on the target. It picks the direction itself and goes
    // the long way rather than cross its stop.
    m_port->write(QByteArrayLiteral("AP1")
                  + QByteArray::number(qRound(degrees)).rightJustified(3, '0') + ';');
    const quint64 generation = m_generation;
    QTimer::singleShot(kExecuteDelayMs, this, [this, generation] {
        if (generation == m_generation && isOpen()) {
            m_port->write(QByteArrayLiteral("AM1;"));
        }
    });
    return true;
}

bool OrionRotor::stop()
{
    if (!isOpen()) {
        return false;
    }
    m_port->write(QByteArrayLiteral(";"));
    return true;
}

void OrionRotor::onReadyRead()
{
    m_buffer += m_port->readAll();
    if (m_buffer.size() > kMaxBuffer) {
        m_buffer = m_buffer.right(kMaxBuffer);
    }
    const double az = parseFrames(m_buffer);
    if (az < 0.0) {
        return;
    }
    m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
    setAlive(true);
    if (az != m_azimuth) {
        m_azimuth = az;
        emit azimuthChanged(az);
    }
}

double OrionRotor::parseFrames(QByteArray& buf)
{
    double last = -1.0;
    qsizetype keepFrom = 0;
    for (qsizetype i = 0; i + 3 < buf.size(); ++i) {
        // A frame is three digits then ';'. Anything else is stream damage
        // and is skipped a byte at a time.
        if (buf[i + 3] != ';') {
            continue;
        }
        const char a = buf[i];
        const char b = buf[i + 1];
        const char c = buf[i + 2];
        if (a < '0' || a > '9' || b < '0' || b > '9' || c < '0' || c > '9') {
            continue;
        }
        const int deg = (a - '0') * 100 + (b - '0') * 10 + (c - '0');
        if (deg > 360) {
            continue;
        }
        last = deg == 360 ? 0.0 : static_cast<double>(deg);
        i += 3;
        keepFrom = i + 1;
    }
    if (last >= 0.0) {
        buf.remove(0, keepFrom);
    } else if (buf.size() > 3) {
        buf = buf.right(3);
    }
    return last;
}

// ---- rotctld -----------------------------------------------------------------

OrionRotctld::OrionRotctld(OrionRotor* rotor, QObject* parent)
    : QObject(parent)
    , m_rotor(rotor)
{
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* c = m_server.nextPendingConnection()) {
            connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
            connect(c, &QTcpSocket::readyRead, this, [this, c] {
                while (c->canReadLine()) {
                    bool quit = false;
                    const QString reply =
                        handleLine(QString::fromLatin1(c->readLine()).trimmed(), &quit);
                    if (!reply.isEmpty()) {
                        c->write(reply.toLatin1());
                    }
                    if (quit) {
                        c->disconnectFromHost();
                        return;
                    }
                }
            });
        }
    });
}

bool OrionRotctld::listen(quint16 port, QString* error)
{
    close();
    if (!m_server.listen(QHostAddress::LocalHost, port)) {
        if (error) {
            *error = tr("cannot listen on localhost:%1: %2").arg(port).arg(m_server.errorString());
        }
        return false;
    }
    return true;
}

void OrionRotctld::close()
{
    if (m_server.isListening()) {
        m_server.close();
    }
}

QString OrionRotctld::handleLine(const QString& line, bool* quit)
{
    if (line.isEmpty()) {
        return {};
    }
    // "+cmd" asks for the labelled long form; cqrlog and Not1MM send the
    // plain form.
    QString cmd = line;
    bool ext = false;
    if (cmd.startsWith(QLatin1Char('+'))) {
        ext = true;
        cmd = cmd.mid(1).trimmed();
    }
    const QStringList tok = cmd.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString verb = tok.value(0);
    const auto rprt = [&](int n) -> QString {
        if (ext) {
            return QStringLiteral("%1:\nRPRT %2\n").arg(verb).arg(n);
        }
        return n != 0 ? QStringLiteral("RPRT %1\n").arg(n) : QString();
    };

    if (verb == QLatin1String("p") || verb == QLatin1String("get_pos")) {
        const double az = m_rotor ? m_rotor->azimuth() : -1.0;
        // No reading yet is an error, not "0.0": that would point a beam
        // at north.
        if (az < 0.0) {
            return rprt(-5);   // RIG_ETIMEOUT
        }
        if (ext) {
            return QStringLiteral("get_pos:\nAzimuth: %1\nElevation: %2\nRPRT 0\n")
                .arg(az, 0, 'f', 2).arg(0.0, 0, 'f', 2);
        }
        return QStringLiteral("%1\n%2\n").arg(az, 0, 'f', 2).arg(0.0, 0, 'f', 2);
    }
    if (verb == QLatin1String("P") || verb == QLatin1String("set_pos")) {
        bool ok = false;
        const double az = tok.value(1).toDouble(&ok);
        if (!ok || az < 0.0 || az > 360.0) {
            return rprt(-1);   // RIG_EINVAL
        }
        const bool sent = m_rotor && m_rotor->turnTo(az);
        QString out;
        if (ext) {
            out = QStringLiteral("set_pos: %1 %2\n").arg(tok.value(1), tok.value(2));
        }
        return out + rprt(sent ? 0 : -5);
    }
    if (verb == QLatin1String("S") || verb == QLatin1String("stop")) {
        return rprt(m_rotor && m_rotor->stop() ? 0 : -5);
    }
    if (verb == QLatin1String("q") || verb == QLatin1String("Q") || verb == QLatin1String("quit")) {
        if (quit) {
            *quit = true;
        }
        return {};
    }
    if (verb == QLatin1String("_") || verb == QLatin1String("get_info")) {
        return QStringLiteral("DCU2/DCU3/YRC-1\n");
    }
    if (verb == QLatin1String("\\dump_state")) {
        // Protocol 1, hamlib's Hy-Gain DCU2/DCU3/YRC-1 model, azimuth only:
        // what a client gets from the real daemon.
        return QStringLiteral("1\n406\nmin_az=0.000000\nmax_az=360.000000\n"
                              "min_el=0.000000\nmax_el=0.000000\n"
                              "south_zero=0\nrot_type=Other\ndone\n");
    }
    return rprt(-11);   // RIG_ENIMPL
}

}  // namespace AetherSDR::orion
