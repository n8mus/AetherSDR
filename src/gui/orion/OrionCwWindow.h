#pragma once

// N8EM fork: the CW sending/reading window, the Orion console's CwWindow
// re-expressed on AetherSDR's own plumbing (step 10). Fork rule 2 allows the
// console's appearance here (operator, 2026-10-19): this window replaces
// AetherSDR's CWX panel for the station, so it has to look and work like the
// one the operator already has muscle memory for.
//
// What is the console's: the layout, the dark shack-distance styling, the
// type-ahead line with Word/Live keying, the DX box, F1-F12 macros on three
// rows of four with right-click editing, and the plain-language macro
// vocabulary (core/backends/orion/CwMacros.h).
//
// What is AetherSDR's: every byte on the air goes through CwxModel, so the
// TX coordinator, TUNE interlock, sidetone and the backend's own keyer path
// are exactly the ones the CWX panel uses. Decoded CW comes from
// AetherSDR's CwRxModel. Nothing here talks to a serial port.
//
// The QSO it is about is shared with the Log QSO window through
// logbook::QsoContext, so a call or a report typed in either window is what
// the F-keys send. See FORK-NOTES.md.

#include <functional>

#include <QDialog>

#include "core/backends/orion/CwMacros.h"
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace AetherSDR {

class CwxModel;

namespace orion::logbook {
class QsoContext;
}

class OrionCwWindow : public QDialog {
    Q_OBJECT

public:
    static constexpr int kFkeys = 12;

    // `cwx` keys the text; `ctx` is the shared current-QSO record (may be
    // null when the logbook is not built in).
    OrionCwWindow(CwxModel* cwx, orion::logbook::QsoContext* ctx, QWidget* parent = nullptr);

    // The operator's own callsign/name/QTH/grid for the [my ...] fields.
    void setStationFields(const QString& call, const QString& name, const QString& qth,
                          const QString& grid);
    // Arms [his call] AND shows the call in the DX box, so every click path
    // (spot, decode pane, skimmer) is visible and correctable.
    void setHisCall(const QString& call);
    [[nodiscard]] QString hisCall() const;

    // Steady key-down for tuning; the host supplies it (AetherSDR's TUNE).
    void setTuneHandler(std::function<void(bool)> handler);

public slots:
    void appendRx(const QString& text);   // decoded CW from AetherSDR's reader
    void setRxStats(float pitchHz, float wpm);
    void clearRx();

signals:
    // True while the window is visible with RX decode checked — the host
    // gates the CW reader on it, so it costs nothing while closed.
    void rxDecodeWanted(bool on);
    // Double-click on a callsign-shaped token in the decode pane: the fldigi
    // move — copy says who they are, one gesture logs them.
    void callDoubleClicked(const QString& call);
    // A DX call is settled (typed and left, or spent on the air).
    void hisCallEntered(const QString& call);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void sendText(const QString& text);
    void sendLine();
    // The fields an F-key macro can spend, read fresh at every press.
    [[nodiscard]] orion::CwMacroFields macroFieldsNow() const;
    void editMemory(int i);
    [[nodiscard]] QString fkeySpec(int i) const;   // "label|text"
    void applyFkeyButton(int i);
    void applyContext();                 // ctx -> the DX box
    void pushDxCall(const QString& call);
    void announceHisCall();
    void tintDxCall();
    void updateRxInfo();
    void updateStatus(const QString& s = QString());

    CwxModel* m_cwx = nullptr;
    orion::logbook::QsoContext* m_ctx = nullptr;
    bool m_ctxEcho = false;              // applying ctx -> don't write back
    std::function<void(bool)> m_tune;

    QLineEdit* m_line = nullptr;
    QLineEdit* m_dxCall = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_sentView = nullptr;
    QSpinBox* m_wpm = nullptr;
    QPushButton* m_tuneBtn = nullptr;
    QCheckBox* m_live = nullptr;         // stream keystrokes as typed
    QCheckBox* m_word = nullptr;         // space bar releases each word
    QCheckBox* m_rxOn = nullptr;
    QLabel* m_rxInfo = nullptr;
    QPlainTextEdit* m_rx = nullptr;
    QPushButton* m_mem[kFkeys] = {};

    QString m_myCall, m_myName, m_myQth, m_myGrid;
    QString m_hisCall;
    QString m_pushedCall;                // last call announced
    int m_prevLen = 0;                   // live mode: chars already streamed
    int m_rxWpm = 0;
    double m_rxPitch = -1.0;
    double m_shownHz = -1.0;             // the Hz actually on the label
};

}  // namespace AetherSDR
