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
#include <QTimer>

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
    bool tunerIn = false;
    int monitor = 30;
    int sidetone = 40;
    int afMain = 102;   // bytes: 40 %
    int afSub = 51;     // 20 %
    QByteArray txMeter = "@STF040R002S384";   // 40 W fwd, 2 W ref, SWR 1.5
    bool tunerFitted = true;
    int tunerCycles = 0;
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
                reply(txMeter);
            } else {
                reply("@SRM" + QByteArray::number(sMain) + "S" + QByteArray::number(sSub));
            }
        } else if (q == "UM") {
            reply("@UM" + QByteArray::number(afMain));
        } else if (q == "US") {
            reply("@US" + QByteArray::number(afSub));
        } else if (q == "CV") {
            reply("@CV" + QByteArray::number(sidetone));
        } else if (q == "TO") {
            reply("@TO" + QByteArray::number(monitor));
        } else if (q == "TT") {
            reply(tunerIn ? "@TT1" : "@TT0");
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
        } else if (cmd.startsWith("*UM")) {
            afMain = cmd.mid(3).toInt();
        } else if (cmd.startsWith("*US")) {
            afSub = cmd.mid(3).toInt();
        } else if (cmd.startsWith("*CV")) {
            sidetone = cmd.mid(3).toInt();
        } else if (cmd.startsWith("*TO")) {
            monitor = cmd.mid(3).toInt();
        } else if (cmd == "*TT1") {
            tunerIn = tunerFitted;
        } else if (cmd == "*TT0") {
            tunerIn = false;
        } else if (cmd == "*TTT") {
            // The tuner's own cycle: the radio keys itself, then drops it.
            if (tunerFitted) {
                ++tunerCycles;
                keyed = true;
                const int cycle = tunerCycles;
                QTimer::singleShot(800, [this, cycle] {
                    if (tunerCycles == cycle) keyed = false;
                });
            }
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
    req.params.insert(QStringLiteral("orion.tx.maxTuneMs"), 1500);
    // Nor the station's WinKeyer, rotor or service ports (orion_peripherals_test
    // covers those against fakes).
    req.params.insert(QStringLiteral("orion.winkeyer.device"), QStringLiteral("none"));
    req.params.insert(QStringLiteral("orion.rotor.device"), QStringLiteral("none"));
    req.params.insert(QStringLiteral("orion.cwdaemon.port"), QStringLiteral("none"));
    req.params.insert(QStringLiteral("orion.pat.url"), QStringLiteral("none"));
    backend.connectRadio(req);
    check(waitFor([&] { return connectedSpy.count() == 1; }, 2000),
          "connected() after the first CAT reply");
    check(backend.capabilities().family == QLatin1String("orion"), "family is orion");
    check(backend.capabilities().canTransmit, "the Orion can transmit (step 6)");
    check(backend.capabilities().backendPanAveraging
              && backend.capabilities().backendPanAveraging->clientPersistsAveraging
              && backend.capabilities().backendPanAveraging->msPerAverageStep == 10,
          "FFT AVG is 10 ms per step and the client keeps it between sessions");
    check(!backend.capabilities().hostModulates && backend.capabilities().takesTxAudioOverSeam,
          "the radio modulates; PC audio crosses the seam");
    check(backend.capabilities().hasSelectableMicInputs,
          "TX inputs are selectable (on air: a leftover false collapsed the list to PC)");
    check(backend.capabilities().extensions.value(QStringLiteral("micInputs")).toStringList()
              == QStringList({QStringLiteral("MIC"), QStringLiteral("PC")}),
          "the two inputs are MIC and PC");
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

    std::printf("-- TUNE: a low-power FM carrier, then mode and power back\n");
    const auto sawTune = [&](bool on) {
        for (const auto& args : txSpy) {
            const auto d = args.at(0).value<TransmitDelta>();
            if (d.tune && *d.tune == on) return true;
        }
        return false;
    };
    const auto sawRfPower = [&](int value) {
        for (const auto& args : txSpy) {
            const auto d = args.at(0).value<TransmitDelta>();
            if (d.rfPower && *d.rfPower == value) return true;
        }
        return false;
    };
    check(backend.capabilities().hasCwTune, "TUNE offered in CW (the carrier is FM)");
    txSpy.clear();
    backend.setSliceMode(0, QStringLiteral("CW"));    // the operator's CW and 50% drive
    backend.setTxPower(50);
    check(waitFor([&] { return radio.mainMode == 2 && sawRfPower(50); }, 2000),
          "the operator's CW mode and 50% drive set and read back first");
    radio.sets.clear();
    backend.setTune(true, 10, {}, {});
    QElapsedTimer tuneGuard;
    tuneGuard.start();
    while (tuneGuard.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    check(!radio.sets.contains("*TK") && !radio.sets.contains("*TP10"),
          "no carrier without an operation from the TX coordinator");
    backend.requestTransmitControl(pc);
    radio.sets.clear();
    txSpy.clear();
    backend.setTune(true, 10, op, {});
    check(waitFor([&] { return radio.keyed; }, 1000), "TUNE keys the (fake) radio");
    check(radio.sets.indexOf("*TP10") >= 0 && radio.sets.indexOf("*RMM5") > radio.sets.indexOf("*TP10")
              && radio.sets.indexOf("*TK") > radio.sets.indexOf("*RMM5"),
          "TUNE drive, then FM, then *TK (the reference program's order)");
    check(sawTune(true), "tune=true reported");
    check(waitFor([&] { return sawMox(true); }, 3000), "the radio's readback says it transmits");
    const quint64 tuneAudioBefore = txAudioWritten();
    backend.submitTxAudio(block, 24000, TxAudioSource::Microphone, {});
    check(txAudioWritten() == tuneAudioBefore, "no PC audio on a TUNE carrier");
    backend.setTxPower(60);    // the operator moves RF power mid-tune
    tuneGuard.restart();
    while (tuneGuard.elapsed() < 800) {   // several polls of the FM mode and 10% drive
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    check(slices[0].mode == QStringLiteral("CW"), "the slice still shows CW, not TUNE's FM");
    check(!sawRfPower(10), "the RF power slider never shows the TUNE drive");
    check(!radio.sets.contains("*TP60"), "RF power moved mid-tune waits for the carrier to end");
    radio.sets.clear();
    backend.setTune(false, 10, op, {});
    check(waitFor([&] { return !radio.keyed && radio.mainMode == 2 && radio.txPower == 60; }, 1000),
          "TUNE off: unkeyed, CW and the drive (now 60%) back");
    check(radio.sets.indexOf("*TU") == 0, "the carrier drops before anything else changes");
    check(sawTune(false), "tune=false reported");

    std::printf("-- TUNE stops itself\n");
    radio.sets.clear();
    txSpy.clear();
    backend.setTune(true, 10, op, {});
    check(waitFor([&] { return radio.keyed; }, 1000), "carrier up");
    check(waitFor([&] { return !radio.keyed && radio.mainMode == 2 && radio.txPower == 60; }, 4000),
          "the TUNE limit unkeys and restores mode and drive");
    check(sawTune(false), "tune=false reported after the limit");

    std::printf("-- disconnect during TUNE\n");
    backend.setTune(true, 10, op, {});
    check(waitFor([&] { return radio.keyed && radio.mainMode == 5; }, 1000), "carrier up in FM");
    radio.sets.clear();
    backend.disconnectRadio();
    check(waitFor([&] { return !radio.keyed && radio.mainMode == 2 && radio.txPower == 60; }, 500),
          "disconnect unkeys and gives back CW and the drive (stand-alone rule)");
    check(radio.sets.indexOf("*TU") == 0, "unkey first");
    disconnectedSpy.clear();
    backend.connectRadio(req);
    check(waitFor([&] { return backend.isConnected(); }, 2000), "reconnected");
    backend.requestTransmitControl(mic);

    std::printf("-- ATU: the internal tuner's own cycle\n");
    check(backend.capabilities().hasTuner, "the Orion offers its internal tuner");
    const auto lastAtu = [&]() -> QString {
        for (int i = txSpy.size() - 1; i >= 0; --i) {
            const auto d = txSpy.at(i).at(0).value<TransmitDelta>();
            if (d.atuStatusRaw) return *d.atuStatusRaw;
        }
        return {};
    };
    const auto lastAtuEnabled = [&]() -> std::optional<bool> {
        for (int i = txSpy.size() - 1; i >= 0; --i) {
            const auto d = txSpy.at(i).at(0).value<TransmitDelta>();
            if (d.atuEnabled) return d.atuEnabled;
        }
        return std::nullopt;
    };
    radio.sets.clear();
    txSpy.clear();
    backend.setAtu(true, {}, {});
    QElapsedTimer atuGuard;
    atuGuard.start();
    while (atuGuard.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    check(!radio.sets.contains("*TTT"), "no tune cycle without an operation from the TX coordinator");
    backend.setAtu(true, op, {});
    check(waitFor([&] { return radio.sets.contains("*TTT"); }, 500), "ATU runs the radio's tune cycle");
    check(radio.sets.indexOf("*TT1") >= 0 && radio.sets.indexOf("*TT1") < radio.sets.indexOf("*TTT"),
          "tuner in, then *TTT (the reference program's order)");
    check(lastAtu() == QStringLiteral("TUNE_IN_PROGRESS") && lastAtuEnabled() == true,
          "status: in progress, tuner in");
    check(!radio.sets.contains("*TK"), "the radio keys itself: no *TK from us");
    check(waitFor([&] { return lastAtu() == QStringLiteral("TUNE_SUCCESSFUL"); }, 4000),
          "the radio drops its carrier at SWR 1.5: successful");
    check(radio.tunerIn, "the tuner is left in");

    std::printf("-- ATU: no match\n");
    radio.txMeter = "@STF040R020S768";   // SWR 3.0
    txSpy.clear();
    backend.setAtu(true, op, {});
    check(waitFor([&] { return lastAtu() == QStringLiteral("TUNE_FAIL"); }, 4000),
          "the cycle ends at SWR 3.0: failed");
    radio.txMeter = "@STF040R002S384";

    std::printf("-- ATU: no internal tuner\n");
    radio.tunerFitted = false;
    radio.tunerIn = false;
    txSpy.clear();
    QSignalSpy warnSpy(&backend, &IRadioBackend::configurationWarning);
    backend.setAtu(true, op, {});
    check(waitFor([&] { return lastAtu() == QStringLiteral("TUNE_FAIL"); }, 8000),
          "no carrier from the radio: failed");
    check(!warnSpy.isEmpty(), "and the operator is told why");
    radio.tunerFitted = true;

    std::printf("-- ATU bypass\n");
    radio.tunerIn = true;
    radio.sets.clear();
    txSpy.clear();
    backend.setAtu(false, op, {});
    check(waitFor([&] { return !radio.tunerIn && radio.sets.contains("*TT0"); }, 500),
          "bypass switches the tuner out (*TT0)");
    check(lastAtu() == QStringLiteral("TUNE_BYPASS") && lastAtuEnabled() == false,
          "status: bypassed, tuner out");

    std::printf("-- MON: the radio's TX monitor\n");
    check(backend.capabilities().txMonitorControl.has_value(), "MON is offered");
    const auto lastMon = [&]() -> std::pair<std::optional<bool>, std::optional<int>> {
        std::optional<bool> on;
        std::optional<int> level;
        for (const auto& args : txSpy) {
            const auto d = args.at(0).value<TransmitDelta>();
            if (d.sbMonitor) on = d.sbMonitor;
            if (d.monGainSb) level = d.monGainSb;
        }
        return {on, level};
    };
    txSpy.clear();
    TransmitControlRequest monOn;
    monOn.field = TransmitControlRequest::Field::MonitorLevel;
    monOn.value = MonitorSetting{true, 45};
    check(backend.requestTransmitControl(monOn) == ReceiveDispatch::Dispatched, "MON level accepted");
    check(waitFor([&] { return radio.monitor == 45 && lastMon().first == true
                               && lastMon().second == 45; }, 1000),
          "MON 45: *TO45, read back as on at 45");
    txSpy.clear();
    TransmitControlRequest monOff;
    monOff.field = TransmitControlRequest::Field::MonitorEnabled;
    monOff.value = MonitorSetting{false, 45};
    backend.requestTransmitControl(monOff);
    check(waitFor([&] { return radio.monitor == 0 && lastMon().first == false; }, 1000),
          "MON off: *TO0, read back as off");
    check(!lastMon().second.has_value(), "off keeps the level slider where it was");

    std::printf("-- CW sidetone: the radio's own (*CV)\n");
    const auto lastSidetone = [&]() -> std::pair<std::optional<bool>, std::optional<int>> {
        std::optional<bool> on;
        std::optional<int> level;
        for (const auto& args : txSpy) {
            const auto d = args.at(0).value<TransmitDelta>();
            if (d.cwSidetone) on = d.cwSidetone;
            if (d.monGainCw) level = d.monGainCw;
        }
        return {on, level};
    };
    txSpy.clear();
    TransmitControlRequest stLevel;
    stLevel.field = TransmitControlRequest::Field::CwMonitorGain;
    stLevel.value = 65;
    check(backend.requestTransmitControl(stLevel) == ReceiveDispatch::Dispatched,
          "sidetone level accepted");
    check(waitFor([&] { return radio.sidetone == 65 && lastSidetone().second == 65; }, 1000),
          "sidetone level 65: *CV65, read back");
    txSpy.clear();
    TransmitControlRequest stOff;
    stOff.field = TransmitControlRequest::Field::CwSidetone;
    stOff.value = false;
    backend.requestTransmitControl(stOff);
    check(waitFor([&] { return radio.sidetone == 0 && lastSidetone().first == false; }, 1000),
          "sidetone off: *CV0, read back as off");
    radio.sets.clear();
    stLevel.value = 30;
    backend.requestTransmitControl(stLevel);
    QElapsedTimer stGuard;
    stGuard.start();
    while (stGuard.elapsed() < 200) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        usleep(2000);
    }
    check(radio.sidetone == 0, "the level moved while off does not unmute it");
    TransmitControlRequest stOn;
    stOn.field = TransmitControlRequest::Field::CwSidetone;
    stOn.value = true;
    backend.requestTransmitControl(stOn);
    check(waitFor([&] { return radio.sidetone == 30 && lastSidetone().first == true; }, 1000),
          "sidetone on: back at the slider's level (30)");

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

    std::printf("-- slice volume: RSP2 with PC Audio on, the radio's own AF with it off\n");
    {
        check(backend.capabilities().extensionNamespaces.contains(QStringLiteral("orion")),
              "the backend answers the orion namespace (PC Audio state)");
        const auto audioReq = [](SliceAudioRequest::Field f, int v) {
            SliceAudioRequest r;
            r.field = f;
            r.value = v;
            return r;
        };
        // Last audio deltas published per slice.
        std::map<int, std::optional<double>> gainSeen;
        std::map<int, std::optional<bool>> muteSeen;
        const auto conn = QObject::connect(&backend, &IRadioBackend::sliceChanged,
                                           [&](int id, const SliceDelta& d) {
                                               if (d.audioGain) gainSeen[id] = d.audioGain;
                                               if (d.audioMute) muteSeen[id] = d.audioMute;
                                           });
        backend.invokeExtension(QStringLiteral("orion"), QStringLiteral("audio.pc.state"), 0, true);
        radio.sets.clear();
        backend.requestSliceAudio(0, audioReq(SliceAudioRequest::Field::Gain, 30));
        QElapsedTimer g;
        g.start();
        while (g.elapsed() < 200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            usleep(2000);
        }
        check(!radio.sets.contains("*UM76"), "PC Audio on: volume stays on the RSP2, not the radio");

        backend.invokeExtension(QStringLiteral("orion"), QStringLiteral("audio.pc"), 0, false);
        check(waitFor([&] { return gainSeen[0] == 40.0 && gainSeen[1] == 20.0; }, 2000),
              "PC Audio off: the sliders show the radio's own A/B volume (40 %, 20 %)");
        radio.sets.clear();
        backend.requestSliceAudio(0, audioReq(SliceAudioRequest::Field::Gain, 60));
        backend.requestSliceAudio(1, audioReq(SliceAudioRequest::Field::Gain, 100));
        check(waitFor([&] { return radio.afMain == 153 && radio.afSub == 255; }, 1000),
              "slice A 60 % -> *UM153, slice B 100 % -> *US255 (byte scale)");
        backend.requestSliceAudio(0, audioReq(SliceAudioRequest::Field::Mute, 1));
        check(waitFor([&] { return radio.afMain == 0; }, 1000), "mute A -> *UM0");
        radio.sets.clear();
        backend.requestSliceAudio(0, audioReq(SliceAudioRequest::Field::Gain, 70));
        g.restart();
        while (g.elapsed() < 200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            usleep(2000);
        }
        check(radio.afMain == 0, "volume moved while muted keeps it muted");
        backend.requestSliceAudio(0, audioReq(SliceAudioRequest::Field::Mute, 0));
        check(waitFor([&] { return radio.afMain == 178; }, 1000), "unmute A -> the new level, *UM178");

        // The radio's own knob, read back while PC Audio is off.
        radio.afSub = 204;   // 80 %
        backend.invokeExtension(QStringLiteral("orion"), QStringLiteral("audio.pc"), 0, true);
        backend.invokeExtension(QStringLiteral("orion"), QStringLiteral("audio.pc"), 0, false);
        check(waitFor([&] { return gainSeen[1] == 80.0; }, 2000),
              "the radio's own volume knob shows on slice B's slider");

        backend.invokeExtension(QStringLiteral("orion"), QStringLiteral("audio.pc"), 0, true);
        check(gainSeen[0] == 30.0, "PC Audio on again: slice A's slider is back at its RSP2 level");

        // Muted on the radio, then the session ends: the radio gets its level back.
        backend.invokeExtension(QStringLiteral("orion"), QStringLiteral("audio.pc"), 0, false);
        backend.requestSliceAudio(1, audioReq(SliceAudioRequest::Field::Mute, 1));
        check(waitFor([&] { return radio.afSub == 0; }, 1000), "mute B -> *US0");
        QObject::disconnect(conn);
        backend.disconnectRadio();
        check(waitFor([&] { return radio.afSub == 204; }, 1000),
              "disconnect gives the muted receiver its volume back (stand-alone rule)");
        disconnectedSpy.clear();
        backend.connectRadio(req);
        check(waitFor([&] { return backend.isConnected(); }, 2000), "reconnected");
        backend.invokeExtension(QStringLiteral("orion"), QStringLiteral("audio.pc"), 0, true);
    }

    std::printf("-- Center Lock after Zero Beat: no echo loop (2026-10-18 crash)\n");
    {
        // Center Lock re-asks for its target from inside the echo whenever the
        // echoed centre differs from it; Zero Beat leaves a fractional hertz.
        const double target = 7'043'400.37;
        int asks = 0;
        double lastEchoMhz = 0.0;
        const auto conn = QObject::connect(
            &backend, &IRadioBackend::panCenterBandwidthChanged,
            [&](const QString&, double centerMhz, double) {
                lastEchoMhz = centerMhz;
                if (centerMhz != target / 1e6 && ++asks < 50) {
                    backend.setPanCenter(QStringLiteral("0x0"), target, IRadioBackend::PanCenterIntent::Range);
                }
            });
        backend.setPanCenter(QStringLiteral("0x0"), target, IRadioBackend::PanCenterIntent::Range);
        QObject::disconnect(conn);
        check(asks == 0, "the echo is the exact centre asked for: Center Lock is satisfied");
        check(lastEchoMhz == target / 1e6, "echoed centre equals the fractional target");
        int echoes = 0;
        const auto again = QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged,
                                            [&](const QString&, double, double) { ++echoes; });
        backend.setPanCenter(QStringLiteral("0x0"), target, IRadioBackend::PanCenterIntent::Range);
        QObject::disconnect(again);
        check(echoes == 0, "asking again for the same centre echoes nothing");
    }

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
