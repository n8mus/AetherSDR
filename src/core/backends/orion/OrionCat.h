#pragma once

// Ten-Tec Orion 565/566 ASCII CAT: command builders and reply decoder.
//
// Pure protocol: no serial port, no timers, no signals. Every command is a
// short ASCII string the link terminates with CR; every reply is one
// '@'-prefixed line. The facts here come from the Ten-Tec Orion programmer's
// reference and were checked on a v3-firmware radio (marked live-verified).
//
// Receivers: Main = VFO A = letter 'M'; Sub = VFO B = letter 'S'.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <variant>

namespace AetherSDR::orion {

enum class Rx { Main, Sub };

// The Orion's mode codes in *R<M|S>M<n>: USB0 LSB1 UCW2 LCW3 AM4 FM5.
enum class Mode { Usb = 0, Lsb = 1, Cw = 2, Cwl = 3, Am = 4, Fm = 5 };

// Hard gate on every frequency set and every parsed frequency reply. A
// malformed request ("*AF0") would otherwise send the radio off frequency.
inline constexpr qint64 kMinHz = 100000;
inline constexpr qint64 kMaxHz = 60000000;

// Filter bandwidth limits. The radio REJECTS out-of-range values rather than
// clamping them (live-verified), so builders clamp before sending. AM runs to
// 9000 Hz (its number is the audio width), everything else to 6000 Hz.
inline constexpr int kMinBandwidthHz = 100;
inline constexpr int kPbtRangeHz = 8000;
[[nodiscard]] int maxBandwidthHz(Mode mode);

// Neutral mode vocabulary shared with SliceModel ("USB", "LSB", "CW", "CWL",
// "AM", "FM"). Unknown names have no Orion mode.
[[nodiscard]] QString neutralMode(Mode mode);
[[nodiscard]] std::optional<Mode> modeFromNeutral(const QString& neutral);
[[nodiscard]] QStringList neutralModeList();

// ---- passband geometry --------------------------------------------------
// The Orion describes its receive filter as bandwidth + PBT. The panadapter
// wants edges in Hz relative to the carrier. The placement follows the radio:
// SSB grows away from the zero-beat edge (USB lo = pbt, LSB hi = -pbt), CW/FM
// sit on the carrier, AM's number is the AUDIO width so its RF span is twice
// that. Both directions are exact inverses of each other.
struct Passband {
    int lowHz = 0;
    int highHz = 0;
};
struct FilterSetting {
    int bandwidthHz = 0;
    int pbtHz = 0;
};
[[nodiscard]] Passband passbandFromFilter(Mode mode, int bandwidthHz, int pbtHz);
[[nodiscard]] FilterSetting filterFromPassband(Mode mode, int lowHz, int highHz);

// ---- S-meter --------------------------------------------------------------
// Raw ?S units (0..300) to dB relative to S9, by linear interpolation in the
// Hamlib TT565 V2 calibration table, clamped at both ends.
[[nodiscard]] double rawToDbRelS9(int raw);
// S9 is -73 dBm on HF by the IARU convention the meter readout uses.
inline constexpr double kS9Dbm = -73.0;
[[nodiscard]] inline double rawToDbm(int raw) { return rawToDbRelS9(raw) + kS9Dbm; }

// ---- AGC ------------------------------------------------------------------
// Orion AGC letters F/M/S/P/O. The neutral vocabulary has no "programmable",
// so 'P' decodes to nothing and setting it is never offered.
[[nodiscard]] std::optional<char> agcLetterFromNeutral(const QString& neutral);
[[nodiscard]] std::optional<QString> neutralFromAgcLetter(char letter);

// ---- command builders (no CR; the link appends it) -------------------------
// Each returns an empty array when the request is out of range.
[[nodiscard]] QByteArray setFrequency(Rx rx, qint64 hz);
[[nodiscard]] QByteArray queryFrequency(Rx rx);
[[nodiscard]] QByteArray setMode(Rx rx, Mode mode);
[[nodiscard]] QByteArray queryMode(Rx rx);
[[nodiscard]] QByteArray setBandwidth(Rx rx, Mode mode, int hz);
[[nodiscard]] QByteArray queryBandwidth(Rx rx);
[[nodiscard]] QByteArray setPbt(Rx rx, int hz);
[[nodiscard]] QByteArray queryPbt(Rx rx);
[[nodiscard]] QByteArray setAgc(Rx rx, char letter);
[[nodiscard]] QByteArray queryAgc(Rx rx);
[[nodiscard]] QByteArray setRfGain(Rx rx, int percent);
[[nodiscard]] QByteArray queryRfGain(Rx rx);
[[nodiscard]] QByteArray setAttenuator(Rx rx, int step);
[[nodiscard]] QByteArray queryAttenuator(Rx rx);
[[nodiscard]] QByteArray setPreamp(Rx rx, bool on);
[[nodiscard]] QByteArray queryPreamp(Rx rx);
[[nodiscard]] QByteArray querySMeter();
[[nodiscard]] QByteArray queryVfoAssignment();
// *KV<main><sub><tx>: route the VFOs. main is A or B; sub and tx A, B or N.
// Empty when a letter is out of range. Split is TX on B with main RX on A.
[[nodiscard]] QByteArray setVfoAssignment(char mainRx, char subRx, char tx);
// ---- transmit (step 6) ---------------------------------------------------
// *TK keys, *TU unkeys. An unkey is never refused anywhere in this backend.
[[nodiscard]] QByteArray setPtt(bool keyed);
// TX levels, each 0..100 (the radio rejects out-of-range values; clamped
// here): power, mic gain, aux (line) input gain. Speech processor 0..9.
[[nodiscard]] QByteArray setTxPower(int percent);
[[nodiscard]] QByteArray setMicGain(int percent);
[[nodiscard]] QByteArray setAuxGain(int percent);
[[nodiscard]] QByteArray setSpeechProcessor(int level);
[[nodiscard]] QByteArray queryTxPower();
[[nodiscard]] QByteArray queryMicGain();
[[nodiscard]] QByteArray queryAuxGain();
[[nodiscard]] QByteArray querySpeechProcessor();

// ?AL / ?BL (undocumented, live-verified): front-panel lock state. The lock
// only freezes the knob; CAT still tunes, so the backend enforces it.
[[nodiscard]] QByteArray queryVfoLock(Rx rx);

// ---- replies ---------------------------------------------------------------
struct FrequencyReply { Rx rx; qint64 hz; };
struct ModeReply { Rx rx; Mode mode; };
struct BandwidthReply { Rx rx; int hz; };
struct PbtReply { Rx rx; int hz; };
struct AgcReply { Rx rx; char letter; };
struct RfGainReply { Rx rx; int percent; };
struct AttenuatorReply { Rx rx; int step; };
struct PreampReply { Rx rx; bool on; };
struct SMeterReply { int mainRaw; int subRaw; };
// @STF<fwd>R<ref>S<swr>: transmit meter. SWR is 8.8 fixed point on v3
// firmware unless the field carries a decimal point.
struct TxMeterReply { double forwardWatts; double reflectedWatts; double swr; };
// @KV<main><sub><tx>: which VFO feeds each receiver and the transmitter.
// Letters A/B, N = unassigned (main is never N).
struct VfoAssignmentReply { char mainRx; char subRx; char tx; };
// @AL / @AU / @BL / @BU
struct VfoLockReply { Rx rx; bool locked; };
// @TP / @TM / @TI <0..100>, @TS <0..9>
struct TxLevelReply {
    enum class Kind { Power, Mic, Aux, Processor };
    Kind kind;
    int value;
};

using Reply = std::variant<FrequencyReply, ModeReply, BandwidthReply, PbtReply,
                           AgcReply, RfGainReply, AttenuatorReply, PreampReply,
                           SMeterReply, TxMeterReply, VfoAssignmentReply, VfoLockReply,
                           TxLevelReply>;

// Decode one line (CR already stripped). nullopt for anything that is not a
// well-formed, in-range reply this decoder knows; replies glued together on
// the wire decode their leading field only, never as a bogus value.
[[nodiscard]] std::optional<Reply> decodeReply(const QByteArray& line);

}  // namespace AetherSDR::orion
