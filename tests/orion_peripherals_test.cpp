// N8EM fork: the Orion backend's station peripherals (step 7), against fakes.
//
// A fake WinKeyer and a fake DCU-3 on ptys, rotctld and cwdaemon on loopback
// ports the OS picks, and Pat's websocket messages fed by hand. Nothing here
// opens the station's real WinKeyer, rotor or service ports, and nothing can
// key: the fake keyer only records the bytes it would have sent.

#include "core/backends/orion/OrionBackend.h"
#include "core/backends/orion/OrionCwDaemon.h"
#include "core/backends/orion/OrionPatMonitor.h"
#include "core/backends/orion/OrionRotor.h"
#include "core/backends/orion/OrionWinKeyer.h"
#include "core/TxCoordinator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSocketNotifier>
#include <QTcpSocket>
#include <QUdpSocket>

#include <cstdio>
#include <functional>
#include <memory>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

using namespace AetherSDR;
using namespace AetherSDR::orion;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("  ok: %s\n", what);
    }
}

static bool waitFor(const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    return true;
}

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
}

// A device on the master side of a pty: every byte the program writes lands
// in `received`, and `onBytes` may answer.
class PtyFake {
public:
    std::function<void(const QByteArray& chunk)> onBytes;
    QByteArray received;

    bool open()
    {
        int slave = -1;
        char name[128] = {};
        if (openpty(&m_master, &slave, name, nullptr, nullptr) != 0) {
            return false;
        }
        termios raw{};
        tcgetattr(slave, &raw);
        cfmakeraw(&raw);
        tcsetattr(slave, TCSANOW, &raw);
        m_slave = slave;
        m_path = QString::fromLatin1(name);
        fcntl(m_master, F_SETFL, fcntl(m_master, F_GETFL) | O_NONBLOCK);
        m_notifier = std::make_unique<QSocketNotifier>(m_master, QSocketNotifier::Read);
        QObject::connect(m_notifier.get(), &QSocketNotifier::activated, [this] {
            char buf[512];
            for (;;) {
                const ssize_t n = ::read(m_master, buf, sizeof buf);
                if (n <= 0) {
                    break;
                }
                const QByteArray chunk(buf, static_cast<qsizetype>(n));
                received += chunk;
                if (onBytes) {
                    onBytes(chunk);
                }
            }
            // Keep the client's exclusive-mode lock from outliving it.
            ::ioctl(m_slave, TIOCNXCL);
        });
        return true;
    }
    void reply(const QByteArray& bytes) { (void)::write(m_master, bytes.constData(), static_cast<size_t>(bytes.size())); }
    QString path() const { return m_path; }
    ~PtyFake()
    {
        m_notifier.reset();
        if (m_master >= 0) ::close(m_master);
        if (m_slave >= 0) ::close(m_slave);
    }

private:
    int m_master = -1;
    int m_slave = -1;
    QString m_path;
    std::unique_ptr<QSocketNotifier> m_notifier;
};

// A WinKeyer that answers the echo test and host open (firmware 31 = WK3).
static void makeKeyer(PtyFake& k)
{
    k.onBytes = [&k](const QByteArray& chunk) {
        if (chunk.contains(QByteArray("\x00\x04\x14", 3))) {
            k.reply(QByteArray(1, '\x14'));
        }
        if (chunk.contains(QByteArray("\x00\x02", 2))) {
            k.reply(QByteArray(1, static_cast<char>(31)));
        }
    };
}

