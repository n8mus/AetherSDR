// SPDX-License-Identifier: GPL-2.0-or-later
// Station logbook (step 9, stage 1), no cqrlog and no mysql:
//  1. The cqrlog -> log mirror: the TSV-to-ADIF converter and the round
//     trip into LogDb (ported from the Orion console's tests/synctest.cpp).
//  2. LogDb + ADIF: log, previous QSOs, near-duplicate, export -> import
//     round trip, band names, country stamping from AetherSDR's cty.dat.
//  3. OrionLogbook: logging fills band and country, and mirrors the QSO to
//     cqrlog's bridge as one headerless ADIF datagram (a stand-in bridge on
//     an ephemeral UDP port).
//
//   cmake --build build --target orion_logbook_test && ./build/orion_logbook_test

#include "core/AppSettings.h"
#include "core/CtyDatParser.h"
#include "core/backends/orion/log/Adif.h"
#include "core/backends/orion/log/CqrlogSync.h"
#include "core/backends/orion/log/LogDb.h"
#include "core/backends/orion/log/LogbookIndex.h"
#include "core/backends/orion/log/OrionLogbook.h"

#include <QBuffer>
#include <QDataStream>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUdpSocket>

#include <cstdio>
#include <functional>

using namespace AetherSDR;
using namespace AetherSDR::orion::logbook;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

// One cqrlog row in mysql -N -B form, CqrlogSync's column order.
const char* kRow1 =
    "4711\t2026-09-07\t14:32\tOA4ENG\t7.074\tFT8\t40M\t-11\t-06\t"
    "Luis\tLima\tFH17MW\t10\t12\tQ\tL\tNULL\n";
const char* kRow2 =
    "4712\t2026-09-07\t14:40\tZL1ABC\t7.076\tFT8\t40M\tNULL\tNULL\t"
    "NULL\tNULL\tNULL\t0\t0\tNULL\tNULL\tE\n";

void testCqrlogMirror(const QString& dir)
{
    std::printf("-- cqrlog -> log mirror (the console's synctest)\n");
    qint64 maxId = 0;
    const QByteArray adif = CqrlogSync::rowsToAdif(QString(kRow1) + kRow2, &maxId);
    check(maxId == 4712, "watermark is the highest id");
    const QList<AdifRecord> recs = Adif::parseBytes(adif);
    check(recs.size() == 2, "two rows -> two ADIF records");
    if (recs.size() == 2) {
        const AdifRecord& r = recs[0];
        check(r.value("CALL") == "OA4ENG" && r.value("QSO_DATE") == "20260907"
                  && r.value("TIME_ON") == "1432", "call, date and time map");
        check(r.value("BAND") == "40M" && r.value("MODE") == "FT8" && r.value("FREQ") == "7.074",
              "band, mode and freq (MHz) map");
        check(r.value("GRIDSQUARE") == "FH17MW" && r.value("CQZ") == "10" && r.value("ITUZ") == "12",
              "grid and zones map");
        check(r.value("QSL_RCVD") == "Y" && r.value("LOTW_QSL_RCVD") == "Y"
                  && !r.contains("EQSL_QSL_RCVD"),
              "Q/L confirmation letters translate, absent E stays absent");
        check(!recs[1].contains("NAME") && !recs[1].contains("CQZ")
                  && recs[1].value("EQSL_QSL_RCVD") == "Y",
              "NULLs and zero zones drop, E translates");
    }
    qint64 junkId = 0;
    check(CqrlogSync::rowsToAdif("mysql: connect error\n", &junkId).isEmpty() && junkId == 0,
          "junk output yields nothing");
    LogDb db;
    check(db.open(dir + "/mirror.sqlite"), "a log opens");
    QBuffer b1;
    b1.setData(adif);
    b1.open(QIODevice::ReadOnly);
    check(db.importAdif(b1, nullptr) == 2, "first pull adds both");
    QBuffer b2;
    b2.setData(adif);
    b2.open(QIODevice::ReadOnly);
    check(db.importAdif(b2, nullptr) == 0, "a re-pull adds nothing (near-duplicates)");
}

