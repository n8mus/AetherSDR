#include "gui/orion/OrionCwWindow.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include "core/AppSettings.h"
#include "core/backends/orion/CwMacros.h"
#include "core/backends/orion/log/QsoContext.h"
#include "models/CwxModel.h"

namespace AetherSDR {

using orion::cwGreetingFor;
using orion::CwFieldInfo;
using orion::cwMacroFields;
using orion::CwMacroFields;
using orion::expandCwMacro;

namespace {

// The console's F1-F12 defaults, in its plain-language spelling.
const char* kFkeyDefault[OrionCwWindow::kFkeys] = {
    "CQ|CQ CQ DE [my call] [my call] PSE K",
    "HIS|[his call] DE [my call]",
    "RST|[his call] DE [my call] UR RST [rst sent] [rst sent] BK",
    "INFO|NAME [my name] [my name] QTH [my qth] [my qth] HW?",
    "TU|TU 73 E E",
    "AGN|AGN AGN PSE",
    "QRS|QRS PSE QRS",
    "MY CALL|DE [my call] [my call] K",
    "QTH|QTH [my qth] [my qth] HW?",
    "NAME|NAME [my name] [my name] BTU [his call] KN",
    "GM|[greeting] [greeting] DR OM",
    "SK|TU [his call] 73 SK E E",
};

// Callsign shape incl. portable forms (K2J/4, F/N8EM). Needs the
// digit-in-the-middle structure, so CQ/TU/73/5NN never match. Shared by
// the decode-pane double-click and the DX box's "looks like a call" tint.
const QRegularExpression& callRe()
{
    static const QRegularExpression re(QStringLiteral("^(?:[A-Z0-9]{1,3}/)?"
                                                      "(?:[A-Z]{1,2}|[0-9][A-Z]|[A-Z][0-9])"
                                                      "[0-9]{1,2}[A-Z]{1,4}"
                                                      "(?:/[A-Z0-9]{1,4})?$"));
    return re;
}

QString fkeyKey(int i)
{
    return QStringLiteral("OrionCw/fkey/F") + QString::number(i + 1);
}

QString setting(const QString& key, const QVariant& fallback)
{
    return AppSettings::instance().value(key, fallback).toString();
}

void saveSetting(const QString& key, const QVariant& value)
{
    auto& s = AppSettings::instance();
    s.setValue(key, value);
    s.save();
}

}  // namespace

OrionCwWindow::OrionCwWindow(CwxModel* cwx, orion::logbook::QsoContext* ctx, QWidget* parent)
    : QDialog(parent)
    , m_cwx(cwx)
    , m_ctx(ctx)
{
    setModal(false);
    setWindowTitle(QStringLiteral("CW"));
    // The console's own styling (fork rule 2, amended by the operator
    // 2026-10-19): every control sets an explicit text colour, because a
    // widget with a styled background otherwise keeps the system palette's
    // text — dark on dark. Sizes are for shack distance, not laptop
    // distance.
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 15px; }"
        "QLabel { color: #b8c8d8; font-size: 14px; }"
        "QLineEdit { background: #1c2430; color: #eef4e2; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 6px 9px; font-size: 18px;"
        " font-family: monospace; }"
        "QPlainTextEdit { background: #10161e; color: #cfe6b8; border: 1px solid"
        " #3a4a5e; border-radius: 3px; font-family: monospace; font-size: 17px; }"
        "QSpinBox { background: #1c2430; color: #eef4e2; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 4px 6px; font-size: 18px;"
        " font-weight: bold; min-width: 72px; }"
        "QCheckBox { color: #b8c8d8; font-size: 14px; }"
        "QCheckBox::indicator { width: 17px; height: 17px; }"
        "QPushButton { background: #24303e; color: #dde7f0; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 8px 12px; font-size: 14px;"
        " font-weight: bold; }"
        "QPushButton:hover { border-color: #6aa5d8; }"
        "QPushButton:checked { background: #8a2727; border-color: #e05d5d;"
        " color: #ffe8e8; }");

    auto* g = new QGridLayout(this);
    g->setContentsMargins(12, 10, 12, 10);
    g->setHorizontalSpacing(8);
    g->setVerticalSpacing(8);

    // Row 0: speed, TUNE, STOP.
    g->addWidget(new QLabel(QStringLiteral("WPM"), this), 0, 0);
    m_wpm = new QSpinBox(this);
    m_wpm->setObjectName(QStringLiteral("orionCwWpm"));
    m_wpm->setRange(5, 60);
    m_wpm->setValue(m_cwx ? m_cwx->speed() : 25);
    m_wpm->setToolTip(tr("Keying speed. The WinKeyer's physical speed pot also sets this —\n"
                         "turn the knob and this box follows."));
    g->addWidget(m_wpm, 0, 1);
    m_tuneBtn = new QPushButton(QStringLiteral("TUNE"), this);
    m_tuneBtn->setObjectName(QStringLiteral("orionCwTune"));
    m_tuneBtn->setCheckable(true);
    m_tuneBtn->setToolTip(tr("Steady key-down for tuning (click again to stop)"));
    // Shown only once a host installs a handler: a TUNE button that keys
    // nothing is worse than no button at all.
    m_tuneBtn->setVisible(false);
    g->addWidget(m_tuneBtn, 0, 2);
    auto* stopBtn = new QPushButton(QStringLiteral("STOP (Esc)"), this);
    stopBtn->setObjectName(QStringLiteral("orionCwStop"));
    stopBtn->setToolTip(tr("Dump the keying buffer immediately — same as touching the paddle"));
    g->addWidget(stopBtn, 0, 3);
    // 0-BEAT lives here too: with this window open the type-ahead line owns
    // the keyboard, and the VFO box's own button is a long way from the
    // hand that is typing.
    m_zeroBeatBtn = new QPushButton(QStringLiteral("0-BEAT"), this);
    m_zeroBeatBtn->setObjectName(QStringLiteral("orionCwZeroBeat"));
    m_zeroBeatBtn->setToolTip(tr("Zero-beat the station you are copying — the same correction\n"
                                 "as the VFO box's Zero Beat, without leaving the keyboard."));
    m_zeroBeatBtn->setVisible(false);
    g->addWidget(m_zeroBeatBtn, 0, 4);
    connect(m_zeroBeatBtn, &QPushButton::clicked, this, [this] {
        if (m_zeroBeat) {
            m_zeroBeat();
            updateStatus(tr("0-beat"));
        }
    });

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("orionCwStatus"));
    g->addWidget(m_status, 0, 5);

    // Row 1: the type-ahead line.
    m_line = new QLineEdit(this);
    m_line->setObjectName(QStringLiteral("orionCwLine"));
    m_line->setPlaceholderText(tr("type CW…  Enter sends the line"));
    m_line->setToolTip(tr("Buffered send: type, Enter transmits the line.\n"
                          "Live keys: every keystroke goes straight to the keyer\n"
                          "(Backspace unsends a not-yet-sent character).\n"
                          "Macro fields in square brackets expand here too.\n"
                          "Esc or the paddle stops everything instantly."));
    m_line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    g->addWidget(m_line, 1, 0, 1, 6);

    // Row 2: what went out this over, and the two send styles. Neither box
    // = the whole line waits for Enter; Word keys = each word goes out when
    // the space bar lands; Live keys = every keystroke immediately.
    m_sentView = new QLabel(this);
    m_sentView->setObjectName(QStringLiteral("orionCwSent"));
    m_sentView->setStyleSheet(QStringLiteral("color: #a4cf82; font-family: monospace;"
                                             " font-size: 15px;"));
    g->addWidget(m_sentView, 2, 0, 1, 2);
    m_word = new QCheckBox(tr("Word keys"), this);
    m_word->setObjectName(QStringLiteral("orionCwWord"));
    m_word->setChecked(setting(QStringLiteral("OrionCw/word"), false) == QLatin1String("true"));
    m_word->setToolTip(tr("Hold letters until the space bar — each completed word is sent as a "
                          "unit.\nBackspace fixes the word you're typing before it goes out.\n"
                          "Enter sends whatever's left on the line."));
    g->addWidget(m_word, 2, 2);
    m_live = new QCheckBox(tr("Live keys"), this);
    m_live->setObjectName(QStringLiteral("orionCwLive"));
    m_live->setChecked(setting(QStringLiteral("OrionCw/live"), false) == QLatin1String("true"));
    m_live->setToolTip(tr("Stream each keystroke to the keyer as you type instead of\n"
                          "waiting for Enter (classic keyboard-keyer feel)"));
    g->addWidget(m_live, 2, 3);
    if (m_live->isChecked() && m_word->isChecked()) {   // stale settings guard
        m_word->setChecked(false);
    }

    // Row 3: the DX call, then the memories that spend it.
    auto* memRow = new QWidget(this);
    auto* memOuter = new QVBoxLayout(memRow);
    memOuter->setContentsMargins(0, 0, 0, 0);
    memOuter->setSpacing(6);
    auto* memLay = new QHBoxLayout;
    memLay->setContentsMargins(0, 0, 0, 0);
    memLay->setSpacing(8);
    memOuter->addLayout(memLay);
    memLay->addWidget(new QLabel(QStringLiteral("DX"), memRow));
    m_dxCall = new QLineEdit(memRow);
    m_dxCall->setObjectName(QStringLiteral("orionCwDxCall"));
    m_dxCall->setPlaceholderText(tr("call"));
    m_dxCall->setMaximumWidth(160);
    m_dxCall->setToolTip(tr("The station you're working — what [his call] sends.\n"
                            "Filled by clicking a spot, a skimmer call, or double-clicking\n"
                            "a call in the decode pane; type here when the copy is broken\n"
                            "up or there's nothing to click. The Log QSO window shares it.\n"
                            "Amber = doesn't look like a callsign yet (sends anyway)."));
    memLay->addWidget(m_dxCall);
    memLay->addStretch(1);

    // F1-F12 on three rows of four under the DX box: four across is the
    // width the window already had, so more keys cost height only.
    auto* fkeyGrid = new QGridLayout;
    fkeyGrid->setContentsMargins(0, 0, 0, 0);
    fkeyGrid->setHorizontalSpacing(6);
    fkeyGrid->setVerticalSpacing(4);
    for (int i = 0; i < kFkeys; ++i) {
        m_mem[i] = new QPushButton(memRow);
        m_mem[i]->setObjectName(QStringLiteral("orionCwF") + QString::number(i + 1));
        m_mem[i]->setContextMenuPolicy(Qt::CustomContextMenu);
        m_mem[i]->setFocusPolicy(Qt::NoFocus);   // typing stays in the line
        applyFkeyButton(i);
        fkeyGrid->addWidget(m_mem[i], i / 4, i % 4);
        fkeyGrid->setColumnStretch(i % 4, 1);
        connect(m_mem[i], &QPushButton::clicked, this,
                [this, i] { sendText(fkeySpec(i).section(QLatin1Char('|'), 1)); });
        connect(m_mem[i], &QWidget::customContextMenuRequested, this,
                [this, i] { editMemory(i); });
        // The key itself, while this window has focus. WindowShortcut
        // scope, so AetherSDR's own F1-F12 set and this one never meet.
        auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + i), this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this,
                [this, i] { sendText(fkeySpec(i).section(QLatin1Char('|'), 1)); });
    }
    memOuter->addLayout(fkeyGrid);
    g->addWidget(memRow, 3, 0, 1, 6);

    connect(m_dxCall, &QLineEdit::textEdited, this, [this](const QString& t) {
        // Uppercase as it lands, and drop whitespace outright: no call has
        // a space in it, so pasting the decoder's own broken "K8 ABC"
        // yields K8ABC instead of a call the macro would key with a hole.
        QString up = t.toUpper();
        up.remove(QLatin1Char(' '));
        if (up != t) {
            const int pos = m_dxCall->cursorPosition();
            m_dxCall->setText(up);
            m_dxCall->setCursorPosition(std::min(pos, static_cast<int>(up.size())));
        }
        pushDxCall(up);
    });
    // textEdited covers typing, but a call can also arrive by paste, by a
    // keyboard shortcut or from another window, and all of those have to
    // reach the Log QSO window too — so read the box on every change.
    connect(m_dxCall, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (t.trimmed().toUpper() != m_hisCall) {
            pushDxCall(t.trimmed().toUpper());
        }
    });
    connect(m_dxCall, &QLineEdit::returnPressed, this, [this] {
        announceHisCall();
        m_line->setFocus();   // keyboard back where CW is typed
    });
    connect(m_dxCall, &QLineEdit::editingFinished, this, [this] { announceHisCall(); });

    // Row 4: the CW reader — AetherSDR's own decoder, in the console's pane.
    m_rxOn = new QCheckBox(tr("RX decode"), this);
    m_rxOn->setObjectName(QStringLiteral("orionCwRxOn"));
    m_rxOn->setChecked(setting(QStringLiteral("OrionCw/rxDecode"), true) != QLatin1String("false"));
    m_rxOn->setToolTip(tr("Decode the tuned CW signal. Best results: click the signal\n"
                          "so the carrier sits on your sidetone pitch."));
    g->addWidget(m_rxOn, 4, 0);
    m_rxInfo = new QLabel(this);
    m_rxInfo->setObjectName(QStringLiteral("orionCwRxInfo"));
    m_rxInfo->setStyleSheet(QStringLiteral("color: #6aa5d8;"));
    // Always rich text: with AutoText the tagless placeholder string is
    // detected as PLAIN text and the entities print literally.
    m_rxInfo->setTextFormat(Qt::RichText);
    // Reserve the widest text this label will ever show: its width must
    // never change at runtime, or the stretched grid re-negotiates and the
    // whole row bounces at the pitch-update rate.
    m_rxInfo->setMinimumWidth(
        m_rxInfo->fontMetrics().horizontalAdvance(QStringLiteral("88 WPM · 8888 Hz")) + 12);
    g->addWidget(m_rxInfo, 4, 1);
    // Which reader is doing the copy. The operator's pick is remembered
    // across restarts; AetherSDR's own is the first-run default.
    m_decoder = new QComboBox(this);
    m_decoder->setObjectName(QStringLiteral("orionCwDecoder"));
    m_decoder->setToolTip(tr("Which CW decoder reads the air.\n"
                             "Your choice is remembered the next time you start."));
    g->addWidget(m_decoder, 4, 2);
    connect(m_decoder, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx < 0 || !m_chooseDecoder) {
            return;
        }
        const QString key = m_decoder->itemData(idx).toString();
        if (key.isEmpty()) {
            return;
        }
        saveSetting(QStringLiteral("OrionCw/decoder"), key);
        m_chooseDecoder(key);
        updateStatus(tr("reader: %1").arg(m_decoder->itemText(idx)));
    });

    auto* rxClear = new QPushButton(tr("Clear"), this);
    rxClear->setObjectName(QStringLiteral("orionCwRxClear"));
    g->addWidget(rxClear, 4, 3);
    connect(rxClear, &QPushButton::clicked, this, &OrionCwWindow::clearRx);

    // Row 5: the decode readout.
    m_rx = new QPlainTextEdit(this);
    m_rx->setObjectName(QStringLiteral("orionCwRx"));
    m_rx->setReadOnly(true);
    m_rx->setMinimumHeight(120);
    m_rx->setToolTip(tr("Decoded CW. Double-click a callsign to put it in the DX box\n"
                        "and the Log QSO window."));
    m_rx->viewport()->installEventFilter(this);
    g->addWidget(m_rx, 5, 0, 1, 6);
    g->setRowStretch(5, 1);

    connect(m_rxOn, &QCheckBox::toggled, this, [this](bool on) {
        saveSetting(QStringLiteral("OrionCw/rxDecode"), on);
        emit rxDecodeWanted(on && isVisible());
    });

    // The keyer's own speed, through AetherSDR's CW speed control.
    connect(m_wpm, &QSpinBox::valueChanged, this, [this](int v) {
        if (m_cwx) {
            m_cwx->setSpeed(v);
        }
        updateStatus();
    });
    if (m_cwx) {
        // The WinKeyer's pot moves the model; follow it without echoing back.
        connect(m_cwx, &CwxModel::speedChanged, this, [this](int wpm) {
            if (wpm > 0 && wpm != m_wpm->value()) {
                const QSignalBlocker block(m_wpm);
                m_wpm->setValue(wpm);
                updateStatus();
            }
        });
    }
    connect(m_live, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            m_word->setChecked(false);   // one send style at a time
        }
        saveSetting(QStringLiteral("OrionCw/live"), on);
        if (m_cwx) {
            m_cwx->setLive(on);
        }
        m_prevLen = 0;
        m_line->clear();
    });
    connect(m_word, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            m_live->setChecked(false);
        }
        saveSetting(QStringLiteral("OrionCw/word"), on);
        m_prevLen = 0;
        m_line->clear();
    });
    connect(stopBtn, &QPushButton::clicked, this, [this] {
        if (m_cwx) {
            m_cwx->clearBuffer();
            m_cwx->resetDrainWatch();
        }
        if (m_tuneBtn->isChecked()) {
            m_tuneBtn->setChecked(false);
        }
        m_sentView->clear();
        m_prevLen = 0;
        m_line->clear();
        updateStatus(tr("stopped"));
    });
    connect(m_tuneBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_tune) {
            m_tune(on);
        }
        updateStatus(on ? tr("TUNE — key down") : QString());
    });
    connect(m_line, &QLineEdit::returnPressed, this, [this] {
        if (m_live->isChecked()) {
            m_prevLen = 0;
            m_line->clear();
            return;
        }
        sendLine();
    });
    connect(m_line, &QLineEdit::textEdited, this, [this](const QString& t) {
        // Word keys: completed words (everything up to the last space)
        // leave for the keyer; the word still being typed stays editable
        // in the line. The trailing space rides along = the word gap.
        if (m_word->isChecked()) {
            const int sp = t.lastIndexOf(QLatin1Char(' '));
            if (sp < 0) {
                return;
            }
            const QString out = expandCwMacro(t.left(sp + 1), macroFieldsNow());
            if (!out.simplified().isEmpty() && m_cwx) {
                m_cwx->send(out);
                m_sentView->setText((m_sentView->text() + out).right(60));
            }
            m_line->setText(t.mid(sp + 1));
            return;
        }
        if (!m_live->isChecked()) {
            return;
        }
        // Stream the delta; backspace unsends if the char hasn't gone out.
        if (!m_cwx) {
            return;
        }
        if (t.length() < m_prevLen) {
            m_cwx->erase(m_prevLen - t.length());
        } else if (t.length() > m_prevLen) {
            const QString add = expandCwMacro(t.mid(m_prevLen), macroFieldsNow());
            for (const QChar& c : add) {
                m_cwx->sendChar(QString(c));
            }
            m_sentView->setText((m_sentView->text() + add).right(60));
        }
        m_prevLen = t.length();
    });

    if (m_ctx) {
        connect(m_ctx, &orion::logbook::QsoContext::changed, this,
                &OrionCwWindow::applyContext);
    }

    resize(760, 620);
    updateRxInfo();
    updateStatus();
}

