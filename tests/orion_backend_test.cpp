// N8EM fork: OrionBackend end to end against a fake Orion on a pty.
//
// The backend opens the pty's slave exactly as it would /dev/orion; the test
// plays the radio on the master side, answering the queries the backend
// polls and recording every set command. No hardware, and the fake never
// keys: the test fails if the backend ever writes a transmit command.

#include "core/backends/orion/OrionBackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSocketNotifier>

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

#include <fcntl.h>
#include <pty.h>
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

// The radio side. Holds the state the real Orion would, answers '?' queries
// with '@' replies, applies '*' sets, and logs every set it received.
class FakeOrion {
public:
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
        // Keep the slave open so the master never sees a hang-up between
        // backend sessions.
        m_slaveKeepAlive = slave;
        m_slavePath = QString::fromLatin1(name);
        fcntl(m_master, F_SETFL, fcntl(m_master, F_GETFL) | O_NONBLOCK);
        m_notifier = std::make_unique<QSocketNotifier>(m_master, QSocketNotifier::Read);
        QObject::connect(m_notifier.get(), &QSocketNotifier::activated, [this] { onRead(); });
        return true;
    }
    ~FakeOrion()
    {
        m_notifier.reset();
        if (m_master >= 0) {
            ::close(m_master);
        }
        if (m_slaveKeepAlive >= 0) {
            ::close(m_slaveKeepAlive);
        }
    }

    QString slavePath() const { return m_slavePath; }
    QList<QByteArray> sets;
    bool answering = true;

    qint64 afHz = 14025000;
    qint64 bfHz = 7040000;
    int mainMode = 0;
    int subMode = 0;
    int mainBw = 2400;
    int mainPbt = 0;
    int subBw = 2400;
    int subPbt = 0;
    char mainAgc = 'M';
    int sMain = 94;
    int sSub = 10;
    QByteArray kv = "ABA";

private:
    void reply(const QByteArray& line)
    {
        const QByteArray out = line + '\r';
        (void)::write(m_master, out.constData(), static_cast<size_t>(out.size()));
    }

    void onRead()
    {
        char buf[512];
        for (;;) {
            const ssize_t n = ::read(m_master, buf, sizeof buf);
            if (n <= 0) {
                break;
            }
            m_buffer.append(buf, static_cast<qsizetype>(n));
        }
        for (;;) {
            const qsizetype cr = m_buffer.indexOf('\r');
            if (cr < 0) {
                break;
            }
            const QByteArray cmd = m_buffer.left(cr);
            m_buffer.remove(0, cr + 1);
            handle(cmd);
        }
    }

    void handle(const QByteArray& cmd)
    {
        if (cmd.startsWith('*')) {
            sets << cmd;
            apply(cmd);
            return;
        }
        if (!answering || !cmd.startsWith('?')) {
            return;
        }
        const QByteArray q = cmd.mid(1);
        if (q == "AF") {
            reply("@AF" + QByteArray::number(afHz));
        } else if (q == "BF") {
            reply("@BF" + QByteArray::number(bfHz));
        } else if (q == "S") {
            reply("@SRM" + QByteArray::number(sMain) + "S" + QByteArray::number(sSub));
        } else if (q == "RMM") {
            reply("@RMM" + QByteArray::number(mainMode));
        } else if (q == "RSM") {
            reply("@RSM" + QByteArray::number(subMode));
        } else if (q == "RMF") {
            reply("@RMF" + QByteArray::number(mainBw));
        } else if (q == "RMP") {
            reply("@RMP" + QByteArray::number(mainPbt));
        } else if (q == "RSF") {
            reply("@RSF" + QByteArray::number(subBw));
        } else if (q == "RSP") {
            reply("@RSP" + QByteArray::number(subPbt));
        } else if (q == "RMA") {
            reply(QByteArray("@RMA") + mainAgc);
        } else if (q == "RSA") {
            reply("@RSAS");
        } else if (q == "KV") {
            reply("@KV" + kv);
        } else if (q == "RMG") {
            reply("@RMG80");
        } else if (q == "RMT") {
            reply("@RMT0");
        } else if (q == "RME") {
            reply("@RME1");
        }
    }

    void apply(const QByteArray& cmd)
    {
        bool ok = false;
        if (cmd.startsWith("*AF")) {
            const qint64 v = cmd.mid(3).toLongLong(&ok);
            if (ok) {
                afHz = v;
            }
        } else if (cmd.startsWith("*BF")) {
            const qint64 v = cmd.mid(3).toLongLong(&ok);
            if (ok) {
                bfHz = v;
            }
        } else if (cmd.startsWith("*RMM")) {
            mainMode = cmd.mid(4).toInt();
        } else if (cmd.startsWith("*RMF")) {
            mainBw = cmd.mid(4).toInt();
        } else if (cmd.startsWith("*RMP")) {
            mainPbt = cmd.mid(4).toInt();
        } else if (cmd.startsWith("*RMA") && cmd.size() == 5) {
            mainAgc = cmd[4];
        }
    }

    int m_master = -1;
    int m_slaveKeepAlive = -1;
    QString m_slavePath;
    QByteArray m_buffer;
    std::unique_ptr<QSocketNotifier> m_notifier;
};

