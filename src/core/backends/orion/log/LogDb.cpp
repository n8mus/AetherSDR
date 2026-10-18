// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/log/LogDb.cpp) for the AetherSDR Orion
// fork (logbook, step 9): namespace and include paths, AetherSDR settings in
// place of QSettings, and AetherSDR's cty.dat parser in place of the console's.
#include "core/backends/orion/log/LogDb.h"

#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QTimeZone>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

#include "core/AppSettings.h"
#include "core/CtyDatParser.h"

namespace AetherSDR::orion::logbook {

namespace {
const char* kSchema =
    "CREATE TABLE IF NOT EXISTS qso ("
    " id INTEGER PRIMARY KEY,"
    " ts_utc TEXT NOT NULL,"               // "yyyy-MM-dd HH:mm:ss"
    " call TEXT NOT NULL,"
    " band TEXT NOT NULL,"
    " freq_hz INTEGER NOT NULL DEFAULT 0,"
    " mode TEXT NOT NULL,"
    " rst_s TEXT DEFAULT '', rst_r TEXT DEFAULT '',"
    " name TEXT DEFAULT '', qth TEXT DEFAULT '', grid TEXT DEFAULT '',"
    " country TEXT DEFAULT '', cqz INTEGER DEFAULT 0, ituz INTEGER DEFAULT 0,"
    " pota TEXT DEFAULT '', comment TEXT DEFAULT '',"
    " qsl_rcvd TEXT DEFAULT '', lotw_rcvd TEXT DEFAULT '',"
    " eqsl_rcvd TEXT DEFAULT '',"
    // Online-log push bookkeeping (v2): '' pending, 'Y' sent, 'E' failed.
    " up_lotw TEXT DEFAULT '', up_eqsl TEXT DEFAULT '', up_qrz TEXT DEFAULT '',"
    " up_club TEXT DEFAULT '', up_hrdlog TEXT DEFAULT '')";

Qso qsoFromQuery(const QSqlQuery& q) {
    Qso o;
    o.id       = q.value("id").toLongLong();
    o.tsUtc    = QDateTime::fromString(q.value("ts_utc").toString(),
                                       "yyyy-MM-dd HH:mm:ss");
    o.tsUtc.setTimeZone(QTimeZone::utc());
    o.call     = q.value("call").toString();
    o.band     = q.value("band").toString();
    o.freqHz   = q.value("freq_hz").toLongLong();
    o.mode     = q.value("mode").toString();
    o.rstS     = q.value("rst_s").toString();
    o.rstR     = q.value("rst_r").toString();
    o.name     = q.value("name").toString();
    o.qth      = q.value("qth").toString();
    o.grid     = q.value("grid").toString();
    o.country  = q.value("country").toString();
    o.cqz      = q.value("cqz").toInt();
    o.ituz     = q.value("ituz").toInt();
    o.pota     = q.value("pota").toString();
    o.comment  = q.value("comment").toString();
    o.qslRcvd  = q.value("qsl_rcvd").toString();
    o.lotwRcvd = q.value("lotw_rcvd").toString();
    o.eqslRcvd = q.value("eqsl_rcvd").toString();
    return o;
}

void bindQso(QSqlQuery& q, const Qso& o) {
    q.bindValue(":ts", o.tsUtc.toUTC().toString("yyyy-MM-dd HH:mm:ss"));
    q.bindValue(":call", o.call.trimmed().toUpper());
    q.bindValue(":band", o.band.trimmed().toUpper());
    q.bindValue(":freq", o.freqHz);
    q.bindValue(":mode", o.mode.trimmed().toUpper());
    q.bindValue(":rsts", o.rstS.trimmed());
    q.bindValue(":rstr", o.rstR.trimmed());
    q.bindValue(":name", o.name.trimmed());
    q.bindValue(":qth", o.qth.trimmed());
    q.bindValue(":grid", o.grid.trimmed().toUpper());
    q.bindValue(":country", o.country.trimmed());
    q.bindValue(":cqz", o.cqz);
    q.bindValue(":ituz", o.ituz);
    q.bindValue(":pota", o.pota.trimmed().toUpper());
    q.bindValue(":comment", o.comment.trimmed());
    q.bindValue(":qsl", o.qslRcvd.trimmed().toUpper());
    q.bindValue(":lotw", o.lotwRcvd.trimmed().toUpper());
    q.bindValue(":eqsl", o.eqslRcvd.trimmed().toUpper());
}
} // namespace

LogDb::LogDb(QObject* parent) : QObject(parent) {
    conn_ = QStringLiteral("ttc-logdb-%1").arg(quintptr(this));
}

LogDb::~LogDb() {
    if (QSqlDatabase::contains(conn_)) {
        QSqlDatabase::database(conn_, false).close();
        QSqlDatabase::removeDatabase(conn_);
    }
}

QString LogDb::defaultPath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + "/logbook.sqlite";
}