static void testWinKeyer()
{
    std::printf("-- WinKeyer\n");
    check(OrionWinKeyer::filterText("cq de n8em!", false) == "CQ DE N8EM",
          "text upper-cased; '!' (null on the keyer) dropped, not sent");
    check(OrionWinKeyer::filterText("ok[", false) == "OK", "'[' (AS) blocked on WK2");
    check(OrionWinKeyer::filterText("ok[", true) == "OK[", "'[' (AS) passed on WK3");

    PtyFake fake;
    if (!fake.open()) { check(false, "pty"); return; }
    makeKeyer(fake);
    OrionWinKeyer keyer;
    QSignalSpy ready(&keyer, &OrionWinKeyer::ready);
    keyer.open(fake.path());
    check(waitFor([&] { return ready.count() == 1; }, 4000), "open sequence: echo, host open, ready");
    check(keyer.firmwareRevision() == 31 && keyer.isWk3(), "firmware 31 read from host open: WK3");
    check(fake.received.startsWith(QByteArray("\x13\x13\x13", 3)), "wakes with three 0x13");
    fake.received.clear();

    keyer.setSpeed(28);
    check(waitFor([&] { return fake.received.contains(QByteArray("\x02\x1c", 2)); }, 500),
          "speed 28 -> 02 1C");
    fake.received.clear();
    check(keyer.sendText("cq test") == "CQ TEST", "text queued");
    check(waitFor([&] { return fake.received.contains("CQ TEST"); }, 500), "text reaches the keyer");
    keyer.sendProsign('s', 'k');
    check(waitFor([&] { return fake.received.contains(QByteArray("\x1bSK", 3)); }, 500),
          "prosign SK as a 0x1B merge");

    QSignalSpy busy(&keyer, &OrionWinKeyer::busyChanged);
    fake.reply(QByteArray(1, static_cast<char>(0xC4)));   // busy
    fake.reply(QByteArray(1, static_cast<char>(0xC0)));   // idle
    check(waitFor([&] { return busy.count() >= 1 && !busy.last().at(0).toBool(); }, 1000),
          "status bytes: busy then idle");
    QSignalSpy pot(&keyer, &OrionWinKeyer::speedPotChanged);
    fake.reply(QByteArray(1, static_cast<char>(0x80 | 15)));   // pot: min 7 + 15
    check(waitFor([&] { return pot.count() == 1 && pot.last().at(0).toInt() == 22; }, 1500),
          "speed pot 22 WPM after the settle time");

    fake.received.clear();
    keyer.close();
    check(waitFor([&] { return fake.received.contains(QByteArray(1, '\x0a'))
                               && fake.received.contains(QByteArray("\x00\x03", 2)); }, 1000),
          "close: stop (0A) then host close (00 03)");

    PtyFake silent;
    silent.open();
    OrionWinKeyer nobody;
    QSignalSpy failed(&nobody, &OrionWinKeyer::failed);
    nobody.open(silent.path());
    check(waitFor([&] { return failed.count() == 1; }, 6000),
          "a port that never echoes is not taken for a WinKeyer");
}

static void testRotor()
{
    std::printf("-- DCU-3 rotor\n");
    QByteArray b("12");
    check(OrionRotor::parseFrames(b) < 0 && b == "12", "a partial frame waits");
    b += "3;";
    check(OrionRotor::parseFrames(b) == 123.0, "completed by the next chunk");
    b = "123;124;12";
    check(OrionRotor::parseFrames(b) == 124.0 && b == "12", "the last whole frame wins, the tail is kept");
    b = "x5;3;123;";
    check(OrionRotor::parseFrames(b) == 123.0, "stream damage skipped");
    b = "360;";
    check(OrionRotor::parseFrames(b) == 0.0, "360 is north");
    b = "999;";
    check(OrionRotor::parseFrames(b) < 0, "out-of-range heading ignored");

    PtyFake dcu;
    if (!dcu.open()) { check(false, "pty"); return; }
    QElapsedTimer sinceTarget;
    qint64 executeGap = -1;
    dcu.onBytes = [&](const QByteArray& chunk) {
        if (chunk.contains("AI1;")) dcu.reply("045;");
        if (chunk.contains("AP1")) sinceTarget.start();
        if (chunk.contains("AM1;") && sinceTarget.isValid()) executeGap = sinceTarget.elapsed();
    };
    OrionRotor rotor;
    QString error;
    check(rotor.open(dcu.path(), &error), "rotor port opens");
    check(waitFor([&] { return rotor.azimuth() == 45.0 && rotor.isAlive(); }, 2000),
          "heading 45 read from AI1;");

    OrionRotctld server(&rotor);
    check(server.handleLine("p") == "45.00\n0.00\n", "rotctld p: azimuth and elevation");
    check(server.handleLine("+p").contains("Azimuth: 45.00"), "rotctld +p: the labelled form");
    check(server.handleLine("P 180 0").isEmpty(), "rotctld P 180: plain RPRT 0 is silent");
    check(waitFor([&] { return dcu.received.contains("AP1180;") && executeGap >= 0; }, 2000),
          "target AP1180; then execute AM1;");
    check(executeGap >= OrionRotor::kExecuteDelayMs - 50, "execute waits ~500 ms after the target");
    check(server.handleLine("P 400 0") == "RPRT -1\n", "rotctld refuses an impossible heading");
    check(server.handleLine("frobnicate") == "RPRT -11\n", "unknown command: RPRT -11");
    check(server.handleLine("\\dump_state").contains("406"), "dump_state names the DCU model");

    // Over a real loopback socket, the way cqrlog talks to it.
    check(server.listen(0, &error), "rotctld listens on loopback");
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.port());
    check(client.waitForConnected(1000), "a client connects");
    client.write("p\n");
    check(waitFor([&] { return client.bytesAvailable() > 0 || client.waitForReadyRead(20); }, 1000)
              && client.readAll() == "45.00\n0.00\n",
          "p over TCP");

    rotor.close();
    check(server.handleLine("p") == "RPRT -5\n", "no reading: RPRT -5, never a fake 0.0");
}

