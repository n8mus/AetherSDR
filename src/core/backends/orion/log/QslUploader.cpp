// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/log/QslUploader.cpp) for the AetherSDR Orion
// fork (logbook uploads): namespace, AetherSDR settings (OrionUp/...) in place
// of QSettings, secrets from the system keychain (OrionUploadSecrets), each
// service's address a setting (tests point it at a local stand-in), and
// AetherSDR as the program id.
#include "core/backends/orion/log/QslUploader.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include "core/AppSettings.h"
#include "core/backends/orion/log/LogDb.h"
#include "core/backends/orion/log/OrionUploadSecrets.h"

namespace AetherSDR::orion::logbook {

namespace {
// An empty stored value means "use the default" — the setup dialog writes
// live on every keystroke, so type-then-clear leaves an empty string
// BEHIND the key and QSettings' own default kicks in only for absent
// keys. (Live-found: Test TQSL launched "" and reported a bad path.)
QString cfg(const QString& key, const QString& def = QString()) {
    const QString v = AppSettings::instance().value("OrionUp/" + key).toString().trimmed();
    return v.isEmpty() ? def : v;
}
bool on(const QString& svc) {
    return AppSettings::instance().value("OrionUp/" + svc + "/enabled", false).toBool();
}
// The services' addresses: settings, so a test can point one at a local
// stand-in; empty means the real service.
QString svcUrl(const char* key, const char* real) {
    return cfg(QLatin1String(key), QLatin1String(real));
}
QString tqslDefault() {
#ifdef Q_OS_WIN
    return QStringLiteral(
        "C:/Program Files (x86)/TrustedQSL/tqsl.exe");
#else
    return QStringLiteral("tqsl");
#endif
}
// Query strings are built by hand — QUrlQuery re-encodes on output, which
// double-encodes anything passed through toPercentEncoding first.
QByteArray enc(const QString& s) {
    return QUrl::toPercentEncoding(s);
}
} // namespace

QslUploader::QslUploader(LogDb* db, OrionUploadSecrets* secrets, QObject* parent)
    : QObject(parent), db_(db), secrets_(secrets) {
    net_ = new QNetworkAccessManager(this);
    // Every result also becomes a line in the scrolling traffic print.
    connect(this, &QslUploader::serviceResult, this,
            [this](const QString& svc, bool ok, const QString& d) {
                const QString line =
                    QDateTime::currentDateTimeUtc().toString("HH:mm:ss ")
                    + (ok ? QStringLiteral("✓ ") : QStringLiteral("✕ "))
                    + svc + "  " + d;
                traffic_.append(line);
                while (traffic_.size() > 100) traffic_.removeFirst();
                emit trafficLine(line);
            });
    // One catch-up sweep after startup (QSOs logged while a service was
    // down); beyond that, failures wait for the Retry button — no
    // periodic background retrying (operator's call).
    sweepSoon(15000);
    // And one QSL download, cqrlog-style, once the startup dust settles.
    QTimer::singleShot(25000, this, [this] {
        if (on("lotw")) syncLotwQsls();
    });
}

QString QslUploader::sec(const QString& key) const {
    return secrets_ ? secrets_->value(key) : QString();
}

void QslUploader::syncLotwQsls() {
    const QString user = cfg("lotw/login"), pass = sec("lotw/password");
    if (user.isEmpty() || pass.isEmpty()) {
        emit serviceResult("lotw", false,
                           "QSL sync needs the LoTW login + password");
        return;
    }
    const QString since =
        cfg("lotw/qslSince", QStringLiteral("1900-01-01"));
    QUrl url(svcUrl("lotw/reportUrl", "https://lotw.arrl.org/lotwuser/lotwreport.adi"));
    url.setQuery(QString::fromLatin1(
                     "login=" + enc(user) + "&password=" + enc(pass)
                     + "&qso_query=1&qso_qsl=yes&qso_qslsince="
                     + enc(since)),
                 QUrl::StrictMode);
    QNetworkReply* rep = net_->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QByteArray body = rep->readAll();
        if (rep->error() != QNetworkReply::NoError) {
            emit serviceResult("lotw", false,
                               "QSL sync: " + rep->errorString());
            return;
        }
        if (body.contains("password incorrect")
            || body.contains("Username/password")) {
            emit serviceResult("lotw", false,
                               "QSL sync: LoTW rejected the login");
            return;
        }
        const auto recs = Adif::parseBytes(body);
        const int n = db_ ? db_->applyLotwConfirmations(recs) : 0;
        // Watermark a day back so a slow LoTW batch is never skipped.
        AppSettings::instance().setValue(
            "OrionUp/lotw/qslSince",
            QDate::currentDate().addDays(-1).toString("yyyy-MM-dd"));
        AppSettings::instance().save();
        emit serviceResult(
            "lotw", true,
            n > 0 ? QString("QSL sync: %1 new confirmation(s)").arg(n)
                  : QString("QSL sync: no new confirmations (%1 in the"
                            " report)").arg(recs.size()));
    });
}

