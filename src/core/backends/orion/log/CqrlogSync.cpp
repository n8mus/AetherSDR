// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/log/CqrlogSync.cpp) for the AetherSDR Orion
// fork (logbook, step 9): namespace and include paths, AetherSDR settings in
// place of QSettings, and AetherSDR's cty.dat parser in place of the console's.
#include "core/backends/orion/log/CqrlogSync.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QProcess>
#include <QSettings>
#include <QTimeZone>
#include <QUdpSocket>

#include "core/AppSettings.h"
#include "core/backends/orion/log/Adif.h"
#include "core/backends/orion/log/LogDb.h"

namespace AetherSDR::orion::logbook {

namespace {
// cqrlog access helpers — same trio as LogbookIndex.cpp (deliberately
// duplicated: both files stay independently readable and the three
// lines each have not changed since cqrlog's directory layout froze).
QString cqrlogHome() {
    const QByteArray env = qgetenv("TTC_CQRLOG_HOME");
    return env.isEmpty() ? QDir::homePath() : QString::fromUtf8(env);
}
QString cqrlogDbName() {
    QSettings login(cqrlogHome() + "/.config/cqrlog/cqrlog_login.cfg",
                    QSettings::IniFormat);
    const int n = login.value("cqrlog/LastOpenedLog",
                              login.value("LastOpenedLog", 1)).toInt();
    return QString("cqrlog%1").arg(n, 3, 10, QChar('0'));
}
QString cqrlogSocket() {
    return cqrlogHome() + "/.config/cqrlog/database/sock";
}

// The SELECT column order rowsToAdif() decodes. waz/itu are cqrlog's
// zone columns (live-verified names from the fork's insert statement);
// loc is the grid.
const char* kColumns =
    "id_cqrlog_main, qsodate, time_on, callsign, freq, mode, band,"
    " rst_s, rst_r, name, qth, loc, waz, itu,"
    " qsl_r, lotw_qslr, eqsl_qsl_rcvd";
constexpr int kNCols = 17;

QString col(const QStringList& f, int i) {
    if (i >= f.size()) return QString();
    const QString v = f.at(i).trimmed();
    return v == QLatin1String("NULL") ? QString() : v;
}
} // namespace

int CqrlogSync::columnCount() { return kNCols; }

CqrlogSync::CqrlogSync(LogDb* db, const CtyDatParser* cty, QObject* parent)
    : QObject(parent), db_(db), cty_(cty) {
    lastId_ = AppSettings::instance().value(QStringLiteral("OrionLog/cqrlogSyncId"), 0).toLongLong();
    timer_.setInterval(2 * 60 * 1000);   // cqrlog only answers while open
    connect(&timer_, &QTimer::timeout, this, [this] { pull(); });
}

void CqrlogSync::start() {
    if (!AppSettings::instance().value(QStringLiteral("OrionLog/cqrlogSync"), true).toBool()) return;
    timer_.start();
    pullSoon(8000);                      // let startup settle first
}

void CqrlogSync::pullSoon(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { pull(); });
}

QByteArray CqrlogSync::rowsToAdif(const QString& tsv, qint64* maxId) {
    QByteArray out;
    for (const QString& line : tsv.split('\n', Qt::SkipEmptyParts)) {
        const QStringList f = line.split('\t');
        if (f.size() < kNCols) continue;
        const qint64 id = col(f, 0).toLongLong();
        if (maxId && id > *maxId) *maxId = id;
        AdifRecord r;
        const QString date = col(f, 1);           // YYYY-MM-DD
        const QString time = col(f, 2);           // HH:MM
        r.insert("QSO_DATE", QString(date).remove('-'));
        r.insert("TIME_ON", QString(time).remove(':'));
        r.insert("CALL", col(f, 3).toUpper());
        if (!col(f, 4).isEmpty()) r.insert("FREQ", col(f, 4));   // MHz
        r.insert("MODE", col(f, 5).toUpper());
        if (!col(f, 6).isEmpty()) r.insert("BAND", col(f, 6).toUpper());
        if (!col(f, 7).isEmpty()) r.insert("RST_SENT", col(f, 7));
        if (!col(f, 8).isEmpty()) r.insert("RST_RCVD", col(f, 8));
        if (!col(f, 9).isEmpty()) r.insert("NAME", col(f, 9));
        if (!col(f, 10).isEmpty()) r.insert("QTH", col(f, 10));
        if (!col(f, 11).isEmpty())
            r.insert("GRIDSQUARE", col(f, 11).toUpper());
        if (col(f, 12).toInt() > 0) r.insert("CQZ", col(f, 12));
        if (col(f, 13).toInt() > 0) r.insert("ITUZ", col(f, 13));
        // cqrlog confirmation letters: card 'Q', LoTW 'L', eQSL 'E'.
        if (col(f, 14) == QLatin1String("Q")) r.insert("QSL_RCVD", "Y");
        if (col(f, 15) == QLatin1String("L"))
            r.insert("LOTW_QSL_RCVD", "Y");
        if (col(f, 16) == QLatin1String("E"))
            r.insert("EQSL_QSL_RCVD", "Y");
        out += Adif::writeRecord(r).toUtf8();
    }
    return out;
}