static void testCwDaemon()
{
    std::printf("-- cwdaemon\n");
    const auto pieces = OrionCwDaemon::decode("CQ*TEST");
    check(pieces.size() == 3, "CQ, AR, TEST");
    check(pieces.size() == 3 && std::get<OrionCwDaemon::Text>(pieces[0]).text == "CQ"
              && std::get<OrionCwDaemon::Prosign>(pieces[1]).a == 'A'
              && std::get<OrionCwDaemon::Prosign>(pieces[1]).b == 'R',
          "cwdaemon '*' is AR, sent as a merge (it is null on the keyer)");
    const auto sk = OrionCwDaemon::decode("TU<");
    check(sk.size() == 2 && std::get<OrionCwDaemon::Prosign>(sk[1]).a == 'S',
          "cwdaemon '<' is SK (the keyer would send AR)");
    const auto speed = OrionCwDaemon::decode("\x1b" "225");
    check(speed.size() == 1 && std::get<OrionCwDaemon::Speed>(speed[0]).wpm == 25, "ESC 2 25: speed");
    check(OrionCwDaemon::decode("\x1b" "299").empty(), "out-of-range speed ignored");
    const auto stop = OrionCwDaemon::decode("\x1b" "4");
    check(stop.size() == 1 && std::holds_alternative<OrionCwDaemon::Stop>(stop[0]), "ESC 4: stop");
}

static void testPat()
{
    std::printf("-- Pat monitor\n");
    OrionPatMonitor pat;
    QSignalSpy session(&pat, &OrionPatMonitor::sessionChanged);
    pat.handleMessage(R"({"Status":{"dialing":true,"connected":false,"remote_addr":"W1AW-10"}})");
    check(session.count() == 1 && session.last().at(0).toBool()
              && session.last().at(1).toString() == "W1AW-10",
          "dialing starts a session, with its peer");
    pat.handleMessage(R"({"Status":{"dialing":false,"connected":true}})");
    check(session.count() == 1, "dialing -> connected is the same session");
    pat.handleMessage(R"({"Progress":{"bytes_transferred":10}})");
    check(session.count() == 1, "progress messages change nothing");
    pat.handleMessage(R"({"Status":{"dialing":false,"connected":false}})");
    check(session.count() == 2 && !session.last().at(0).toBool(), "session over");
}

