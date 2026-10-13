#include "Kat500Model.h"
#include "core/Kat500Protocol.h"
#include "core/Kat500Maintenance.h"
#include <QTcpSocket>
#include <QIODevice>
#include <QLoggingCategory>
#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif
#include <algorithm>
#include <cmath>
#include <utility>

Q_LOGGING_CATEGORY(lcKat500, "aethersdr.kat500")

namespace AetherSDR {
using namespace Kat500;
namespace {
const QList<QByteArray> kPolls{"PS;","TP;","VFWD;","VRFL;","VSWR;","VSWRB;","F;","BN;","AN;","MD;",
    "AMPI;","ATTN;","BYP;","FLT;","C;","L;","SIDE;","FY;","FC;","FX;","I;","RV;","SN;"};
QByteArray hexMask(int mask) { return QByteArray::number(mask,16).rightJustified(2,'0').toUpper(); }
}
Kat500Model::Kat500Model(QObject* parent) : QObject(parent), m_parser(std::make_unique<FrameParser>()),
    m_socket(std::make_unique<QTcpSocket>())
{
    m_clock.start(); m_now = [this] { return m_clock.elapsed(); };
    m_timer.setInterval(100); m_reconnectTimer.setSingleShot(true); m_reconnectTimer.setInterval(5000);
    connect(&m_timer,&QTimer::timeout,this,&Kat500Model::tick);
    connect(&m_reconnectTimer,&QTimer::timeout,this,&Kat500Model::reconnect);
    m_parser->setCallback([this](const QByteArray& frame) { receiveFrame(frame); });
    connect(m_socket.get(),&QTcpSocket::connected,this,&Kat500Model::transportOpened);
    connect(m_socket.get(),&QTcpSocket::readyRead,this,[this] { receiveBytes(m_socket->readAll()); });
    connect(m_socket.get(),&QTcpSocket::disconnected,this,[this] { transportClosed(); });
    connect(m_socket.get(),&QTcpSocket::errorOccurred,this,[this](QAbstractSocket::SocketError) {
        transportClosed(m_socket->errorString());
    });
#ifdef HAVE_SERIALPORT
    m_serial = std::make_unique<QSerialPort>();
    connect(m_serial.get(),&QSerialPort::readyRead,this,[this] { receiveBytes(m_serial->readAll()); });
    connect(m_serial.get(),&QSerialPort::errorOccurred,this,[this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError || error == QSerialPort::PermissionError) {
            transportClosed(m_serial->errorString());
        }
    });
#endif
}
Kat500Model::~Kat500Model() { m_autoReconnect = false; disconnectDevice(); }
void Kat500Model::connectToHost(const QString& host, quint16 port)
{
    if (m_connected || m_connecting || host.trimmed().isEmpty() || !port) { return; }
    m_network = true; m_host = host.trimmed(); m_port = port; m_connecting = true;
    m_deliberateDisconnect = false; m_connectStarted = m_now(); m_timer.start();
    m_socket->connectToHost(m_host,m_port); emit availabilityChanged();
}
void Kat500Model::connectSerial(const QString& portName, int baudRate)
{
#ifdef HAVE_SERIALPORT
    if (m_connected || m_connecting || portName.trimmed().isEmpty() ||
        (baudRate != 4800 && baudRate != 9600 && baudRate != 19200 && baudRate != 38400)) { return; }
    m_network = false; m_portName = portName; m_baud = baudRate; m_deliberateDisconnect = false;
    m_serial->setPortName(portName); m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8); m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop); m_serial->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_serial->open(QIODevice::ReadWrite)) { transportClosed(m_serial->errorString()); return; }
    transportOpened();
#else
    Q_UNUSED(portName); Q_UNUSED(baudRate);
    emit connectionFailed(QStringLiteral("This build does not include serial-port support."));