void OrionCwWindow::setTuneHandler(std::function<void(bool)> handler)
{
    m_tune = std::move(handler);
    if (m_tuneBtn) {
        m_tuneBtn->setVisible(static_cast<bool>(m_tune));
    }
}

void OrionCwWindow::setZeroBeatHandler(std::function<void()> handler)
{
    m_zeroBeat = std::move(handler);
    if (m_zeroBeatBtn) {
        m_zeroBeatBtn->setVisible(static_cast<bool>(m_zeroBeat));
    }
}

// A readable name for each decoder key.
static QString decoderLabel(const QString& key)
{
    if (key == QLatin1String("ggmorse")) return QObject::tr("AetherSDR");
    if (key == QLatin1String("orion-fldigi")) return QObject::tr("Orion (fldigi)");
    if (key == QLatin1String("orion-bayes")) return QObject::tr("Orion (Bayes)");
    if (key == QLatin1String("deepfist")) return QObject::tr("DeepFist");
    return key;
}

void OrionCwWindow::setDecoders(const QStringList& keys, const QString& current,
                                std::function<void(const QString&)> choose)
{
    m_chooseDecoder = {};   // filling the box must not look like a choice
    const QSignalBlocker block(m_decoder);
    m_decoder->clear();
    for (const QString& key : keys) {
        m_decoder->addItem(decoderLabel(key), key);
    }
    const int at = m_decoder->findData(current);
    if (at >= 0) {
        m_decoder->setCurrentIndex(at);
    }
    m_decoder->setVisible(keys.size() > 1);
    m_chooseDecoder = std::move(choose);
}