bool LogDb::open(const QString& path) {
    path_ = path.isEmpty()
        ? AppSettings::instance().value(QStringLiteral("OrionLog/dbPath"), defaultPath()).toString()
        : path;
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSqlDatabase db = QSqlDatabase::contains(conn_)
        ? QSqlDatabase::database(conn_, false)
        : QSqlDatabase::addDatabase("QSQLITE", conn_);
    db.setDatabaseName(path_);
    if (!db.open()) return false;
    QSqlQuery q(db);
    if (!q.exec(QString::fromLatin1(kSchema))) return false;
    q.exec("CREATE INDEX IF NOT EXISTS idx_qso_call ON qso(call)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_qso_call_band ON qso(call, band)");
    // Repair migration: early imports bound absent text fields as SQL
    // NULL (a null QString binds as NULL, not ''), and NULL poisons
    // every <> comparison — the first LoTW sync skipped 6734 rows that
    // way. Normalize once per open; idempotent and cheap.
    for (const char* col : {"rst_s", "rst_r", "name", "qth", "grid",
                            "country", "pota", "comment", "qsl_rcvd",
                            "lotw_rcvd", "eqsl_rcvd", "up_lotw", "up_eqsl",
                            "up_qrz", "up_club", "up_hrdlog"})
        q.exec(QString("UPDATE qso SET %1='' WHERE %1 IS NULL")
                   .arg(QLatin1String(col)));
    return true;
}

bool LogDb::isOpen() const {
    return QSqlDatabase::contains(conn_)
        && QSqlDatabase::database(conn_, false).isOpen();
}

QSqlDatabase LogDb::database() const {
    return QSqlDatabase::database(conn_, false);
}

qint64 LogDb::addQso(const Qso& o) {
    if (!isOpen() || o.call.trimmed().isEmpty()) return -1;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(
        "INSERT INTO qso (ts_utc, call, band, freq_hz, mode, rst_s, rst_r,"
        " name, qth, grid, country, cqz, ituz, pota, comment,"
        " qsl_rcvd, lotw_rcvd, eqsl_rcvd) VALUES"
        " (:ts, :call, :band, :freq, :mode, :rsts, :rstr,"
        "  :name, :qth, :grid, :country, :cqz, :ituz, :pota, :comment,"
        "  :qsl, :lotw, :eqsl)");
    bindQso(q, o);
    if (!q.exec()) return -1;
    const qint64 id = q.lastInsertId().toLongLong();
    emit changed();
    return id;
}

bool LogDb::updateQso(const Qso& o) {
    if (!isOpen() || o.id < 0) return false;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(
        "UPDATE qso SET ts_utc=:ts, call=:call, band=:band, freq_hz=:freq,"
        " mode=:mode, rst_s=:rsts, rst_r=:rstr, name=:name, qth=:qth,"
        " grid=:grid, country=:country, cqz=:cqz, ituz=:ituz, pota=:pota,"
        " comment=:comment, qsl_rcvd=:qsl, lotw_rcvd=:lotw, eqsl_rcvd=:eqsl"
        " WHERE id=:id");
    bindQso(q, o);
    q.bindValue(":id", o.id);
    const bool ok = q.exec();
    if (ok) emit changed();
    return ok;
}

bool LogDb::deleteQso(qint64 id) {
    if (!isOpen()) return false;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("DELETE FROM qso WHERE id=:id");
    q.bindValue(":id", id);
    const bool ok = q.exec();
    if (ok) emit changed();
    return ok;
}

Qso LogDb::qso(qint64 id) const {
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("SELECT * FROM qso WHERE id=:id");
    q.bindValue(":id", id);
    if (q.exec() && q.next()) return qsoFromQuery(q);
    return {};
}

