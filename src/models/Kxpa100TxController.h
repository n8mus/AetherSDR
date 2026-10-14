#pragma once

#include "TxController.h"
#include "Kxpa100Types.h"
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace AetherSDR {
class Kxpa100Model;
class RadioModel;

// TX Controls' external-tuner sequence. A captured Tune input is carried through
// every asynchronous step; status and timers can never manufacture TX authority.
class Kxpa100TxController final : public QObject {
    Q_OBJECT
public:
    Kxpa100TxController(RadioModel* radio, Kxpa100Model* tuner, QObject* parent = nullptr);
    ~Kxpa100TxController() override;
    static bool associate(RadioModel* radio, Kxpa100Model* tuner);
    static bool dissociate(RadioModel* radio, Kxpa100Model* tuner);
    static int effectiveTxFrequencyKhz(RadioModel* radio);
    bool associated() const;
    bool selected() const { return m_selected; }
    bool busy() const { return m_phase != Phase::Idle; }
    bool matched() const;
    QString unavailableReason() const;
    QString message() const { return m_message; }
    bool start(const TxController::Input& carrier);
    bool saveMatch();
    void cancel();

signals:
    void changed();
    void tunerDisconnected();

private:
    enum class Phase { Idle, Bypass, Frequency, Search, Carrier, Result, Cancelling, Saving };
    void refresh();
    void loadAssociation();
    void savePreference(bool enabled);
    void publish();
    bool matchContextCurrent() const;
    void ensureBypass();
    bool internalBypassed() const;
    bool originalIntentValid() const;
    bool saveIntentValid() const;
    void recallFrequency();
    void beginSearch();
    void operationFinished();
    void telemetry(const QString& command);
    void stop(const QString& reason, bool cancelSearch);
    void pollCancellation();
    void setPhase(Phase phase, int timeoutMs);

    QPointer<RadioModel> m_radio;
    QPointer<Kxpa100Model> m_tuner;
    TxController::Input m_carrier;
    QTimer m_deadline;
    QTimer m_watchdog;
    Phase m_phase{Phase::Idle};
    bool m_selected{false};
    bool m_refreshing{false};
    bool m_bypassRequested{false};
    bool m_ownsOperation{false};
    bool m_searchRequested{false};
    bool m_cancelPending{false};
    bool m_completed{false};
    int m_frequencyKhz{0};
    std::optional<int> m_antenna;
    std::optional<Kxpa100::Mode> m_mode;
    QString m_serial;
    QString m_boundFamily, m_boundRadio, m_boundSerial;
    bool m_loadingPreferences = false;
    QString m_message;
    Kxpa100::Status m_matchContext;
    using Presentation = std::tuple<bool, bool, bool, bool, std::optional<Kxpa100::Mode>,
                                    std::optional<bool>, bool, QString, QString>;
    std::optional<Presentation> m_presentation;
};
} // namespace AetherSDR
