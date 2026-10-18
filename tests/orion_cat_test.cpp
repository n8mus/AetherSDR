// N8EM fork: Ten-Tec Orion CAT codec and poll cadence.
//
// Pure protocol: no serial port, no event loop, no hardware. Pins the wire
// strings, the reply decoder's range gates, the passband geometry in both
// directions, the S-meter calibration, and the poll rotation.

#include "core/backends/orion/OrionCat.h"
#include "core/backends/orion/OrionPollScheduler.h"

#include <cmath>
#include <cstdio>

using namespace AetherSDR::orion;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

template <typename T>
static const T* as(const std::optional<Reply>& r)
{
    return r ? std::get_if<T>(&*r) : nullptr;
}

static void testCommands()
{
    check(setFrequency(Rx::Main, 14025000) == "*AF14025000", "*AF builder");
    check(setFrequency(Rx::Sub, 7040000) == "*BF7040000", "*BF builder");
    check(setFrequency(Rx::Main, 0).isEmpty(), "0 Hz is refused, never sent");
    check(setFrequency(Rx::Main, 99999).isEmpty(), "below 100 kHz refused");
    check(setFrequency(Rx::Main, 60000001).isEmpty(), "above 60 MHz refused");
    check(queryFrequency(Rx::Main) == "?AF", "?AF");
    check(queryFrequency(Rx::Sub) == "?BF", "?BF");
    check(setMode(Rx::Main, Mode::Lsb) == "*RMM1", "*RMM1 LSB");
    check(setMode(Rx::Sub, Mode::Cwl) == "*RSM3", "*RSM3 CWL");
    check(queryMode(Rx::Main) == "?RMM", "?RMM");
    check(setBandwidth(Rx::Main, Mode::Usb, 2400) == "*RMF2400", "*RMF");
    check(setBandwidth(Rx::Main, Mode::Usb, 9000) == "*RMF6000",
          "SSB width clamps to 6000: the radio rejects, never clamps");
    check(setBandwidth(Rx::Main, Mode::Am, 9000) == "*RMF9000", "AM reaches 9000");
    check(setBandwidth(Rx::Main, Mode::Usb, 20) == "*RMF100", "width floor 100");
    check(setPbt(Rx::Sub, -200) == "*RSP-200", "*RSP negative");
    check(setPbt(Rx::Main, 9000) == "*RMP8000", "PBT clamps to 8000");
    check(setAgc(Rx::Main, 'F') == "*RMAF", "*RMAF");
    check(setAgc(Rx::Main, 'P').isEmpty(), "programmable AGC is never set");
    check(setRfGain(Rx::Main, 150) == "*RMG100", "RF gain clamps");
    check(setAttenuator(Rx::Main, 2) == "*RMT2", "*RMT");
    check(setPreamp(Rx::Main, true) == "*RME1", "*RME1");
    check(querySMeter() == "?S", "?S");
    check(queryVfoAssignment() == "?KV", "?KV");
    check(setVfoAssignment('A', 'B', 'B') == "*KVABB", "*KVABB: split, TX on B");
    check(setVfoAssignment('A', 'N', 'A') == "*KVANA", "sub unassigned");
    check(setVfoAssignment('N', 'B', 'A').isEmpty(), "main receiver is never unassigned");
    check(setVfoAssignment('A', 'B', 'X').isEmpty(), "bad letter refused");
    check(queryVfoLock(Rx::Sub) == "?BL", "?BL");
}