void OrionCwWindow::setStationFields(const QString& call, const QString& name, const QString& qth,
                                     const QString& grid)
{
    m_myCall = call.trimmed().toUpper();
    m_myName = name.trimmed();
    m_myQth = qth.trimmed();
    m_myGrid = grid.trimmed().toUpper();
    for (int i = 0; i < kFkeys; ++i) {
        applyFkeyButton(i);
    }
}

void OrionCwWindow::setHisCall(const QString& call)
{
    m_hisCall = call.trimmed().toUpper();
    // A call that arrived by CLICK was already announced by the owner of
    // that click — mark it pushed so spending it doesn't announce twice.
    m_pushedCall = m_hisCall;
    if (!m_dxCall) {
        return;
    }
    m_dxCall->setText(m_hisCall);   // textEdited only fires on typing
    tintDxCall();
    if (m_ctx && !m_ctxEcho) {
        m_ctxEcho = true;
        m_ctx->setCall(m_hisCall);
        m_ctxEcho = false;
    }
    for (int i = 0; i < kFkeys; ++i) {
        applyFkeyButton(i);
    }
}

QString OrionCwWindow::hisCall() const
{
    return m_hisCall;
}

void OrionCwWindow::appendRx(const QString& text)
{
    if (text.isEmpty()) {
        return;
    }
    m_rx->moveCursor(QTextCursor::End);
    m_rx->insertPlainText(text);
    m_rx->moveCursor(QTextCursor::End);
    // Keep it bounded on long monitoring sessions.
    if (m_rx->document()->characterCount() > 4000) {
        QTextCursor c(m_rx->document());
        c.setPosition(0);
        c.setPosition(1000, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        m_rx->moveCursor(QTextCursor::End);
    }
}

void OrionCwWindow::setRxStats(float pitchHz, float wpm)
{
    m_rxWpm = wpm > 0.0f ? static_cast<int>(std::lround(wpm)) : 0;
    m_rxPitch = pitchHz > 0.0f ? static_cast<double>(pitchHz) : -1.0;
    updateRxInfo();
}

void OrionCwWindow::clearRx()
{
    m_rx->clear();
}

void OrionCwWindow::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    m_line->setFocus();
    updateStatus();
    emit rxDecodeWanted(m_rxOn->isChecked());
}