void QslUploader::pushQso(qint64 id) {
    if (!db_ || id < 0) return;
    const Qso q = db_->qso(id);
    if (q.id < 0) return;
    for (const char* svc : {"eqsl", "qrz", "club", "hrdlog"})
        if (on(QLatin1String(svc))) pushHttp(q, QLatin1String(svc));
    if (on("lotw")) runTqslBatch();
    mirrorToLoggers(q);
}

void QslUploader::sweepSoon(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { sweep(); });
}

void QslUploader::sweep() {
    if (!db_) return;
    for (const char* svc : {"eqsl", "qrz", "club", "hrdlog"}) {
        if (!on(QLatin1String(svc))) continue;
        const auto pend = db_->pendingUploads(QLatin1String(svc), 10);
        for (const Qso& q : pend) pushHttp(q, QLatin1String(svc));
    }
    if (on("lotw")) runTqslBatch();
}

QString QslUploader::adifFor(const Qso& q, const QString& svc) const {
    AdifRecord r = LogDb::toAdif(q);
    // QSL-received flags describe MY log, not the outgoing report.
    r.remove("QSL_RCVD");
    r.remove("LOTW_QSL_RCVD");
    r.remove("EQSL_QSL_RCVD");
    if (svc == "qrz") {                 // the QRZ API rejects these fields
        r.remove("POTA_REF");
        r.remove("SIG");
        r.remove("SIG_INFO");
    }
    return Adif::writeRecord(r).trimmed();
}

