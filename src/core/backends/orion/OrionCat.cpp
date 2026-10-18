#include "core/backends/orion/OrionCat.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace AetherSDR::orion {

namespace {

char rxLetter(Rx rx) { return rx == Rx::Main ? 'M' : 'S'; }
char vfoLetter(Rx rx) { return rx == Rx::Main ? 'A' : 'B'; }

QByteArray receiverCommand(char prefix, Rx rx, const char* field, const QByteArray& value = {})
{
    QByteArray out;
    out += prefix;
    out += 'R';
    out += rxLetter(rx);
    out += field;
    out += value;
    return out;
}

// A leading run of digits, optionally signed, ignoring trailing bytes. Replies
// occasionally arrive glued together on the wire, and a whole-line conversion
// would read those as 0, which must never pass for a frequency.
bool parseLeadingInt(const QByteArray& s, qint64& out)
{
    qsizetype i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        negative = s[i] == '-';
        ++i;
    }
    qint64 value = 0;
    int digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 12) {
        value = value * 10 + (s[i] - '0');
        ++i;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    out = negative ? -value : value;
    return true;
}

std::optional<Rx> receiverFromLetter(char c)
{
    if (c == 'M') {
        return Rx::Main;
    }
    if (c == 'S') {
        return Rx::Sub;
    }
    return std::nullopt;
}

bool isVfoLetter(char c, bool allowUnassigned)
{
    return c == 'A' || c == 'B' || (allowUnassigned && c == 'N');
}

// Hamlib TT565_STR_CAL_V2: raw ?S units to dB relative to S9.
struct CalPoint {
    int raw;
    double db;
};
constexpr std::array<CalPoint, 14> kSMeterCal{{
    {10, -48.0}, {24, -42.0}, {38, -36.0}, {47, -30.0}, {61, -24.0},
    {70, -18.0}, {79, -12.0}, {84, -6.0},  {94, 0.0},   {103, 10.0},
    {118, 20.0}, {134, 30.0}, {147, 40.0}, {161, 50.0},
}};