void OrionCwWindow::hideEvent(QHideEvent* e)
{
    QDialog::hideEvent(e);
    // A spontaneous hide comes from the window system — the main window
    // being minimized takes this one with it. Only a real close stops the
    // reader.
    if (!e->spontaneous()) {
        emit rxDecodeWanted(false);
    }
}

void OrionCwWindow::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {   // Esc = stop, never close
        if (m_cwx) {
            m_cwx->clearBuffer();
            m_cwx->resetDrainWatch();
        }
        if (m_tuneBtn->isChecked()) {
            m_tuneBtn->setChecked(false);
        }
        m_sentView->clear();
        m_prevLen = 0;
        m_line->clear();
        updateStatus(tr("stopped"));
        return;
    }
    QDialog::keyPressEvent(e);
}

// The fldigi gesture: double-click a decoded token; if it's shaped like a
// callsign it goes to the DX box. Token = the whitespace-delimited run
// around the click — Qt's own word selection would split "W1AW/4".
bool OrionCwWindow::eventFilter(QObject* obj, QEvent* ev)
{
    if (m_rx && obj == m_rx->viewport() && ev->type() == QEvent::MouseButtonDblClick) {
        auto* me = static_cast<QMouseEvent*>(ev);
        const QTextCursor c = m_rx->cursorForPosition(me->pos());
        const QString line = c.block().text();
        const int i = std::min(c.positionInBlock(), static_cast<int>(line.size()) - 1);
        if (line.isEmpty() || i < 0) {
            return true;
        }
        int a = i;
        int b = i;
        while (a > 0 && !line[a - 1].isSpace()) {
            --a;
        }
        while (b < static_cast<int>(line.size()) && !line[b].isSpace()) {
            ++b;
        }
        QString tok = line.mid(a, b - a).toUpper();
        tok.remove(QRegularExpression(QStringLiteral("[^A-Z0-9/]")));
        if (callRe().match(tok).hasMatch()) {
            QTextCursor sel = c;   // show what was grabbed
            sel.setPosition(c.block().position() + a);
            sel.setPosition(c.block().position() + b, QTextCursor::KeepAnchor);
            m_rx->setTextCursor(sel);
            setHisCall(tok);
            emit callDoubleClicked(tok);
        }
        return true;   // suppress Qt's word-select
    }
    return QDialog::eventFilter(obj, ev);
}