void QslUploader::pushHttp(const Qso& q, const QString& svc) {
    const QString adif = adifFor(q, svc);
    const qint64 id = q.id;
    QNetworkReply* rep = nullptr;

    if (svc == "eqsl") {
        const QString user = cfg("eqsl/user"), pass = sec("eqsl/password");
        if (user.isEmpty() || pass.isEmpty()) return;
        QByteArray qs = "ADIFData="
            + enc("<PROGRAMID:9>AetherSDR <EOH>\r\n" + adif)
            + "&EQSL_USER=" + enc(user) + "&EQSL_PSWD=" + enc(pass);
        const QString nick = cfg("eqsl/nickname");
        if (!nick.isEmpty()) qs += "&EQSL_QTH_NICKNAME=" + enc(nick);
        QUrl url(svcUrl("eqsl/uploadUrl", "https://www.eQSL.cc/qslcard/importADIF.cfm"));
        url.setQuery(QString::fromLatin1(qs), QUrl::StrictMode);
        rep = net_->get(QNetworkRequest(url));
    } else if (svc == "qrz") {
        const QString key = sec("qrz/key");
        if (key.isEmpty()) return;
        QNetworkRequest req(QUrl(svcUrl("qrz/url", "https://logbook.qrz.com/api")));
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
        const QByteArray body =
            "KEY=" + enc(key) + "&ACTION=INSERT&ADIF=" + enc(adif);
        rep = net_->post(req, body);
    } else if (svc == "club") {
        const QString mail = cfg("club/email"), pass = sec("club/password");
        const QString call = cfg("club/call"), api = sec("club/apikey");
        if (mail.isEmpty() || pass.isEmpty() || call.isEmpty()
            || api.isEmpty())
            return;
        // The documented endpoint is secure.clublog.org (GridTracker posts
        // to clublog.org and rides the redirect; go direct instead — a
        // redirected POST can degrade to GET).
        QNetworkRequest req(
            QUrl(svcUrl("club/url", "https://secure.clublog.org/realtime.php")));
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
        const QByteArray body = "email=" + enc(mail)
            + "&password=" + enc(pass) + "&callsign=" + enc(call)
            + "&api=" + enc(api) + "&adif=" + enc(adif);
        rep = net_->post(req, body);
    } else if (svc == "hrdlog") {
        const QString call = cfg("hrdlog/call"), code = sec("hrdlog/code");
        if (call.isEmpty() || code.isEmpty()) return;
        QNetworkRequest req(QUrl(svcUrl("hrdlog/url", "https://www.hrdlog.net/NewEntry.aspx")));
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
        const QByteArray body = "Callsign=" + enc(call) + "&Code="
            + enc(code) + "&App=AetherSDR&ADIFData=" + enc(adif);
        rep = net_->post(req, body);
    }
    if (!rep) return;

    const QString call = q.call;
    connect(rep, &QNetworkReply::finished, this,
            [this, rep, svc, id, call] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        bool ok = rep->error() == QNetworkReply::NoError;
        bool dupe = false;
        QString why = ok ? QString() : rep->errorString();
        if (ok) {
            if (svc == "eqsl") {
                if (body.contains("Duplicate", Qt::CaseInsensitive))
                    dupe = true;                  // already there = delivered
                else if (body.contains("Error", Qt::CaseInsensitive)
                         || body.contains("No such",  Qt::CaseInsensitive)) {
                    ok = false;
                    why = body.contains("Username", Qt::CaseInsensitive)
                        ? "eQSL rejected the credentials"
                        : "eQSL rejected the record";
                }
            } else if (svc == "qrz") {
                if (body.contains("STATUS=FAIL", Qt::CaseInsensitive)) {
                    if (body.contains("duplicate", Qt::CaseInsensitive))
                        dupe = true;
                    else {
                        ok = false;
                        why = "QRZ: " + body.left(120);
                    }
                }
            }
            // club/hrdlog: HTTP success is the acknowledgment we get.
        }
        db_->setUploadState(id, svc, ok ? QChar('Y') : QChar('E'));
        // Every outcome is announced — accepted, duplicate, or failed —
        // GridTracker-style, so the operator sees each QSO land.
        emit serviceResult(svc, ok,
                           ok ? call + (dupe ? " — duplicate, already there"
                                             : " accepted")
                              : call + ": " + why);
    });
}

void QslUploader::runTqslBatch() {
    if (tqslRunning_ || !db_) return;
    const QString station = cfg("lotw/station");
    const QString tqsl = cfg("lotw/tqslPath", tqslDefault());
    if (station.isEmpty()) return;
    const auto pend = db_->pendingUploads("lotw", 100);
    if (pend.isEmpty()) return;

    QString text = "<PROGRAMID:9>AetherSDR <EOH>\n";
    QList<qint64> ids;
    for (const Qso& q : pend) {
        text += Adif::writeRecord(LogDb::toAdif(q));
        ids.push_back(q.id);
    }
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString path = dir + QString("/aethersdr-lotw-%1.adi")
                                   .arg(QCoreApplication::applicationPid());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(text.toUtf8());
    f.close();

    QStringList args{"-a", "all", "-l", station};
    const QString pw = sec("lotw/tqslPassword");
    if (!pw.isEmpty()) { args << "-p" << pw; }
    args << "-q" << "-x" << "-d" << "-u" << path;

    tqslRunning_ = true;
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this,
            [this, p, path, ids](int code, QProcess::ExitStatus st) {
        const QString err = QString::fromUtf8(p->readAllStandardError());
        p->deleteLater();
        tqslRunning_ = false;
        QFile::remove(path);
        const bool dupes = err.contains("duplicate", Qt::CaseInsensitive);
        const bool ok = st == QProcess::NormalExit
            && (err.contains("Final Status: Success") || dupes
                || code == 0);
        for (const qint64 id : ids)
            db_->setUploadState(id, "lotw", ok ? QChar('Y') : QChar('E'));
        if (ok)
            emit serviceResult(
                "lotw", true,
                dupes ? QString("%1 QSO(s) — duplicates, already at LoTW")
                            .arg(ids.size())
                      : QString("%1 QSO(s) signed and uploaded")
                            .arg(ids.size()));
        else
            emit serviceResult(
                "lotw", false,
                err.isEmpty() ? QString("tqsl exit %1").arg(code)
                              : err.right(160).trimmed());
    });
    connect(p, &QProcess::errorOccurred, this,
            [this, p, path, ids](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;   // finished handles others
        p->deleteLater();
        tqslRunning_ = false;
        QFile::remove(path);
        for (const qint64 id : ids) db_->setUploadState(id, "lotw", 'E');
        emit serviceResult("lotw", false, "can't run tqsl — check the path");
    });
    p->start(tqsl, args);
    QTimer::singleShot(60000, p, [p] {
        if (p->state() != QProcess::NotRunning) p->kill();
    });
}