QList<Qso> LogDb::prevQsos(const QString& call, int limit) const {
    QList<Qso> out;
    if (!isOpen()) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("SELECT * FROM qso WHERE call=:c ORDER BY ts_utc DESC"
              " LIMIT :n");
    q.bindValue(":c", call.trimmed().toUpper());
    q.bindValue(":n", limit);
    if (q.exec())
        while (q.next()) out.append(qsoFromQuery(q));
    return out;
}

QList<Qso> LogDb::allQsos() const {
    QList<Qso> out;
    if (!isOpen()) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (q.exec("SELECT * FROM qso ORDER BY id"))
        while (q.next()) out.append(qsoFromQuery(q));
    return out;
}

int LogDb::count() const {
    if (!isOpen()) return 0;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (q.exec("SELECT COUNT(*) FROM qso") && q.next())
        return q.value(0).toInt();
    return 0;
}

AdifRecord LogDb::toAdif(const Qso& o) {
    AdifRecord r;
    const QDateTime ts = o.tsUtc.toUTC();
    r.insert("CALL", o.call.trimmed().toUpper());
    r.insert("QSO_DATE", ts.toString("yyyyMMdd"));
    r.insert("TIME_ON", ts.toString("HHmmss"));
    if (!o.band.isEmpty()) r.insert("BAND", o.band.toUpper());
    if (o.freqHz > 0)
        r.insert("FREQ", QString::number(o.freqHz / 1e6, 'f', 6));
    r.insert("MODE", o.mode.trimmed().toUpper());
    if (!o.rstS.isEmpty()) r.insert("RST_SENT", o.rstS);
    if (!o.rstR.isEmpty()) r.insert("RST_RCVD", o.rstR);
    if (!o.name.isEmpty()) r.insert("NAME", o.name);
    if (!o.qth.isEmpty()) r.insert("QTH", o.qth);
    if (!o.grid.isEmpty()) r.insert("GRIDSQUARE", o.grid);
    if (!o.country.isEmpty()) r.insert("COUNTRY", o.country);
    if (o.cqz > 0) r.insert("CQZ", QString::number(o.cqz));
    if (o.ituz > 0) r.insert("ITUZ", QString::number(o.ituz));
    if (!o.pota.isEmpty()) {
        r.insert("POTA_REF", o.pota);
        r.insert("SIG", "POTA");
        r.insert("SIG_INFO", o.pota);
    }
    if (!o.comment.isEmpty()) r.insert("COMMENT", o.comment);
    if (o.qslRcvd == "Y") r.insert("QSL_RCVD", "Y");
    if (o.lotwRcvd == "Y") r.insert("LOTW_QSL_RCVD", "Y");
    if (o.eqslRcvd == "Y") r.insert("EQSL_QSL_RCVD", "Y");
    return r;
}

Qso LogDb::fromAdif(const AdifRecord& r) {
    Qso o;
    o.call = r.value("CALL").toUpper();
    const QString d = r.value("QSO_DATE");
    QString t = r.value("TIME_ON");
    if (t.size() == 4) t += "00";
    o.tsUtc = QDateTime::fromString(d + t, "yyyyMMddHHmmss");
    o.tsUtc.setTimeZone(QTimeZone::utc());
    o.freqHz = qint64(r.value("FREQ").toDouble() * 1e6 + 0.5);
    o.band = r.value("BAND").toUpper();
    if (o.band.isEmpty() && o.freqHz > 0)
        o.band = bandForHz(o.freqHz);
    o.mode = r.value("MODE").toUpper();
    if (o.mode == "MFSK" && !r.value("SUBMODE").isEmpty())
        o.mode = r.value("SUBMODE").toUpper();    // FT4 travels as MFSK/FT4
    o.rstS = r.value("RST_SENT");
    o.rstR = r.value("RST_RCVD");
    o.name = r.value("NAME");
    o.qth = r.value("QTH");
    o.grid = r.value("GRIDSQUARE").toUpper();
    o.country = r.value("COUNTRY");
    o.cqz = r.value("CQZ").toInt();
    o.ituz = r.value("ITUZ").toInt();
    o.pota = r.value("POTA_REF").toUpper();
    if (o.pota.isEmpty() && r.value("SIG").compare("POTA", Qt::CaseInsensitive) == 0)
        o.pota = r.value("SIG_INFO").toUpper();
    o.comment = r.value("COMMENT");
    const auto yes = [&r](const char* k) {
        // Empty, never null — a null QString binds as SQL NULL and NULL
        // poisons every later <> comparison (the 6734-row lesson).
        return r.value(QLatin1String(k)).startsWith('Y', Qt::CaseInsensitive)
            ? QStringLiteral("Y") : QStringLiteral("");
    };
    o.qslRcvd  = yes("QSL_RCVD");
    o.lotwRcvd = yes("LOTW_QSL_RCVD");
    o.eqslRcvd = yes("EQSL_QSL_RCVD");
    return o;
}