static void testReplies()
{
    const auto f = decodeReply("@AF14025000");
    check(as<FrequencyReply>(f) && as<FrequencyReply>(f)->rx == Rx::Main
              && as<FrequencyReply>(f)->hz == 14025000,
          "@AF decodes");
    const auto b = decodeReply("@BF7040000");
    check(as<FrequencyReply>(b) && as<FrequencyReply>(b)->rx == Rx::Sub, "@BF decodes");
    check(!decodeReply("@AF0"), "@AF0 is not a frequency");
    check(!decodeReply("@AF"), "@AF with no digits");
    check(!decodeReply("@AF99999"), "@AF below range");
    // Glued replies decode their leading field only.
    const auto glued = decodeReply("@AF14025000@RMM0");
    check(as<FrequencyReply>(glued) && as<FrequencyReply>(glued)->hz == 14025000,
          "glued reply keeps only the leading frequency");

    const auto m = decodeReply("@RMM2");
    check(as<ModeReply>(m) && as<ModeReply>(m)->mode == Mode::Cw, "@RMM2 = CW");
    check(!decodeReply("@RMM9"), "mode code out of range");
    check(!decodeReply("@RXM0"), "unknown receiver letter");

    const auto tunerIn = decodeReply("@TT1");
    check(as<TunerReply>(tunerIn) && as<TunerReply>(tunerIn)->in, "@TT1 = tuner in");
    const auto tunerOut = decodeReply("@TT0");
    check(as<TunerReply>(tunerOut) && !as<TunerReply>(tunerOut)->in, "@TT0 = tuner bypassed");
    check(!decodeReply("@TT2"), "tuner state out of range");
    check(setTuner(true) == "*TT1" && setTuner(false) == "*TT0" && startTunerCycle() == "*TTT"
              && queryTuner() == "?TT",
          "tuner commands as the reference program sends them");

    const auto w = decodeReply("@RSF9000");
    check(as<BandwidthReply>(w) && as<BandwidthReply>(w)->hz == 9000
              && as<BandwidthReply>(w)->rx == Rx::Sub,
          "AM-wide 9000 accepted (the old 6000 cap dropped it)");
    check(!decodeReply("@RMF50"), "width below 100 rejected");

    const auto p = decodeReply("@RMP-200");
    check(as<PbtReply>(p) && as<PbtReply>(p)->hz == -200, "@RMP-200");

    const auto a = decodeReply("@RMAS");
    check(as<AgcReply>(a) && as<AgcReply>(a)->letter == 'S', "@RMAS");
    check(!decodeReply("@RMAT1.50"), "AGC threshold is not an AGC letter");
    check(!decodeReply("@RMAH0.20"), "AGC hang is not an AGC letter");
    check(!decodeReply("@RMAD100"), "AGC decay is not an AGC letter");

    const auto s = decodeReply("@SRM94S10");
    check(as<SMeterReply>(s) && as<SMeterReply>(s)->mainRaw == 94
              && as<SMeterReply>(s)->subRaw == 10,
          "@SRM94S10");
    check(!decodeReply("@SRM400S10"), "S-meter above 300 rejected");

    const auto t = decodeReply("@STF024R004S608");
    check(as<TxMeterReply>(t) && as<TxMeterReply>(t)->forwardWatts == 24.0
              && as<TxMeterReply>(t)->reflectedWatts == 4.0
              && std::fabs(as<TxMeterReply>(t)->swr - 2.375) < 1e-9,
          "live trace @STF024R004S608 = 24 W, 4 W, SWR 2.38 (8.8 fixed point)");
    const auto td = decodeReply("@STF050R002S1.1");
    check(as<TxMeterReply>(td) && std::fabs(as<TxMeterReply>(td)->swr - 1.1) < 1e-9,
          "decimal SWR form");

    const auto kv = decodeReply("@KVABB");
    check(as<VfoAssignmentReply>(kv) && as<VfoAssignmentReply>(kv)->tx == 'B',
          "@KVABB: split, TX on B");
    check(!decodeReply("@KVNAA"), "main receiver is never unassigned");

    const auto bl = decodeReply("@BL");
    check(as<VfoLockReply>(bl) && as<VfoLockReply>(bl)->rx == Rx::Sub
              && as<VfoLockReply>(bl)->locked,
          "@BL: B locked");
    const auto au = decodeReply("@AU");
    check(as<VfoLockReply>(au) && !as<VfoLockReply>(au)->locked, "@AU: A unlocked");
    check(!decodeReply("@ALX"), "lock reply is exactly three characters");
    check(as<RfGainReply>(decodeReply("@RMG73")) != nullptr, "@RMG73");
    check(as<AttenuatorReply>(decodeReply("@RMT3")) != nullptr, "@RMT3");
    check(as<PreampReply>(decodeReply("@RME0")) != nullptr, "@RME0");
    check(!decodeReply("RMM0"), "no @ prefix");
    check(!decodeReply(""), "empty line");
}

