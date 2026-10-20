#include "gui/orion/OrionLogbookUi.h"

#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include <QDesktopServices>
#include <QUrl>
#include <QLocale>
#include "core/MaidenheadLocator.h"
#include "core/CtyDatParser.h"
#include "core/backends/orion/log/LogbookIndex.h"
#include "core/CallsignInfo.h"
#include "core/CallsignLookupService.h"
#include "core/backends/orion/log/CqrlogSync.h"
#include "core/backends/orion/log/OrionLogbook.h"
#include "core/backends/orion/log/OrionUploadSecrets.h"
#include "core/backends/orion/log/QslUploader.h"
#include "core/backends/orion/log/QsoContext.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/SpotModel.h"

#include <QAction>
#include <QHash>
#include <QCheckBox>
#include <QGroupBox>
#include <QListWidget>
#include <QScrollArea>
#include <QSpinBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSqlTableModel>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTableView>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <tuple>

namespace AetherSDR {

using orion::logbook::CqrlogSync;
using orion::logbook::OrionUploadSecrets;
using orion::logbook::QslUploader;
using orion::logbook::OrionLogbook;
using orion::logbook::Qso;
using orion::logbook::QsoContext;

namespace {

// The logbook the spot colouring reads (one per application).
QPointer<OrionLogbook> g_book;

// The station being worked right now, shared with the CW window (one per
// application; see log/QsoContext.h).
QPointer<QsoContext> g_qsoContext;

bool workedColoursOn()
{
    return AppSettings::instance().value(QStringLiteral("OrionLog/workedColours"), true).toBool();
}

// AetherSDR's slice mode -> the ADIF mode the log and cqrlog use.
QString adifMode(const QString& sliceMode)
{
    const QString m = sliceMode.trimmed().toUpper();
    if (m == QLatin1String("USB") || m == QLatin1String("LSB")) return QStringLiteral("SSB");
    if (m.startsWith(QLatin1String("CW"))) return QStringLiteral("CW");
    if (m == QLatin1String("DIGU") || m == QLatin1String("DIGL")) return QStringLiteral("DATA");
    return m;
}

QString defaultRst(const QString& adif)
{
    return adif == QLatin1String("CW") || adif == QLatin1String("DATA") ? QStringLiteral("599")
                                                                        : QStringLiteral("59");
}

// Callsign shape, incl. portable forms. A partial call typed on the way to
// a whole one must not go to the callbook: it costs a round trip, it
// answers "not found", and its late reply has to be thrown away.
const QRegularExpression& callShape()
{
    static const QRegularExpression re(QStringLiteral("^(?:[A-Z0-9]{1,3}/)?"
                                                      "(?:[A-Z]{1,2}|[0-9][A-Z]|[A-Z][0-9])"
                                                      "[0-9]{1,2}[A-Z]{1,4}"
                                                      "(?:/[A-Z0-9]{1,4})?$"));
    return re;
}

// The worked-before ramp, the same colours the spot markers use.
const char* workedColour(QChar st)
{
    switch (st.toLatin1()) {
    case 'N': return "#ff5c46";    // never worked — a new one
    case 'B': return "#ffbe3c";    // worked, not this band
    case 'W': return "#82de8c";    // worked this band
    case 'C': return "#96a2b2";    // confirmed
    default: return "#dde7f0";     // no log data yet
    }
}

QString neededText(QChar st, const QString& what)
{
    switch (st.toLatin1()) {
    case 'N': return what + QObject::tr(" \u2014 needed");
    case 'W': return what + QObject::tr(" worked, unconfirmed");
    case 'C': return what + QObject::tr(" confirmed");
    default: return what + QObject::tr(" \u2014 no log data");
    }
}

// The Log QSO window: the console's New-QSO flow in AetherSDR's widgets.
class OrionQsoDialog : public QDialog {
    Q_OBJECT

signals:
    // Him on the map, and him on the cluster: the host owns both, so the
    // window only says what it wants.
    void mapRequested(double lat, double lon, const QString& call);
    void spotRequested(const QString& call, double freqMhz, const QString& comment);

public:
    OrionQsoDialog(OrionLogbook* book, RadioModel* model,
                   std::function<SliceModel*()> activeSlice, QWidget* parent)
        : QDialog(parent)
        , m_book(book)
        , m_model(model)
        , m_activeSlice(std::move(activeSlice))
    {
        setWindowTitle(tr("Log QSO"));
        setObjectName(QStringLiteral("orionLogQsoWindow"));
        // Four fields across need the width to show them; the console's
        // window is this shape too. Squeezed into 320 px the names and
        // towns were clipped to a few characters.
        setMinimumWidth(720);
        // Every box gets a surface and an edge of its own: on a dark theme
        // an unstyled input is black on black, and the operator cannot see
        // where the fields are — only where the text happens to be. Theme
        // tokens, so it follows the app's theme rather than pinning colours.
        AetherSDR::ThemeManager::instance().applyStyleSheet(this,
            "QLineEdit, QDateTimeEdit {"
            " background: {{color.background.0}};"
            " color: {{color.text.primary}};"
            " border: 1px solid {{color.border.strong}};"
            " border-radius: 3px; padding: 3px 6px; }"
            "QLineEdit:focus, QDateTimeEdit:focus {"
            " border-color: {{color.accent}}; }"
            "QLineEdit:disabled { color: {{color.text.secondary}}; }"
            "QTableWidget { background: {{color.background.0}};"
            " alternate-background-color: {{color.background.1}};"
            " gridline-color: {{color.border.subtle}}; }");
        auto* vbox = new QVBoxLayout(this);

        // ── Call row: the call, the three things you do with it, country ──
        auto* callRow = new QHBoxLayout;
        callRow->setSpacing(6);
        m_call = new QLineEdit;
        m_call->setObjectName(QStringLiteral("orionLogCall"));
        m_call->setPlaceholderText(tr("Callsign"));
        QFont big = m_call->font();
        big.setPointSizeF(big.pointSizeF() * 1.4);
        big.setBold(true);
        m_call->setFont(big);
        m_call->setMaximumWidth(190);
        callRow->addWidget(m_call, 0);

        // The callbook, as a ham means it: the page opens in the browser and
        // the boxes below fill behind it.
        m_qrz = new QPushButton(tr("QRZ"));
        m_qrz->setObjectName(QStringLiteral("orionLogQrz"));
        m_qrz->setFocusPolicy(Qt::NoFocus);
        m_qrz->setToolTip(tr("Open this call on QRZ.com and fill Name, QTH and Grid.\n"
                             "Only empty boxes are filled \u2014 what you typed is never "
                             "overwritten.\nThe fill also runs by itself when a call looks "
                             "complete."));
        callRow->addWidget(m_qrz, 0);
        connect(m_qrz, &QPushButton::clicked, this, [this] {
            const QString call = m_call->text().trimmed().toUpper();
            if (call.isEmpty()) {
                m_status->setText(tr("Enter a callsign first."));
                return;
            }
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.qrz.com/db/") + call));
            lookupCall(true);
        });

        m_globe = new QPushButton(tr("Globe"));
        m_globe->setObjectName(QStringLiteral("orionLogGlobe"));
        m_globe->setFocusPolicy(Qt::NoFocus);
        m_globe->setToolTip(tr("Show this station on the map, with the great-circle path\n"
                               "from your own grid. Needs his grid, or a country from "
                               "cty.dat."));
        callRow->addWidget(m_globe, 0);
        connect(m_globe, &QPushButton::clicked, this, [this] { showOnGlobe(); });

        m_spot = new QPushButton(tr("Spot"));
        m_spot->setObjectName(QStringLiteral("orionLogSpot"));
        m_spot->setFocusPolicy(Qt::NoFocus);
        m_spot->setToolTip(tr("Send this call to the DX cluster you are connected to,\n"
                              "at the frequency in the box below."));
        callRow->addWidget(m_spot, 0);
        connect(m_spot, &QPushButton::clicked, this, [this] { spotCall(); });

        m_country = new QLabel;
        m_country->setObjectName(QStringLiteral("orionLogCountry"));
        m_country->setWordWrap(true);
        callRow->addWidget(m_country, 1);
        vbox->addLayout(callRow);

        // ── Needed badges: country, band, mode ─────────────────────────────
        auto* badges = new QHBoxLayout;
        badges->setSpacing(8);
        for (QLabel** b : {&m_bCountry, &m_bBand, &m_bMode}) {
            *b = new QLabel;
            badges->addWidget(*b);
        }
        m_bCountry->setObjectName(QStringLiteral("orionLogNeedCountry"));
        m_bBand->setObjectName(QStringLiteral("orionLogNeedBand"));
        m_bMode->setObjectName(QStringLiteral("orionLogNeedMode"));
        badges->addStretch(1);
        vbox->addLayout(badges);

        // ── The fields, four across ────────────────────────────────────────
        auto* form = new QGridLayout;
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(6);
        const auto field = [&](int row, int col, const QString& label, QLineEdit*& e,
                               int width, const char* name) {
            auto* l = new QLabel(label);
            l->setStyleSheet(QStringLiteral("QLabel { font-size: 11px; }"));
            form->addWidget(l, row * 2, col);
            e = new QLineEdit;
            e->setObjectName(QLatin1String(name));
            if (width > 0) e->setMaximumWidth(width);
            form->addWidget(e, row * 2 + 1, col);
        };
        field(0, 0, tr("FREQ (MHz)"), m_freq, 130, "orionLogFreq");
        field(0, 1, tr("MODE"), m_mode, 90, "orionLogMode");
        field(0, 2, tr("RST SENT"), m_rstS, 80, "orionLogRstSent");
        field(0, 3, tr("RST RCVD"), m_rstR, 80, "orionLogRstRcvd");
        field(1, 2, tr("NAME"), m_name, 0, "orionLogName");
        field(1, 3, tr("QTH"), m_qth, 0, "orionLogQth");
        field(2, 0, tr("GRID"), m_grid, 130, "orionLogGrid");
        field(2, 1, tr("POTA"), m_pota, 110, "orionLogPota");
        // The name he gives ON THE AIR, routinely not the callbook's (QRZ
        // says Robert, he sends BOB). [his handle] in a CW macro reads this
        // one, and the callbook fill never touches it.
        field(2, 2, tr("HANDLE"), m_handle, 110, "orionLogHandle");
        field(2, 3, tr("COMMENT"), m_comment, 0, "orionLogComment");
        m_pota->setPlaceholderText(tr("e.g. K-1234"));
        m_handle->setPlaceholderText(tr("what he sends"));
        m_handle->setToolTip(tr("The on-air name he gave you. [his handle] in a CW macro "
                                "sends this;\n[his name] sends the callbook's Name."));

        // Date and time keep the one editor the clock drives.
        auto* timeLabel = new QLabel(tr("DATE / TIME (UTC)"));
        timeLabel->setStyleSheet(QStringLiteral("QLabel { font-size: 11px; }"));
        form->addWidget(timeLabel, 2, 0, 1, 2);
        m_time = new QDateTimeEdit;
        m_time->setObjectName(QStringLiteral("orionLogTime"));
        m_time->setTimeZone(QTimeZone::utc());
        m_time->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        form->addWidget(m_time, 3, 0, 1, 2);
        vbox->addLayout(form);

        // ── Rotor bar: where he is, and the two bearings ───────────────────
        auto* rotRow = new QHBoxLayout;
        rotRow->setSpacing(6);
        m_heading = new QLabel;
        m_heading->setObjectName(QStringLiteral("orionLogHeading"));
        rotRow->addWidget(m_heading, 1);
        m_sp = new QPushButton;
        m_sp->setObjectName(QStringLiteral("orionLogShortPath"));
        m_sp->setFocusPolicy(Qt::NoFocus);
        m_sp->setToolTip(tr("Turn the rotor to the short-path bearing."));
        m_lp = new QPushButton;
        m_lp->setObjectName(QStringLiteral("orionLogLongPath"));
        m_lp->setFocusPolicy(Qt::NoFocus);
        m_lp->setToolTip(tr("Turn the rotor to the long-path bearing."));
        rotRow->addWidget(m_sp);
        rotRow->addWidget(m_lp);
        connect(m_sp, &QPushButton::clicked, this, [this] { turnRotor(m_spAz); });
        connect(m_lp, &QPushButton::clicked, this, [this] { turnRotor(m_lpAz); });
        vbox->addLayout(rotRow);
        m_status = new QLabel;
        m_status->setObjectName(QStringLiteral("orionLogStatus"));
        m_status->setWordWrap(true);
        vbox->addWidget(m_status);

        m_prev = new QTableWidget(0, 6);
        m_prev->setObjectName(QStringLiteral("orionLogPrevious"));
        m_prev->setHorizontalHeaderLabels({tr("DATE"), tr("FREQ"), tr("MODE"),
                                           tr("RST"), tr("QSL"), tr("NAME")});
        m_prev->horizontalHeader()->setStretchLastSection(true);
        m_prev->verticalHeader()->setVisible(false);
        m_prev->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_prev->setSelectionMode(QAbstractItemView::NoSelection);
        m_prev->setMinimumHeight(110);
        vbox->addWidget(new QLabel(tr("Previous QSOs with this station:")));
        vbox->addWidget(m_prev);

        auto* buttons = new QDialogButtonBox;
        auto* logBtn = buttons->addButton(tr("LOG QSO"), QDialogButtonBox::AcceptRole);
        logBtn->setObjectName(QStringLiteral("orionLogButton"));
        logBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: #2b62c9; border: 1px solid #4f86ef;"
            " border-radius: 3px; padding: 6px 18px; font-weight: bold; }"));
        auto* clearBtn = buttons->addButton(tr("Clear"), QDialogButtonBox::ResetRole);
        buttons->addButton(QDialogButtonBox::Close);
        logBtn->setDefault(true);
        vbox->addWidget(buttons);
        connect(logBtn, &QPushButton::clicked, this, [this] { logNow(); });
        connect(clearBtn, &QPushButton::clicked, this, [this] {
            clearQsoContext();
            clearForNext(true);
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

        connect(m_call, &QLineEdit::textEdited, this, [this](const QString& t) {
            const int pos = m_call->cursorPosition();
            m_call->setText(t.toUpper());
            m_call->setCursorPosition(pos);
            onCallChanged();
        });
        // Finished typing: leaving the box (Tab, or a click elsewhere) is
        // when the callbook is asked.
        connect(m_call, &QLineEdit::editingFinished, this, [this] { lookupCall(false); });
        // Enter in the callsign moves on rather than logging: LOG QSO is the
        // dialog's default button, so Enter here would otherwise file the
        // QSO while the operator was still filling it in.
        connect(m_call, &QLineEdit::returnPressed, this, [this] {
            lookupCall(false);
            m_rstS->setFocus();
            m_rstS->selectAll();
        });
        // textChanged, not just textEdited: a call can arrive from a spot,
        // from the CW window's DX box or from a paste, and the country line,
        // the needed badges and the bearings have to follow it.
        connect(m_call, &QLineEdit::textChanged, this, [this] { onCallChanged(); });
        connect(m_freq, &QLineEdit::textChanged, this, [this] { updateBadges(); });
        connect(m_mode, &QLineEdit::textChanged, this, [this] { updateBadges(); });
        connect(m_grid, &QLineEdit::textChanged, this, [this] { updateRotorBar(); });
        connect(m_mode, &QLineEdit::textEdited, this, [this] { m_modeEdited = true; });
        connect(m_freq, &QLineEdit::textEdited, this, [this] { m_freqEdited = true; });
        // The clock runs until the operator sets a time by hand.
        connect(m_time, &QDateTimeEdit::dateTimeChanged, this, [this] {
            if (!m_settingTime) m_timeEdited = true;
        });
        m_clock = new QTimer(this);
        m_clock->setInterval(1000);
        connect(m_clock, &QTimer::timeout, this, [this] { tick(); });
        auto& lookup = CallsignLookupService::instance();
        connect(&lookup, &CallsignLookupService::infoReady, this,
                [this](const CallsignInfo& info, bool) { applyLookup(info); });
        connect(&lookup, &CallsignLookupService::lookupFailed, this,
                [this](const QString& call, const QString& message) {
                    if (call.trimmed().toUpper() == m_call->text().trimmed().toUpper()) {
                        m_status->setText(tr("Callbook: %1").arg(message));
                    }
                });
        m_clock->start();
        clearForNext(true);
        // After the form is seeded, not before: clearForNext() empties every
        // box, so binding first adopted the station the CW window was working
        // and then wiped it again.
        bindContext();
    }

    // Fills frequency and mode from the active slice, and the call from a
    // spot sitting on that frequency (a spot click tunes the slice there).
    void refreshFromRadio()
    {
        SliceModel* s = m_activeSlice ? m_activeSlice() : nullptr;
        if (!s) return;
        const double mhz = s->frequency();
        if (!m_freqEdited) {
            m_freq->setText(QString::number(mhz, 'f', 6));
        }
        if (!m_modeEdited) {
            const QString mode = adifMode(s->mode());
            if (mode != m_mode->text()) {
                m_mode->setText(mode);
                m_rstS->setText(defaultRst(mode));
                m_rstR->setText(defaultRst(mode));
            }
        }
        if (m_call->text().trimmed().isEmpty() && m_model) {
            QString best;
            double bestHz = 300.0;
            for (const SpotData& spot : m_model->spotModel().spots()) {
                const double d = std::abs(spot.rxFreqMhz - mhz) * 1e6;
                if (!spot.callsign.isEmpty() && d < bestHz) {
                    bestHz = d;
                    best = spot.callsign;
                }
            }
            if (!best.isEmpty()) {
                m_call->setText(best.toUpper());
                onCallChanged();
                lookupCall(false);   // a spot's call arrives whole
            }
        }
    }

    // WSJT-X is working a station (a decode was clicked): take its call,
    // grid and mode while the form is still empty, as the console does.
    void setFromWsjtx(const QString& call, const QString& grid, const QString& mode)
    {
        if (!m_call->text().trimmed().isEmpty()) {
            return;
        }
        m_call->setText(call.toUpper());
        lookupCall(false);   // likewise whole
        if (m_grid->text().isEmpty()) m_grid->setText(grid.toUpper());
        if (!mode.isEmpty() && !m_modeEdited) {
            m_mode->setText(mode.toUpper());
            m_modeEdited = true;   // WSJT-X's FT8 outranks the rig's USB
        }
        onCallChanged();
    }

private:
    // The shared current-QSO record (log/QsoContext.h), mirrored both ways
    // with the CW window: the call and the report typed here are what the
    // F-key macros send, and a call clicked in the CW window's decode pane
    // or typed in its DX box fills this form. The echo flag keeps the two
    // from writing each other in a loop.
    void bindContext()
    {
        if (!g_qsoContext) return;
        for (QLineEdit* e : {m_call, m_handle, m_name, m_qth, m_grid, m_rstS, m_rstR}) {
            connect(e, &QLineEdit::textEdited, this, [this] { pushContext(); });
        }
        // Typing in a box the callbook filled makes it the operator's.
        for (QLineEdit* e : {m_name, m_qth, m_grid}) {
            connect(e, &QLineEdit::textEdited, this,
                    [this, e] { m_fromCallbook.remove(e); });
        }
        // The form's own fills count too: the reports this window puts up by
        // default (599 on CW) are what [rst sent] has to send, without the
        // operator having to retype them.
        pushContext();
        connect(g_qsoContext, &QsoContext::changed, this, [this] { adoptContext(); });
        adoptContext();   // a station may already be on the CW window's DX box
    }

    // The form -> the shared record the CW macros read.
    void pushContext()
    {
        if (!g_qsoContext || m_ctxEcho) return;
        m_ctxEcho = true;
        g_qsoContext->setCall(m_call->text());
        g_qsoContext->setHandle(m_handle->text());
        g_qsoContext->setName(m_name->text());
        g_qsoContext->setQth(m_qth->text());
        g_qsoContext->setGrid(m_grid->text());
        g_qsoContext->setRstSent(m_rstS->text());
        g_qsoContext->setRstRcvd(m_rstR->text());
        m_ctxEcho = false;
    }

    void clearQsoContext()
    {
        if (g_qsoContext) {
            m_ctxEcho = true;
            g_qsoContext->clear();
            m_ctxEcho = false;
        }
    }

    void adoptContext()
    {
        if (m_ctxEcho || !g_qsoContext) return;
        {
            m_ctxEcho = true;
            // A call arriving from the CW window fills the form the same way
            // a spot does — including the country line and the previous-QSO
            // table — but never overwrites something typed here.
            const QString call = g_qsoContext->call();
            if (!call.isEmpty() && call != m_call->text()) {
                m_call->setText(call);
                onCallChanged();
                lookupCall(false);   // it arrived whole
            }
            const auto fill = [](QLineEdit* e, const QString& v) {
                if (!v.isEmpty() && e->text().isEmpty()) e->setText(v);
            };
            fill(m_handle, g_qsoContext->handle());
            fill(m_name, g_qsoContext->name());
            fill(m_qth, g_qsoContext->qth());
            fill(m_grid, g_qsoContext->grid());
            m_ctxEcho = false;
        }
    }

    void tick()
    {
        if (!m_timeEdited) {
            m_settingTime = true;
            m_time->setDateTime(QDateTime::currentDateTimeUtc());
            m_settingTime = false;
        }
    }

    // The callbook fill. `manual` is the QRZ button: it says so in the
    // status line and asks even when the service would rather wait.
    void lookupCall(bool manual)
    {
        const QString call = m_call->text().trimmed().toUpper();
        if (call.size() < 3) {
            if (manual) m_status->setText(tr("Enter a callsign first."));
            return;
        }
        auto& svc = CallsignLookupService::instance();
        if (!svc.enabled() || (!svc.hasCredentials() && !svc.hasCachedEntry(call))) {
            if (manual) {
                m_status->setText(tr("No callbook login yet — Settings -> Radio Setup -> "
                                     "Callsign Lookup."));
            }
            return;
        }
        if (!manual) {
            if (call == m_lookedUp) {
                return;   // one lookup per call, however the call got here
            }
            // Half a call on the way to a whole one is not a question worth
            // asking: wait until it looks like a callsign.
            if (!callShape().match(call).hasMatch()) {
                return;
            }
        }
        m_lookedUp = call;
        // Say so on BOTH paths. The first lookup of a session also fetches a
        // session key, so an answer can be seconds away, and silence while
        // that happens reads as "it does not work".
        m_status->setText(tr("Asking the callbook about %1...").arg(call));
        svc.lookup(call);
    }

    // What came back. Empty boxes only: the operator's copy outranks the
    // callbook, and the handle is never touched — that is what he SAID.
    void applyLookup(const CallsignInfo& info)
    {
        if (info.call.trimmed().toUpper() != m_call->text().trimmed().toUpper()) {
            return;   // a late answer for a station we have moved off
        }
        if (info.prefixOnly) {
            return;   // country-level only; cty.dat already filled that line
        }
        // A box the operator filled is theirs and is never overwritten; one
        // the callbook filled for a DIFFERENT call is stale and must be.
        const auto fill = [this](QLineEdit* e, const QString& v) {
            if (v.isEmpty()) {
                return;
            }
            if (!e->text().trimmed().isEmpty() && !m_fromCallbook.contains(e)) {
                return;   // the operator's own copy wins
            }
            e->setText(v);
            m_fromCallbook.insert(e);
        };
        QString qth = info.city;
        if (!info.state.isEmpty()) {
            qth += (qth.isEmpty() ? QString() : QStringLiteral(", ")) + info.state;
        }
        fill(m_name, info.displayName() == info.call ? QString() : info.displayName());
        fill(m_qth, qth);
        fill(m_grid, info.grid.toUpper());
        pushContext();   // the CW macros read these
        m_status->setText(tr("Callbook: %1").arg(info.displayName()));
    }

    // Where he is: his own grid if he gave one, else his country's centre.
    bool dxLatLon(double& lat, double& lon) const
    {
        if (MaidenheadLocator::toLatLon(m_grid->text().trimmed(), lat, lon)) {
            return true;
        }
        const QString call = m_call->text().trimmed().toUpper();
        if (call.isEmpty() || !m_book || !m_book->cty()) {
            return false;
        }
        if (const DxccEntity* e = m_book->cty()->entityForCallsign(call)) {
            if (e->hasLatLon) {
                lat = e->latitude;
                lon = e->longitude;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static bool myLatLon(double& lat, double& lon)
    {
        return MaidenheadLocator::toLatLon(
            AppSettings::instance().value(QStringLiteral("OrionCw/myGrid")).toString().trimmed(),
            lat, lon);
    }

    // The bearings and the distance. No rotor is required to show them —
    // knowing where to point the beam is useful whether or not this program
    // can turn it.
    void updateRotorBar()
    {
        m_spAz = m_lpAz = -1.0;
        double myLat = 0, myLon = 0, dxLat = 0, dxLon = 0;
        const bool haveMe = myLatLon(myLat, myLon);
        const bool haveDx = dxLatLon(dxLat, dxLon);
        if (!haveMe || !haveDx) {
            m_sp->setText(tr("SP \u2014"));
            m_lp->setText(tr("LP \u2014"));
            m_sp->setEnabled(false);
            m_lp->setEnabled(false);
            m_heading->setText(haveMe ? QString()
                                      : tr("Set your grid in Radio Setup \u2192 Orion for "
                                           "bearings."));
            return;
        }
        m_spAz = MaidenheadLocator::bearingDeg(myLat, myLon, dxLat, dxLon);
        m_lpAz = m_spAz >= 180.0 ? m_spAz - 180.0 : m_spAz + 180.0;
        const double km = MaidenheadLocator::distanceKm(myLat, myLon, dxLat, dxLon);
        m_sp->setText(tr("SP %1\u00b0").arg(qRound(m_spAz)));
        m_lp->setText(tr("LP %1\u00b0").arg(qRound(m_lpAz)));
        const bool rotor = m_model && m_model->isConnected()
            && m_model->backendDeclaresExtension(QStringLiteral("orion"));
        m_sp->setEnabled(rotor);
        m_lp->setEnabled(rotor);
        const bool miles = QLocale().measurementSystem() != QLocale::MetricSystem;
        m_heading->setText(miles
                               ? tr("%L1 mi").arg(qRound(km * 0.621371))
                               : tr("%L1 km").arg(qRound(km)));
    }

    // The one call that can move the antenna from this window; it goes
    // through the backend's rotor.turn, the same path the rotor tile uses.
    void turnRotor(double degrees)
    {
        if (!(degrees >= 0.0 && degrees <= 360.0) || !m_model) {
            return;
        }
        QVariantMap arg;
        arg.insert(QStringLiteral("azimuth"), degrees);
        m_model->invokeBackendExtension(QStringLiteral("orion"),
                                        QStringLiteral("rotor.turn"), 0, arg);
        m_status->setText(tr("Rotor: turning to %1\u00b0.").arg(qRound(degrees)));
    }

    // Him on AetherSDR's own map, with the path from home.
    void showOnGlobe()
    {
        double myLat = 0, myLon = 0, dxLat = 0, dxLon = 0;
        if (!dxLatLon(dxLat, dxLon)) {
            m_status->setText(tr("No position for this station yet \u2014 a grid or a "
                                 "country from cty.dat is needed."));
            return;
        }
        const QString call = m_call->text().trimmed().toUpper();
        if (myLatLon(myLat, myLon)) {
            const double km = MaidenheadLocator::distanceKm(myLat, myLon, dxLat, dxLon);
            const double az = MaidenheadLocator::bearingDeg(myLat, myLon, dxLat, dxLon);
            m_status->setText(tr("%1: %2\u00b0, %L3 km \u2014 opening the map.")
                                  .arg(call)
                                  .arg(qRound(az))
                                  .arg(qRound(km)));
        } else {
            m_status->setText(tr("%1 \u2014 opening the map.").arg(call));
        }
        emit mapRequested(dxLat, dxLon, call);
    }

    // The call to the cluster this station is connected to.
    void spotCall()
    {
        const QString call = m_call->text().trimmed().toUpper();
        const double mhz = m_freq->text().trimmed().toDouble();
        if (call.isEmpty() || mhz <= 0.0) {
            m_status->setText(tr("A callsign and a frequency are needed to spot."));
            return;
        }
        emit spotRequested(call, mhz, m_comment->text().trimmed());
    }

    // The three "do I need him" badges, and the call in its worked colour.
    void updateBadges()
    {
        const QString call = m_call->text().trimmed().toUpper();
        const QString band =
            orion::logbook::LogDb::bandForHz(
                qint64(std::llround(m_freq->text().trimmed().toDouble() * 1e6)));
        const QString mode = m_mode->text().trimmed().toUpper();
        orion::logbook::LogbookIndex* idx = m_book ? m_book->index() : nullptr;

        const QChar st = (idx && !call.isEmpty()) ? idx->status(call, band) : QChar('?');
        m_call->setStyleSheet(QStringLiteral("QLineEdit { color: %1; }")
                                  .arg(QLatin1String(workedColour(st))));

        const orion::logbook::LogbookIndex::Need need =
            (idx && !call.isEmpty()) ? idx->need(call, band, mode)
                                     : orion::logbook::LogbookIndex::Need{};
        const auto paint = [](QLabel* l, QChar s2, const QString& what) {
            l->setVisible(!what.isEmpty());
            l->setText(neededText(s2, what));
            l->setStyleSheet(QStringLiteral(
                                 "QLabel { border: 1px solid %1; border-radius: 3px;"
                                 " padding: 3px 9px; font-size: 12px; font-weight: bold;"
                                 " color: %1; }")
                                 .arg(QLatin1String(workedColour(s2 == 'W' ? QChar('B') : s2))));
        };
        const QString country = m_country->text().section(QChar(0x00B7), 0, 0).trimmed();
        paint(m_bCountry, need.country, country.isEmpty() ? tr("country") : country);
        paint(m_bBand, need.band, band.isEmpty() ? tr("band") : band);
        paint(m_bMode, need.mode, mode.isEmpty() ? tr("mode") : mode);
        updateRotorBar();
    }

    void onCallChanged()
    {
        const QString call = m_call->text().trimmed().toUpper();
        // Moving to a different callsign retires the last one's callbook
        // details, so a half-typed call's answer cannot survive into the
        // whole one. What the operator typed themselves stays.
        if (call != m_lookedUp && !m_fromCallbook.isEmpty()) {
            for (QLineEdit* e : m_fromCallbook) {
                e->clear();
            }
            m_fromCallbook.clear();
        }
        m_prev->setRowCount(0);
        if (call.size() < 3) {
            m_country->clear();
            return;
        }
        const OrionLogbook::Country c = m_book->country(call);
        m_country->setText(c.known ? tr("%1  (CQ %2, ITU %3)").arg(c.name).arg(c.cqz).arg(c.ituz)
                                   : tr("Country not found in cty.dat"));
        updateBadges();
        // NOT the callbook: that waits until the call is finished (Tab or
        // Enter, or a call that arrives whole from a spot). Asking on every
        // keystroke spends a round trip per character and, worse, answers
        // for the real callsigns you pass through on the way — DL1A is a
        // station in its own right on the way to DL1AB.
        const QList<Qso> prev = m_book->db()->prevQsos(call, 8);
        m_prev->setRowCount(int(prev.size()));
        for (int i = 0; i < prev.size(); ++i) {
            const Qso& q = prev[i];
            const QStringList cells{q.tsUtc.toString(QStringLiteral("yyyy-MM-dd HH:mm")), q.band,
                                    q.mode, q.rstS, q.rstR, q.name};
            for (int c2 = 0; c2 < cells.size(); ++c2) {
                m_prev->setItem(i, c2, new QTableWidgetItem(cells[c2]));
            }
        }
        // Worked before beats the callbook: his own history fills first,
        // and the lookup below then only fills what is still empty.
        lookupCall(false);
        if (!prev.isEmpty()) {
            // Worked before: the operator's name and QTH carry over, as in the
            // console.
            if (m_name->text().isEmpty()) m_name->setText(prev.first().name);
            if (m_handle->text().isEmpty()) m_handle->setText(prev.first().handle);
            if (m_qth->text().isEmpty()) m_qth->setText(prev.first().qth);
            if (m_grid->text().isEmpty()) m_grid->setText(prev.first().grid);
        }
        pushContext();   // the name and handle a macro would send
    }

    void logNow()
    {
        Qso q;
        q.call = m_call->text().trimmed().toUpper();
        if (q.call.isEmpty()) {
            m_status->setText(tr("Enter a callsign first."));
            m_call->setFocus();
            return;
        }
        q.tsUtc = m_time->dateTime().toUTC();
        q.freqHz = qint64(std::llround(m_freq->text().trimmed().toDouble() * 1e6));
        q.mode = m_mode->text().trimmed().toUpper();
        q.rstS = m_rstS->text().trimmed();
        q.rstR = m_rstR->text().trimmed();
        q.name = m_name->text().trimmed();
        q.handle = m_handle->text().trimmed();
        q.qth = m_qth->text().trimmed();
        q.grid = m_grid->text().trimmed().toUpper();
        q.pota = m_pota->text().trimmed().toUpper();
        q.comment = m_comment->text().trimmed();
        q.band = orion::logbook::LogDb::bandForHz(q.freqHz);
        if (m_book->db()->hasNearDuplicate(q)
            && QMessageBox::question(this, tr("Log QSO"),
                                     tr("%1 on %2 %3 is already in the log within the last 5 "
                                        "minutes. Log it again?")
                                         .arg(q.call, q.band, q.mode))
                   != QMessageBox::Yes) {
            return;
        }
        const qint64 id = m_book->logQso(q);
        if (id >= 0) clearQsoContext();   // that QSO is over
        if (id < 0) {
            m_status->setText(tr("Could not write the log (%1).").arg(m_book->path()));
            return;
        }
        m_status->setText(OrionLogbook::mirrorsToCqrlog()
                              ? tr("Logged %1 (%2 %3) and sent to cqrlog.").arg(q.call, q.band, q.mode)
                              : tr("Logged %1 (%2 %3).").arg(q.call, q.band, q.mode));
        clearForNext(false);
    }

    void clearForNext(bool clearStatus)
    {
        for (QLineEdit* e : {m_call, m_name, m_handle, m_qth, m_grid, m_pota, m_comment}) {
            e->clear();
        }
        // The shared record is NOT cleared here: this also runs when the
        // window is first built, and the CW window may already be working a
        // station. Only an explicit Clear and a successful log wipe it
        // (clearQsoContext()).
        m_country->clear();
        m_prev->setRowCount(0);
        m_lookedUp.clear();
        m_fromCallbook.clear();
        m_timeEdited = false;
        m_freqEdited = false;
        m_modeEdited = false;
        if (clearStatus) m_status->clear();
        tick();
        refreshFromRadio();
        pushContext();
        m_call->setFocus();
    }

    OrionLogbook* m_book;
    RadioModel* m_model;
    std::function<SliceModel*()> m_activeSlice;
    QLineEdit* m_call = nullptr;
    QLabel* m_country = nullptr;
    QLineEdit* m_freq = nullptr;
    QLineEdit* m_mode = nullptr;
    QLineEdit* m_rstS = nullptr;
    QLineEdit* m_rstR = nullptr;
    QDateTimeEdit* m_time = nullptr;
    QLineEdit* m_name = nullptr;
    QLineEdit* m_handle = nullptr;
    QLineEdit* m_qth = nullptr;
    QLineEdit* m_grid = nullptr;
    QLineEdit* m_pota = nullptr;
    QLineEdit* m_comment = nullptr;
    QPushButton* m_qrz = nullptr;
    QPushButton* m_globe = nullptr;
    QPushButton* m_spot = nullptr;
    QLabel* m_bCountry = nullptr;
    QLabel* m_bBand = nullptr;
    QLabel* m_bMode = nullptr;
    QLabel* m_heading = nullptr;
    QPushButton* m_sp = nullptr;
    QPushButton* m_lp = nullptr;
    double m_spAz = -1.0;
    double m_lpAz = -1.0;
    QString m_lookedUp;            // the call the callbook was last asked about
    // Boxes whose contents came from the CALLBOOK, not from the operator.
    // Typing through one real call on the way to another (DL1A on the way to
    // DL1AB) filled these with the wrong station's details, and "fill only
    // empty boxes" then refused to correct them — the operator was left
    // looking at DL1A's name under DL1AB's callsign.
    QSet<QLineEdit*> m_fromCallbook;
    QLabel* m_status = nullptr;
    QTableWidget* m_prev = nullptr;
    QTimer* m_clock = nullptr;
    bool m_timeEdited = false;
    bool m_settingTime = false;
    bool m_freqEdited = false;
    bool m_modeEdited = false;
    bool m_ctxEcho = false;
};

// The logbook browser: the console's Logbook window, re-expressed in
// AetherSDR's widgets. The whole log as a table (search, edit in place,
// delete), ADIF in and out, and the on-demand cqrlog comparison.
class OrionLogbookWindow : public QDialog {
public:
    OrionLogbookWindow(OrionLogbook* book, QWidget* parent)
        : QDialog(parent)
        , m_book(book)
    {
        setWindowTitle(tr("Logbook"));
        resize(1000, 560);
        auto* vbox = new QVBoxLayout(this);

        auto* top = new QHBoxLayout;
        m_search = new QLineEdit;
        m_search->setPlaceholderText(tr("Search callsign"));
        m_search->setClearButtonEnabled(true);
        top->addWidget(m_search, 1);
        m_count = new QLabel;
        top->addWidget(m_count);
        vbox->addLayout(top);

        m_model = new QSqlTableModel(this, book->db()->database());
        m_model->setTable(QStringLiteral("qso"));
        m_model->setEditStrategy(QSqlTableModel::OnFieldChange);
        const QList<QPair<QString, QString>> headers = {
            {QStringLiteral("ts_utc"), tr("Date/time (UTC)")}, {QStringLiteral("call"), tr("Call")},
            {QStringLiteral("band"), tr("Band")}, {QStringLiteral("freq_hz"), tr("Freq (Hz)")},
            {QStringLiteral("mode"), tr("Mode")}, {QStringLiteral("rst_s"), tr("Sent")},
            {QStringLiteral("rst_r"), tr("Rcvd")}, {QStringLiteral("name"), tr("Name")},
            {QStringLiteral("qth"), tr("QTH")}, {QStringLiteral("grid"), tr("Grid")},
            {QStringLiteral("country"), tr("Country")}, {QStringLiteral("pota"), tr("POTA")},
            {QStringLiteral("comment"), tr("Comment")}, {QStringLiteral("qsl_rcvd"), tr("QSL")},
            {QStringLiteral("lotw_rcvd"), tr("LoTW")}, {QStringLiteral("eqsl_rcvd"), tr("eQSL")}};
        m_model->setSort(m_model->fieldIndex(QStringLiteral("ts_utc")), Qt::DescendingOrder);
        m_model->select();
        for (const auto& h : headers) {
            const int col = m_model->fieldIndex(h.first);
            if (col >= 0) m_model->setHeaderData(col, Qt::Horizontal, h.second);
        }
        m_proxy = new QSortFilterProxyModel(this);
        m_proxy->setSourceModel(m_model);
        m_proxy->setFilterKeyColumn(m_model->fieldIndex(QStringLiteral("call")));
        m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

        m_table = new QTableView;
        m_table->setModel(m_proxy);
        m_table->setSortingEnabled(true);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->verticalHeader()->setVisible(false);
        m_table->horizontalHeader()->setStretchLastSection(true);
        // Bookkeeping columns stay out of sight.
        for (const char* hidden : {"id", "cqz", "ituz", "up_lotw", "up_eqsl", "up_qrz",
                                   "up_club", "up_hrdlog"}) {
            const int col = m_model->fieldIndex(QLatin1String(hidden));
            if (col >= 0) m_table->setColumnHidden(col, true);
        }
        vbox->addWidget(m_table, 1);

        auto* row = new QHBoxLayout;
        auto* delBtn = new QPushButton(tr("Delete selected"));
        auto* importBtn = new QPushButton(tr("Import ADIF..."));
        auto* exportBtn = new QPushButton(tr("Export ADIF..."));
        auto* cqrBtn = new QPushButton(tr("Check against cqrlog"));
        cqrBtn->setToolTip(tr("Find QSOs that are in this log but not in cqrlog, then offer "
                              "to send them (cqrlog must be running)"));
        auto* closeBtn = new QPushButton(tr("Close"));
        row->addWidget(delBtn);
        row->addWidget(importBtn);
        row->addWidget(exportBtn);
        row->addWidget(cqrBtn);
        row->addStretch(1);
        row->addWidget(closeBtn);
        vbox->addLayout(row);
        m_status = new QLabel;
        m_status->setWordWrap(true);
        vbox->addWidget(m_status);

        connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) {
            m_proxy->setFilterFixedString(t.trimmed());
            updateCount();
        });
        connect(m_model, &QSqlTableModel::dataChanged, this,
                [this] { m_book->db()->notifyExternalChange(); });
        connect(book->db(), &orion::logbook::LogDb::changed, this, [this] { reload(); });
        connect(delBtn, &QPushButton::clicked, this, [this] { deleteSelected(); });
        connect(importBtn, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getOpenFileName(
                this, tr("Import ADIF"), QString(),
                tr("ADIF files (*.adi *.adif *.ADI);;All files (*)"));
            if (file.isEmpty()) return;
            QString err;
            const int added = m_book->importAdif(file, &err);
            m_status->setText(added < 0 ? err
                                        : tr("%1 QSO(s) imported (duplicates within 5 minutes "
                                             "skipped).").arg(added));
        });
        connect(exportBtn, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getSaveFileName(
                this, tr("Export ADIF"),
                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                    + QStringLiteral("/aethersdr-log.adi"),
                tr("ADIF files (*.adi)"));
            if (file.isEmpty()) return;
            QString err;
            m_status->setText(m_book->exportAdif(file, &err) ? tr("Exported to %1.").arg(file)
                                                             : err);
        });
        connect(cqrBtn, &QPushButton::clicked, this, [this] {
            m_status->setText(tr("Asking cqrlog..."));
            m_book->cqrlogSync()->checkMissing();
        });
        // Nothing is sent to cqrlog without the operator's say-so (console).
        connect(m_book->cqrlogSync(), &CqrlogSync::missingReady, this,
                [this](const QList<qint64>& ids, const QString& error) {
            if (!error.isEmpty()) {
                m_status->setText(error);
                return;
            }
            if (ids.isEmpty()) {
                m_status->setText(tr("cqrlog has every QSO in this log."));
                return;
            }
            if (QMessageBox::question(this, tr("Check against cqrlog"),
                                      tr("%1 QSO(s) in this log are not in cqrlog. Send them "
                                         "to cqrlog now?").arg(ids.size()))
                == QMessageBox::Yes) {
                m_book->cqrlogSync()->sendMissing(ids);
                m_status->setText(tr("Sending %1 QSO(s) to cqrlog...").arg(ids.size()));
            } else {
                m_status->setText(tr("%1 QSO(s) not in cqrlog; nothing sent.").arg(ids.size()));
            }
        });
        connect(m_book->cqrlogSync(), &CqrlogSync::pushedToCqrlog, this,
                [this](int sent) { m_status->setText(tr("%1 QSO(s) sent to cqrlog.").arg(sent)); });
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::hide);
        updateCount();
    }

