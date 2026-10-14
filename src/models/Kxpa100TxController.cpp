#include "Kxpa100TxController.h"
#include "Kxpa100Model.h"
#include "RadioModel.h"
#include "SliceModel.h"
#include <QScopedValueRollback>
#include <cmath>
#include <utility>

namespace AetherSDR {
int Kxpa100TxController::effectiveTxFrequencyKhz(RadioModel* radio)
{
    const SliceModel* slice = radio && radio->isConnected() ? radio->txSlice() : nullptr;
    if (!slice) { return 0; }
    const double mhz = slice->frequency()
        + ((slice->mode() == "FM" || slice->mode() == "NFM") ? slice->txOffsetFreq() : 0.0)
        + (slice->xitOn() ? slice->xitFreq() / 1.0e6 : 0.0);
    return std::isfinite(mhz) && mhz >= 0.001 && mhz < 100.0
        ? static_cast<int>(std::llround(mhz * 1000.0)) : 0;
}
Kxpa100TxController::Kxpa100TxController(RadioModel* radio, Kxpa100Model* tuner, QObject* parent)
    : QObject(parent), m_radio(radio), m_tuner(tuner)
{
    m_deadline.setParent(this);
    m_deadline.setObjectName(QStringLiteral("kxTxDeadline"));
    m_deadline.setSingleShot(true);
    m_watchdog.setInterval(100);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_phase == Phase::Cancelling) {
            m_cancelPending = false;
            stop(tr("Carrier stopped; KXPA100 cancellation was not confirmed."), false);
        } else {
            stop(tr("KXPA100 tune timed out; carrier stopped."), true);
        }
    });
    connect(&m_watchdog, &QTimer::timeout, this, &Kxpa100TxController::refresh);
    if (tuner) {
        connect(tuner, &Kxpa100Model::statusUpdated, this, &Kxpa100TxController::refresh);
        connect(tuner, &Kxpa100Model::availabilityChanged, this, &Kxpa100TxController::refresh);
        connect(tuner, &Kxpa100Model::useTxControlsChanged, this, [this](bool enabled) { savePreference(enabled); refresh(); });
        connect(tuner, &Kxpa100Model::associationChanged, this, &Kxpa100TxController::loadAssociation);
        connect(tuner, &Kxpa100Model::detectedChanged, this, &Kxpa100TxController::loadAssociation);
        connect(tuner, &Kxpa100Model::operationFinished, this, &Kxpa100TxController::operationFinished);
        connect(tuner, &Kxpa100Model::operationFailed, this, [this](const QString& reason) {
            if (m_ownsOperation) { stop(reason, m_phase != Phase::Saving && m_phase != Phase::Result && m_phase != Phase::Cancelling); }
        });
        connect(tuner, &Kxpa100Model::telemetryReceived, this, [this](const QString& command) { telemetry(command); });
        connect(tuner, &Kxpa100Model::disconnected, this, [this] {
            m_completed = false;
            stop(tr("KXPA100 disconnected; carrier stopped."), false);
            emit tunerDisconnected();
        });
    }
    if (radio) {
        connect(radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            m_bypassRequested = false;
            if (!connected) { stop(tr("Radio disconnected; tune canceled."), true); }
            refresh();
        });
        connect(radio, &RadioModel::infoChanged, this, &Kxpa100TxController::loadAssociation);
        connect(radio, &RadioModel::capabilitiesChanged, this, &Kxpa100TxController::refresh);
        connect(&radio->transmitModel(), &TransmitModel::stateChanged, this, &Kxpa100TxController::refresh);
        connect(&radio->transmitModel(), &TransmitModel::atuStateChanged, this, &Kxpa100TxController::refresh);
        connect(&radio->transmitModel(), &TransmitModel::tuneChanged, this, &Kxpa100TxController::refresh);
    }
    loadAssociation();
    m_watchdog.start();
    refresh();
}
bool Kxpa100TxController::associated() const
{
    if (!m_radio || !m_tuner || !m_radio->isConnected() || m_boundSerial.isEmpty()) { return false; }
    const auto scope = m_radio->settingsScope();
    return scope.family() == m_boundFamily && scope.radioId() == m_boundRadio
        && m_tuner->status().serial == m_boundSerial;
}
bool Kxpa100TxController::associate(RadioModel* radio, Kxpa100Model* tuner)
{
    if (!radio || !radio->isConnected() || !tuner || !tuner->isDetected() || !tuner->isResponding()) { return false; }
    const auto scope = radio->settingsScope();
    if (!scope.isValid() || scope.radioId().isEmpty()) { return false; }
    int version = 0;
    AppSettings::FeatureReadStatus state;
    QJsonObject doc = scope.featureExact("kxpa100", &version, &state);
    if (version > 1 || state == AppSettings::FeatureReadStatus::Corrupt || state == AppSettings::FeatureReadStatus::Unavailable) { return false; }
    doc["serial"] = tuner->status().serial;
    doc["useTxControls"] = true;
    if (!scope.setFeature("kxpa100", 1, doc)) { return false; }
    emit tuner->associationChanged();
    return true;
}
void Kxpa100TxController::loadAssociation()
{
    m_boundFamily.clear(); m_boundRadio.clear(); m_boundSerial.clear();
    if (m_radio && m_tuner) {
        const auto scope = m_radio->settingsScope();
        int version = 0;
        const QJsonObject doc = scope.featureExact("kxpa100", &version);
        if (version == 1 && !scope.radioId().isEmpty() && doc.value("serial").toString() == m_tuner->status().serial) {
            m_boundFamily = scope.family(); m_boundRadio = scope.radioId(); m_boundSerial = m_tuner->status().serial;
            QScopedValueRollback loading(m_loadingPreferences, true);
            m_tuner->setUseTxControls(doc.value("useTxControls").toBool(true));
        }
    }
    refresh();
}
bool Kxpa100TxController::dissociate(RadioModel* radio, Kxpa100Model* tuner)
{
    if (!radio || !tuner) { return false; }
    const auto scope = radio->settingsScope();
    if (scope.radioId().isEmpty()) { return false; }
    int version = 0;
    QJsonObject doc = scope.featureExact("kxpa100", &version);
    if (version != 1) { return false; }
    doc.remove("serial");
    if (!scope.setFeature("kxpa100", 1, doc)) { return false; }
    emit tuner->associationChanged();
    return true;
}
void Kxpa100TxController::savePreference(bool enabled)
{
    if (m_loadingPreferences || !associated()) { return; }
    const auto scope = m_radio->settingsScope();
    int version = 0;
    AppSettings::FeatureReadStatus state;
    QJsonObject doc = scope.featureExact("kxpa100", &version, &state);
    if (version != 1 || state != AppSettings::FeatureReadStatus::Present) { return; }
    doc["useTxControls"] = enabled;
    scope.setFeature("kxpa100", 1, doc);
}
Kxpa100TxController::~Kxpa100TxController()
{
    m_deadline.stop(); m_watchdog.stop();
    if (m_tuner) { disconnect(m_tuner, nullptr, this, nullptr); }
    if (m_radio) { disconnect(&m_radio->transmitModel(), nullptr, this, nullptr); }
    m_carrier.stop();
    if (m_tuner && m_searchRequested) { m_tuner->cancelTuneAfterPendingOperation(); }
    else if (m_tuner && m_ownsOperation) { m_tuner->cancelOperation(); }
}
bool Kxpa100TxController::internalBypassed() const
{
    if (!m_radio || !m_radio->isConnected()) { return false; }
    const TransmitModel& tx = m_radio->transmitModel();
    // The operational bypass status is authoritative. A radio can retain its
    // separate ATU-enabled flag while reporting a bypassed matching network.
    return !tx.hasTuner() || tx.atuStatus() == ATUStatus::Bypass
        || tx.atuStatus() == ATUStatus::ManualBypass;
}
void Kxpa100TxController::ensureBypass()
{
    if (!m_selected || !m_radio || !m_radio->isConnected()) { return; }
    if (internalBypassed()) { m_bypassRequested = false; return; }
    const auto& tx = m_radio->transmitModel();
    if (!m_bypassRequested && !tx.isTransmitting() && !tx.isTuning()) {
        m_bypassRequested = true;
        m_radio->transmitModel().atuBypass();
    }
}
QString Kxpa100TxController::unavailableReason() const
{
    if (!m_selected || !m_tuner) { return tr("KXPA100 is not selected for TX Controls."); }
    if (!m_radio || !m_radio->isConnected()) { return tr("Connect a radio first."); }
    if (busy()) { return tr("KXPA100 operation in progress; click to cancel."); }
    const auto& tx = m_radio->transmitModel();
    if (!m_radio->backendCapabilities().canTransmit || !tx.tuneAvailable()) { return tr("Tune carrier is unavailable for this radio or mode."); }
    if (tx.isTransmitting() || tx.isTuning()) { return tr("Stop transmitting before tuning or saving a match."); }
    const QString reason = m_tuner->unavailableReason(true);
    if (!reason.isEmpty()) { return reason; }
    const auto& status = m_tuner->status();
    if (!status.mode || status.mode == Kxpa100::Mode::Bypass || m_tuner->fieldAgeMs("^MD") < 0
        || m_tuner->fieldAgeMs("^MD") > 5000) { return tr("Waiting for fresh KXPA100 Auto or Manual mode readback."); }
    if (m_tuner->fieldAgeMs("^FL") < 0 || m_tuner->fieldAgeMs("^FL") > 5000) { return tr("Waiting for current fault readback."); }
    if (status.faultCode != 'N') { return tr("Clear the KXPA100 fault before tuning."); }
    if (!Kxpa100::isSupportedFrequency(effectiveTxFrequencyKhz(m_radio))) { return tr("KXPA100 requires a TX frequency from 1800 to 54000 kHz."); }
    return {};
}
bool Kxpa100TxController::matchContextCurrent() const
{
    if (!m_tuner || !m_tuner->isResponding() || effectiveTxFrequencyKhz(m_radio) != m_frequencyKhz) { return false; }
    const auto& status = m_tuner->status();
    return status.tunerPresent == true && status.serial == m_matchContext.serial
        && status.mode == m_matchContext.mode && status.antenna == m_matchContext.antenna
        && status.bypassed == m_matchContext.bypassed && status.side == m_matchContext.side
        && status.capacitorMask == m_matchContext.capacitorMask && status.inductorMask == m_matchContext.inductorMask;
}
bool Kxpa100TxController::matched() const
{
    return m_selected && m_completed && m_tuner
        && matchContextCurrent() && m_tuner->status().faultCode == 'N'
        && m_tuner->status().swr.has_value() && effectiveTxFrequencyKhz(m_radio) == m_frequencyKhz;
}
bool Kxpa100TxController::originalIntentValid() const
{
    if (!m_radio) { return false; }
    const auto& tx = m_radio->transmitModel();
    if (m_phase != Phase::Carrier && (tx.isTransmitting() || tx.isTuning())) { return false; }
    return m_selected && associated() && m_tuner->tunerEligible() && m_radio && m_radio->isConnected() && m_tuner && m_tuner->isResponding()
        && m_tuner->useTxControls() && m_tuner->status().tunerPresent == true
        && m_tuner->status().mode && m_tuner->status().mode != Kxpa100::Mode::Bypass
        && m_tuner->status().mode == m_mode && m_tuner->status().antenna == m_antenna
        && m_tuner->status().serial == m_serial && m_tuner->status().faultCode == 'N'
        && effectiveTxFrequencyKhz(m_radio) == m_frequencyKhz && m_carrier.valid()
        && m_carrier.belongsTo(m_radio);
}
void Kxpa100TxController::refresh()
{
    if (m_refreshing) { return; }
    QScopedValueRollback guard(m_refreshing, true);
    const bool previouslySelected = m_selected;
    m_selected = associated() && m_tuner->useTxControls() && m_tuner->tunerEligible();
    if (m_completed && !matchContextCurrent()) { m_completed = false; }
    if (previouslySelected != m_selected) { m_bypassRequested = false; m_completed = false; }
    if (busy() && m_phase != Phase::Cancelling && m_phase != Phase::Saving && m_phase != Phase::Result) {
        if (!originalIntentValid() || (m_phase == Phase::Carrier &&
            (!m_radio->transmitModel().isTuning() || !internalBypassed()))) {
            stop(tr("Tune canceled: device, radio, frequency or input authorization changed."), true);
        }
    }
    if (m_phase == Phase::Result && (!m_selected || !m_tuner || !m_tuner->isConnected()
        || m_tuner->status().serial != m_serial || effectiveTxFrequencyKhz(m_radio) != m_frequencyKhz)) {
        stop(tr("Tune result discarded: matching context changed."), false);
    }
    if (m_phase == Phase::Saving && (!m_selected || !m_tuner || !m_tuner->isConnected())) {
        stop(tr("Saving canceled: tuner selection changed."), false);
    }
    ensureBypass();
    if (m_phase == Phase::Bypass && internalBypassed()) { recallFrequency(); }
    if (m_phase == Phase::Cancelling) { pollCancellation(); }
    publish();
}
void Kxpa100TxController::publish()
{
    const Kxpa100::Status status = m_tuner ? m_tuner->status() : Kxpa100::Status{};
    const Presentation current{m_selected, busy(), matched(), m_tuner && m_tuner->isResponding(),
                               status.mode, status.bypassed, m_tuner && m_tuner->amplifierMeterEligible(),
                               unavailableReason(), m_message};
    if (m_presentation == current) { return; }
    m_presentation = current;
    emit changed();
}
void Kxpa100TxController::setPhase(Phase phase, int timeoutMs)
{
    m_phase = phase;
    if (timeoutMs > 0) { m_deadline.start(timeoutMs); } else { m_deadline.stop(); }
    publish();
}
bool Kxpa100TxController::start(const TxController::Input& carrier)
{
    if (!unavailableReason().isEmpty() || !carrier.valid() || !carrier.belongsTo(m_radio)) { return false; }
    m_carrier = carrier;
    m_frequencyKhz = effectiveTxFrequencyKhz(m_radio); m_serial = m_tuner->status().serial;
    m_antenna = m_tuner->status().antenna;
    m_mode = m_tuner->status().mode;
    m_completed = false;
    m_searchRequested = false; m_cancelPending = false;
    m_message = tr("Waiting for internal ATU bypass confirmation.");
    setPhase(Phase::Bypass, 3000);
    m_bypassRequested = false;
    ensureBypass();
    if (internalBypassed()) { recallFrequency(); }
    return busy();
}
void Kxpa100TxController::recallFrequency()
{
    if (!originalIntentValid()) { stop(tr("Tune input expired."), false); return; }
    m_message = tr("Selecting the KXPA100 TX frequency.");
    setPhase(Phase::Frequency, 5000); m_ownsOperation = true;
    if (!m_tuner->recallFrequency(m_frequencyKhz)) { stop(tr("KXPA100 frequency request was refused."), false); }
}
void Kxpa100TxController::beginSearch()
{
    if (!originalIntentValid() || !internalBypassed()) { stop(tr("Tune preflight changed."), false); return; }
    m_message = tr("Requesting a fresh KXPA100 tune.");
    setPhase(Phase::Search, 5000); m_ownsOperation = true; m_searchRequested = true;
    const QPointer<Kxpa100TxController> self(this);
    if (!m_tuner->startTune([self] {
        return self && self->m_phase == Phase::Search && self->originalIntentValid() && self->internalBypassed();
    })) { stop(tr("KXPA100 tune request was refused."), true); }
}
void Kxpa100TxController::operationFinished()
{
    if (!m_ownsOperation) { return; }
    m_ownsOperation = false;
    switch (m_phase) {
    case Phase::Frequency: beginSearch(); break;
    case Phase::Search:
        if (!originalIntentValid() || !internalBypassed() || m_tuner->status().tuning != true) {
            stop(tr("KXPA100 did not confirm a pending tune."), true); return;
        }
        m_message = tr("KXPA100 tuning; click to cancel.");
        // TransmitModel::startTune publishes local Tune state synchronously for every
        // backend, before dispatch; backend status later reconciles that state.
        setPhase(Phase::Carrier, 30000);
        if (!m_carrier.start()) { stop(tr("Radio refused the tune carrier."), true); }
        break;
    case Phase::Result:
        m_matchContext = m_tuner->status();
        stop(tr("KXPA100 tune completed; result readback confirmed."), false);
        m_completed = true;
        publish();
        break;
    case Phase::Cancelling: m_searchRequested = false; stop(tr("KXPA100 tune canceled."), false); break;
    case Phase::Saving: stop(tr("KXPA100 match saved and verified."), false); break;
    default: break;
    }
}
void Kxpa100TxController::telemetry(const QString& command)
{
    if (command == "^FE" && m_phase == Phase::Carrier) {
        m_searchRequested = false;
        stop(tr("KXPA100 tune completed; waiting for result readback."), false);
        setPhase(Phase::Result, 5000);
        m_ownsOperation = true;
        if (!m_tuner->readMatchStatus()) { stop(tr("KXPA100 tune result could not be read."), false); }
    }
    publish();
}
void Kxpa100TxController::stop(const QString& reason, bool cancelSearch)
{
    const bool owns = std::exchange(m_ownsOperation, false);
    const bool search = m_searchRequested;
    m_phase = Phase::Idle; m_deadline.stop(); m_message = reason;
    const auto carrier = std::exchange(m_carrier, {});
    carrier.stop();
    if (owns && m_tuner) { m_tuner->cancelOperation(); }
    m_cancelPending = cancelSearch && search && m_tuner && m_tuner->isConnected();
    if (m_cancelPending) { setPhase(Phase::Cancelling, 5000); pollCancellation(); }
    else { m_searchRequested = false; }
    publish();
}
void Kxpa100TxController::pollCancellation()
{
    if (!m_cancelPending || !m_tuner || !m_tuner->isConnected() || m_tuner->status().serial != m_serial) { return; }
    if (m_tuner->operationBusy()) { return; }
    m_ownsOperation = true;
    if (m_tuner->cancelTune()) { m_cancelPending = false; }
    else { m_ownsOperation = false; }
}
void Kxpa100TxController::cancel()
{
    if (m_phase == Phase::Cancelling) { return; }
    stop(tr("Tune canceled; carrier stopped."), true);
}
bool Kxpa100TxController::saveMatch()
{
    if (!unavailableReason().isEmpty()) { return false; }
    m_message = tr("Saving the current KXPA100 match.");
    setPhase(Phase::Saving, 15000); m_ownsOperation = true;
    if (!m_tuner->memorize(effectiveTxFrequencyKhz(m_radio))) { stop(tr("KXPA100 save request was refused."), false); return false; }
    return true;
}
} // namespace AetherSDR