// The backend: CW text needs the TX gate, cwdaemon reaches the keyer, and
// every peripheral is released on disconnect.
static void testBackend()
{
    std::printf("-- backend wiring\n");
    PtyFake cat;
    cat.open();
    cat.onBytes = [&cat](const QByteArray& chunk) {
        if (chunk.contains("?AF")) cat.reply("@AF14025000\r");
        if (chunk.contains("?S")) cat.reply("@SRM94S10\r");
    };
    PtyFake keyerPty;
    keyerPty.open();
    makeKeyer(keyerPty);
    PtyFake dcu;
    dcu.open();
    dcu.onBytes = [&dcu](const QByteArray& chunk) {
        if (chunk.contains("AI1;")) dcu.reply("270;");
    };

    OrionBackend backend;
    RadioConnectRequest req;
    req.serial = "orion:" + cat.path();
    req.params.insert("orion.sdr", false);
    req.params.insert("orion.tx.audioDevice", "none");
    req.params.insert("orion.winkeyer.device", keyerPty.path());
    req.params.insert("orion.rotor.device", dcu.path());
    req.params.insert("orion.rotor.port", 0);
    req.params.insert("orion.cwdaemon.port", 0);
    req.params.insert("orion.pat.url", "none");
    QSignalSpy caps(&backend, &IRadioBackend::capabilitiesChanged);
    backend.connectRadio(req);
    check(waitFor([&] { return backend.isConnected(); }, 2000), "connected to the fake radio");
    check(waitFor([&] { return backend.capabilities().hasRadioSideCwKeyer; }, 4000),
          "CW text keyer offered once the WinKeyer answers");
    check(backend.capabilities().cwTextKeyerName == "WinKeyer", "named WinKeyer");
    check(waitFor([&] { return backend.healthSnapshot().values.value("rotorAzimuth").toDouble() == 270.0; },
                  3000),
          "rotor heading in the health report");

    keyerPty.received.clear();
    const QString refused = backend.sendCwText("TEST", {}, {});
    check(!refused.isEmpty(), "CW text without the TX gate's admission is refused");
    pump(200);
    check(!keyerPty.received.contains("TEST"), "and never reaches the keyer");

    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const auto op = coordinator.acquire(coordinator.registerActor({true, 0}), TxCoordinator::monotonicMs()).operation;
    bool done = false;
    const QString ok = backend.sendCwText("CQ TEST", op, TxCoordinator::Completion([&done] { done = true; }));
    check(ok.isEmpty(), "admitted CW text accepted");
    check(waitFor([&] { return keyerPty.received.contains("CQ TEST"); }, 1000), "and sent to the keyer");
    check(!done, "completion waits for the keyer");
    keyerPty.reply(QByteArray(1, static_cast<char>(0xC4)));
    keyerPty.reply(QByteArray(1, static_cast<char>(0xC0)));
    check(waitFor([&] { return done; }, 1000), "completion when the keyer reports idle");

    // The CWX panel's and CW applet's speed arrive as a transmit control.
    keyerPty.received.clear();
    TransmitControlRequest speed;
    speed.field = TransmitControlRequest::Field::CwSpeed;
    speed.value = 31;
    check(backend.requestTransmitControl(speed) == ReceiveDispatch::Dispatched,
          "CW speed control accepted");
    check(waitFor([&] { return keyerPty.received.contains(QByteArray("\x02\x1f", 2)); }, 500),
          "CW speed 31 reaches the keyer");

    // cwdaemon, over UDP, on the port the backend reports.
    const QString status = backend.healthSnapshot().values.value("cwDaemonStatus").toString();
    const quint16 port = static_cast<quint16>(status.section(':', -1).toUInt());
    check(port != 0, "cwdaemon listening");
    keyerPty.received.clear();
    QUdpSocket udp;
    udp.writeDatagram("CQ*", QHostAddress::LocalHost, port);
    check(waitFor([&] { return keyerPty.received.contains("CQ")
                               && keyerPty.received.contains(QByteArray("\x1b" "AR", 3)); }, 1000),
          "cwdaemon text and its AR prosign reach the keyer");

    keyerPty.received.clear();
    backend.disconnectRadio();
    check(waitFor([&] { return keyerPty.received.contains(QByteArray("\x00\x03", 2)); }, 1000),
          "disconnect gives the keyer back (host close)");
    check(backend.healthSnapshot().values.value("rotctldPort").isNull(), "rotctld closed with the session");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testWinKeyer();
    testRotor();
    testCwDaemon();
    testPat();
    testBackend();
    if (g_failures == 0) {
        std::printf("orion_peripherals_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_peripherals_test: %d failure(s)\n", g_failures);
    return 1;
}