void testLogDb(const QString& dir, const CtyDatParser& cty)
{
    std::printf("-- the log and ADIF\n");
    LogDb db;
    check(db.open(dir + "/log.sqlite"), "log opens");
    Qso q;
    q.call = QStringLiteral("w1aw");
    q.tsUtc = QDateTime(QDate(2026, 10, 18), QTime(14, 5, 0), QTimeZone::utc());
    q.freqHz = 14025300;
    q.band = LogDb::bandForHz(q.freqHz);
    q.mode = QStringLiteral("CW");
    q.rstS = QStringLiteral("599");
    q.rstR = QStringLiteral("579");
    q.name = QStringLiteral("Hiram");
    // The ON-AIR name, kept apart from the callbook's: the CW macros send
    // what he SAID, not what QRZ says.
    q.handle = QStringLiteral("HI");
    const qint64 id = db.addQso(q);
    check(id > 0 && db.count() == 1, "a QSO is logged");
    check(db.qso(id).call == "W1AW" && db.qso(id).band == "20M", "call uppercased, band 20M");
    Qso again = q;
    again.tsUtc = q.tsUtc.addSecs(120);
    check(db.hasNearDuplicate(again), "same call/band/mode 2 minutes later is a near-duplicate");
    again.tsUtc = q.tsUtc.addSecs(600);
    check(!db.hasNearDuplicate(again), "10 minutes later is not");
    check(db.qso(id).handle == "HI", "the on-air handle is stored and read back");
    check(db.prevQsos(QStringLiteral("W1AW")).size() == 1
              && db.prevQsos(QStringLiteral("W1AW")).first().name == "Hiram"
              && db.prevQsos(QStringLiteral("W1AW")).first().handle == "HI",
          "previous QSOs with the station, handle included");
    {
        // Standard ADIF by default: nothing we invented goes to an online log.
        const AdifRecord plain = LogDb::toAdif(db.qso(id));
        check(!plain.contains(QStringLiteral("APP_TTC_HANDLE")),
              "an upload record carries no application field");
        // Our own file export keeps it, so a file round-trips either way
        // between this log and the station's Orion console.
        const AdifRecord local = LogDb::toAdif(db.qso(id), /*withLocalFields=*/true);
        check(local.value(QStringLiteral("APP_TTC_HANDLE")) == "HI",
              "our own export carries the handle");
        check(LogDb::fromAdif(local).handle == "HI", "and reads it back");
    }
    check(LogDb::bandForHz(7074000) == "40M" && LogDb::bandForHz(50313000) == "6M"
              && LogDb::bandForHz(9000000).isEmpty(),
          "band names, empty off-band");

    QBuffer out;
    out.open(QIODevice::WriteOnly);
    check(db.exportAdif(out), "ADIF export");
    const QByteArray adif = out.data();
    check(adif.contains("<PROGRAMID:9>AetherSDR") && adif.contains("<CALL:4>W1AW")
              && adif.contains("<RST_RCVD:3>579"),
          "the export carries AetherSDR's program id and the QSO's fields");
    check(adif.contains("<APP_TTC_HANDLE:2>HI"), "and the handle, in the file export");

    LogDb other;
    check(other.open(dir + "/other.sqlite"), "a second log opens");
    QBuffer in;
    in.setData(adif + "<CALL:5>DL1AB <QSO_DATE:8>20261018 <TIME_ON:4>1500 <FREQ:5>7.025 "
                      "<MODE:2>CW <EOR>\n");
    in.open(QIODevice::ReadOnly);
    QString err;
    check(other.importAdif(in, &cty, &err) == 2, "import adds both records");
    check(!other.prevQsos(QStringLiteral("W1AW")).isEmpty()
              && other.prevQsos(QStringLiteral("W1AW")).first().handle == "HI",
          "the handle survives an export/import round trip");
    const QList<Qso> dl = other.prevQsos(QStringLiteral("DL1AB"));
    check(!dl.isEmpty() && dl.first().band == "40M", "a missing BAND comes from the frequency");
    check(!dl.isEmpty() && dl.first().country == "Fed. Rep. of Germany" && dl.first().cqz == 14,
          "a missing country and zone come from AetherSDR's cty.dat");
}

