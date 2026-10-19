#pragma once

// N8EM fork: the Hy-Gain DCU-3 rotor's controls (step 7's driver, which
// has been turning nothing until now). A tile for the right-hand panel,
// built on AetherSDR's own RotorCompass so it reads like the Green Heron
// tile the app already has — and on the same safety rule:
//
//   Choosing a bearing and sending it are SEPARATE gestures. The compass
//   has no mouse handlers, so no click or drag can aim the antenna; a turn
//   happens only on Turn, on Enter in the bearing box, or on a short/long
//   path button. Every one of them goes through one call, turnTo().
//
// Unlike the Green Heron, the DCU-3 can be told to stop, so this tile has
// a Stop button — the control that matters when the beam is going the
// wrong way. See FORK-NOTES.md.

#include <QWidget>

class QLabel;
class QPushButton;
class QSpinBox;

namespace AetherSDR {

class RadioModel;
class RotorCompass;

class OrionRotorApplet : public QWidget {
    Q_OBJECT

public:
    explicit OrionRotorApplet(RadioModel* model, QWidget* parent = nullptr);

    // Docked vs floating, as the Green Heron tile does it: the compass is
    // the one thing that only earns its space when the tile is floated.
    void setFloating(bool on);

    // Point the bearing box at a computed heading without sending it — the
    // Log QSO window's short/long path buttons arm the tile this way.
    void showBearing(double degrees);

    // For tests and the automation bridge.
    [[nodiscard]] const RotorCompass* compass() const { return m_compass; }
    [[nodiscard]] double reportedAzimuth() const { return m_reported; }

public slots:
    // The one place a turn is ever sent from.
    void turnTo(double degrees);
    void stopRotor();

private:
    void refresh();                      // ask the backend where it is
    void applyState(const QVariantMap& state);
    void note(const QString& text);

    RadioModel* m_model = nullptr;
    RotorCompass* m_compass = nullptr;
    QSpinBox* m_bearing = nullptr;
    QPushButton* m_turn = nullptr;
    QPushButton* m_stop = nullptr;
    QLabel* m_readout = nullptr;
    QLabel* m_status = nullptr;
    double m_reported = -1.0;
    double m_asked = -1.0;
    bool m_hasAsked = false;
};

}  // namespace AetherSDR