#endif
}
void Kat500Model::disconnectDevice()
{
    m_deliberateDisconnect = true; m_reconnectTimer.stop();
    transportClosed();
}
void Kat500Model::reconnect()
{
    if (!m_autoReconnect || m_deliberateDisconnect || m_connected || m_connecting) { return; }
    if (m_network) { connectToHost(m_host,m_port); } else { connectSerial(m_portName,m_baud); }
}
void Kat500Model::transportOpened()
{
    ++m_generation; m_resetAt = 0;
    m_connected = true; m_connecting = false; m_detected = false; m_status = {};
    m_configuration.clear(); m_received.clear(); m_lastStateFrames.clear(); m_queue.clear(); m_active.reset(); m_parser->reset();
    m_memoryReading = false; m_dropMemory = false; m_waking = false; m_wakeCount = 0;
    m_lastIo = m_lastReply = -10000; m_quarantineUntil = 0; m_nextPoll = 0; m_pollIndex = 0;
    m_operation.clear(); m_timer.start(); queueIdentity();
    report(QStringLiteral("Transport connected: %1").arg(description())); emit connected(); emit availabilityChanged();
}
void Kat500Model::transportClosed(const QString& reason)
{
    ++m_generation; m_resetAt = 0;
    const bool wasOpen = m_connected || m_connecting;
    m_connected = false; m_connecting = false; m_timer.stop();
    m_socket->abort();
#ifdef HAVE_SERIALPORT
    if (m_serial->isOpen()) { m_serial->close(); }
#endif
    if (operationBusy()) { fail(reason.isEmpty() ? QStringLiteral("Transport closed; operation stopped.") : reason); }
    m_queue.clear(); m_active.reset(); m_parser->reset(); m_received.clear(); m_configuration.clear();
    m_memoryReading = false; m_dropMemory = false;
    if (m_detected) { m_detected = false; emit detectedChanged(false); }
    if (wasOpen) { report(QStringLiteral("Transport closed.")); emit disconnected(); }
    if (!reason.isEmpty()) { report(reason); emit connectionFailed(reason); }
    emit availabilityChanged();
    if (m_autoReconnect && !m_deliberateDisconnect) { m_reconnectTimer.start(); }
}
bool Kat500Model::isResponding() const { return m_connected && m_now() - m_lastReply <= 5000; }
QString Kat500Model::description() const
{
    return m_network ? QStringLiteral("NETWORK %1:%2 (bridge controls baud)").arg(m_host).arg(m_port)
                     : QStringLiteral("DIRECT %1 at %2 baud").arg(m_portName).arg(m_baud);
}
qint64 Kat500Model::fieldAgeMs(const QString& command) const
{
    const auto it = m_received.constFind(command);
    return it == m_received.cend() ? -1 : std::max<qint64>(0,m_now() - *it);
}
QString Kat500Model::unavailableReason(bool writing) const
{
    if (!m_connected) { return QStringLiteral("Connect the KAT500 first."); }
    if (!m_detected) { return QStringLiteral("Waiting for KAT500 identification."); }
    if (m_status.identity == QStringLiteral("kat500")) { return QStringLiteral("Bootloader detected. Use the official Elecraft Utility."); }
    if (!isResponding()) { return QStringLiteral("KAT500 readback is stale; waiting for a wake response."); }
    if (operationBusy()) { return QStringLiteral("Another KAT500 operation is in progress."); }
    if (m_now() < m_quarantineUntil) { return QStringLiteral("Waiting for late replies to drain."); }
    if (m_status.powerOn != true) { return QStringLiteral("KAT500 power is off or unknown."); }
    if (writing && (m_status.tuning != false || m_status.forwardAdc != 0 || fieldAgeMs(QStringLiteral("TP")) < 0 ||
                    fieldAgeMs(QStringLiteral("VFWD")) < 0 || fieldAgeMs(QStringLiteral("TP")) > 5000 || fieldAgeMs(QStringLiteral("VFWD")) > 5000)) {
        return QStringLiteral("A fresh idle tuner and zero forward ADC count are required.");
    }
    return {};
}
void Kat500Model::report(const QString& text, bool polling)
{
    if (polling && !m_verbose) { return; }
    const QString line = QStringLiteral("%1 %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),text);
    qCDebug(lcKat500).noquote() << text;
    m_debug.append(line); while (m_debug.size() > 1000) { m_debug.removeFirst(); }
    emit diagnostic(line); emit debugChanged();
}
bool Kat500Model::write(const QByteArray& bytes)
{
    if (!m_connected || bytes.isEmpty()) { return false; }
    bool success = false;
    if (m_write) { success = m_write(bytes); }
    else {
        QIODevice* device = m_socket.get();
#ifdef HAVE_SERIALPORT
        if (!m_network) { device = m_serial.get(); }
#endif
        success = device->bytesToWrite() < 1024 && device->write(bytes) == bytes.size();
    }
    if (!success) { transportClosed(QStringLiteral("KAT500 transport write failed.")); return false; }
    m_lastIo = m_now(); report(QStringLiteral("TX %1").arg(QString::fromLatin1(bytes)), !m_operationWriting);
    return true;
}
void Kat500Model::enqueue(Request request)
{
    if (!m_connected || (request.mutation && !operationBusy())) { return; }
    if (request.wire.isEmpty() || m_queue.size() >= 128) { fail(QStringLiteral("Invalid or excessive KAT500 request queue.")); return; }
    m_queue.enqueue(std::move(request));
}
void Kat500Model::query(const QByteArray& wire, std::function<void()> complete)
{
    enqueue({wire,[wire](const QByteArray& frame) { return matchesStatusQuery(wire,frame); },
        [complete = std::move(complete)](const QByteArray&) { if (complete) { complete(); } }});
}
void Kat500Model::queueIdentity()
{
    for (const QByteArray& command : {QByteArray("I;"),QByteArray("RV;"),QByteArray("SN;"),QByteArray("PS;"),QByteArray("TP;"),QByteArray("VFWD;")}) { query(command); }
}
void Kat500Model::tick()
{
    const qint64 now = m_now();
    if (m_connecting && now - m_connectStarted > 10000) { transportClosed(QStringLiteral("Connection timed out.")); return; }
    if (!m_connected) { return; }
    emit availabilityChanged();
    if (!m_connected) { return; }
    if (m_resetAt) {
        if (now >= m_resetAt) {
            m_resetAt = 0;
            if (!write("RST0;")) { return; }
            finish(QStringLiteral("Factory reset and restart requested. Reconnect at the default baud to verify identity and defaults. No power-on command was sent."));
            disconnectDevice();
        }
        return;
    }
    if (m_dropMemory && now >= m_quarantineUntil) {
        transportClosed(QStringLiteral("Incomplete memory response; close the stream before further commands.")); return;
    }
    if (m_active) {
        if (now - m_sentAt > m_active->timeoutMs) {
            const bool mutation = m_active->mutation;
            if (operationBusy()) { fail(mutation ? QStringLiteral("Write completion is uncertain; no retry was sent.") : QStringLiteral("KAT500 reply timed out.")); }
            else { m_active.reset(); m_queue.clear(); m_quarantineUntil = now + 2000; m_lastReply = -10000; }
        }
        return;
    }
    if (now < m_quarantineUntil) { return; }
    if (m_queue.isEmpty()) {
        if (now < m_nextPoll || operationBusy()) { return; }
        if (m_status.powerOn == false) { query("PS;"); m_nextPoll = now + 5000; }
        else if (m_status.identity == QStringLiteral("kat500")) { query("I;"); m_nextPoll = now + 5000; }
        else { query(kPolls[m_pollIndex++ % kPolls.size()]); m_nextPoll = now + 200; }
    }
    if (m_waking || now - m_lastIo > 1500) {
        m_waking = true;
        if (++m_wakeCount > 10) {
            m_waking = false; m_wakeCount = 0; m_lastReply = -10000;
            if (operationBusy()) { fail(QStringLiteral("KAT500 did not acknowledge the wake handshake.")); }
            else { m_queue.clear(); m_nextPoll = now + 5000; }
            return;
        }
        write(";"); return;
    }
    m_active = m_queue.dequeue(); m_sentAt = now;
    if (m_active->wire.startsWith("DM") || m_active->wire.contains(";DM")) { m_memoryReading = true; }
    write(m_active->wire);
}
void Kat500Model::receiveBytes(const QByteArray& bytes) { m_parser->feed(bytes); }
void Kat500Model::receiveFrame(const QByteArray& frame)
{
    if (!m_connected) { return; }
    report(QStringLiteral("RX %1;").arg(QString::fromLatin1(frame)),true);
    if (m_dropMemory) { if (frame.endsWith(" UNUSED")) { m_dropMemory = false; } return; }
    if (m_now() < m_quarantineUntil) { return; }
    if (m_waking) {
        if (frame.isEmpty()) { m_waking = false; m_wakeCount = 0; m_lastIo = m_now(); m_lastReply = m_now(); }
        return;
    }
    // DM fields look like status replies. Keep the entire compound transaction
    // isolated, including any late continuation after cancellation.
    if (m_memoryReading) {
        if (m_memoryFrames.size() >= 40) { fail(QStringLiteral("Memory response exceeds six slots.")); return; }
        m_memoryFrames.append(frame);
        if (frame.endsWith(" UNUSED")) {
            const auto bin = decodeMemoryBin(m_memoryFrames);
            m_memoryReading = false; m_active.reset();
            if (!bin) { fail(QStringLiteral("Malformed memory response.")); return; }
            m_memories = *bin;
            if (m_memoryExpected) {
                const MemoryMatch expected = *m_memoryExpected;
                const bool found = std::any_of(bin->matches.cbegin(),bin->matches.cend(),[&](const MemoryMatch& match) {
                    return match.antenna == expected.antenna && match.bypassed == expected.bypassed &&
                        (expected.bypassed || (match.side == expected.side && match.capacitorMask == expected.capacitorMask && match.inductorMask == expected.inductorMask));
                });
                if (!found) { fail(QStringLiteral("Memorize was not confirmed by memory-bin readback.")); return; }
                m_memoryExpected.reset();
            }
            m_lastReply = m_now();
            const MemoryBin result = m_memories;
            finish(QStringLiteral("Memory bin readback confirmed.")); emit memoriesRead(result);
        }
        return;
    }
    if (frame == "FT" && m_operation == QStringLiteral("Request tuner search")) { m_tuneCompleted = true; return; }
    // Never treat unsolicited DM continuation or EEPROM data as live telemetry.
    if (frame.startsWith("DM ")) { m_dropMemory = true; return; }
    Status identity = m_status;
    if ((frame == "KAT500" || frame == "kat500" || frame.startsWith("SN") || frame.startsWith("RV")) && applyStatus(frame,identity) &&
        ((!m_status.serial.isEmpty() && identity.serial != m_status.serial) ||
         (!m_status.firmware.isEmpty() && identity.firmware != m_status.firmware) ||
         (!m_status.identity.isEmpty() && identity.identity != m_status.identity))) {
        transportClosed(QStringLiteral("KAT500 identity changed; pending operations were discarded.")); return;
    }
    const bool matched = m_active && m_active->matches(frame);
    if (!matched) { return; }
    Request completed = std::move(*m_active); m_active.reset();
    const QByteArray requested = completed.wire;
    const quint64 generation = m_generation;
    m_lastReply = m_now();
    Status parsed = m_status;
    if (applyStatus(frame,parsed)) {
        if ((!m_status.serial.isEmpty() && parsed.serial != m_status.serial) ||
            (!m_status.firmware.isEmpty() && parsed.firmware != m_status.firmware) ||
            (!m_status.identity.isEmpty() && parsed.identity != m_status.identity)) {
            transportClosed(QStringLiteral("KAT500 identity changed; pending operations were discarded.")); return;
        }
        m_status = parsed;
        if (!m_status.identity.isEmpty() && !m_detected) { m_detected = true; emit detectedChanged(true); }
        const QList<QByteArray> commands = requested.split(';');
        const QString key = QString::fromLatin1(commands.size() >= 2 ? commands[commands.size()-2] : requested);
        m_received[key] = m_now(); m_lastReply = m_now();
        static const QStringList stateKeys{QStringLiteral("I"),QStringLiteral("RV"),QStringLiteral("SN"),QStringLiteral("PS"),
            QStringLiteral("MD"),QStringLiteral("AN"),QStringLiteral("FLT"),QStringLiteral("BYP"),QStringLiteral("AMPI"),
            QStringLiteral("ATTN"),QStringLiteral("SIDE"),QStringLiteral("C"),QStringLiteral("L")};
        if (stateKeys.contains(key) && m_lastStateFrames.value(key) != frame) {
            m_lastStateFrames[key] = frame; report(QStringLiteral("Readback %1").arg(QString::fromLatin1(frame)));
        }
        emit statusUpdated(m_status); emit telemetryReceived(key,m_status);
    }
    if (generation == m_generation && m_connected && completed.complete) { completed.complete(frame); }
}
bool Kat500Model::begin(const QString& name, bool writing)
{
    const QString reason = unavailableReason(false);
    if (!reason.isEmpty()) { report(reason); emit operationFailed(reason); return false; }
    ++m_generation;
    m_operation = name; m_operationWriting = writing; m_operationSerial = m_status.serial; m_operationFirmware = m_status.firmware;
    m_progress = 0; m_total = 0; m_queue.clear();
    const quint64 generation = m_generation;
    emit operationBusyChanged(true); emit availabilityChanged(); report(name);
    return m_connected && m_generation == generation && operationBusy();
}
void Kat500Model::finish(const QString& message)
{
    m_operation.clear(); m_operationWriting = false; m_regionComplete = {}; m_edits.clear();
    m_nextPoll = m_now() + 200; report(message); emit operationBusyChanged(false); emit operationFinished(message); emit availabilityChanged();
}
void Kat500Model::fail(const QString& reason)
{
    ++m_generation; m_resetAt = 0;
    qCWarning(lcKat500).noquote() << reason;
    const bool busy = operationBusy();
    if (m_memoryReading) { m_dropMemory = true; }
    m_memoryReading = false; m_memoryExpected.reset(); m_memoryFrames.clear(); m_queue.clear(); m_active.reset(); m_edits.clear();
    m_regionComplete = {}; m_regionData.clear(); m_eraseBands.clear();
    m_operation.clear(); m_operationWriting = false; m_quarantineUntil = m_now() + 2000;
    report(reason); if (busy) { emit operationBusyChanged(false); } emit operationFailed(reason); emit availabilityChanged();
}
void Kat500Model::cancelOperation()
{
    if (operationBusy()) { fail(QStringLiteral("Operation canceled. Already confirmed writes remain applied; pending writes were discarded.")); }
}
bool Kat500Model::requestValue(const QByteArray& setter, const QByteArray& getter, std::function<bool(const Status&)> verify,
                              const QString& name, bool allowOff)
{
    if (setter.isEmpty()) { return false; }
    if (allowOff) {
        if (!m_connected || !m_detected || m_status.identity != QStringLiteral("KAT500") || operationBusy() || m_now() < m_quarantineUntil) { return false; }
        ++m_generation; m_operation = name; m_operationWriting = true; m_queue.clear(); emit operationBusyChanged(true);
    } else {
        const QString reason = unavailableReason(true);
        if (!reason.isEmpty()) { emit operationFailed(reason); return false; }
        if (!begin(name,true)) { return false; }
    }
    enqueue({setter + getter,[getter](const QByteArray& frame) { return matchesStatusQuery(getter,frame); },
        [this,verify = std::move(verify)](const QByteArray&) {
            if (!verify(m_status)) { fail(QStringLiteral("Requested value was not confirmed by device readback.")); }
            else { finish(QStringLiteral("Device readback confirmed.")); }
        },2000,true});
    return true;
}
bool Kat500Model::setPower(bool on) { return requestValue(on ? "PS1;" : "PS0;","PS;",[on](const Status& s) { return s.powerOn == on; },QStringLiteral("Set power"),true); }
bool Kat500Model::setMode(Mode mode)
{
    if (mode != Mode::Auto && mode != Mode::Manual && mode != Mode::Bypass) { return false; }
    return requestValue(mode == Mode::Auto ? "MDA;" : mode == Mode::Manual ? "MDM;" : "MDB;","MD;",
        [mode](const Status& s) { return s.mode == mode; },QStringLiteral("Set mode"));
}
bool Kat500Model::setAntenna(int antenna)
{
    if (antenna < 1 || antenna > 3) { return false; }
    if (m_status.band && m_configuration.value({Setting::EnabledAntenna,*m_status.band,antenna},-1) == 0) { return false; }
    return requestValue("AN" + QByteArray::number(antenna) + ';',"AN;",[antenna](const Status& s) { return s.antenna == antenna; },QStringLiteral("Select antenna"));
}
bool Kat500Model::setCapacitorMask(int mask)
{
    return mask >= 0 && mask <= 255 && requestValue("C" + hexMask(mask) + ';',"C;",[mask](const Status& s) { return s.capacitorMask == mask; },QStringLiteral("Set capacitors"));
}
bool Kat500Model::setInductorMask(int mask)
{
    return mask >= 0 && mask <= 255 && requestValue("L" + hexMask(mask) + ';',"L;",[mask](const Status& s) { return s.inductorMask == mask; },QStringLiteral("Set inductors"));
}
bool Kat500Model::setCapacitorSide(CapacitorSide side)
{
    if (side != CapacitorSide::Transmitter && side != CapacitorSide::Antenna) { return false; }
    return requestValue(side == CapacitorSide::Transmitter ? "SIDET;" : "SIDEA;","SIDE;",[side](const Status& s) { return s.side == side; },QStringLiteral("Set capacitor side"));
}
bool Kat500Model::setBypassed(bool bypassed) { return requestValue(bypassed ? "BYPB;" : "BYPN;","BYP;",[bypassed](const Status& s) { return s.bypassed == bypassed; },QStringLiteral("Set bypass")); }
bool Kat500Model::setAttenuator(bool enabled) { return requestValue(enabled ? "ATTN1;" : "ATTN0;","ATTN;",[enabled](const Status& s) { return s.attenuator == enabled; },QStringLiteral("Set attenuator")); }
bool Kat500Model::setAmpKeyInterrupted(bool interrupted) { return requestValue(interrupted ? "AMPI1;" : "AMPI0;","AMPI;",[interrupted](const Status& s) { return s.ampKeyInterrupted == interrupted; },QStringLiteral("Set amplifier key interrupt")); }
bool Kat500Model::clearFault() { return requestValue("FLTC;","FLT;",[](const Status& s) { return s.faultCode == 0; },QStringLiteral("Clear current fault")); }
bool Kat500Model::startTune(bool saveMatch)
{
    m_tuneCompleted = false;
    return requestValue(saveMatch ? "T;" : "FTNS;","TP;",[this](const Status& s) { return s.tuning == true || m_tuneCompleted; },QStringLiteral("Request tuner search"));
}
bool Kat500Model::cancelTune()
{
    if (!m_connected || !m_detected || m_status.identity != QStringLiteral("KAT500") || operationBusy() || m_now() < m_quarantineUntil) { return false; }
    ++m_generation; m_operation = QStringLiteral("Cancel tuner search"); m_operationWriting = true;
    m_queue.clear(); m_cancelTuneDeadline = m_now() + 10000; emit operationBusyChanged(true);
    enqueue({"CT;TP;",[](const QByteArray& frame) { return matchesStatusQuery("TP;",frame); },
        [this](const QByteArray&) { pollTuneCancellation(); },2000,true});
    return true;
}
bool Kat500Model::recallFrequency(int frequencyKhz)
{
    return requestValue(buildFrequency(frequencyKhz),"F;",[frequencyKhz](const Status& s) { return s.frequencyKhz == frequencyKhz; },QStringLiteral("Recall frequency"));
}
void Kat500Model::setEffectiveTxFrequencyHz(qint64 frequencyHz)
{
    if (!m_followTx || frequencyHz < 1000 || frequencyHz > 99999000 || !unavailableReason(true).isEmpty()) { return; }
    const int khz = static_cast<int>(frequencyHz / 1000);
    if (m_status.frequencyKhz != khz) { recallFrequency(khz); }
}
void Kat500Model::pollTuneCancellation()
{
    if (!m_connected || !operationBusy()) { return; }
    if (m_status.tuning == false) { finish(QStringLiteral("Tuner search cancellation confirmed.")); return; }
    if (m_now() >= m_cancelTuneDeadline) { fail(QStringLiteral("Tuner has not confirmed cancellation; CT was not retried.")); return; }
    query("TP;",[this] { pollTuneCancellation(); });
}
bool Kat500Model::memorize(int frequencyKhz)
{
    if (frequencyKhz < 0 || frequencyKhz > 99999 || !begin(QStringLiteral("Memorize match"),true)) { return false; }
    if (m_status.firmware != QStringLiteral("02.16")) { fail(QStringLiteral("Memory verification is supported for firmware 02.16.")); return false; }
    preflight([this,frequencyKhz] {
        query("AN;"); query("BYP;"); query("SIDE;"); query("C;");
        query("L;",[this,frequencyKhz] {
            MemoryMatch expected;
            expected.antenna = *m_status.antenna; expected.bypassed = *m_status.bypassed;
            expected.side = m_status.side; expected.capacitorMask = m_status.capacitorMask; expected.inductorMask = m_status.inductorMask;
            m_memoryExpected = expected;
            requestMemory(frequencyKhz,frequencyKhz ? "SM " + QByteArray::number(frequencyKhz) + ';' : QByteArray("SM;"));
        });
    });
    return true;
}
void Kat500Model::preflight(std::function<void()> ready)
{
    if (!m_connected || !operationBusy()) { return; }
    query("I;"); query("RV;"); query("SN;"); query("PS;"); query("TP;");
    query("VFWD;",[this,ready = std::move(ready)] {
        if (m_status.identity != QStringLiteral("KAT500") || m_status.firmware != m_operationFirmware || m_status.serial != m_operationSerial ||
            m_status.powerOn != true || m_status.tuning != false || m_status.forwardAdc != 0 || m_status.serial.isEmpty()) {
            fail(QStringLiteral("Fresh identity, power, idle or no-RF preflight failed.")); return;
        }
        ready();
    });
}
bool Kat500Model::readConfiguration()
{
    if (!begin(QStringLiteral("Read configuration"),false)) { return false; }
    if (m_status.firmware != QStringLiteral("02.16")) { fail(QStringLiteral("The complete configuration schema is verified for firmware 02.16.")); return false; }
    m_configuration.clear(); m_total = configurationKeys().size();
    for (const SettingKey key : configurationKeys()) {
        enqueue({configurationQuery(key),[key](const QByteArray& frame) { return configurationValue(key,frame).has_value(); },
            [this,key](const QByteArray& frame) {
                m_configuration[key] = *configurationValue(key,frame); emit operationProgress(++m_progress,m_total);
                if (m_progress == m_total && operationBusy()) { const Configuration result = m_configuration; finish(QStringLiteral("Configuration read.")); emit configurationRead(result); }
            }});
    }
    return true;
}
bool Kat500Model::applyConfiguration(const Configuration& changedFields)
{
    const QString invalid = validateConfiguration(m_configuration,changedFields);
    if (!invalid.isEmpty()) { emit operationFailed(invalid); return false; }
    if (!begin(QStringLiteral("Apply configuration"),true)) { return false; }
    // Enable antennas before preference changes; disable old antennas last.
    QList<QPair<SettingKey,double>> edits;
    for (auto it = changedFields.cbegin(); it != changedFields.cend(); ++it) {
        if (m_configuration.value(it.key()) != it.value()) { edits.append({it.key(),it.value()}); }
    }
    auto order = [](const QPair<SettingKey,double>& edit) {
        return edit.first.setting == Setting::EnabledAntenna ? (edit.second == 1 ? 0 : 2) : 1;
    };
    std::stable_sort(edits.begin(),edits.end(),[&](const auto& a,const auto& b) { return order(a) < order(b); });
    m_edits.clear(); for (const auto& edit : edits) { m_edits.enqueue(edit); }
    m_total = m_edits.size();
    preflight([this] { applyNextSetting(); }); return true;
}
void Kat500Model::applyNextSetting()
{
    if (!m_connected || !operationBusy()) { return; }
    if (m_edits.isEmpty()) { const Configuration result = m_configuration; finish(QStringLiteral("All changed settings confirmed by readback.")); emit configurationRead(result); return; }
    if (fieldAgeMs(QStringLiteral("VFWD")) > 1000 || fieldAgeMs(QStringLiteral("TP")) > 1000) {
        preflight([this] { applyNextSetting(); }); return;
    }
    const auto edit = m_edits.dequeue(); const SettingKey key = edit.first; const double requested = edit.second;
    const QByteArray wire = configurationSet(key,requested) + configurationQuery(key);
    enqueue({wire,[key](const QByteArray& frame) { return configurationValue(key,frame).has_value(); },
        [this,key,requested](const QByteArray& frame) {
            const double actual = *configurationValue(key,frame); m_configuration[key] = actual; emit configurationRead(m_configuration);
            const bool swr = key.setting >= Setting::AutoSwr && key.setting <= Setting::KeySwr;
            if (std::abs(actual-requested) > (swr ? 0.011 : 0.0)) {
                fail(QStringLiteral("%1 readback differs; %2 settings confirmed. Remaining edits discarded.").arg(settingName(key.setting)).arg(m_progress)); return;
            }
            emit operationProgress(++m_progress,m_total); applyNextSetting();
        },2000,true});
}
bool Kat500Model::optimizeForKpa500() { return applyConfiguration({{{Setting::AmpKeyPower},1500}}); }
void Kat500Model::readRegion(bool external, int address, int end, std::function<void(QByteArray)> complete)
{
    m_regionExternal = external; m_regionAddress = address; m_regionEnd = end;
    m_regionData.clear(); m_regionComplete = std::move(complete); readRegionBlock();
}
void Kat500Model::readRegionBlock()
{
    if (!m_connected || !operationBusy()) { return; }
    if (m_regionAddress >= m_regionEnd) {
        auto complete = std::move(m_regionComplete); QByteArray data = std::move(m_regionData);
        if (complete) { complete(data); } return;
    }
    const int address = m_regionAddress; const int length = std::min(64,m_regionEnd-address); const bool external = m_regionExternal;
    enqueue({buildEepromRead(external,address,length),[external,address,length](const QByteArray& frame) {
        const auto reply = parseEepromReply(frame);
        return reply && !reply->writeAck && reply->external == external && reply->address == address && reply->length == length;
    },[this,length](const QByteArray& frame) {
        m_regionData += parseEepromReply(frame)->data; m_regionAddress += length;
        m_progress += length; emit operationProgress(m_progress,m_total); readRegionBlock();
    }});
}
bool Kat500Model::readBackup()
{
    if (!begin(QStringLiteral("Read complete backup"),false)) { return false; }
    if (m_status.firmware != QStringLiteral("02.16")) { fail(QStringLiteral("Complete EEPROM support is verified only for firmware 02.16.")); return false; }
    m_backup = {}; m_total = 1024 + 65536;
    preflight([this] {
        m_backup.serial = m_status.serial; m_backup.firmware = m_status.firmware;
        readRegion(false,0,1024,[this](QByteArray data) {
            m_backup.internal = std::move(data);
            readRegion(true,0,65536,[this](QByteArray data) {
                m_backup.external = std::move(data);
                preflight([this] { const Backup result = m_backup; finish(QStringLiteral("Complete backup read, including fault history.")); emit backupRead(result); });
            });
        });
    }); return true;
}
bool Kat500Model::restoreBackup(const Backup& backup, bool confirmed)
{
    if (!confirmed) { emit operationFailed(QStringLiteral("Explicit restore confirmation is required.")); return false; }
    if (backup.internal.size() != 1024 || backup.external.size() != 65536 || backup.serial != m_status.serial ||
        backup.firmware != QStringLiteral("02.16") || m_status.firmware != backup.firmware || encodeBackupFile(backup).isEmpty()) {
        emit operationFailed(QStringLiteral("Restore needs a complete firmware 02.16 backup matching this tuner serial and firmware.")); return false;
    }
    if (!begin(QStringLiteral("Restore and verify backup"),true)) { return false; }
    m_restore = backup; m_restoreExternal = false; m_restoreAddress = 32; m_total = 992 + 61440;
    preflight([this] { restoreBlock(); }); return true;
}
void Kat500Model::restoreBlock()
{
    if (!m_connected || !operationBusy()) { return; }
    const int end = m_restoreExternal ? 65536 : 1024;
    if (m_restoreAddress >= end) { restoreNextRegion(); return; }
    if (fieldAgeMs(QStringLiteral("VFWD")) > 1000 || fieldAgeMs(QStringLiteral("TP")) > 1000) {
        preflight([this] { restoreBlock(); }); return;
    }
    const int address = m_restoreAddress; const bool external = m_restoreExternal;
    const QByteArray data = (external ? m_restore.external : m_restore.internal).mid(address,std::min(32,end-address));
    const int length = data.size();
    enqueue({buildEepromWrite(external,address,data),[external,address,length](const QByteArray& frame) {
        const auto reply = parseEepromReply(frame); return reply && reply->writeAck && reply->external == external && reply->address == address && reply->length == length;
    },[this,external,address,data,length](const QByteArray&) {
        enqueue({buildEepromRead(external,address,length),[external,address,length](const QByteArray& frame) {
            const auto reply = parseEepromReply(frame); return reply && !reply->writeAck && reply->external == external && reply->address == address && reply->length == length;
        },[this,data,length](const QByteArray& frame) {
            if (parseEepromReply(frame)->data != data) { fail(QStringLiteral("EEPROM readback mismatch. Restore stopped without retry or restart.")); return; }
            m_restoreAddress += length; m_progress += length; emit operationProgress(m_progress,m_total); restoreBlock();
        }});
    },3000,true});
}
void Kat500Model::restoreNextRegion()
{
    if (!m_connected || !operationBusy()) { return; }
    if (!m_restoreExternal) { m_restoreExternal = true; m_restoreAddress = 4096; restoreBlock(); }
    else {
        m_configuration.clear();
        finish(QStringLiteral("Restore verified. Identity bytes and fault history preserved. Power cycle manually to load restored settings."));
    }
}
bool Kat500Model::readFaultHistory()
{
    if (!begin(QStringLiteral("Read fault history"),false)) { return false; }
    if (m_status.firmware != QStringLiteral("02.16")) { fail(QStringLiteral("Fault-history decoding is verified only for firmware 02.16.")); return false; }
    m_total = 4097;
    preflight([this] {
        readRegion(false,2,3,[this](QByteArray pointer) {
            const int slot = static_cast<quint8>(pointer[0]);
            readRegion(true,0,4096,[this,slot](QByteArray data) {
                m_faults = decodeFaultHistory(slot,data); const QList<FaultRecord> result = m_faults;
                finish(QStringLiteral("Fault history read; current fault unchanged.")); emit faultHistoryRead(result);
            });
        });
    }); return true;
}
bool Kat500Model::inspectMemories(int frequencyKhz)
{
    if (frequencyKhz < 0 || frequencyKhz > 99999 || !begin(QStringLiteral("Inspect memory bin"),false)) { return false; }
    if (m_status.firmware != QStringLiteral("02.16")) { fail(QStringLiteral("Memory response decoding is verified only for firmware 02.16.")); return false; }
    m_memoryExpected.reset(); requestMemory(frequencyKhz); return true;
}
void Kat500Model::requestMemory(int frequencyKhz, const QByteArray& prefix)
{
    if (!m_connected || !operationBusy()) { return; }
    m_memoryFrames.clear();
    const QByteArray command = frequencyKhz ? "DM" + QByteArray::number(frequencyKhz) + ';' : QByteArray("DM;");
    enqueue({prefix + command,[](const QByteArray& frame) { return frame.startsWith("DM "); },{},4000,!prefix.isEmpty()});
}
bool Kat500Model::eraseMemories(int band, int antenna, bool confirmed)
{
    if (!confirmed || band < -1 || band > 10 || antenna < 0 || antenna > 3) { return false; }
    if (!begin(QStringLiteral("Erase selected memories"),true)) { return false; }
    m_eraseBands.clear(); m_eraseAntenna = antenna;
    if (band == -1) { for (int i = 0; i < 11; ++i) { m_eraseBands.enqueue(i); } }
    else { m_eraseBands.enqueue(band); }
    m_total = m_eraseBands.size(); preflight([this] { eraseNextBand(); }); return true;
}
void Kat500Model::eraseNextBand()
{
    if (!m_connected || !operationBusy()) { return; }
    if (m_eraseBands.isEmpty()) { finish(QStringLiteral("Erase commands processed. Inspect affected memory bins to verify contents.")); return; }
    const int band = m_eraseBands.dequeue();
    enqueue({buildErase(band,m_eraseAntenna) + "RV;",[](const QByteArray& frame) { return matchesStatusQuery("RV;",frame); },[this](const QByteArray&) {
        emit operationProgress(++m_progress,m_total);
        if (m_eraseBands.isEmpty()) { eraseNextBand(); } else { preflight([this] { eraseNextBand(); }); }
    },6000,true});
}
bool Kat500Model::factoryReset(bool confirmed)
{
    if (!confirmed || !begin(QStringLiteral("Factory reset"),true)) { return false; }
    preflight([this] {
        // Utility 1.23.11.9 waits several seconds between EEINIT and RST0.
        // This restart is part of the explicitly confirmed factory reset.
        if (!write("EEINIT;")) { return; }
        m_resetAt = m_now() + 4000;
    }); return true;
}
QByteArray Kat500Model::encodeBackupFile(const Backup& backup) { return encodeBackup(backup,QDateTime::currentDateTime()); }
std::optional<Backup> Kat500Model::decodeBackupFile(const QByteArray& data, QString* error) { return decodeBackup(data,error); }
bool Kat500Model::saveBackupFile(const QString& path, const Backup& backup, QString* error) { return saveBackup(path,backup,error); }
QByteArray Kat500Model::faultsCsv(const QList<FaultRecord>& faults) { return faultHistoryCsv(faults); }
} // namespace AetherSDR
