// SPDX-License-Identifier: GPL-2.0-or-later
// Online-log uploads (logbook): the console's QslUploader, ported, against a
// local stand-in HTTP server and a fake tqsl. No real service is contacted,
// the secrets never touch the keychain, and the settings live in a
// throwaway folder.
//
//   cmake --build build --target orion_upload_test && ./build/orion_upload_test

#include "core/AppSettings.h"
#include "core/backends/orion/log/LogDb.h"
#include "core/backends/orion/log/OrionUploadSecrets.h"
#include "core/backends/orion/log/QslUploader.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUdpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::orion::logbook;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// A minimal HTTP/1.1 server: records each request, answers with the
// configured status and body.
class StandIn : public QObject {
public:
    struct Request {
        QByteArray method, path;
        QByteArray body;
    };
    QList<Request> requests;
    int status = 200;
    QByteArray reply = "OK";

    bool listen() { return m_srv.listen(QHostAddress::LocalHost, 0); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_srv.serverPort()); }

    StandIn()
    {
        connect(&m_srv, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_srv.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                connect(s, &QTcpSocket::readyRead, this, [this, s, buf] {
                    *buf += s->readAll();
                    const int hdrEnd = buf->indexOf("\r\n\r\n");
                    if (hdrEnd < 0) return;
                    const QByteArray head = buf->left(hdrEnd);
                    int len = 0;
                    for (const QByteArray& line : head.split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            len = line.mid(15).trimmed().toInt();
                    }
                    if (buf->size() < hdrEnd + 4 + len) return;
                    const QList<QByteArray> first = head.split('\n').first().split(' ');
                    requests.append({first.value(0), first.value(1), buf->mid(hdrEnd + 4, len)});
                    const QByteArray resp = "HTTP/1.1 " + QByteArray::number(status)
                        + (status == 200 ? " OK" : " Error") + "\r\nContent-Type: text/plain\r\n"
                        + "Content-Length: " + QByteArray::number(reply.size())
                        + "\r\nConnection: close\r\n\r\n" + reply;
                    s->write(resp);
                    s->disconnectFromHost();
                });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }

private:
    QTcpServer m_srv;
};

void setCfg(const QString& key, const QVariant& v)
{
    AppSettings::instance().setValue(QStringLiteral("OrionUp/") + key, v);
}

QString upState(LogDb& db, qint64 id, const char* col)
{
    QSqlQuery q(db.database());
    q.prepare(QStringLiteral("SELECT %1 FROM qso WHERE id=:id").arg(QLatin1String(col)));
    q.bindValue(QStringLiteral(":id"), id);
    return q.exec() && q.next() ? q.value(0).toString() : QStringLiteral("?");
}

qint64 addQso(LogDb& db, const char* call, const QString& pota = QString())
{
    Qso q;
    q.call = QString::fromLatin1(call);
    q.tsUtc = QDateTime(QDate(2026, 10, 18), QTime(15, 0, 0), QTimeZone::utc());
    q.freqHz = 14025000;
    q.band = QStringLiteral("20M");
    q.mode = QStringLiteral("CW");
    q.rstS = q.rstR = QStringLiteral("599");
    q.pota = pota;
    return db.addQso(q);
}

bool waitResults(QSignalSpy& spy, int n, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (spy.count() < n && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return spy.count() >= n;
}

}  // namespace

