#include "core/backends/orion/OrionWinKeyer.h"

#include <QDir>
#include <QLoggingCategory>
#include <QSerialPort>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <sys/ioctl.h>
#include <termios.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(lcOrion)

namespace AetherSDR::orion {

namespace {
// WK status byte (0xC0 | bits), K1EL datasheet.
constexpr quint8 kStatusBreakIn = 0x02;
constexpr quint8 kStatusBusy = 0x04;
}  // namespace

OrionWinKeyer::OrionWinKeyer(QObject* parent)
    : QObject(parent)
{
    m_stepTimer.setSingleShot(true);
    connect(&m_stepTimer, &QTimer::timeout, this, &OrionWinKeyer::step);
    // The keyer reports each new pot value once; settle so a knob flapping
    // between detents does not chatter the speed.
    m_potTimer.setSingleShot(true);
    m_potTimer.setInterval(kPotSettleMs);
    connect(&m_potTimer, &QTimer::timeout, this, [this] {
        if (m_potPending >= 0 && m_potPending != m_potEmitted) {
            m_potEmitted = m_potPending;
            // A host speed overrides the knob on the keyer, so following
            // the knob means sending its speed back, as the reference did.
            setSpeed(m_potEmitted);
            emit speedPotChanged(m_potEmitted);
        }
    });
}

OrionWinKeyer::~OrionWinKeyer()
{
    close();
}

QString OrionWinKeyer::findStationKeyer()
{
    const QDir byId(QStringLiteral("/dev/serial/by-id"));
    const QStringList hits = byId.entryList(
        {QStringLiteral("*%1*").arg(QString::fromLatin1(kFtdiSerial))}, QDir::System | QDir::Files);
    return hits.isEmpty() ? QString() : byId.absoluteFilePath(hits.first());
}

void OrionWinKeyer::open(const QString& device)
{
    close();
    m_device = device;
    m_port = std::make_unique<QSerialPort>();
    m_port->setPortName(device);
    m_port->setBaudRate(1200);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::TwoStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port->open(QIODevice::ReadWrite)) {
        const QString reason = m_port->errorString();
        m_port.reset();
        fail(tr("cannot open %1: %2").arg(device, reason));
        return;
    }
    m_port->setDataTerminalReady(true);
    m_port->setRequestToSend(false);
    connect(m_port.get(), &QSerialPort::readyRead, this, &OrionWinKeyer::onReadyRead);
    m_rx.clear();
    m_firmware = -1;
    // The WKUSB needs a beat after DTR before it listens (live-found).
    m_state = State::WaitDtr;
    m_stepTimer.start(kDtrSettleMs);
}

void OrionWinKeyer::close()
{
    m_stepTimer.stop();
    m_potTimer.stop();
    if (m_port && m_port->isOpen()) {
        if (m_state == State::Ready) {
            write(QByteArray(1, '\x0A'));           // stop: nothing left buffered
            write(QByteArray("\x00\x03", 2));       // host close
            m_port->flush();
            m_port->waitForBytesWritten(200);
        }
#if defined(Q_OS_UNIX) && defined(TIOCNXCL)
        ::ioctl(m_port->handle(), TIOCNXCL);
#endif
        m_port->disconnect(this);
        m_port->close();
    }
    m_port.reset();
    m_state = State::Closed;
    if (m_busy) {
        m_busy = false;
        emit busyChanged(false);
    }
}

void OrionWinKeyer::fail(const QString& reason)
{
    qCWarning(lcOrion) << "WinKeyer:" << reason;
    close();
    emit failed(reason);
}

void OrionWinKeyer::write(const QByteArray& bytes)
{
    if (m_port && m_port->isOpen()) {
        m_port->write(bytes);
    }
}

void OrionWinKeyer::step()
{
    switch (m_state) {
    case State::WaitDtr:
        m_port->clear();
        write(QByteArray("\x13\x13\x13", 3));       // wake
        m_state = State::WaitNulls;
        m_stepTimer.start(kNullSettleMs);
        break;
    case State::WaitNulls:
        m_port->readAll();
        m_rx.clear();
        write(QByteArray("\x00\x04\x14", 3));       // Admin: echo 0x14
        m_state = State::WaitEcho;
        m_stepTimer.start(kEchoTimeoutMs);
        break;
    case State::WaitEcho:
        fail(tr("no echo from %1: is this the WinKeyer?").arg(m_device));
        break;
    case State::WaitHostOpen:
        // The reply byte is the firmware major revision (31 = rev 31.03).
        m_firmware = m_rx.isEmpty() ? -1 : static_cast<int>(static_cast<uchar>(m_rx.back()));
        m_rx.clear();
        m_state = State::Ready;
        setSpeed(m_wpm);
        // The knob's range, as the reference program set it (7..45 WPM,
        // held in the keyer's RAM only), then ask where the knob is.
        write(QByteArray::fromRawData("\x05\x07\x26\x00", 4));
        write(QByteArray("\x07", 1));
        qCInfo(lcOrion) << "WinKeyer: ready on" << m_device << "firmware" << m_firmware
                        << (isWk3() ? "(WK3)" : "");
        emit ready(m_firmware);
        break;
    default:
        break;
    }
}