CwMacroFields OrionCwWindow::macroFieldsNow() const
{
    CwMacroFields f;
    f.myCall = m_myCall;
    f.myName = m_myName;
    f.myQth = m_myQth;
    f.myGrid = m_myGrid;
    f.hisCall = m_hisCall;
    if (m_ctx) {
        f.hisHandle = m_ctx->handle();
        f.hisName = m_ctx->name();
        f.hisQth = m_ctx->qth();
        f.hisGrid = m_ctx->grid();
        f.rstSent = m_ctx->rstSent();
        f.rstRcvd = m_ctx->rstRcvd();
    }
    f.greeting = cwGreetingFor(f.hisGrid);
    return f;
}

void OrionCwWindow::sendLine()
{
    sendText(m_line->text());
    m_line->clear();
}

void OrionCwWindow::sendText(const QString& text)
{
    // A field the macro asks for that nobody filled used to key a hole in
    // the message. Refuse, and name the box that is empty — the DX box and
    // the Log QSO window are both a glance away.
    QString missing;
    const QString out = expandCwMacro(text, macroFieldsNow(), &missing);
    if (!missing.isEmpty()) {
        updateStatus(tr("nothing in %1 — fill it first").arg(missing));
        if (missing == QLatin1String("his call")) {
            m_dxCall->setFocus();
        }
        return;
    }
    if (out.isEmpty() || !m_cwx) {
        return;
    }
    if (!m_cwx->canSend()) {
        updateStatus(tr("the radio will not take CW text right now"));
        return;
    }
    // Sending his call ON THE AIR is the moment the QSO is real — make
    // sure the rest of the station has it even if the box never lost focus
    // (keyboard-only operating: type the call, hit the macro key).
    if (!m_hisCall.isEmpty() && out.contains(m_hisCall)) {
        announceHisCall();
    }
    m_cwx->send(out);
    m_sentView->setText(out.right(60));
    updateStatus();
}

