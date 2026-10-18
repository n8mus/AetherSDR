// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/log/CqrlogSync.h) for the AetherSDR Orion
// fork (logbook, step 9): namespace and include paths, AetherSDR settings in
// place of QSettings, and AetherSDR's cty.dat parser in place of the console's.
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class QProcess;
class QUdpSocket;

namespace AetherSDR {
class CtyDatParser;
}

namespace AetherSDR::orion::logbook {

class LogDb;
struct Qso;

// cqrlog -> console log mirror, so the two logs stay in sync for ALL
// modes (operator's requirement). The console->cqrlog direction already
// exists (the LOG window and contest pushes send ADIF to the bridge);
// this is the missing half: QSOs that land in cqrlog first — WSJT-X
// digi (cqrlog wins the unicast 2237), hand entries in cqrlog itself —
// are pulled into the console's own log, which feeds the spot table's
// country/band/mode dots.
//
// Same access pattern as LogbookIndex: the mysql CLI over cqrlog's
// embedded-server socket, only while cqrlog is running, one query at a
// time. Incremental on id_cqrlog_main (watermark in the AetherSDR
// setting OrionLog/cqrlogSyncId); rows convert to ADIF and go through
// LogDb::importAdif, whose near-duplicate check makes re-pulls and
// bridge-echoes harmless and whose upload stamps keep mirrored history
// out of every online-log push.
class CqrlogSync : public QObject {
    Q_OBJECT
public:
    CqrlogSync(LogDb* db, const CtyDatParser* cty, QObject* parent = nullptr);

    void start();                    // first pull soon + periodic retries
    void pullSoon(int delayMs = 3000);

    // mysql -N -B rows -> ADIF records (pure, for synctest). Column
    // order matches kColumns below; maxId receives the highest
    // id_cqrlog_main seen (untouched when there are no rows).
    static QByteArray rowsToAdif(const QString& tsv, qint64* maxId);
    static int columnCount();

    // One QSO as the bridge's headerless ADIF datagram (starts with
    // <CALL — the bridge drops anything else). Shared by every path
    // that mirrors toward cqrlog.
    static QByteArray bridgeDatagram(const Qso& q);

    // The other sync direction, on demand (the Logbook window's
    // "→ cqrlog" button): ask cqrlog what it has, compare against the
    // whole console log by call+band+time (±10 min), report the ids
    // cqrlog is missing. Then sendMissing() pushes them through the
    // bridge, paced. Nothing is sent without the caller's say-so —
    // cqrlog is the award log, a mass mis-push there is the nightmare.
    void checkMissing();
    void sendMissing(const QList<qint64>& ids);

signals:
    void pulled(int added);          // added > 0: new QSOs mirrored in
    // checkMissing result. error nonempty = couldn't check (cqrlog
    // closed, query failed); ids empty + no error = logs are in sync.
    void missingReady(const QList<qint64>& ids, const QString& error);
    void pushedToCqrlog(int sent);   // sendMissing finished scheduling

private:
    void pull();

    LogDb* db_;
    const CtyDatParser* cty_;
    QProcess* proc_ = nullptr;
    QProcess* checkProc_ = nullptr;  // the on-demand compare query
    QUdpSocket* udp_ = nullptr;
    QTimer timer_;
    qint64 lastId_ = 0;
};

} // namespace AetherSDR::orion::logbook