void OrionWinKeyer::onReadyRead()
{
    const QByteArray data = m_port->readAll();
    if (m_state == State::WaitEcho) {
        m_rx += data;
        if (m_rx.contains('\x14')) {
            m_stepTimer.stop();
            m_rx.clear();
            write(QByteArray("\x00\x02", 2));       // Admin: host open
            m_state = State::WaitHostOpen;
            m_stepTimer.start(kHostOpenWaitMs);
        }
        return;
    }
    if (m_state == State::WaitHostOpen) {
        m_rx += data;
        return;
    }
    if (m_state != State::Ready) {
        return;
    }
    for (const char ch : data) {
        const auto b = static_cast<quint8>(ch);
        if ((b & 0xC0) == 0xC0) {
            // Status byte. WK1 mode (the only mode this fork uses) sends no
            // pushbutton bytes, so every one of these is keyer status.
            const bool busy = b & kStatusBusy;
            if (b & kStatusBreakIn) {
                emit breakIn();
            }
            if (busy != m_busy) {
                m_busy = busy;
                emit busyChanged(busy);
            }
        } else if ((b & 0xC0) == 0x80) {
            // Speed pot, as an offset from the pot minimum.
            m_potPending = m_potMin + (b & 0x3F);
            m_potTimer.start();
        }
    }
}

void OrionWinKeyer::setSpeed(int wpm)
{
    m_wpm = std::clamp(wpm, kMinWpm, kMaxWpm);
    if (m_state == State::Ready) {
        QByteArray cmd("\x02", 1);
        cmd += static_cast<char>(m_wpm);
        write(cmd);
    }
}

QByteArray OrionWinKeyer::filterText(const QString& text, bool wk3)
{
    QByteArray out;
    for (const QChar qc : text.toUpper()) {
        const char c = qc.toLatin1();
        // The keyer's own ASCII table (WK3 datasheet p17). ! # % & * map to
        // null there and would vanish silently, so they are dropped here.
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.'
            || c == ',' || c == '?' || c == '/' || c == ':' || c == ';' || c == '<'
            || c == '=' || c == '>' || c == '(' || c == ')' || c == '+' || c == '-'
            || c == '\'' || c == '"' || c == '@' || c == '$' || c == '|') {
            out.append(c);
        } else if (wk3 && (c == '[' || c == '\\' || c == ']')) {
            out.append(c);   // AS, DN, KN exist only on WK3 silicon
        }
    }
    return out;
}

QString OrionWinKeyer::allowedCharacters(bool wk3)
{
    QString s = QStringLiteral(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,?/:;<=>()+-'\"@$|");
    if (wk3) {
        s += QStringLiteral("[\\]");
    }
    return s;
}

QByteArray OrionWinKeyer::sendText(const QString& text)
{
    if (m_state != State::Ready) {
        return {};
    }
    const QByteArray out = filterText(text, isWk3());
    if (out.isEmpty()) {
        return {};
    }
    write(out);
    if (!m_busy) {
        m_busy = true;
        emit busyChanged(true);
    }
    return out;
}

void OrionWinKeyer::sendProsign(char a, char b)
{
    if (m_state != State::Ready) {
        return;
    }
    QByteArray cmd("\x1B", 1);
    cmd += QChar::fromLatin1(a).toUpper().toLatin1();
    cmd += QChar::fromLatin1(b).toUpper().toLatin1();
    write(cmd);
    if (!m_busy) {
        m_busy = true;
        emit busyChanged(true);
    }
}

void OrionWinKeyer::stop()
{
    if (m_state != State::Ready) {
        return;
    }
    write(QByteArray(1, '\x0A'));
    if (m_port) {
        m_port->flush();
    }
}

}  // namespace AetherSDR::orion