void testOrionLogbook(const QString& dir, const QString& ctyPath)
{
    std::printf("-- OrionLogbook: logging and the mirror to cqrlog's bridge\n");
    QUdpSocket bridge;
    check(bridge.bind(QHostAddress::LocalHost, 0), "a stand-in cqrlog bridge listens");
    AppSettings::instance().setValue(QStringLiteral("OrionLog/port"), bridge.localPort());
    AppSettings::instance().setValue(QStringLiteral("OrionLog/mirrorCqrlog"), true);

    OrionLogbook book(nullptr, /*memoryOnlySecrets=*/true);
    QString err;
    check(book.open(dir + "/station.sqlite", ctyPath, &err), "the logbook opens");
    check(book.country(QStringLiteral("JA1XYZ")).known
              && book.country(QStringLiteral("JA1XYZ")).name == "Japan",
          "country lookup from cty.dat");
    Qso q;
    q.call = QStringLiteral("JA1XYZ");
    q.tsUtc = QDateTime::currentDateTimeUtc();
    q.freqHz = 21025000;
    q.mode = QStringLiteral("CW");
    q.rstS = q.rstR = QStringLiteral("599");
    const qint64 id = book.logQso(q);
    check(id > 0, "logged");
    const Qso stored = book.db()->qso(id);
    check(stored.band == "15M" && stored.country == "Japan" && stored.cqz == 25,
          "band and country filled in when logging");
    QByteArray datagram;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 2000 && datagram.isEmpty()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (bridge.hasPendingDatagrams()) {
            datagram.resize(int(bridge.pendingDatagramSize()));
            bridge.readDatagram(datagram.data(), datagram.size());
        }
    }
    check(datagram.startsWith("<CALL:6>JA1XYZ") && datagram.contains("<BAND:3>15M")
              && datagram.endsWith("<EOR>"),
          "the QSO reaches cqrlog's bridge as one headerless ADIF record");

    AppSettings::instance().setValue(QStringLiteral("OrionLog/mirrorCqrlog"), false);
    q.call = QStringLiteral("VK2ABC");
    book.logQso(q);
    QElapsedTimer t2;
    t2.start();
    while (t2.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    check(!bridge.hasPendingDatagrams(), "mirroring off: nothing is sent");
}

void testWorkedIndex(const QString& dir, const CtyDatParser& cty)
{
    std::printf("-- worked-before index (stage 2)\n");
    LogDb db;
    check(db.open(dir + "/worked.sqlite"), "log opens");
    const auto add = [&](const char* call, qint64 hz, bool lotw) {
        Qso q;
        q.call = QString::fromLatin1(call);
        q.tsUtc = QDateTime::currentDateTimeUtc().addDays(-3);
        q.freqHz = hz;
        q.band = LogDb::bandForHz(hz);
        q.mode = QStringLiteral("CW");
        if (lotw) q.lotwRcvd = QStringLiteral("Y");
        db.addQso(q);
    };
    LogbookIndex idx;
    check(idx.status(QStringLiteral("W1AW"), QStringLiteral("20M")) == QChar('?'),
          "no data yet: '?' (spots keep their own colours)");
    idx.attachCty(&cty);
    idx.attachDb(&db);
    add("W1AW", 14025000, false);
    add("DL1ABC", 7025000, true);
    check(idx.status(QStringLiteral("JA1XYZ"), QStringLiteral("20M")) == QChar('N'),
          "never worked: N");
    check(idx.status(QStringLiteral("W1AW"), QStringLiteral("40M")) == QChar('B'),
          "worked on another band: B");
    check(idx.status(QStringLiteral("W1AW"), QStringLiteral("20M")) == QChar('W'),
          "worked on this band: W");
    check(idx.status(QStringLiteral("DL1ABC"), QStringLiteral("40M")) == QChar('C'),
          "confirmed on this band: C");
    const LogbookIndex::Need nd = idx.need(QStringLiteral("DL9ZZ"), QStringLiteral("20M"),
                                           QStringLiteral("CW"));
    check(nd.country == QChar('C') && nd.band == QChar('N') && nd.mode == QChar('C'),
          "country needs: Germany confirmed, 20 m needed, CW confirmed");
}