int main(int argc, char** argv)
{
    QTemporaryDir settingsDir;
    qputenv("AETHER_SETTINGS_DIR", settingsDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;

    StandIn web;
    if (!web.listen()) {
        std::printf("orion_upload_test: cannot listen\n");
        return 1;
    }
    setCfg(QStringLiteral("eqsl/uploadUrl"), web.base() + "/eqsl/importADIF.cfm");
    setCfg(QStringLiteral("eqsl/inboxUrl"), web.base() + "/eqsl/DownloadInBox.cfm");
    setCfg(QStringLiteral("qrz/url"), web.base() + "/qrz/api");
    setCfg(QStringLiteral("club/url"), web.base() + "/club/realtime.php");
    setCfg(QStringLiteral("hrdlog/url"), web.base() + "/hrdlog/NewEntry.aspx");
    setCfg(QStringLiteral("lotw/reportUrl"), web.base() + "/lotw/lotwreport.adi");

    LogDb db;
    if (!db.open(dir.filePath("log.sqlite"))) {
        std::printf("orion_upload_test: cannot open log\n");
        return 1;
    }
    OrionUploadSecrets secrets(/*memoryOnly=*/true);
    secrets.load();
    QslUploader up(&db, &secrets);
    QSignalSpy results(&up, &QslUploader::serviceResult);

    std::printf("-- nothing goes out until a service is switched on\n");
    const qint64 first = addQso(db, "W1AW");
    up.pushQso(first);
    pump(300);
    check(web.requests.isEmpty() && results.isEmpty(), "all services off by default: nothing sent");

    std::printf("-- eQSL\n");
    setCfg(QStringLiteral("eqsl/enabled"), true);
    setCfg(QStringLiteral("eqsl/user"), QStringLiteral("N8EM"));
    secrets.setValue(QStringLiteral("eqsl/password"), QStringLiteral("eqsl-secret"));
    web.reply = "Result: 1 out of 1 records added";
    up.pushQso(first);
    check(waitResults(results, 1) && results.last().at(1).toBool(), "accepted");
    const QUrlQuery eq(QUrl(QString::fromLatin1(web.requests.last().path)).query());
    check(web.requests.last().method == "GET"
              && eq.queryItemValue(QStringLiteral("EQSL_USER")) == "N8EM"
              && eq.queryItemValue(QStringLiteral("EQSL_PSWD")) == "eqsl-secret"
              && QUrl::fromPercentEncoding(eq.queryItemValue(QStringLiteral("ADIFData"), QUrl::FullyEncoded).toLatin1())
                     .contains(QStringLiteral("<PROGRAMID:9>AetherSDR"))
              && QUrl::fromPercentEncoding(eq.queryItemValue(QStringLiteral("ADIFData"), QUrl::FullyEncoded).toLatin1())
                     .contains(QStringLiteral("<CALL:4>W1AW")),
          "the console's request: user, password from the keychain store, ADIF with AetherSDR's id");
    check(upState(db, first, "up_eqsl") == "Y", "marked delivered");
    web.reply = "Warning: Duplicate";
    results.clear();
    up.pushQso(first);
    check(waitResults(results, 1) && results.last().at(1).toBool()
              && results.last().at(2).toString().contains("duplicate"),
          "a duplicate counts as delivered");
    web.reply = "Error: No such Username/Password found";
    results.clear();
    const qint64 second = addQso(db, "DL1ABC");
    up.pushQso(second);
    check(waitResults(results, 1) && !results.last().at(1).toBool()
              && upState(db, second, "up_eqsl") == "E",
          "a rejection is a failure, marked for retry");
    setCfg(QStringLiteral("eqsl/enabled"), false);

    std::printf("-- QRZ logbook\n");
    setCfg(QStringLiteral("qrz/enabled"), true);
    secrets.setValue(QStringLiteral("qrz/key"), QStringLiteral("ABCD-1234"));
    web.reply = "RESULT=OK&LOGID=42&COUNT=1";
    results.clear();
    const qint64 pota = addQso(db, "K2POTA", QStringLiteral("US-1234"));
    up.pushQso(pota);
    check(waitResults(results, 1) && results.last().at(1).toBool(), "accepted");
    const QUrlQuery qz(QString::fromLatin1(web.requests.last().body));
    const QString qzAdif = QUrl::fromPercentEncoding(qz.queryItemValue(QStringLiteral("ADIF"), QUrl::FullyEncoded).toLatin1());
    check(web.requests.last().method == "POST" && qz.queryItemValue(QStringLiteral("KEY")) == "ABCD-1234"
              && qz.queryItemValue(QStringLiteral("ACTION")) == "INSERT"
              && qzAdif.contains(QStringLiteral("K2POTA")) && !qzAdif.contains(QStringLiteral("POTA_REF"))
              && !qzAdif.contains(QStringLiteral("SIG_INFO")),
          "POST with the key, ACTION=INSERT, POTA fields stripped (the API rejects them)");
    web.reply = "STATUS=FAIL&REASON=wrong key";
    results.clear();
    up.pushQso(first);
    check(waitResults(results, 1) && !results.last().at(1).toBool(), "STATUS=FAIL is a failure");
    setCfg(QStringLiteral("qrz/enabled"), false);

    std::printf("-- ClubLog\n");
    setCfg(QStringLiteral("club/enabled"), true);
    setCfg(QStringLiteral("club/email"), QStringLiteral("op@example.org"));
    setCfg(QStringLiteral("club/call"), QStringLiteral("N8EM"));
    secrets.setValue(QStringLiteral("club/password"), QStringLiteral("club-secret"));
    const int before = web.requests.size();
    results.clear();
    up.pushQso(first);
    pump(300);
    check(web.requests.size() == before, "no application API key: nothing sent");
    secrets.setValue(QStringLiteral("club/apikey"), QStringLiteral("app-key"));
    web.reply = "OK";
    up.pushQso(first);
    check(waitResults(results, 1) && results.last().at(1).toBool(), "with all four fields: accepted");
    const QUrlQuery cl(QString::fromLatin1(web.requests.last().body));
    check(cl.queryItemValue(QStringLiteral("api")) == "app-key"
              && cl.queryItemValue(QStringLiteral("callsign")) == "N8EM"
              && web.requests.last().path == "/club/realtime.php",
          "POST to realtime.php with the api key");
    web.status = 500;
    results.clear();
    up.pushQso(second);
    check(waitResults(results, 1) && !results.last().at(1).toBool()
              && upState(db, second, "up_club") == "E",
          "an HTTP error is a failure, marked for retry");
    web.status = 200;
    setCfg(QStringLiteral("club/enabled"), false);

    std::printf("-- HRDLOG.net\n");
    setCfg(QStringLiteral("hrdlog/enabled"), true);
    setCfg(QStringLiteral("hrdlog/call"), QStringLiteral("N8EM"));
    secrets.setValue(QStringLiteral("hrdlog/code"), QStringLiteral("upload-code"));
    results.clear();
    up.pushQso(first);
    check(waitResults(results, 1) && results.last().at(1).toBool(), "accepted");
    const QUrlQuery hr(QString::fromLatin1(web.requests.last().body));
    check(hr.queryItemValue(QStringLiteral("Code")) == "upload-code"
              && hr.queryItemValue(QStringLiteral("App")) == "AetherSDR",
          "POST with the upload code, App=AetherSDR");
    setCfg(QStringLiteral("hrdlog/enabled"), false);

    std::printf("-- LoTW: tqsl batch and QSL download\n");
    const QString fakeTqsl = dir.filePath("tqsl");
    const QString argsFile = dir.filePath("tqsl-args");
    {
        QFile f(fakeTqsl);
        f.open(QIODevice::WriteOnly);
        f.write(QStringLiteral("#!/bin/sh\necho \"$@\" > '%1'\necho 'Final Status: Success(0)' 1>&2\nexit 0\n")
                    .arg(argsFile).toUtf8());
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    setCfg(QStringLiteral("lotw/enabled"), true);
    setCfg(QStringLiteral("lotw/station"), QStringLiteral("Home"));
    setCfg(QStringLiteral("lotw/tqslPath"), fakeTqsl);
    results.clear();
    up.pushQso(first);
    check(waitResults(results, 1, 5000) && results.last().at(1).toBool()
              && results.last().at(2).toString().contains("signed and uploaded"),
          "tqsl signs and uploads the pending QSOs");
    QFile af(argsFile);
    af.open(QIODevice::ReadOnly);
    const QString targs = QString::fromUtf8(af.readAll());
    check(targs.contains("-l Home") && targs.contains("-q") && targs.contains("-u")
              && !targs.contains("-p "),
          "tqsl -a all -l <station> -q -x -d -u <file>, no -p without a key password");
    check(upState(db, first, "up_lotw") == "Y" && upState(db, second, "up_lotw") == "Y",
          "every pending QSO marked delivered");
    secrets.setValue(QStringLiteral("lotw/password"), QStringLiteral("lotw-secret"));
    setCfg(QStringLiteral("lotw/login"), QStringLiteral("N8EM"));
    web.reply = "<eoh><CALL:4>W1AW <BAND:3>20M <MODE:2>CW <QSO_DATE:8>20261018 <QSL_RCVD:1>Y <eor>";
    results.clear();
    up.syncLotwQsls();
    check(waitResults(results, 1) && results.last().at(1).toBool()
              && db.prevQsos(QStringLiteral("W1AW")).first().lotwRcvd == "Y",
          "the LoTW QSL report confirms the matching QSO");
    setCfg(QStringLiteral("lotw/enabled"), false);

    std::printf("-- history is never uploaded\n");
    QBuffer in;
    in.setData("<CALL:5>JA1XY <QSO_DATE:8>20200101 <TIME_ON:4>1200 <BAND:3>20M <MODE:2>CW <EOR>");
    in.open(QIODevice::ReadOnly);
    db.importAdif(in, nullptr);
    const qint64 imported = db.prevQsos(QStringLiteral("JA1XY")).first().id;
    check(upState(db, imported, "up_eqsl") == "Y" && upState(db, imported, "up_lotw") == "Y",
          "imported QSOs count as already uploaded");

    std::printf("-- N1MM+ mirror\n");
    QUdpSocket n1mm;
    n1mm.bind(QHostAddress::LocalHost, 0);
    setCfg(QStringLiteral("n1mm/enabled"), true);
    setCfg(QStringLiteral("n1mm/ip"), QStringLiteral("127.0.0.1"));
    setCfg(QStringLiteral("n1mm/port"), n1mm.localPort());
    up.pushQso(first);
    pump(300);
    QByteArray dg;
    if (n1mm.hasPendingDatagrams()) {
        dg.resize(int(n1mm.pendingDatagramSize()));
        n1mm.readDatagram(dg.data(), dg.size());
    }
    check(dg.contains("<CALL:4>W1AW") && !dg.contains("LOTW_QSL_RCVD"),
          "one ADIF record per QSO, without my own QSL flags");

    std::printf("-- secrets stay out of the settings file\n");
    bool leaked = false;
    for (const QString& f : QDir(settingsDir.path()).entryList(QDir::Files)) {
        QFile sf(settingsDir.filePath(f));
        sf.open(QIODevice::ReadOnly);
        const QByteArray all = sf.readAll();
        for (const char* s : {"eqsl-secret", "ABCD-1234", "club-secret", "app-key", "upload-code", "lotw-secret"}) {
            if (all.contains(s)) leaked = true;
        }
    }
    AppSettings::instance().save();
    check(!leaked, "no password, key or code in AetherSDR's settings");

    if (g_failures == 0) {
        std::printf("orion_upload_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_upload_test: %d failure(s)\n", g_failures);
    return 1;
}