static void testPassband()
{
    // USB grows up from the zero-beat edge: lo = pbt.
    Passband p = passbandFromFilter(Mode::Usb, 2400, 0);
    check(p.lowHz == 0 && p.highHz == 2400, "USB 2400/0 -> 0..2400");
    p = passbandFromFilter(Mode::Usb, 2400, 200);
    check(p.lowHz == 200 && p.highHz == 2600, "USB PBT shifts both edges");
    p = passbandFromFilter(Mode::Lsb, 2400, 200);
    check(p.lowHz == -2600 && p.highHz == -200, "LSB mirrors: hi = -pbt");
    p = passbandFromFilter(Mode::Cw, 500, 0);
    check(p.lowHz == -250 && p.highHz == 250, "CW centred on the carrier");
    p = passbandFromFilter(Mode::Cwl, 500, 100);
    check(p.lowHz == -350 && p.highHz == 150, "CWL PBT is sideband-signed");
    p = passbandFromFilter(Mode::Am, 6000, 0);
    check(p.lowHz == -6000 && p.highHz == 6000, "AM number is the audio width");

    // Exact inverse in every mode, including odd widths.
    for (Mode mode : {Mode::Usb, Mode::Lsb, Mode::Cw, Mode::Cwl, Mode::Am, Mode::Fm}) {
        for (int bw : {101, 500, 2401, 6000}) {
            for (int pbt : {-1201, -200, 0, 333, 1000}) {
                const Passband e = passbandFromFilter(mode, bw, pbt);
                const FilterSetting f = filterFromPassband(mode, e.lowHz, e.highHz);
                if (f.bandwidthHz != bw || f.pbtHz != pbt) {
                    std::fprintf(stderr, "  mode %d bw %d pbt %d -> %d..%d -> bw %d pbt %d\n",
                                 static_cast<int>(mode), bw, pbt, e.lowHz, e.highHz,
                                 f.bandwidthHz, f.pbtHz);
                    check(false, "passband round trip is exact");
                }
            }
        }
    }
    const FilterSetting wide = filterFromPassband(Mode::Usb, 0, 9000);
    check(wide.bandwidthHz == 6000, "a too-wide SSB drag clamps to the ceiling");
}

static void testSMeter()
{
    check(rawToDbRelS9(94) == 0.0, "raw 94 is S9");
    check(rawToDbRelS9(0) == -48.0, "clamped at the bottom");
    check(rawToDbRelS9(300) == 50.0, "clamped at the top");
    check(std::fabs(rawToDbRelS9(89) - (-3.0)) < 1e-9, "linear between 84 and 94");
    check(std::fabs(rawToDbm(103) - (10.0 + kS9Dbm)) < 1e-9, "S9+10 in dBm");
}

static void testModesAndAgc()
{
    check(neutralMode(Mode::Cwl) == "CWL", "neutral CWL");
    check(modeFromNeutral("cw") == Mode::Cw, "neutral cw is case-insensitive");
    check(modeFromNeutral("CWU") == Mode::Cw, "CWU alias");
    check(!modeFromNeutral("DIGU"), "no data modes on the Orion");
    check(neutralModeList().size() == 6, "six modes");
    check(agcLetterFromNeutral("med") == 'M', "med -> M");
    check(!agcLetterFromNeutral("prog"), "no programmable via neutral");
    check(neutralFromAgcLetter('O') == QStringLiteral("off"), "O -> off");
    check(!neutralFromAgcLetter('P'), "P has no neutral name");
}

