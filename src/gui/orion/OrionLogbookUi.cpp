#include "gui/orion/OrionLogbookUi.h"

#include "core/AppSettings.h"
#include "core/backends/orion/log/CqrlogSync.h"
#include "core/backends/orion/log/OrionLogbook.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/SpotModel.h"

#include <QAction>
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

namespace AetherSDR {

using orion::logbook::CqrlogSync;
using orion::logbook::OrionLogbook;
using orion::logbook::Qso;

namespace {

// The logbook the spot colouring reads (one per application).
QPointer<OrionLogbook> g_book;

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

// The Log QSO window: the console's New-QSO flow in AetherSDR's widgets.
class OrionQsoDialog : public QDialog {
public:
    OrionQsoDialog(OrionLogbook* book, RadioModel* model,
                   std::function<SliceModel*()> activeSlice, QWidget* parent)
        : QDialog(parent)
        , m_book(book)
        , m_model(model)
        , m_activeSlice(std::move(activeSlice))
    {
        setWindowTitle(tr("Log QSO"));
        auto* vbox = new QVBoxLayout(this);

        auto* form = new QGridLayout;
        int row = 0;
        const auto add = [&](const QString& label, QWidget* w, int col = 0, int span = 1) {
            form->addWidget(new QLabel(label), row, col * 2);
            form->addWidget(w, row, col * 2 + 1, 1, span);
        };
        m_call = new QLineEdit;
        m_call->setPlaceholderText(tr("Callsign"));
        QFont big = m_call->font();
        big.setPointSizeF(big.pointSizeF() * 1.4);
        big.setBold(true);
        m_call->setFont(big);
        add(tr("Call:"), m_call, 0, 3);
        ++row;
        m_country = new QLabel;
        form->addWidget(m_country, row, 1, 1, 3);
        ++row;
        m_freq = new QLineEdit;
        m_mode = new QLineEdit;
        add(tr("Freq (MHz):"), m_freq, 0);
        add(tr("Mode:"), m_mode, 1);
        ++row;
        m_rstS = new QLineEdit;
        m_rstR = new QLineEdit;
        add(tr("RST sent:"), m_rstS, 0);
        add(tr("RST rcvd:"), m_rstR, 1);
        ++row;
        m_time = new QDateTimeEdit;
        m_time->setTimeZone(QTimeZone::utc());
        m_time->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
        add(tr("Time:"), m_time, 0, 3);
        ++row;
        m_name = new QLineEdit;
        m_qth = new QLineEdit;
        add(tr("Name:"), m_name, 0);
        add(tr("QTH:"), m_qth, 1);
        ++row;
        m_grid = new QLineEdit;
        m_pota = new QLineEdit;
        m_pota->setPlaceholderText(tr("e.g. K-1234"));
        add(tr("Grid:"), m_grid, 0);
        add(tr("POTA:"), m_pota, 1);
        ++row;
        m_comment = new QLineEdit;
        add(tr("Comment:"), m_comment, 0, 3);
        vbox->addLayout(form);

        m_status = new QLabel;
        m_status->setWordWrap(true);
        vbox->addWidget(m_status);

        m_prev = new QTableWidget(0, 6);
        m_prev->setHorizontalHeaderLabels({tr("Date (UTC)"), tr("Band"), tr("Mode"),
                                           tr("Sent"), tr("Rcvd"), tr("Name")});
        m_prev->horizontalHeader()->setStretchLastSection(true);
        m_prev->verticalHeader()->setVisible(false);
        m_prev->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_prev->setSelectionMode(QAbstractItemView::NoSelection);
        m_prev->setMinimumHeight(110);
        vbox->addWidget(new QLabel(tr("Previous QSOs with this station:")));
        vbox->addWidget(m_prev);

        auto* buttons = new QDialogButtonBox;
        auto* logBtn = buttons->addButton(tr("Log"), QDialogButtonBox::AcceptRole);
        auto* clearBtn = buttons->addButton(tr("Clear"), QDialogButtonBox::ResetRole);
        buttons->addButton(QDialogButtonBox::Close);
        logBtn->setDefault(true);
        vbox->addWidget(buttons);
        connect(logBtn, &QPushButton::clicked, this, [this] { logNow(); });
        connect(clearBtn, &QPushButton::clicked, this, [this] { clearForNext(true); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

        connect(m_call, &QLineEdit::textEdited, this, [this](const QString& t) {
            const int pos = m_call->cursorPosition();
            m_call->setText(t.toUpper());
            m_call->setCursorPosition(pos);
            onCallChanged();
        });
        connect(m_mode, &QLineEdit::textEdited, this, [this] { m_modeEdited = true; });
        connect(m_freq, &QLineEdit::textEdited, this, [this] { m_freqEdited = true; });
        // The clock runs until the operator sets a time by hand.
        connect(m_time, &QDateTimeEdit::dateTimeChanged, this, [this] {
            if (!m_settingTime) m_timeEdited = true;
        });
        m_clock = new QTimer(this);
        m_clock->setInterval(1000);
        connect(m_clock, &QTimer::timeout, this, [this] { tick(); });
        m_clock->start();
        clearForNext(true);
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
        if (m_grid->text().isEmpty()) m_grid->setText(grid.toUpper());
        if (!mode.isEmpty() && !m_modeEdited) {
            m_mode->setText(mode.toUpper());
            m_modeEdited = true;   // WSJT-X's FT8 outranks the rig's USB
        }
        onCallChanged();
    }

private:
    void tick()
    {
        if (!m_timeEdited) {
            m_settingTime = true;
            m_time->setDateTime(QDateTime::currentDateTimeUtc());
            m_settingTime = false;
        }
    }

    void onCallChanged()
    {
        const QString call = m_call->text().trimmed().toUpper();
        m_prev->setRowCount(0);
        if (call.size() < 3) {
            m_country->clear();
            return;
        }
        const OrionLogbook::Country c = m_book->country(call);
        m_country->setText(c.known ? tr("%1  (CQ %2, ITU %3)").arg(c.name).arg(c.cqz).arg(c.ituz)
                                   : tr("Country not found in cty.dat"));
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
        if (!prev.isEmpty()) {
            // Worked before: the operator's name and QTH carry over, as in the
            // console.
            if (m_name->text().isEmpty()) m_name->setText(prev.first().name);
            if (m_qth->text().isEmpty()) m_qth->setText(prev.first().qth);
            if (m_grid->text().isEmpty()) m_grid->setText(prev.first().grid);
        }
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
        for (QLineEdit* e : {m_call, m_name, m_qth, m_grid, m_pota, m_comment}) {
            e->clear();
        }
        m_country->clear();
        m_prev->setRowCount(0);
        m_timeEdited = false;
        m_freqEdited = false;
        m_modeEdited = false;
        if (clearStatus) m_status->clear();
        tick();
        refreshFromRadio();
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
    QLineEdit* m_qth = nullptr;
    QLineEdit* m_grid = nullptr;
    QLineEdit* m_pota = nullptr;
    QLineEdit* m_comment = nullptr;
    QLabel* m_status = nullptr;
    QTableWidget* m_prev = nullptr;
    QTimer* m_clock = nullptr;
    bool m_timeEdited = false;
    bool m_settingTime = false;
    bool m_freqEdited = false;
    bool m_modeEdited = false;
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

}  // namespace

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
                         std::function<SliceModel*()> activeSlice)
{
    auto* book = new OrionLogbook(window);
    g_book = book;
    QString error;
    const bool open = book->open(QString(), QString(), &error);
    if (open) {
        book->startCqrlogSync();
        book->startWsjtx();
    }

    auto* menu = bar->addMenu(QObject::tr("&Logbook"));
    menu->setToolTipsVisible(true);
    auto* logAct = menu->addAction(QObject::tr("Log QSO..."));
    logAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+L")));
    logAct->setToolTip(QObject::tr("Log a contact: frequency and mode come from the active "
                                   "slice, the call from a spot on that frequency"));
    auto* browseAct = menu->addAction(QObject::tr("Logbook..."));
    browseAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+B")));
    browseAct->setToolTip(QObject::tr("The whole log: search, edit, delete, ADIF, and a check "
                                      "against cqrlog"));
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
    for (QAction* a : {logAct, browseAct, importAct, exportAct}) {
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