QByteArray CqrlogSync::bridgeDatagram(const Qso& q) {
    QByteArray d;
    const auto tag = [&d](const char* n, const QString& v) {
        const QByteArray b = v.trimmed().toUtf8();
        if (!b.isEmpty())
            d += '<' + QByteArray(n) + ':' + QByteArray::number(b.size())
               + '>' + b + ' ';
    };
    const QDateTime ts = q.tsUtc.toUTC();
    tag("CALL", q.call.toUpper());
    tag("QSO_DATE", ts.toString("yyyyMMdd"));
    tag("TIME_ON", ts.toString("HHmmss"));
    tag("BAND", q.band);
    if (q.freqHz > 0)
        tag("FREQ", QString::number(q.freqHz / 1e6, 'f', 6));
    tag("MODE", q.mode);
    tag("RST_SENT", q.rstS);
    tag("RST_RCVD", q.rstR);
    tag("NAME", q.name);
    tag("QTH", q.qth);
    tag("GRIDSQUARE", q.grid);
    if (!q.pota.trimmed().isEmpty()) {
        tag("SIG", "POTA");
        tag("SIG_INFO", q.pota);
    }
    tag("COMMENT", q.comment);
    d += "<EOR>";
    return d;
}

void CqrlogSync::checkMissing() {
    if (checkProc_ || !db_) return;
    if (!QFile::exists(cqrlogSocket())) {
        emit missingReady({}, "cqrlog is not running — open it first");
        return;
    }
    checkProc_ = new QProcess(this);
    connect(checkProc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        const QString out =
            QString::fromUtf8(checkProc_->readAllStandardOutput());
        checkProc_->deleteLater();
        checkProc_ = nullptr;
        if (st != QProcess::NormalExit || code != 0) {
            emit missingReady({}, "cqrlog query failed");
            return;
        }
        // cqrlog's QSOs as CALL|BAND -> epoch times.
        QHash<QString, QList<qint64>> have;
        for (const QString& line : out.split('\n', Qt::SkipEmptyParts)) {
            const QStringList f = line.split('\t');
            if (f.size() < 4) continue;
            QString t = f.at(2).trimmed();          // HH:MM[:SS]
            if (t.size() == 5) t += ":00";
            const QDateTime ts(
                QDate::fromString(f.at(1).trimmed(), "yyyy-MM-dd"),
                QTime::fromString(t, "HH:mm:ss"), QTimeZone::utc());
            if (!ts.isValid()) continue;
            have[f.at(3).trimmed().toUpper() + '|'
                 + f.at(0).trimmed().toUpper()]
                << ts.toSecsSinceEpoch();
        }
        QList<qint64> missing;
        for (const Qso& q : db_->allQsos()) {
            const qint64 secs = q.tsUtc.toUTC().toSecsSinceEpoch();
            bool found = false;
            for (const qint64 h :
                 have.value(q.call.trimmed().toUpper() + '|'
                            + q.band.trimmed().toUpper()))
                if (std::llabs(h - secs) <= 600) { found = true; break; }
            if (!found) missing << q.id;
        }
        emit missingReady(missing, QString());
    });
    checkProc_->start(
        "mysql", {"--socket=" + cqrlogSocket(), "-u", "root",
                  cqrlogDbName(), "-N", "-B", "--connect-timeout=3", "-e",
                  "SELECT band, qsodate, time_on, callsign"
                  " FROM cqrlog_main;"});
    QTimer::singleShot(20000, checkProc_, [p = checkProc_] {
        if (p->state() != QProcess::NotRunning) p->kill();
    });
}

void CqrlogSync::sendMissing(const QList<qint64>& ids) {
    if (!db_ || ids.isEmpty()) return;
    if (!udp_) udp_ = new QUdpSocket(this);
    const quint16 port =
        quint16(AppSettings::instance().value(QStringLiteral("OrionLog/port"), 2334).toInt());
    int i = 0;
    for (const qint64 id : ids) {
        const Qso q = db_->qso(id);
        if (q.id < 0) continue;
        const QByteArray d = bridgeDatagram(q);
        // Paced: the bridge fills cqrlog's form and saves per datagram.
        QTimer::singleShot(250 * i++, this, [this, d, port] {
            udp_->writeDatagram(d, QHostAddress::LocalHost, port);
        });
    }
    QTimer::singleShot(250 * i + 500, this,
                       [this, i] { emit pushedToCqrlog(i); });
}

void CqrlogSync::pull() {
    if (proc_ || !db_) return;           // one at a time
    if (!QFile::exists(cqrlogSocket())) return;   // cqrlog not running
    proc_ = new QProcess(this);
    const QString sql =
        QString("SELECT %1 FROM cqrlog_main WHERE id_cqrlog_main > %2 "
                "ORDER BY id_cqrlog_main LIMIT 2000;")
            .arg(QLatin1String(kColumns))
            .arg(lastId_);
    connect(proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        const QString out =
            QString::fromUtf8(proc_->readAllStandardOutput());
        proc_->deleteLater();
        proc_ = nullptr;
        if (st != QProcess::NormalExit || code != 0) return;
        qint64 maxId = lastId_;
        const QByteArray adif = rowsToAdif(out, &maxId);
        const int rows = out.count('\n');
        if (maxId <= lastId_) return;    // nothing new
        int added = 0;
        if (!adif.isEmpty()) {
            QBuffer buf;
            buf.setData(adif);
            buf.open(QIODevice::ReadOnly);
            added = db_->importAdif(buf, cty_);   // near-dups skipped
        }
        lastId_ = maxId;
        AppSettings::instance().setValue(QStringLiteral("OrionLog/cqrlogSyncId"), lastId_);
        AppSettings::instance().save();
        if (added > 0) emit pulled(added);
        if (rows >= 2000) pullSoon(500); // more batches behind this one
    });
    proc_->start("mysql",
                 {"--socket=" + cqrlogSocket(), "-u", "root",
                  cqrlogDbName(), "-N", "-B", "--connect-timeout=3",
                  "-e", sql});
    QTimer::singleShot(20000, proc_, [p = proc_] {
        if (p->state() != QProcess::NotRunning) p->kill();
    });
}

} // namespace AetherSDR::orion::logbook