QString OrionCwWindow::fkeySpec(int i) const
{
    return setting(fkeyKey(i), QLatin1String(kFkeyDefault[i]));
}

void OrionCwWindow::applyFkeyButton(int i)
{
    if (!m_mem[i]) {
        return;
    }
    const QString spec = fkeySpec(i);
    const QString label = spec.section(QLatin1Char('|'), 0, 0).trimmed();
    const QString text = spec.section(QLatin1Char('|'), 1);
    m_mem[i]->setText(QStringLiteral("F%1  %2")
                          .arg(i + 1)
                          .arg(label.isEmpty() ? QStringLiteral("—") : label));
    // The expanded message on hover: the one place to check a macro
    // without keying it.
    const QString live = expandCwMacro(text, macroFieldsNow());
    m_mem[i]->setToolTip(tr("F%1  %2\n\nsends:  %3\nright now:  %4\n\n"
                            "click or press F%1 to send    right-click to edit")
                             .arg(i + 1)
                             .arg(label, text,
                                  live.isEmpty() ? tr("(nothing)") : live));
}

void OrionCwWindow::applyContext()
{
    if (!m_ctx) {
        return;
    }
    if (!m_ctxEcho) {
        m_ctxEcho = true;
        if (m_dxCall->text() != m_ctx->call()) {
            m_dxCall->setText(m_ctx->call());
        }
        m_hisCall = m_ctx->call();
        tintDxCall();
        m_ctxEcho = false;
    }
    // Every tooltip quotes live field values, so they go stale together
    // whenever the shared record moves.
    for (int i = 0; i < kFkeys; ++i) {
        applyFkeyButton(i);
    }
}