struct SliceView {
    std::optional<double> mhz;
    std::optional<QString> mode;
    std::optional<int> low;
    std::optional<int> high;
    std::optional<bool> txSlice;
    std::optional<QString> agc;
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();

    FakeOrion radio;
    if (!radio.open()) {
        std::fprintf(stderr, "FAIL: could not open a pty\n");
        return 1;
    }

    OrionBackend backend;
    std::map<int, SliceView> slices;
    std::map<QString, double> meters;
    QObject::connect(&backend, &IRadioBackend::sliceChanged,
                     [&](int id, const SliceDelta& d) {
                         SliceView& v = slices[id];
                         if (d.frequency) v.mhz = d.frequency;
                         if (d.mode) v.mode = d.mode;
                         if (d.filterLow) v.low = d.filterLow;
                         if (d.filterHigh) v.high = d.filterHigh;
                         if (d.txSlice) v.txSlice = d.txSlice;
                         if (d.agcMode) v.agc = d.agcMode;
                     });
    QObject::connect(&backend, &IRadioBackend::meterUpdate,
                     [&](const QString& id, double value) { meters[id] = value; });
    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    QSignalSpy disconnectedSpy(&backend, &IRadioBackend::disconnected);
    QSignalSpy errorSpy(&backend, &IRadioBackend::connectionError);

    std::printf("-- connect\n");
    RadioConnectRequest req;
    req.serial = QStringLiteral("orion:") + radio.slavePath();
    backend.connectRadio(req);
    check(waitFor([&] { return connectedSpy.count() == 1; }, 2000),
          "connected() after the first CAT reply");
    check(backend.capabilities().family == QLatin1String("orion"), "family is orion");
    check(!backend.capabilities().canTransmit, "step 1 is receive-only");
    check(backend.capabilities().maxSlices == 2, "two receivers");

    check(waitFor([&] {
              return slices[0].mhz && std::fabs(*slices[0].mhz - 14.025) < 1e-9
                     && slices[1].mhz && std::fabs(*slices[1].mhz - 7.040) < 1e-9;
          }, 2000),
          "slice 0 = VFO A 14.025, slice 1 = VFO B 7.040");
    check(waitFor([&] { return slices[0].mode == QStringLiteral("USB"); }, 2000),
          "main mode USB");
    check(waitFor([&] { return slices[0].low == 0 && slices[0].high == 2400; }, 2000),
          "USB 2400/0 publishes 0..2400");
    check(waitFor([&] { return meters.count(QStringLiteral("SLC:LEVEL"))
                               && std::fabs(meters[QStringLiteral("SLC:LEVEL")] - kS9Dbm) < 1e-9; },
                  2000),
          "main S-meter raw 94 = S9 = -73 dBm");
    check(meters.count(QStringLiteral("SLC1:LEVEL")) == 1, "sub S-meter published");
    check(waitFor([&] { return slices[0].txSlice == true && slices[1].txSlice == false; }, 2000),
          "?KV ABA: transmit on A");

