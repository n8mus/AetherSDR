// N8EM fork: OrionBackend end to end against a fake Orion on a pty.
//
// The backend opens the pty's slave exactly as it would /dev/orion; the test
// plays the radio on the master side, answering the queries the backend
// polls and recording every set command. No hardware, and the fake never
// keys: the test fails if the backend ever writes a transmit command.

#include "core/backends/orion/OrionBackend.h"
#include "core/TxCoordinator.h"

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
    bool aLocked = false;
    bool bLocked = false;
    bool keyed = false;
    int txPower = 50;
    int micGain = 51;
    int procLevel = 2;
    int auxGain = 0;

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
            // Keyed, the Orion answers with its TX meter (the PTT readback).
            if (keyed) {
                reply("@STF040R002S384");   // 40 W fwd, 2 W ref, SWR 1.5
            } else {
                reply("@SRM" + QByteArray::number(sMain) + "S" + QByteArray::number(sSub));
            }
        } else if (q == "TP") {
            reply("@TP" + QByteArray::number(txPower));
        } else if (q == "TM") {
            reply("@TM" + QByteArray::number(micGain));
        } else if (q == "TS") {
            reply("@TS" + QByteArray::number(procLevel));
        } else if (q == "TI") {
            reply("@TI" + QByteArray::number(auxGain));
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
        } else if (q == "AL") {
            reply(aLocked ? "@AL" : "@AU");
        } else if (q == "BL") {
            reply(bLocked ? "@BL" : "@BU");
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
        } else if (cmd.startsWith("*KV") && cmd.size() == 6) {
            kv = cmd.mid(3);
        } else if (cmd == "*TK") {
            keyed = true;    // nothing radiates: this is a pty
        } else if (cmd == "*TU") {
            keyed = false;
        } else if (cmd.startsWith("*TP")) {
            txPower = cmd.mid(3).toInt();
        } else if (cmd.startsWith("*TM")) {
            micGain = cmd.mid(3).toInt();
        } else if (cmd.startsWith("*TS")) {
            procLevel = cmd.mid(3).toInt();
        } else if (cmd.startsWith("*TI")) {
            auxGain = cmd.mid(3).toInt();
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
    std::optional<bool> locked;
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();
    qRegisterMetaType<TransmitDelta>();

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
                         if (d.locked) v.locked = d.locked;
                     });
    QObject::connect(&backend, &IRadioBackend::meterUpdate,
                     [&](const QString& id, double value) { meters[id] = value; });
    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    QSignalSpy disconnectedSpy(&backend, &IRadioBackend::disconnected);
    QSignalSpy errorSpy(&backend, &IRadioBackend::connectionError);

    std::printf("-- connect\n");
    RadioConnectRequest req;
    req.serial = QStringLiteral("orion:") + radio.slavePath();
    // Never touch the station's RSP2 or the SignaLink from a unit test: the
    // SignaLink can key a real radio by itself when audio reaches it.
    req.params.insert(QStringLiteral("orion.sdr"), false);
    req.params.insert(QStringLiteral("orion.tx.audioDevice"), QStringLiteral("none"));
    req.params.insert(QStringLiteral("orion.tx.maxKeyMs"), 1500);
    backend.connectRadio(req);
    check(waitFor([&] { return connectedSpy.count() == 1; }, 2000),
          "connected() after the first CAT reply");
    check(backend.capabilities().family == QLatin1String("orion"), "family is orion");
    check(backend.capabilities().canTransmit, "the Orion can transmit (step 6)");
    check(!backend.capabilities().hostModulates && backend.capabilities().takesTxAudioOverSeam,
          "the radio modulates; PC audio crosses the seam");
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

    std::printf("-- split: TX to B parks B above A (the operator's habit)\n");
    // Mode is LSB from the section above: voice, so B parks A + 5 kHz.
    radio.sets.clear();
    const qint64 aBefore = radio.afHz;
    SliceControlRequest toB;
    toB.field = SliceControlRequest::Field::TxSlice;
    toB.value = true;
    check(backend.requestSliceControl(1, toB) == ReceiveDispatch::Dispatched,
          "TX-slice request is accepted (not refused as before)");
    const QByteArray park = "*BF" + QByteArray::number(aBefore + 5000);
    check(waitFor([&] { return radio.sets.contains(park) && radio.sets.contains("*KVABB"); }, 1000),
          "B parks at A + 5 kHz, then *KVABB routes TX to B");
    check(radio.sets.indexOf(park) < radio.sets.indexOf("*KVABB"),
          "B is parked before the routing changes");
    check(waitFor([&] { return slices[1].txSlice == true && slices[0].txSlice == false; }, 2000),
          "TX badge moves to slice B and stays after the radio's readback");
    check(waitFor([&] { return slices[1].mhz
                               && std::fabs(*slices[1].mhz * 1e6 - (aBefore + 5000)) < 1.0; }, 2000),
          "slice B shows the parked frequency");

    std::printf("-- split off: TX back to A, B is left alone\n");
    radio.sets.clear();
    backend.requestSliceControl(0, toB);
    check(waitFor([&] { return radio.sets.contains("*KVABA"); }, 1000), "*KVABA routes TX back to A");
    bool movedB = false;
    for (const QByteArray& c : radio.sets) {
        if (c.startsWith("*BF")) movedB = true;
    }
    check(!movedB, "moving TX back to A does not touch B");

    std::printf("-- a locked B is never parked or tuned\n");
    radio.bLocked = true;
    check(waitFor([&] { return slices[1].locked == true; }, 10000), "B's front-panel lock is read within a poll lap");
    radio.sets.clear();
    backend.requestSliceControl(1, toB);
    check(waitFor([&] { return radio.sets.contains("*KVABB"); }, 1000), "TX still moves to B");
    backend.setSliceFrequency(1, 7'200'000.0);
    QElapsedTimer quiet;
    quiet.start();
    while (quiet.elapsed() < 400) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    movedB = false;
    for (const QByteArray& c : radio.sets) {
        if (c.startsWith("*BF")) movedB = true;
    }
    check(!movedB, "no *BF while B is locked: neither the park nor a tune");
    backend.requestSliceControl(0, toB);
    radio.bLocked = false;

    std::printf("-- a key without an admitted operation is refused\n");
    radio.sets.clear();
    backend.setKeying(true, {}, {});
    QElapsedTimer guard;
    guard.start();
    while (guard.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    check(!radio.sets.contains("*TK"), "no *TK without an operation from the TX coordinator");

    // An admitted operation, the way the engine's TX gate hands one out.
    TxCoordinator coordinator([](const TxCoordinator::Operation&, TxCoordinator::StopReason) {});
    const auto admission = coordinator.acquire(coordinator.registerActor({true, 0}),
                                               TxCoordinator::monotonicMs());
    check(admission.accepted(), "test operation admitted");
    const TxCoordinator::Operation op = admission.operation;

    QSignalSpy txSpy(&backend, &IRadioBackend::transmitChanged);
    QSignalSpy confirmSpy(&backend, &IRadioBackend::keyingStateConfirmed);
    const auto health = [&](const char* key) {
        return backend.healthSnapshot().values.value(QString::fromLatin1(key));
    };
    const auto sawMox = [&](bool on) {
        for (const auto& args : txSpy) {
            const auto d = args.at(0).value<TransmitDelta>();
            if (d.mox && *d.mox == on) return true;
        }
        return false;
    };
    QByteArray block(480 * 2 * 2, '\0');   // 20 ms of 24 kHz int16 stereo
    for (int i = 0; i < 480; ++i) {
        reinterpret_cast<qint16*>(block.data())[2 * i] = 8000;
        reinterpret_cast<qint16*>(block.data())[2 * i + 1] = 8000;
    }

    std::printf("-- the PC path\n");
    radio.sets.clear();
    TransmitControlRequest pc;
    pc.field = TransmitControlRequest::Field::MicInput;
    pc.value = QStringLiteral("PC");
    check(backend.requestTransmitControl(pc) == ReceiveDispatch::Dispatched, "PC path accepted");
    check(waitFor([&] { return radio.sets.contains("*TM0") && radio.sets.contains("*TS0")
                               && radio.sets.contains("*TI100"); }, 1000),
          "PC path: radio mic 0, processor 0, aux 100 (the reference DIG profile)");
    const auto txAudioWritten = [&] { return health("txAudioWritten").toULongLong(); };
    const quint64 before = txAudioWritten();
    backend.submitTxAudio(block, 24000, TxAudioSource::Microphone, {});
    check(txAudioWritten() == before, "unkeyed: not one sample reaches the SignaLink");

    std::printf("-- key, readback, audio, unkey\n");
    txSpy.clear();
    confirmSpy.clear();
    radio.sets.clear();
    backend.setKeying(true, op, {});
    check(waitFor([&] { return radio.sets.contains("*TK"); }, 500), "*TK keys the (fake) radio");
    check(waitFor([&] { return sawMox(true); }, 3000), "MOX comes from the radio's @STF readback");
    check(confirmSpy.count() >= 1 && confirmSpy.last().at(0).toBool(),
          "keyingStateConfirmed(true) from the readback");
    check(waitFor([&] { return meters.count(QStringLiteral("TX:FWDPWR"))
                               && meters[QStringLiteral("TX:FWDPWR")] == 40.0; }, 2000),
          "TX:FWDPWR 40 W from @STF040R002S384");
    check(std::fabs(meters[QStringLiteral("TX:SWR")] - 1.5) < 1e-9, "TX:SWR 1.5");
    backend.submitTxAudio(block, 24000, TxAudioSource::Microphone, {});
    check(txAudioWritten() == before + 960, "keyed on PC: 20 ms at 24 kHz -> 960 samples at 48 kHz");
    check(backend.finishTxAudio({}) >= OrionBackend::kTxAudioTailMs, "finishTxAudio holds PTT for the tail");
    radio.sets.clear();
    backend.setKeying(false, op, {});
    check(waitFor([&] { return radio.sets.contains("*TU"); }, 500), "*TU unkeys");
    const quint64 afterUnkey = txAudioWritten();
    backend.submitTxAudio(block, 24000, TxAudioSource::Microphone, {});
    check(txAudioWritten() == afterUnkey, "after unkey: nothing more to the SignaLink");
    check(waitFor([&] { return sawMox(false); }, 4000), "MOX off once the radio is back in RX");

    std::printf("-- the radio-mic path sends no PC audio\n");
    TransmitControlRequest mic;
    mic.field = TransmitControlRequest::Field::MicInput;
    mic.value = QStringLiteral("MIC");
    radio.sets.clear();
    backend.requestTransmitControl(mic);
    check(waitFor([&] { return radio.sets.contains("*TI0") && radio.sets.contains("*TM51")
                               && radio.sets.contains("*TS2"); }, 1000),
          "MIC path: aux 0, the radio's own mic 51 and processor 2 back");
    backend.setKeying(true, op, {});
    check(waitFor([&] { return sawMox(true); }, 3000), "keyed on MIC");
    const quint64 micBefore = txAudioWritten();
    backend.submitTxAudio(block, 24000, TxAudioSource::Microphone, {});
    check(txAudioWritten() == micBefore, "MIC path: host audio is never played to the SignaLink");
    backend.setKeying(false, op, {});
    check(waitFor([&] { return !radio.keyed; }, 500), "unkeyed");

    std::printf("-- failsafe\n");
    radio.sets.clear();
    backend.setKeying(true, op, {});
    check(waitFor([&] { return radio.keyed; }, 500), "keyed and left keyed");
    check(waitFor([&] { return !radio.keyed && radio.sets.contains("*TU"); }, 4000),
          "the failsafe unkeys a transmission nobody released");

    std::printf("-- disconnect while keyed on PC\n");
    backend.requestTransmitControl(pc);
    backend.setKeying(true, op, {});
    check(waitFor([&] { return radio.keyed && radio.auxGain == 100; }, 1000), "keyed on the PC path");
    radio.sets.clear();
    backend.disconnectRadio();
    check(waitFor([&] { return !radio.keyed; }, 500), "disconnect unkeys first");
    check(waitFor([&] { return radio.auxGain == 0 && radio.micGain == 51 && radio.procLevel == 2; }, 500),
          "and gives the radio its own mic back (stand-alone rule)");
    check(radio.sets.indexOf("*TU") >= 0 && radio.sets.indexOf("*TU") < radio.sets.indexOf("*TI0"),
          "unkey before the profile change");
    // Reconnect for the remaining sections.
    disconnectedSpy.clear();
    backend.connectRadio(req);
    check(waitFor([&] { return backend.isConnected(); }, 2000), "reconnected");

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