QByteArray wsjtxDatagram(quint32 type, const std::function<void(QDataStream&)>& body)
{
    QByteArray d;
    QDataStream out(&d, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    out << quint32(0xadbccbda) << quint32(2) << type << QByteArray("WSJT-X");
    body(out);
    return d;
}

void testWsjtx(const QString& dir, const QString& ctyPath)
{
    std::printf("-- WSJT-X auto-logging (stage 2)\n");
    // A free port for the listener; unicast to it stands in for the group.
    quint16 port = 0;
    {
        QUdpSocket probe;
        probe.bind(QHostAddress::LocalHost, 0);
        port = probe.localPort();
    }
    AppSettings::instance().setValue(QStringLiteral("OrionLog/wsjtxAddr"), QStringLiteral("127.0.0.1"));
    AppSettings::instance().setValue(QStringLiteral("OrionLog/wsjtxPort"), port);
    AppSettings::instance().setValue(QStringLiteral("OrionLog/mirrorCqrlog"), false);

    OrionLogbook book(nullptr, /*memoryOnlySecrets=*/true);
    check(book.open(dir + "/wsjtx.sqlite", ctyPath), "the logbook opens");
    QSignalSpy logged(&book, &OrionLogbook::wsjtxLogged);
    QSignalSpy dx(&book, &OrionLogbook::wsjtxDx);
    book.startWsjtx();

    const QByteArray adif =
        "<call:6>OA4ENG <gridsquare:4>FH17 <mode:3>FT8 <rst_sent:3>-11 <rst_rcvd:3>-06 "
        "<qso_date:8>20261018 <time_on:6>143200 <band:3>40m <freq:8>7.075123 <eor>";
    QUdpSocket tx;
    const auto send = [&](const QByteArray& d) {
        tx.writeDatagram(d, QHostAddress::LocalHost, port);
    };
    send(wsjtxDatagram(12, [&](QDataStream& o) { o << adif; }));
    check(logged.wait(2000) && logged.first().at(1).toString() == "OA4ENG",
          "a Logged ADIF message lands in the log");
    const QList<Qso> prev = book.db()->prevQsos(QStringLiteral("OA4ENG"));
    check(prev.size() == 1 && prev.first().mode == "FT8" && prev.first().band == "40M"
              && prev.first().country == "Peru",
          "with its mode, band and (from cty.dat) country");
    send(wsjtxDatagram(12, [&](QDataStream& o) { o << adif; }));
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 400) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(book.db()->prevQsos(QStringLiteral("OA4ENG")).size() == 1, "a resend is not logged twice");

    send(wsjtxDatagram(1, [&](QDataStream& o) {
        o << quint64(7074000) << QByteArray("FT8") << QByteArray("ZL1ABC") << QByteArray("-10")
          << QByteArray("FT8") << true << false << true << quint32(1500) << quint32(1500)
          << QByteArray("N8EM") << QByteArray("EN81") << QByteArray("RF72");
    }));
    check(dx.wait(2000) && dx.first().at(0).toString() == "ZL1ABC"
              && dx.first().at(1).toString() == "RF72" && dx.first().at(2).toString() == "FT8",
          "the DX WSJT-X is working (call, grid, mode) is passed on");
    check(book.workedStatus(QStringLiteral("OA4ENG"), 7075123) == QChar('W'),
          "a WSJT-X QSO counts as worked at once");
}

}  // namespace

int main(int argc, char** argv)
{
    // Never the operator's settings or log.
    QTemporaryDir settingsDir;
    qputenv("AETHER_SETTINGS_DIR", settingsDir.path().toUtf8());
    // Nor the operator's cqrlog: its home points at an empty folder here.
    qputenv("TTC_CQRLOG_HOME", settingsDir.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    CtyDatParser cty;
    if (!cty.loadFromFile(QStringLiteral(ORION_TEST_CTY_DAT))) {
        std::printf("orion_logbook_test: cannot load %s\n", ORION_TEST_CTY_DAT);
        return 1;
    }
    testCqrlogMirror(dir.path());
    testLogDb(dir.path(), cty);
    testOrionLogbook(dir.path(), QStringLiteral(ORION_TEST_CTY_DAT));
    testWorkedIndex(dir.path(), cty);
    testWsjtx(dir.path(), QStringLiteral(ORION_TEST_CTY_DAT));
    if (g_failures == 0) {
        std::printf("orion_logbook_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_logbook_test: %d failure(s)\n", g_failures);
    return 1;
}