    void reload()
    {
        if (m_editing) return;
        m_model->select();
        while (m_model->canFetchMore()) m_model->fetchMore();
        updateCount();
    }

private:
    void updateCount()
    {
        while (m_model->canFetchMore()) m_model->fetchMore();
        m_count->setText(m_search->text().trimmed().isEmpty()
                             ? tr("%1 QSOs").arg(m_book->db()->count())
                             : tr("%1 of %2 QSOs").arg(m_proxy->rowCount())
                                   .arg(m_book->db()->count()));
    }

    void deleteSelected()
    {
        const QModelIndexList rows = m_table->selectionModel()->selectedRows();
        if (rows.isEmpty()) return;
        if (QMessageBox::question(this, tr("Delete QSOs"),
                                  tr("Delete %1 QSO(s) from this log? cqrlog is not changed.")
                                      .arg(rows.size()))
            != QMessageBox::Yes) {
            return;
        }
        QList<qint64> ids;
        const int idCol = m_model->fieldIndex(QStringLiteral("id"));
        for (const QModelIndex& r : rows) {
            ids << m_model->data(m_model->index(m_proxy->mapToSource(r).row(), idCol)).toLongLong();
        }
        m_editing = true;
        for (qint64 id : ids) m_book->db()->deleteQso(id);
        m_editing = false;
        reload();
        m_status->setText(tr("%1 QSO(s) deleted.").arg(ids.size()));
    }