void QslUploader::mirrorToLoggers(const Qso& q) {
    // N1MM+: a raw ADIF record as one UDP datagram (WSJT-X convention).
    if (on("n1mm")) {
        const QString ip = cfg("n1mm/ip", "127.0.0.1");
        const quint16 port =
            quint16(AppSettings::instance().value("OrionUp/n1mm/port", 2333).toUInt());
        QUdpSocket s;
        s.writeDatagram(adifFor(q, "n1mm").toUtf8(), QHostAddress(ip), port);
    }
    // HRD Logbook: its TCP command console ("db add {F=\"v\" ...}").
    if (on("hrd")) {
        const QString ip = cfg("hrd/ip", "127.0.0.1");
        const quint16 port =
            quint16(AppSettings::instance().value("OrionUp/hrd/port", 7826).toUInt());
        AdifRecord r = LogDb::toAdif(q);
        // HRD wants the frequency without the decimal point (Hz digits).
        if (r.contains("FREQ"))
            r["FREQ"] = r["FREQ"].remove('.');
        QString cmd = "ver\rdb add {";
        for (auto it = r.constBegin(); it != r.constEnd(); ++it)
            cmd += it.key() + "=\"" + it.value() + "\" ";
        cmd += "}\rexit\r";
        auto* sock = new QTcpSocket(this);
        connect(sock, &QTcpSocket::connected, this, [sock, cmd] {
            sock->write(cmd.toUtf8());
            sock->flush();
            sock->disconnectFromHost();
        });
        connect(sock, &QTcpSocket::disconnected, sock,
                &QObject::deleteLater);
        connect(sock, &QTcpSocket::errorOccurred, this,
                [this, sock](QAbstractSocket::SocketError) {
            emit serviceResult("hrd", false,
                               "HRD Logbook not reachable: "
                                   + sock->errorString());
            sock->deleteLater();
        });
        sock->connectToHost(ip, port);
    }
}

// ---- Setup-window tests ----------------------------------------------

void QslUploader::testLotwDownload() {
    const QString user = cfg("lotw/login"), pass = sec("lotw/password");
    if (user.isEmpty() || pass.isEmpty()) {
        emit serviceResult("lotw-dl", false, "enter LoTW login + password");
        return;
    }
    QUrl url(svcUrl("lotw/reportUrl", "https://lotw.arrl.org/lotwuser/lotwreport.adi"));
    url.setQuery(QString::fromLatin1(
                     "login=" + enc(user) + "&password=" + enc(pass)
                     + "&qso_query=1&qso_qsosince=2100-01-01"),
                 QUrl::StrictMode);                  // auth check, no data
    QNetworkReply* rep = net_->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        if (rep->error() != QNetworkReply::NoError)
            emit serviceResult("lotw-dl", false, rep->errorString());
        else if (body.contains("password incorrect", Qt::CaseInsensitive)
                 || body.contains("Username/password", Qt::CaseInsensitive))
            emit serviceResult("lotw-dl", false, "LoTW rejected the login");
        else
            emit serviceResult("lotw-dl", true, "logged in OK");
    });
}

void QslUploader::testTqsl() {
    const QString tqsl = cfg("lotw/tqslPath", tqslDefault());
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this,
            [this, p](int code, QProcess::ExitStatus st) {
        const QString err =
            QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();
        // tqsl -q -v prints the version to stderr and exits NONZERO even
        // when perfectly healthy (live-verified: 2.7.3 exits 255) — the
        // version line is the success signal, not the exit code.
        if (st != QProcess::NormalExit
            || !err.contains("TQSL Version", Qt::CaseInsensitive))
            emit serviceResult("lotw-tqsl", false,
                               err.isEmpty()
                                   ? QString("tqsl exit %1").arg(code)
                                   : err.left(120));
        else {
            QString d = err.left(80);
            const QString pw = sec("lotw/tqslPassword");
            d += pw.isEmpty() ? " · no key password (correct for this"
                                " station's certificates)"
                              : " · key password set";
            emit serviceResult("lotw-tqsl", true, d);
        }
    });
    connect(p, &QProcess::errorOccurred, this,
            [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        emit serviceResult("lotw-tqsl", false,
                           "can't run tqsl — check the path");
        p->deleteLater();
    });
    p->start(tqsl, {"-q", "-v"});
}

