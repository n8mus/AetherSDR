#include "gui/orion/OrionLogbookUi.h"

#include "core/AppSettings.h"
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
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

using orion::logbook::OrionLogbook;
using orion::logbook::Qso;

namespace {

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

}  // namespace

void installOrionLogbook(QMainWindow* window, QMenuBar* bar, RadioModel* model,
                         std::function<SliceModel*()> activeSlice)
{
    auto* book = new OrionLogbook(window);
    QString error;
    const bool open = book->open(QString(), QString(), &error);
    if (open) {
        book->startCqrlogSync();
    }

    auto* menu = bar->addMenu(QObject::tr("&Logbook"));
    menu->setToolTipsVisible(true);
    auto* logAct = menu->addAction(QObject::tr("Log QSO..."));
    logAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+L")));
    logAct->setToolTip(QObject::tr("Log a contact: frequency and mode come from the active "
                                   "slice, the call from a spot on that frequency"));
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
    menu->addSeparator();
    auto* info = menu->addAction(open ? QObject::tr("Log file: %1").arg(book->path())
                                      : QObject::tr("Log not available: %1").arg(error));
    info->setEnabled(false);
    for (QAction* a : {logAct, importAct, exportAct}) {
        a->setEnabled(open);
    }

    auto dialog = std::make_shared<QPointer<OrionQsoDialog>>();
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