// The DX box -> everything that cares: the macros, the tint and the shared
// record the Log QSO window reads.
void OrionCwWindow::pushDxCall(const QString& call)
{
    m_hisCall = call;
    tintDxCall();
    if (m_ctx && !m_ctxEcho) {
        m_ctxEcho = true;
        m_ctx->setCall(m_hisCall);   // the Log QSO window fills with it
        m_ctxEcho = false;
    }
    for (int i = 0; i < kFkeys; ++i) {
        applyFkeyButton(i);
    }
}

void OrionCwWindow::announceHisCall()
{
    if (m_hisCall.isEmpty() || m_hisCall == m_pushedCall) {
        return;
    }
    m_pushedCall = m_hisCall;
    emit hisCallEntered(m_hisCall);
}

void OrionCwWindow::tintDxCall()
{
    // Amber while it isn't call-shaped — a hint, never a block: the
    // operator gets to send whatever he means to send.
    const bool ok = m_hisCall.isEmpty() || callRe().match(m_hisCall).hasMatch();
    m_dxCall->setStyleSheet(ok ? QString() : QStringLiteral("color: #e0b45d;"));
}

// "18 WPM · 547 Hz" — green when the tone is within +/-10 Hz of the
// operator's sidetone pitch and amber when off: on-the-note at a glance.
// Both slots always render (em-dash placeholders) so the text width stays
// constant, and the shown Hz only moves when the measurement really moved
// (>1.5 Hz) — no last-digit flicker.
void OrionCwWindow::updateRxInfo()
{
    if (m_rxPitch < 0) {
        m_shownHz = -1.0;
    } else if (m_shownHz < 0 || std::abs(m_rxPitch - m_shownHz) > 1.5) {
        m_shownHz = m_rxPitch;
    }
    const QString wpmPart = m_rxWpm > 0 ? QStringLiteral("%1 WPM").arg(m_rxWpm)
                                        : QStringLiteral("— WPM");
    QString hzPart = QStringLiteral("— Hz");
    if (m_shownHz > 0) {
        const int target =
            AppSettings::instance().value(QStringLiteral("CwSidetonePitch"), 600).toInt();
        const bool on = std::abs(m_shownHz - target) <= 10.0;
        hzPart = QStringLiteral("<span style='color:%1'>%2 Hz</span>")
                     .arg(on ? QStringLiteral("#8fd48f") : QStringLiteral("#e0b060"))
                     .arg(qRound(m_shownHz));
    }
    m_rxInfo->setText(wpmPart + QStringLiteral("&nbsp;·&nbsp;") + hzPart);
}