    std::printf("-- tune from AetherSDR\n");
    backend.setSliceFrequency(0, 7074000.0);
    check(waitFor([&] { return radio.sets.contains("*AF7074000"); }, 1000),
          "setSliceFrequency writes *AF7074000");
    QElapsedTimer settle;
    settle.start();
    bool snappedBack = false;
    while (settle.elapsed() < 1200) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (slices[0].mhz && std::fabs(*slices[0].mhz - 14.025) < 1e-9) {
            snappedBack = true;
        }
        usleep(2000);
    }
    check(!snappedBack, "no snap-back to the old frequency after tuning");

    std::printf("-- drag coalescing\n");
    radio.sets.clear();
    for (int i = 1; i <= 20; ++i) {
        backend.setSliceFrequency(0, 7074000.0 + i * 10);
    }
    check(waitFor([&] { return radio.sets.contains("*AF7074200"); }, 1000),
          "the last drag position reaches the radio");
    int afWrites = 0;
    for (const QByteArray& s : radio.sets) {
        if (s.startsWith("*AF")) ++afWrites;
    }
    check(afWrites <= 2, "twenty drag steps coalesce into one or two writes");

    std::printf("-- front panel knob\n");
    radio.afHz = 7075500;   // within 100 kHz: accepted on one read
    check(waitFor([&] { return slices[0].mhz && std::fabs(*slices[0].mhz - 7.0755) < 1e-9; },
                  3000),
          "a knob turn on the radio reaches slice 0");
    radio.afHz = 21074000;  // a band jump: needs two agreeing reads
    check(waitFor([&] { return slices[0].mhz && std::fabs(*slices[0].mhz - 21.074) < 1e-9; },
                  3000),
          "a band change on the radio is accepted after two reads");

    std::printf("-- mode and filter\n");
    radio.sets.clear();
    backend.setSliceMode(0, QStringLiteral("LSB"));
    check(waitFor([&] { return radio.sets.contains("*RMM1"); }, 1000), "LSB writes *RMM1");
    backend.setSliceFilter(0, -2600, -200);
    check(waitFor([&] { return radio.sets.contains("*RMF2400") && radio.sets.contains("*RMP200"); },
                  1000),
          "LSB -2600..-200 writes width 2400 and PBT 200");
    check(waitFor([&] { return slices[0].mode == QStringLiteral("LSB"); }, 3000),
          "mode change confirmed by the radio");

    std::printf("-- AGC\n");
    backend.setSliceAgc(0, QStringLiteral("fast"), 50);
    check(waitFor([&] { return radio.sets.contains("*RMAF"); }, 1000), "fast writes *RMAF");
    check(waitFor([&] { return slices[0].agc == QStringLiteral("fast"); }, 2000),
          "AGC read back as fast");

    std::printf("-- the transmit guard\n");
    backend.setKeying(true, {}, {});
    QElapsedTimer guard;
    guard.start();
    while (guard.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    bool keyed = false;
    for (const QByteArray& s : radio.sets) {
        if (s.startsWith("*T")) keyed = true;
    }
    check(!keyed, "setKeying writes nothing: no *T command ever reached the radio");

    std::printf("-- disconnect\n");
    backend.disconnectRadio();
    check(disconnectedSpy.count() == 1, "disconnected() exactly once");
    backend.disconnectRadio();
    check(disconnectedSpy.count() == 1, "a second disconnect emits nothing");
    check(errorSpy.count() == 0, "no connection errors in a clean session");

    std::printf("-- a silent radio\n");
    radio.answering = false;
    const int connectsBefore = connectedSpy.count();
    backend.connectRadio(req);
    check(waitFor([&] { return errorSpy.count() == 1; },
                  OrionBackend::kConnectTimeoutMs + 1500),
          "a radio that never answers reports a connection error");
    check(errorSpy.count() == 1
              && errorSpy.at(0).at(0).toString().contains(QStringLiteral("No reply")),
          "and it is the no-reply error: the port reopened after the last session");
    check(connectedSpy.count() == connectsBefore, "and is never reported connected");
    check(disconnectedSpy.count() == 1, "and emits no disconnected()");

    std::printf("-- a missing device\n");
    RadioConnectRequest missing;
    missing.serial = QStringLiteral("orion:/dev/does-not-exist-orion");
    backend.connectRadio(missing);
    check(errorSpy.count() == 2, "a missing device fails at once");

    if (g_failures == 0) {
        std::printf("orion_backend_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_backend_test: %d failure(s)\n", g_failures);
    return 1;
}