int LogDb::applyLotwConfirmations(const QList<AdifRecord>& recs) {
    if (!isOpen() || recs.isEmpty()) return 0;
    QSqlDatabase db = QSqlDatabase::database(conn_);
    db.transaction();
    QSqlQuery q(db);
    q.prepare(
        "UPDATE qso SET lotw_rcvd='Y' WHERE call=:call AND band=:band"
        " AND mode=:mode AND date(ts_utc)=:day"
        " AND IFNULL(lotw_rcvd,'')<>'Y'");
    int n = 0;
    for (const AdifRecord& r : recs) {
        if (!r.value("QSL_RCVD").startsWith('Y', Qt::CaseInsensitive))
            continue;
        const QString call = r.value("CALL").toUpper();
        QString mode = r.value("MODE").toUpper();
        if (mode == "MFSK" && !r.value("SUBMODE").isEmpty())
            mode = r.value("SUBMODE").toUpper();
        const QString band = r.value("BAND").toUpper();
        const QDate day =
            QDate::fromString(r.value("QSO_DATE"), "yyyyMMdd");
        if (call.isEmpty() || band.isEmpty() || !day.isValid()) continue;
        // Exact day first; a QSO logged around 0000Z can sit a day off.
        for (int dd : {0, -1, 1}) {
            q.bindValue(":call", call);
            q.bindValue(":band", band);
            q.bindValue(":mode", mode);
            q.bindValue(":day", day.addDays(dd).toString("yyyy-MM-dd"));
            if (q.exec() && q.numRowsAffected() > 0) {
                n += q.numRowsAffected();
                break;
            }
        }
    }
    db.commit();
    if (n > 0) emit changed();
    return n;
}

bool LogDb::hasNearDuplicate(const Qso& o) const {
    if (!isOpen()) return false;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(
        "SELECT id FROM qso WHERE call=:call AND band=:band AND mode=:mode"
        " AND ABS(strftime('%s', ts_utc) - :secs) < 300 LIMIT 1");
    q.bindValue(":call", o.call.trimmed().toUpper());
    q.bindValue(":band", o.band.trimmed().toUpper());
    q.bindValue(":mode", o.mode.trimmed().toUpper());
    q.bindValue(":secs", o.tsUtc.toUTC().toSecsSinceEpoch());
    return q.exec() && q.next();
}

int LogDb::importAdif(QIODevice& in, const CtyDatParser* cty, QString* err) {
    if (!isOpen()) { if (err) *err = "log database not open"; return -1; }
    const QList<AdifRecord> recs = Adif::parse(in);
    if (recs.isEmpty()) { if (err) *err = "no ADIF records found"; return 0; }
    QSqlDatabase db = QSqlDatabase::database(conn_);
    db.transaction();
    QSqlQuery dup(db);
    dup.prepare(
        "SELECT id FROM qso WHERE call=:call AND band=:band AND mode=:mode"
        " AND ABS(strftime('%s', ts_utc) - :secs) < 300 LIMIT 1");
    QSqlQuery ins(db);
    // Imported QSOs are HISTORY: stamp every upload column 'Y' so the
    // online-log push never re-uploads a 13k-QSO archive to anyone.
    ins.prepare(
        "INSERT INTO qso (ts_utc, call, band, freq_hz, mode, rst_s, rst_r,"
        " name, qth, grid, country, cqz, ituz, pota, comment,"
        " qsl_rcvd, lotw_rcvd, eqsl_rcvd,"
        " up_lotw, up_eqsl, up_qrz, up_club, up_hrdlog) VALUES"
        " (:ts, :call, :band, :freq, :mode, :rsts, :rstr,"
        "  :name, :qth, :grid, :country, :cqz, :ituz, :pota, :comment,"
        "  :qsl, :lotw, :eqsl, 'Y', 'Y', 'Y', 'Y', 'Y')");
    int added = 0;
    for (const AdifRecord& r : recs) {
        Qso o = fromAdif(r);
        if (o.call.isEmpty() || !o.tsUtc.isValid()) continue;
        if (o.country.isEmpty() && cty) {
            if (const DxccEntity* e = cty->entityForCallsign(o.call)) {
                o.country = e->name;
                o.cqz = e->cqZone;
                o.ituz = e->ituZone;
            }
        }
        dup.bindValue(":call", o.call.trimmed().toUpper());
        dup.bindValue(":band", o.band);
        dup.bindValue(":mode", o.mode);
        dup.bindValue(":secs", o.tsUtc.toUTC().toSecsSinceEpoch());
        if (dup.exec() && dup.next()) continue;   // near-duplicate: skip
        bindQso(ins, o);
        if (ins.exec()) ++added;
    }
    db.commit();
    if (added > 0) emit changed();
    return added;
}