void OrionCwWindow::updateStatus(const QString& s)
{
    const QString base = tr("%1 WPM").arg(m_wpm->value());
    m_status->setText(s.isEmpty() ? base : base + QStringLiteral(" · ") + s);
}

void OrionCwWindow::editMemory(int i)
{
    const QString key = fkeyKey(i);
    const QString spec = fkeySpec(i);
    QDialog d(this);
    d.setWindowTitle(tr("F%1 — CW macro").arg(i + 1));
    auto* form = new QFormLayout(&d);
    auto* label = new QLineEdit(spec.section(QLatin1Char('|'), 0, 0), &d);
    label->setMaxLength(10);
    auto* text = new QLineEdit(spec.section(QLatin1Char('|'), 1), &d);
    text->setMinimumWidth(420);
    form->addRow(tr("Button says"), label);
    form->addRow(tr("Sends"), text);

    // The field chips: click one and it lands at the cursor. The point of
    // plain-language fields is that you can READ them back later — not
    // that you can spell them from memory now.
    auto* chipBox = new QWidget(&d);
    auto* chips = new QGridLayout(chipBox);
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(4);
    int n = 0;
    for (const CwFieldInfo& fi : cwMacroFields()) {
        auto* c = new QPushButton(QLatin1String(fi.token), chipBox);
        c->setFocusPolicy(Qt::NoFocus);
        c->setToolTip(QLatin1String(fi.what));
        chips->addWidget(c, n / 3, n % 3);
        ++n;
        const QString tok = QLatin1String(fi.token);
        connect(c, &QPushButton::clicked, text, [text, tok] {
            text->insert(tok);
            text->setFocus();
        });
    }
    form->addRow(tr("Fields"), chipBox);

    auto* preview = new QLabel(&d);
    preview->setWordWrap(true);
    preview->setStyleSheet(QStringLiteral("QLabel { color: #6aa5d8; }"));
    const auto refresh = [this, text, preview] {
        QString missing;
        const QString out = expandCwMacro(text->text(), macroFieldsNow(), &missing);
        preview->setText(out.isEmpty()
                             ? tr("(nothing to send)")
                             : (missing.isEmpty()
                                    ? out
                                    : out + tr("      [%1 is empty]").arg(missing)));
    };
    connect(text, &QLineEdit::textChanged, &d, refresh);
    refresh();
    form->addRow(tr("Right now"), preview);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::RestoreDefaults
                                        | QDialogButtonBox::Cancel,
                                    &d);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, [&] {
        saveSetting(key, label->text().trimmed() + QLatin1Char('|') + text->text().trimmed());
        d.accept();
    });
    connect(bb->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &d, [&] {
        saveSetting(key, QLatin1String(kFkeyDefault[i]));
        d.accept();
    });
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    d.exec();
    applyFkeyButton(i);
}

}  // namespace AetherSDR