std::optional<Reply> decodeReceiverReply(const QByteArray& line)
{
    // @R<M|S><field><value>
    if (line.size() < 5) {
        return std::nullopt;
    }
    const std::optional<Rx> rx = receiverFromLetter(line[2]);
    if (!rx) {
        return std::nullopt;
    }
    const char field = line[3];
    const QByteArray rest = line.mid(4);
    qint64 v = 0;
    switch (field) {
    case 'F':
        // 9000 cap, not 6000: AM filters run to 9000 (live-verified).
        if (parseLeadingInt(rest, v) && v >= kMinBandwidthHz && v <= 9000) {
            return BandwidthReply{*rx, static_cast<int>(v)};
        }
        return std::nullopt;
    case 'P':
        if (parseLeadingInt(rest, v) && v >= -kPbtRangeHz && v <= kPbtRangeHz) {
            return PbtReply{*rx, static_cast<int>(v)};
        }
        return std::nullopt;
    case 'M':
        if (rest[0] >= '0' && rest[0] <= '5') {
            return ModeReply{*rx, static_cast<Mode>(rest[0] - '0')};
        }
        return std::nullopt;
    case 'A': {
        // AGC letter, or the programmable-AGC sub-fields AT/AH/AD, which are
        // not decoded here. 'P' may carry trailing data.
        const char a = rest[0];
        if (a == 'F' || a == 'M' || a == 'S' || a == 'P' || a == 'O') {
            return AgcReply{*rx, a};
        }
        return std::nullopt;
    }
    case 'G':
        if (parseLeadingInt(rest, v) && v >= 0 && v <= 100) {
            return RfGainReply{*rx, static_cast<int>(v)};
        }
        return std::nullopt;
    case 'T':
        if (rest[0] >= '0' && rest[0] <= '3') {
            return AttenuatorReply{*rx, rest[0] - '0'};
        }
        return std::nullopt;
    case 'E':
        if (rest[0] == '0' || rest[0] == '1') {
            return PreampReply{*rx, rest[0] == '1'};
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

}  // namespace

int maxBandwidthHz(Mode mode)
{
    return mode == Mode::Am ? 9000 : 6000;
}

QString neutralMode(Mode mode)
{
    switch (mode) {
    case Mode::Usb: return QStringLiteral("USB");
    case Mode::Lsb: return QStringLiteral("LSB");
    case Mode::Cw:  return QStringLiteral("CW");
    case Mode::Cwl: return QStringLiteral("CWL");
    case Mode::Am:  return QStringLiteral("AM");
    case Mode::Fm:  return QStringLiteral("FM");
    }
    return {};
}

std::optional<Mode> modeFromNeutral(const QString& neutral)
{
    const QString u = neutral.trimmed().toUpper();
    if (u == QLatin1String("USB")) {
        return Mode::Usb;
    }
    if (u == QLatin1String("LSB")) {
        return Mode::Lsb;
    }
    if (u == QLatin1String("CW") || u == QLatin1String("CWU")) {
        return Mode::Cw;
    }
    if (u == QLatin1String("CWL")) {
        return Mode::Cwl;
    }
    if (u == QLatin1String("AM")) {
        return Mode::Am;
    }
    if (u == QLatin1String("FM")) {
        return Mode::Fm;
    }
    return std::nullopt;
}

QStringList neutralModeList()
{
    return {QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("CW"),
            QStringLiteral("CWL"), QStringLiteral("AM"), QStringLiteral("FM")};
}

namespace {
int pbtSign(Mode mode)
{
    return (mode == Mode::Lsb || mode == Mode::Cwl) ? -1 : +1;
}
}  // namespace

Passband passbandFromFilter(Mode mode, int bandwidthHz, int pbtHz)
{
    switch (mode) {
    case Mode::Usb:
        // Grows up from the zero-beat edge, which PBT moves.
        return {pbtHz, pbtHz + bandwidthHz};
    case Mode::Lsb:
        return {-pbtHz - bandwidthHz, -pbtHz};
    case Mode::Am: {
        // The number is the audio width; the RF passband spans both sidebands.
        const int low = pbtHz - bandwidthHz;
        return {low, low + 2 * bandwidthHz};
    }
    default: {
        // CW, CWL, FM: centred on the carrier, PBT signed by sideband. The far
        // edge is anchored on the width so odd widths round-trip exactly.
        const int low = pbtSign(mode) * pbtHz - bandwidthHz / 2;
        return {low, low + bandwidthHz};
    }
    }
}

FilterSetting filterFromPassband(Mode mode, int lowHz, int highHz)
{
    if (highHz < lowHz) {
        std::swap(lowHz, highHz);
    }
    int width = highHz - lowHz;
    if (mode == Mode::Am) {
        width /= 2;
    }
    width = std::clamp(width, kMinBandwidthHz, maxBandwidthHz(mode));
    int pbt = 0;
    switch (mode) {
    case Mode::Usb:
        pbt = lowHz;
        break;
    case Mode::Lsb:
        pbt = -highHz;
        break;
    default:
        // Centre measured from the low edge, the same integer half the
        // forward direction subtracts, so odd spans round-trip exactly.
        pbt = pbtSign(mode) * (lowHz + (highHz - lowHz) / 2);
        break;
    }
    return {width, std::clamp(pbt, -kPbtRangeHz, kPbtRangeHz)};
}

double rawToDbRelS9(int raw)
{
    if (raw <= kSMeterCal.front().raw) {
        return kSMeterCal.front().db;
    }
    for (std::size_t i = 1; i < kSMeterCal.size(); ++i) {
        const CalPoint& hi = kSMeterCal[i];
        if (raw <= hi.raw) {
            const CalPoint& lo = kSMeterCal[i - 1];
            const double t = static_cast<double>(raw - lo.raw) / (hi.raw - lo.raw);
            return lo.db + t * (hi.db - lo.db);
        }
    }
    return kSMeterCal.back().db;
}

std::optional<char> agcLetterFromNeutral(const QString& neutral)
{
    const QString n = neutral.trimmed().toLower();
    if (n == QLatin1String("off")) {
        return 'O';
    }
    if (n == QLatin1String("slow")) {
        return 'S';
    }
    if (n == QLatin1String("med") || n == QLatin1String("medium")) {
        return 'M';
    }
    if (n == QLatin1String("fast")) {
        return 'F';
    }
    return std::nullopt;
}

std::optional<QString> neutralFromAgcLetter(char letter)
{
    switch (letter) {
    case 'O': return QStringLiteral("off");
    case 'S': return QStringLiteral("slow");
    case 'M': return QStringLiteral("med");
    case 'F': return QStringLiteral("fast");
    default:  return std::nullopt;
    }
}

QByteArray setFrequency(Rx rx, qint64 hz)
{
    if (hz < kMinHz || hz > kMaxHz) {
        return {};
    }
    QByteArray out("*");
    out += vfoLetter(rx);
    out += 'F';
    out += QByteArray::number(hz);
    return out;
}

QByteArray queryFrequency(Rx rx)
{
    QByteArray out("?");
    out += vfoLetter(rx);
    out += 'F';
    return out;
}

QByteArray setMode(Rx rx, Mode mode)
{
    return receiverCommand('*', rx, "M", QByteArray::number(static_cast<int>(mode)));
}

QByteArray queryMode(Rx rx) { return receiverCommand('?', rx, "M"); }

QByteArray setBandwidth(Rx rx, Mode mode, int hz)
{
    const int clamped = std::clamp(hz, kMinBandwidthHz, maxBandwidthHz(mode));
    return receiverCommand('*', rx, "F", QByteArray::number(clamped));
}

QByteArray queryBandwidth(Rx rx) { return receiverCommand('?', rx, "F"); }

QByteArray setPbt(Rx rx, int hz)
{
    const int clamped = std::clamp(hz, -kPbtRangeHz, kPbtRangeHz);
    return receiverCommand('*', rx, "P", QByteArray::number(clamped));
}

QByteArray queryPbt(Rx rx) { return receiverCommand('?', rx, "P"); }

QByteArray setAgc(Rx rx, char letter)
{
    if (!std::strchr("FMSO", letter) || letter == '\0') {
        return {};
    }
    return receiverCommand('*', rx, "A", QByteArray(1, letter));
}

QByteArray queryAgc(Rx rx) { return receiverCommand('?', rx, "A"); }

QByteArray setRfGain(Rx rx, int percent)
{
    return receiverCommand('*', rx, "G", QByteArray::number(std::clamp(percent, 0, 100)));
}

QByteArray queryRfGain(Rx rx) { return receiverCommand('?', rx, "G"); }

QByteArray setAttenuator(Rx rx, int step)
{
    return receiverCommand('*', rx, "T", QByteArray::number(std::clamp(step, 0, 3)));
}

QByteArray queryAttenuator(Rx rx) { return receiverCommand('?', rx, "T"); }

QByteArray setPreamp(Rx rx, bool on)
{
    return receiverCommand('*', rx, "E", on ? QByteArray("1") : QByteArray("0"));
}

QByteArray queryPreamp(Rx rx) { return receiverCommand('?', rx, "E"); }

QByteArray querySMeter() { return QByteArrayLiteral("?S"); }

QByteArray queryVfoAssignment() { return QByteArrayLiteral("?KV"); }

QByteArray setVfoAssignment(char mainRx, char subRx, char tx)
{
    if (!isVfoLetter(mainRx, false) || !isVfoLetter(subRx, true) || !isVfoLetter(tx, true)) {
        return {};
    }
    QByteArray out("*KV");
    out += mainRx;
    out += subRx;
    out += tx;
    return out;
}

QByteArray setPtt(bool keyed) { return keyed ? QByteArrayLiteral("*TK") : QByteArrayLiteral("*TU"); }

QByteArray setTxPower(int percent)
{
    return "*TP" + QByteArray::number(std::clamp(percent, 0, 100));
}

QByteArray setMicGain(int percent)
{
    return "*TM" + QByteArray::number(std::clamp(percent, 0, 100));
}

QByteArray setAuxGain(int percent)
{
    return "*TI" + QByteArray::number(std::clamp(percent, 0, 100));
}

QByteArray setSpeechProcessor(int level)
{
    return "*TS" + QByteArray::number(std::clamp(level, 0, 9));
}

QByteArray queryTxPower() { return QByteArrayLiteral("?TP"); }
QByteArray setTuner(bool in) { return in ? QByteArrayLiteral("*TT1") : QByteArrayLiteral("*TT0"); }
QByteArray startTunerCycle() { return QByteArrayLiteral("*TTT"); }
QByteArray queryTuner() { return QByteArrayLiteral("?TT"); }
QByteArray queryMicGain() { return QByteArrayLiteral("?TM"); }
QByteArray queryAuxGain() { return QByteArrayLiteral("?TI"); }
QByteArray querySpeechProcessor() { return QByteArrayLiteral("?TS"); }
QByteArray setMonitor(int percent) { return "*TO" + QByteArray::number(std::clamp(percent, 0, 100)); }
QByteArray queryMonitor() { return QByteArrayLiteral("?TO"); }
QByteArray setCwSidetoneVolume(int percent)
{
    return "*CV" + QByteArray::number(std::clamp(percent, 0, 100));
}
QByteArray queryCwSidetoneVolume() { return QByteArrayLiteral("?CV"); }
QByteArray setAfVolume(Rx rx, int percent)
{
    return QByteArray(rx == Rx::Main ? "*UM" : "*US")
        + QByteArray::number(std::clamp(percent, 0, 100) * 255 / 100);
}
QByteArray queryAfVolume(Rx rx) { return rx == Rx::Main ? QByteArrayLiteral("?UM") : QByteArrayLiteral("?US"); }

QByteArray queryVfoLock(Rx rx)
{
    QByteArray out("?");
    out += vfoLetter(rx);
    out += 'L';
    return out;
}

std::optional<Reply> decodeReply(const QByteArray& line)
{
    if (line.size() < 3 || line[0] != '@') {
        return std::nullopt;
    }
    qint64 v = 0;
    switch (line[1]) {
    case 'U': {
        // @UM<b> / @US<b> / @UB<b> / @UM<b>S<b>: AF volume, a byte on the wire
        if (line.size() < 4 || !parseLeadingInt(line.mid(3), v) || v < 0 || v > 255) {
            return std::nullopt;
        }
        const int pct = static_cast<int>(v * 100 / 255);
        AfVolumeReply r;
        switch (line[2]) {
        case 'M': {
            r.mainPercent = pct;
            const qsizetype s = line.indexOf('S', 3);
            qint64 v2 = 0;
            if (s > 0 && parseLeadingInt(line.mid(s + 1), v2) && v2 >= 0 && v2 <= 255) {
                r.subPercent = static_cast<int>(v2 * 100 / 255);
            }
            return r;
        }
        case 'S':
            r.subPercent = pct;
            return r;
        case 'B':
            r.mainPercent = pct;
            r.subPercent = pct;
            return r;
        default:
            return std::nullopt;
        }
    }
    case 'C':
        // @CV<0..100>: CW sidetone volume
        if (line[2] == 'V' && line.size() >= 4 && parseLeadingInt(line.mid(3), v) && v >= 0
            && v <= 100) {
            return SidetoneReply{static_cast<int>(v)};
        }
        return std::nullopt;
    case 'A':
    case 'B':
        // @AF<hz> / @BF<hz>
        if (line[2] == 'F' && parseLeadingInt(line.mid(3), v) && v >= kMinHz && v <= kMaxHz) {
            return FrequencyReply{line[1] == 'A' ? Rx::Main : Rx::Sub, v};
        }
        // @AL / @AU / @BL / @BU: exactly three characters
        if (line.size() == 3 && (line[2] == 'L' || line[2] == 'U')) {
            return VfoLockReply{line[1] == 'A' ? Rx::Main : Rx::Sub, line[2] == 'L'};
        }
        return std::nullopt;
    case 'S':
        if (line.size() < 5) {
            return std::nullopt;
        }
        if (line[2] == 'R' && line[3] == 'M') {
            // @SRM<main>S<sub>, each 0..300
            qint64 mainRaw = 0;
            qint64 subRaw = 0;
            const qsizetype sPos = line.indexOf('S', 4);
            if (parseLeadingInt(line.mid(4), mainRaw) && mainRaw >= 0 && mainRaw <= 300
                && sPos > 4 && parseLeadingInt(line.mid(sPos + 1), subRaw)
                && subRaw >= 0 && subRaw <= 300) {
                return SMeterReply{static_cast<int>(mainRaw), static_cast<int>(subRaw)};
            }
            return std::nullopt;
        }
        if (line[2] == 'T' && line[3] == 'F') {
            // @STF<fwd>R<ref>S<swr>; live trace @STF024R004S608 = 24 W, 4 W, 2.38
            const qsizetype rPos = line.indexOf('R', 4);
            const qsizetype sPos = rPos > 0 ? line.indexOf('S', rPos + 1) : -1;
            qint64 fwd = 0;
            qint64 ref = 0;
            if (rPos > 4 && sPos > rPos + 1 && parseLeadingInt(line.mid(4, rPos - 4), fwd)
                && parseLeadingInt(line.mid(rPos + 1, sPos - rPos - 1), ref)) {
                const QByteArray swrField = line.mid(sPos + 1);
                double swr = std::atof(swrField.constData());
                if (!swrField.contains('.')) {
                    swr /= 256.0;
                }
                return TxMeterReply{static_cast<double>(fwd), static_cast<double>(ref), swr};
            }
            return std::nullopt;
        }
        return std::nullopt;
    case 'K':
        // @KV<main><sub><tx>
        if (line.size() >= 6 && line[2] == 'V' && isVfoLetter(line[3], false)
            && isVfoLetter(line[4], true) && isVfoLetter(line[5], true)) {
            return VfoAssignmentReply{line[3], line[4], line[5]};
        }
        return std::nullopt;
    case 'T': {
        // @TT1 / @TT0: the internal tuner
        if (line.size() == 4 && line[2] == 'T' && (line[3] == '0' || line[3] == '1')) {
            return TunerReply{line[3] == '1'};
        }
        // @TP / @TM / @TI <0..100>, @TS <0..9>
        if (line.size() < 4 || !parseLeadingInt(line.mid(3), v)) {
            return std::nullopt;
        }
        switch (line[2]) {
        case 'P':
            if (v >= 0 && v <= 100) {
                return TxLevelReply{TxLevelReply::Kind::Power, static_cast<int>(v)};
            }
            break;
        case 'M':
            if (v >= 0 && v <= 100) {
                return TxLevelReply{TxLevelReply::Kind::Mic, static_cast<int>(v)};
            }
            break;
        case 'I':
            if (v >= 0 && v <= 100) {
                return TxLevelReply{TxLevelReply::Kind::Aux, static_cast<int>(v)};
            }
            break;
        case 'S':
            if (v >= 0 && v <= 9) {
                return TxLevelReply{TxLevelReply::Kind::Processor, static_cast<int>(v)};
            }
            break;
        case 'O':
            if (v >= 0 && v <= 100) {
                return TxLevelReply{TxLevelReply::Kind::Monitor, static_cast<int>(v)};
            }
            break;
        default:
            break;
        }
        return std::nullopt;
    }
    case 'R':
        return decodeReceiverReply(line);
    default:
        return std::nullopt;
    }
}

}  // namespace AetherSDR::orion