bool LogDb::exportAdif(QIODevice& out) const {
    if (!isOpen()) return false;
    out.write(Adif::fileHeader().toUtf8());
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (!q.exec("SELECT * FROM qso ORDER BY ts_utc")) return false;
    while (q.next())
        out.write(Adif::writeRecord(toAdif(qsoFromQuery(q))).toUtf8());
    return true;
}

namespace {
// Whitelisted: svc names come from QslUploader's fixed table, but the
// column name lands in SQL text, so map explicitly.
QString upColumn(const QString& svc) {
    if (svc == "lotw") return QStringLiteral("up_lotw");
    if (svc == "eqsl") return QStringLiteral("up_eqsl");
    if (svc == "qrz") return QStringLiteral("up_qrz");
    if (svc == "club") return QStringLiteral("up_club");
    if (svc == "hrdlog") return QStringLiteral("up_hrdlog");
    return QString();
}
} // namespace

bool LogDb::setUploadState(qint64 id, const QString& svc, QChar st) {
    const QString col = upColumn(svc);
    if (!isOpen() || col.isEmpty() || id < 0) return false;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(QString("UPDATE qso SET %1=:st WHERE id=:id").arg(col));
    q.bindValue(":st", st == ' ' ? QString() : QString(st));
    q.bindValue(":id", id);
    return q.exec();     // deliberately no changed(): bookkeeping, not data
}

QList<Qso> LogDb::pendingUploads(const QString& svc, int limit) const {
    QList<Qso> out;
    const QString col = upColumn(svc);
    if (!isOpen() || col.isEmpty()) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(QString("SELECT * FROM qso WHERE %1='' OR %1='E'"
                      " ORDER BY ts_utc LIMIT :n").arg(col));
    q.bindValue(":n", limit);
    if (q.exec())
        while (q.next()) out.append(qsoFromQuery(q));
    return out;
}

QString LogDb::bandForHz(qint64 hz) {
    struct B { qint64 lo, hi; const char* name; };
    static const B bands[] = {
        {1800000, 2000000, "160M"}, {3500000, 4000000, "80M"},
        {5250000, 5450000, "60M"},  {7000000, 7300000, "40M"},
        {10100000, 10150000, "30M"}, {14000000, 14350000, "20M"},
        {18068000, 18168000, "17M"}, {21000000, 21450000, "15M"},
        {24890000, 24990000, "12M"}, {28000000, 29700000, "10M"},
        {50000000, 54000000, "6M"},  {144000000, 148000000, "2M"},
        {420000000, 450000000, "70CM"}, {902000000, 928000000, "33CM"},
    };
    for (const B& b : bands)
        if (hz >= b.lo && hz <= b.hi) return QString::fromLatin1(b.name);
    return QString();
}

QList<LogDb::WorkedRow> LogDb::workedRows() const {
    QList<WorkedRow> out;
    if (!isOpen()) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (!q.exec("SELECT call, band, mode, country, pota,"
                " (qsl_rcvd='Y' OR lotw_rcvd='Y' OR eqsl_rcvd='Y') FROM qso"))
        return out;
    while (q.next()) {
        WorkedRow r;
        r.call    = q.value(0).toString();
        r.band    = q.value(1).toString();
        r.mode    = q.value(2).toString();
        r.country = q.value(3).toString();
        r.park    = q.value(4).toString();
        r.conf    = q.value(5).toBool();
        out.append(r);
    }
    return out;
}

} // namespace AetherSDR::orion::logbook