static void testScheduler()
{
    OrionPollScheduler s;
    qint64 now = 10'000;
    // First five ticks: ?AF, ?S, fast wheel, slow wheel, ?AF (re-armed:
    // no reply came, but 800 ms has passed since the first one).
    QList<QByteArray> t0 = s.next(now);
    check(t0 == QList<QByteArray>{"?AF"}, "tick 0: ?AF");
    now += 200;
    check(s.next(now) == QList<QByteArray>{"?S"}, "tick 1: ?S");
    now += 200;
    check(s.next(now) == QList<QByteArray>{"?RMM"}, "tick 2: main mode");
    now += 200;
    check(s.next(now) == QList<QByteArray>{"?KV"},
          "tick 3: the first slow-wheel slot borrows an S-meter turn");
    now += 200;
    check(s.next(now) == QList<QByteArray>{"?AF"},
          "tick 4: ?AF re-armed after 600 ms without a reply");
    s.noteFrequencyReply(Rx::Main);
    now += 200;
    check(s.next(now) == QList<QByteArray>{"?AF"}, "tick 5: ?AF after a reply");
    now += 200;
    check(s.next(now) == QList<QByteArray>{"?S"}, "tick 6: ?S");
    now += 200;
    check((s.next(now) == QList<QByteArray>{"?RMF", "?RMP"}), "tick 7: main filter pair");

    // Burst: right after a dial move the S slots go to ?AF.
    OrionPollScheduler burst;
    now = 50'000;
    (void)burst.next(now);   // tick 0 ?AF
    burst.noteFrequencyReply(Rx::Main);
    burst.noteDialMoved(Rx::Main, now);
    now += 200;
    check(burst.next(now) == QList<QByteArray>{"?AF"}, "burst: S slot lent to ?AF");

    // VFO B moving: the S slots go to ?BF (on-air finding: B lagged).
    OrionPollScheduler sub;
    now = 70'000;
    (void)sub.next(now);   // tick 0 ?AF
    sub.noteFrequencyReply(Rx::Main);
    sub.noteDialMoved(Rx::Sub, now);
    now += 200;
    check(sub.next(now) == QList<QByteArray>{"?BF"}, "burst follows VFO B");

    // B active: B gets the fast slots, A drops to the fast wheel.
    OrionPollScheduler activeB;
    activeB.setActiveReceiver(Rx::Sub);
    now = 80'000;
    check(activeB.next(now) == QList<QByteArray>{"?BF"}, "active B: tick 0 polls ?BF");
    now += 200;
    (void)activeB.next(now);   // tick 1 ?S
    now += 200;
    (void)activeB.next(now);   // tick 2 main mode
    now += 200;
    (void)activeB.next(now);   // tick 3 slow wheel
    activeB.noteFrequencyReply(Rx::Sub);
    now += 200;
    check(activeB.next(now) == QList<QByteArray>{"?BF"}, "active B: tick 4 polls ?BF");
    QList<QByteArray> lap;
    for (int i = 0; i < 25; ++i) {
        now += 200;
        lap += activeB.next(now);
        activeB.noteFrequencyReply(Rx::Main);
        activeB.noteFrequencyReply(Rx::Sub);
    }
    check(lap.contains("?AF"), "active B: dial A is still read in the fast wheel");

    // The slow wheel lands on tick 3, 28, 53 ... and starts with ?KV.
    OrionPollScheduler slow;
    now = 90'000;
    QList<QByteArray> seen;
    for (int i = 0; i < 30; ++i) {
        const QList<QByteArray> q = slow.next(now);
        if (i == 3 || i == 28) {
            seen += q;
        }
        slow.noteFrequencyReply(Rx::Main);
        now += 200;
    }
    check((seen == QList<QByteArray>{"?KV", "?RMA"}), "slow wheel: ?KV then main AGC");
}

int main()
{
    testCommands();
    testReplies();
    testPassband();
    testSMeter();
    testModesAndAgc();
    testScheduler();
    if (g_failures == 0) {
        std::printf("orion_cat_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_cat_test: %d failure(s)\n", g_failures);
    return 1;
}