    OrionLogbook* m_book;
    QSqlTableModel* m_model = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;
    QTableView* m_table = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_count = nullptr;
    QLabel* m_status = nullptr;
    bool m_editing = false;
};

// Online logs: the console's Online Logs / setup window, re-expressed in
// AetherSDR's widgets. Ordinary fields are settings (OrionUp/...); secrets
// go to the system keychain through OrionUploadSecrets.
class OrionOnlineLogsDialog : public QDialog {
public:
    OrionOnlineLogsDialog(OrionLogbook* book, QWidget* parent)
        : QDialog(parent)
        , m_book(book)
    {
        setWindowTitle(tr("Online logs"));
        resize(720, 780);
        auto* outer = new QVBoxLayout(this);
        auto* intro = new QLabel(tr(
            "Each QSO you log is sent at once to every service switched on here, and every "
            "result is listed below. Passwords, keys and codes are kept in the system "
            "keychain, never in AetherSDR's settings file. QSOs brought in from cqrlog or an "
            "ADIF file count as already uploaded and are never sent."));
        intro->setWordWrap(true);
        outer->addWidget(intro);
        if (!book->uploadSecrets()->persistent()) {
            auto* warn = new QLabel(tr("No system keychain is available: secrets typed here "
                                       "last only until AetherSDR closes."));
            warn->setWordWrap(true);
            outer->addWidget(warn);
        }

        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto* content = new QWidget;
        auto* col = new QVBoxLayout(content);
        scroll->setWidget(content);
        outer->addWidget(scroll, 2);

        QslUploader* up = book->uploader();

        // LoTW
        {
            auto* box = section(col, QStringLiteral("lotw"), tr("LoTW (ARRL)"));
            auto* form = qobject_cast<QFormLayout*>(box->layout());
            form->addRow(tr("TQSL station location:"), field(QStringLiteral("lotw/station")));
            form->addRow(tr("tqsl program:"), field(QStringLiteral("lotw/tqslPath"), QStringLiteral("tqsl")));
            form->addRow(tr("Certificate key password:"), secret(QStringLiteral("lotw/tqslPassword"),
                                                                 tr("blank if the key has none")));
            form->addRow(tr("LoTW website login:"), field(QStringLiteral("lotw/login")));
            form->addRow(tr("LoTW website password:"), secret(QStringLiteral("lotw/password")));
            auto* row = new QHBoxLayout;
            auto* t1 = new QPushButton(tr("Test tqsl"));
            auto* t2 = new QPushButton(tr("Test login"));
            auto* sync = new QPushButton(tr("Sync LoTW QSLs now"));
            row->addWidget(t1);
            row->addWidget(t2);
            row->addWidget(sync);
            row->addStretch(1);
            form->addRow(row);
            form->addRow(resultLabel(QStringLiteral("lotw-tqsl")));
            form->addRow(resultLabel(QStringLiteral("lotw-dl")));
            form->addRow(resultLabel(QStringLiteral("lotw")));
            connect(t1, &QPushButton::clicked, this, [up] { up->testTqsl(); });
            connect(t2, &QPushButton::clicked, this, [up] { up->testLotwDownload(); });
            connect(sync, &QPushButton::clicked, this, [up] { up->syncLotwQsls(); });
        }
        // eQSL
        {
            auto* box = section(col, QStringLiteral("eqsl"), tr("eQSL"));
            auto* form = qobject_cast<QFormLayout*>(box->layout());
            form->addRow(tr("User name:"), field(QStringLiteral("eqsl/user")));
            form->addRow(tr("Password:"), secret(QStringLiteral("eqsl/password")));
            form->addRow(tr("QTH nickname:"), field(QStringLiteral("eqsl/nickname"), tr("optional")));
            addTest(form, QStringLiteral("eqsl"), [up] { up->testEqsl(); });
        }
        // QRZ
        {
            auto* box = section(col, QStringLiteral("qrz"), tr("QRZ logbook"));
            auto* form = qobject_cast<QFormLayout*>(box->layout());
            form->addRow(tr("Logbook API key:"), secret(QStringLiteral("qrz/key")));
            addTest(form, QStringLiteral("qrz"), [up] { up->testQrz(); });
        }
        // ClubLog
        {
            auto* box = section(col, QStringLiteral("club"), tr("ClubLog"));
            auto* form = qobject_cast<QFormLayout*>(box->layout());
            form->addRow(tr("E-mail:"), field(QStringLiteral("club/email")));
            form->addRow(tr("Password:"), secret(QStringLiteral("club/password")));
            form->addRow(tr("Callsign:"), field(QStringLiteral("club/call")));
            form->addRow(tr("Application API key:"), secret(QStringLiteral("club/apikey"),
                                                          tr("ClubLog issues these per program")));
            form->addRow(resultLabel(QStringLiteral("club")));
        }
        // HRDLOG
        {
            auto* box = section(col, QStringLiteral("hrdlog"), tr("HRDLOG.net"));
            auto* form = qobject_cast<QFormLayout*>(box->layout());
            form->addRow(tr("Callsign:"), field(QStringLiteral("hrdlog/call")));
            form->addRow(tr("Upload code:"), secret(QStringLiteral("hrdlog/code")));
            addTest(form, QStringLiteral("hrdlog"), [up] { up->testHrdlogNet(); });
        }
        // Logger mirrors
        for (const auto& [svc, title, port] :
             {std::tuple{QStringLiteral("hrd"), tr("HRD Logbook (mirror)"), 7826},
              std::tuple{QStringLiteral("n1mm"), tr("N1MM+ (mirror)"), 2333}}) {
            auto* box = section(col, svc, title);
            auto* form = qobject_cast<QFormLayout*>(box->layout());
            form->addRow(tr("Address:"), field(svc + QStringLiteral("/ip"), QStringLiteral("127.0.0.1")));
            auto* portBox = new QSpinBox;
            portBox->setRange(1, 65535);
            portBox->setValue(AppSettings::instance()
                                  .value(QStringLiteral("OrionUp/") + svc + QStringLiteral("/port"), port).toInt());
            const QString key = QStringLiteral("OrionUp/") + svc + QStringLiteral("/port");
            connect(portBox, &QSpinBox::valueChanged, this, [key](int v) {
                AppSettings::instance().setValue(key, v);
                AppSettings::instance().save();
            });
            form->addRow(tr("Port:"), portBox);
            const QString s = svc;
            addTest(form, svc, [up, s] { up->testLoggerPush(s); });
        }
        auto* wsjtx = new QCheckBox(tr("Upload WSJT-X QSOs too (off: leave that to GridTracker)"));
        wsjtx->setChecked(AppSettings::instance().value(QStringLiteral("OrionLog/wsjtxPush"), true).toBool());
        connect(wsjtx, &QCheckBox::toggled, this, [](bool on) {
            AppSettings::instance().setValue(QStringLiteral("OrionLog/wsjtxPush"), on);
            AppSettings::instance().save();
        });
        col->addWidget(wsjtx);
        col->addStretch(1);

        outer->addWidget(new QLabel(tr("Upload traffic:")));
        m_traffic = new QListWidget;
        m_traffic->addItems(up->recentTraffic());
        m_traffic->scrollToBottom();
        outer->addWidget(m_traffic, 1);
        auto* buttons = new QHBoxLayout;
        auto* retry = new QPushButton(tr("Retry failed now"));
        auto* close = new QPushButton(tr("Close"));
        buttons->addWidget(retry);
        buttons->addStretch(1);
        buttons->addWidget(close);
        outer->addLayout(buttons);
        connect(retry, &QPushButton::clicked, this, [up] { up->retryFailedNow(); });
        connect(close, &QPushButton::clicked, this, &QDialog::hide);
        connect(up, &QslUploader::trafficLine, this, [this](const QString& line) {
            m_traffic->addItem(line);
            while (m_traffic->count() > 100) delete m_traffic->takeItem(0);
            m_traffic->scrollToBottom();
        });
        connect(up, &QslUploader::serviceResult, this,
                [this](const QString& svc, bool ok, const QString& detail) {
            if (QLabel* l = m_results.value(svc)) {
                l->setText((ok ? tr("OK: %1") : tr("Failed: %1")).arg(detail));
            }
        });
        connect(book->uploadSecrets(), &OrionUploadSecrets::saveFailed, this,
                [this](const QString& key, const QString& reason) {
            m_traffic->addItem(tr("keychain: could not save %1 (%2)").arg(key, reason));
        });
    }

private:
    QGroupBox* section(QVBoxLayout* col, const QString& svc, const QString& title)
    {
        auto* box = new QGroupBox(title);
        box->setCheckable(true);
        const QString key = QStringLiteral("OrionUp/") + svc + QStringLiteral("/enabled");
        box->setChecked(AppSettings::instance().value(key, false).toBool());
        connect(box, &QGroupBox::toggled, this, [key](bool on) {
            AppSettings::instance().setValue(key, on);
            AppSettings::instance().save();
        });
        box->setLayout(new QFormLayout);
        col->addWidget(box);
        return box;
    }