void QslUploader::testEqsl() {
    const QString user = cfg("eqsl/user"), pass = sec("eqsl/password");
    if (user.isEmpty() || pass.isEmpty()) {
        emit serviceResult("eqsl", false, "enter eQSL user + password");
        return;
    }
    QUrl url(svcUrl("eqsl/inboxUrl", "https://www.eQSL.cc/qslcard/DownloadInBox.cfm"));
    url.setQuery(QString::fromLatin1("UserName=" + enc(user) + "&Password="
                                     + enc(pass) + "&RcvdSince=21000101"),
                 QUrl::StrictMode);
    QNetworkReply* rep = net_->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        if (rep->error() != QNetworkReply::NoError)
            emit serviceResult("eqsl", false, rep->errorString());
        else if (body.contains("No such Username", Qt::CaseInsensitive)
                 || body.contains("incorrect password",
                                  Qt::CaseInsensitive))
            emit serviceResult("eqsl", false, "eQSL rejected the login");
        else
            emit serviceResult("eqsl", true, "logged in OK");
    });
}

void QslUploader::testQrz() {
    const QString key = sec("qrz/key");
    if (key.isEmpty()) {
        emit serviceResult("qrz", false, "enter the logbook API key");
        return;
    }
    QNetworkRequest req(QUrl(svcUrl("qrz/url", "https://logbook.qrz.com/api")));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    QNetworkReply* rep =
        net_->post(req, "KEY=" + enc(key) + "&ACTION=STATUS");
    connect(rep, &QNetworkReply::finished, this, [this, rep] {
        rep->deleteLater();
        const QString body = QString::fromUtf8(rep->readAll());
        if (rep->error() != QNetworkReply::NoError)
            emit serviceResult("qrz", false, rep->errorString());
        else if (body.contains("RESULT=OK", Qt::CaseInsensitive)) {
            QString n;
            for (const QString& kv : body.split('&'))
                if (kv.startsWith("COUNT=")) n = kv.mid(6);
            emit serviceResult("qrz", true,
                               n.isEmpty() ? "key OK"
                                           : n + " records in the logbook");
        } else
            emit serviceResult("qrz", false,
                               "QRZ rejected the key: " + body.left(100));
    });
}

void QslUploader::testHrdlogNet() {
    // hrdlog.net has no cheap auth probe that doesn't insert a QSO; the
    // honest test is field presence (the first real upload proves it).
    const QString call = cfg("hrdlog/call"), code = sec("hrdlog/code");
    if (call.isEmpty() || code.isEmpty())
        emit serviceResult("hrdlog", false, "enter callsign + upload code");
    else
        emit serviceResult("hrdlog", true,
                           "fields set — verified on first upload");
}

void QslUploader::testLoggerPush(const QString& svc) {
    const QString ip = cfg(svc + "/ip", "127.0.0.1");
    const quint16 port = quint16(AppSettings::instance()
        .value("OrionUp/" + svc + "/port", svc == "hrd" ? 7826 : 2333).toUInt());
    auto* sock = new QTcpSocket(this);
    connect(sock, &QTcpSocket::connected, this, [this, sock, svc] {
        emit serviceResult(svc, true, "reachable");
        sock->disconnectFromHost();
        sock->deleteLater();
    });
    connect(sock, &QTcpSocket::errorOccurred, this,
            [this, sock, svc](QAbstractSocket::SocketError) {
        emit serviceResult(svc, false,
                           svc == "n1mm"
                               ? "nothing listening (UDP is fire-and-forget;"
                                 " this checks a TCP listener)"
                               : "not reachable: " + sock->errorString());
        sock->deleteLater();
    });
    sock->connectToHost(ip, port);
}

} // namespace AetherSDR::orion::logbook