    QLineEdit* field(const QString& key, const QString& placeholder = QString())
    {
        auto* e = new QLineEdit(AppSettings::instance().value(QStringLiteral("OrionUp/") + key).toString());
        e->setPlaceholderText(placeholder);
        const QString k = QStringLiteral("OrionUp/") + key;
        connect(e, &QLineEdit::editingFinished, this, [e, k] {
            AppSettings::instance().setValue(k, e->text().trimmed());
            AppSettings::instance().save();
        });
        return e;
    }

    QLineEdit* secret(const QString& key, const QString& placeholder = QString())
    {
        auto* e = new QLineEdit(m_book->uploadSecrets()->value(key));
        e->setEchoMode(QLineEdit::Password);
        e->setPlaceholderText(placeholder);
        OrionUploadSecrets* secrets = m_book->uploadSecrets();
        connect(e, &QLineEdit::editingFinished, this, [e, key, secrets] {
            if (e->text().trimmed() != secrets->value(key)) secrets->setValue(key, e->text());
        });
        // Secrets may still be loading from the keychain when the window opens.
        connect(secrets, &OrionUploadSecrets::loaded, e, [e, key, secrets] {
            if (e->text().isEmpty()) e->setText(secrets->value(key));
        });
        return e;
    }

    QLabel* resultLabel(const QString& svc)
    {
        auto* l = new QLabel;
        l->setWordWrap(true);
        m_results.insert(svc, l);
        return l;
    }

    void addTest(QFormLayout* form, const QString& svc, std::function<void()> run)
    {
        auto* b = new QPushButton(tr("Test"));
        connect(b, &QPushButton::clicked, this, [run] { run(); });
        auto* row = new QHBoxLayout;
        row->addWidget(b);
        row->addWidget(resultLabel(svc), 1);
        form->addRow(row);
    }

    OrionLogbook* m_book;
    QListWidget* m_traffic = nullptr;
    QHash<QString, QLabel*> m_results;
};

}  // namespace

orion::logbook::QsoContext* orionQsoContext()
{
    return g_qsoContext;
}

QColor orionWorkedSpotColour(const QString& call, double freqMhz)
{
    if (!g_book || !workedColoursOn() || call.trimmed().isEmpty()) {
        return {};
    }
    switch (g_book->workedStatus(call, qint64(std::llround(freqMhz * 1e6))).toLatin1()) {
    case 'N': return QColor(255, 92, 70);     // never worked: a new one
    case 'B': return QColor(255, 190, 60);    // worked, not on this band
    case 'W': return QColor(130, 222, 140);   // worked this band
    case 'C': return QColor(150, 162, 178);   // confirmed this band
    default: return {};                       // no log data yet
    }
}

void installOrionLogbook(QMainWindow* window, QMenuBar* bar, RadioModel* model,
                         std::function<SliceModel*()> activeSlice,
                         std::function<void(const QString&, double, const QString&)> spot,
                         std::function<void(double, double, const QString&)> showOnMap)
{
    auto* book = new OrionLogbook(window);
    g_book = book;
    // The shared current-QSO record, created before the Log QSO window so
    // it binds to it (and before the CW window asks for it).
    auto* ctx = new QsoContext(window);
    g_qsoContext = ctx;
    QString error;
    const bool open = book->open(QString(), QString(), &error);
    if (open) {
        book->startCqrlogSync();
        book->startWsjtx();
    }

    auto* menu = bar->addMenu(QObject::tr("&Logbook"));
    menu->setToolTipsVisible(true);
    auto* logAct = menu->addAction(QObject::tr("Log QSO..."));
    logAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    logAct->setToolTip(QObject::tr("Log a contact: frequency and mode come from the active "
                                   "slice, the call from a spot on that frequency"));
    auto* browseAct = menu->addAction(QObject::tr("Logbook..."));
    browseAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    browseAct->setToolTip(QObject::tr("The whole log: search, edit, delete, ADIF, and a check "
                                      "against cqrlog"));
    auto* onlineAct = menu->addAction(QObject::tr("Online logs..."));
    onlineAct->setToolTip(QObject::tr("LoTW, eQSL, QRZ logbook, ClubLog and HRDLOG uploads, and "
                                      "the HRD / N1MM+ mirrors"));
    menu->addSeparator();
    auto* importAct = menu->addAction(QObject::tr("Import ADIF..."));
    auto* exportAct = menu->addAction(QObject::tr("Export ADIF..."));
    menu->addSeparator();
    auto* mirrorAct = menu->addAction(QObject::tr("Send each QSO to cqrlog"));
    mirrorAct->setCheckable(true);
    mirrorAct->setChecked(OrionLogbook::mirrorsToCqrlog());
    mirrorAct->setToolTip(QObject::tr("Mirror every logged QSO to cqrlog's bridge on "
                                      "localhost:%1").arg(OrionLogbook::bridgePort()));
    auto* pullAct = menu->addAction(QObject::tr("Bring in QSOs logged in cqrlog"));
    pullAct->setCheckable(true);
    pullAct->setChecked(AppSettings::instance()
                            .value(QStringLiteral("OrionLog/cqrlogSync"), true).toBool());
    pullAct->setToolTip(QObject::tr("While cqrlog is running, its new QSOs are copied into "
                                    "this log every couple of minutes (takes effect at the "
                                    "next start)"));
    auto* wsjtxAct = menu->addAction(QObject::tr("Log WSJT-X QSOs"));
    wsjtxAct->setCheckable(true);
    wsjtxAct->setChecked(AppSettings::instance()
                             .value(QStringLiteral("OrionLog/wsjtx"), true).toBool());
    wsjtxAct->setToolTip(QObject::tr("Every QSO WSJT-X logs (its UDP broadcast on "
                                     "224.0.0.1:2237) lands in this log (takes effect at the "
                                     "next start)"));
    auto* coloursAct = menu->addAction(QObject::tr("Colour spots by worked-before"));
    coloursAct->setCheckable(true);
    coloursAct->setChecked(workedColoursOn());
    coloursAct->setToolTip(QObject::tr("Red: never worked. Amber: worked on another band. "
                                       "Green: worked on this band. Grey: confirmed. "
                                       "AetherSDR's DXCC colouring, when on, takes precedence."));
    menu->addSeparator();
    auto* info = menu->addAction(open ? QObject::tr("Log file: %1").arg(book->path())
                                      : QObject::tr("Log not available: %1").arg(error));
    info->setEnabled(false);
    for (QAction* a : {logAct, browseAct, onlineAct, importAct, exportAct}) {
        a->setEnabled(open);
    }

    // Spot colours follow the log: redraw the markers when it changes.
    const auto refreshSpots = [model] {
        if (model) emit model->spotModel().spotsRefreshed();
    };
    QObject::connect(book, &OrionLogbook::workedIndexUpdated, window, refreshSpots);
    QObject::connect(coloursAct, &QAction::toggled, window, [refreshSpots](bool on) {
        AppSettings::instance().setValue(QStringLiteral("OrionLog/workedColours"), on);
        AppSettings::instance().save();
        refreshSpots();
    });
    QObject::connect(wsjtxAct, &QAction::toggled, window, [](bool on) {
        AppSettings::instance().setValue(QStringLiteral("OrionLog/wsjtx"), on);
        AppSettings::instance().save();
    });

    auto online = std::make_shared<QPointer<OrionOnlineLogsDialog>>();
    QObject::connect(onlineAct, &QAction::triggered, window, [=] {
        if (!*online) *online = new OrionOnlineLogsDialog(book, window);
        (*online)->show();
        (*online)->raise();
        (*online)->activateWindow();
    });
    // Upload failures surface in the status bar too.
    if (book->uploader()) {
        QObject::connect(book->uploader(), &QslUploader::serviceResult, window,
                         [window](const QString& svc, bool ok, const QString& detail) {
            if (!ok && window->statusBar()) {
                window->statusBar()->showMessage(QObject::tr("%1 upload failed: %2").arg(svc, detail), 8000);
            }
        });
    }

    auto browser = std::make_shared<QPointer<OrionLogbookWindow>>();
    QObject::connect(browseAct, &QAction::triggered, window, [=] {
        if (!*browser) {
            *browser = new OrionLogbookWindow(book, window);
        }
        (*browser)->reload();
        (*browser)->show();
        (*browser)->raise();
        (*browser)->activateWindow();
    });

    auto dialog = std::make_shared<QPointer<OrionQsoDialog>>();
    // WSJT-X: its logged QSOs land here (console behaviour); the DX it is
    // working fills an open Log QSO window.
    QObject::connect(book, &OrionLogbook::wsjtxLogged, window,
                     [window](qint64, const QString& call) {
        if (window->statusBar()) {
            window->statusBar()->showMessage(QObject::tr("Logged %1 (WSJT-X)").arg(call), 6000);
        }
    });
    QObject::connect(book, &OrionLogbook::wsjtxDx, window,
                     [dialog](const QString& call, const QString& grid, const QString& mode) {
        if (*dialog && (*dialog)->isVisible()) {
            (*dialog)->setFromWsjtx(call, grid, mode);
        }
    });
    QObject::connect(logAct, &QAction::triggered, window, [=] {
        if (!*dialog) {
            *dialog = new OrionQsoDialog(book, model, activeSlice, window);
            if (spot) {
                QObject::connect(*dialog, &OrionQsoDialog::spotRequested, window, spot);
            }
            if (showOnMap) {
                QObject::connect(*dialog, &OrionQsoDialog::mapRequested, window, showOnMap);
            }
        }
        (*dialog)->refreshFromRadio();
        (*dialog)->show();
        (*dialog)->raise();
        (*dialog)->activateWindow();
    });
    QObject::connect(importAct, &QAction::triggered, window, [=] {
        const QString file = QFileDialog::getOpenFileName(
            window, QObject::tr("Import ADIF"), QString(),
            QObject::tr("ADIF files (*.adi *.adif *.ADI);;All files (*)"));
        if (file.isEmpty()) return;
        QString err;
        const int added = book->importAdif(file, &err);
        if (added < 0) {
            QMessageBox::warning(window, QObject::tr("Import ADIF"), err);
        } else {
            QMessageBox::information(window, QObject::tr("Import ADIF"),
                QObject::tr("%1 QSO(s) added (duplicates within 5 minutes were skipped).%2")
                    .arg(added).arg(err.isEmpty() ? QString() : QStringLiteral("\n") + err));
        }
    });
    QObject::connect(exportAct, &QAction::triggered, window, [=] {
        const QString file = QFileDialog::getSaveFileName(
            window, QObject::tr("Export ADIF"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                + QStringLiteral("/aethersdr-log.adi"),
            QObject::tr("ADIF files (*.adi)"));
        if (file.isEmpty()) return;
        QString err;
        if (!book->exportAdif(file, &err)) {
            QMessageBox::warning(window, QObject::tr("Export ADIF"), err);
        }
    });
    QObject::connect(mirrorAct, &QAction::toggled, window, [](bool on) {
        AppSettings::instance().setValue(QStringLiteral("OrionLog/mirrorCqrlog"), on);
        AppSettings::instance().save();
    });
    QObject::connect(pullAct, &QAction::toggled, window, [](bool on) {
        AppSettings::instance().setValue(QStringLiteral("OrionLog/cqrlogSync"), on);
        AppSettings::instance().save();
    });
}

}  // namespace AetherSDR

#include "OrionLogbookUi.moc"
