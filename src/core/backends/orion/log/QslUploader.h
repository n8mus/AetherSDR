// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/log/QslUploader.h) for the AetherSDR Orion
// fork (logbook uploads): namespace, AetherSDR settings (OrionUp/...) in place
// of QSettings, secrets from the system keychain (OrionUploadSecrets), each
// service's address a setting (tests point it at a local stand-in), and
// AetherSDR as the program id.
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace AetherSDR::orion::logbook {

class LogDb;
class OrionUploadSecrets;
struct Qso;

// GridTracker-style instant uploads: the moment a QSO lands in the log it
// is pushed to every enabled online service, and every outcome is
// ANNOUNCED — accepted, duplicate, or failed (operator's call 2026-09-01:
// confirmations must be visible, like GridTracker's traffic lines).
// Per-QSO state rides in LogDb's up_* columns ('' pending, 'Y' delivered
// — a duplicate counts as delivered and is never re-sent, 'E' failed).
// There is NO periodic retry loop (operator's explicit dislike): one
// catch-up sweep shortly after startup collects QSOs logged while a
// service was down, and retryFailedNow() is wired to a button for the
// rest. Corrections re-send and the services dedupe; deletes stay local
// by design (operator's accepted trade).
//
// Services (settings under OrionUp/<svc>/..., secrets in the keychain):
//   lotw    tqsl CLI:  -a all -l <station> [-p pw] -q -x -d -u <file>
//           (batched: one temp ADIF per sweep; "Final Status: Success"
//            or an all-duplicates report both count as delivered)
//   eqsl    GET  eQSL.cc/qslcard/importADIF.cfm (creds as URL params)
//   qrz     POST logbook.qrz.com/api  KEY/ACTION=INSERT/ADIF
//           (POTA/SIG fields stripped — the API rejects them)
//   club    POST clublog.org/realtime.php (needs an APPLICATION api key —
//           ClubLog issues these per program on request; blank = disabled)
//   hrdlog  POST hrdlog.net/NewEntry.aspx  Callsign/Code/App/ADIFData
// Logger mirrors (no bookkeeping, fire-and-forget like GridTracker's):
//   hrd     TCP "ver\rdb add {F=\"v\" ...}\rexit\r"  (HRD Logbook :7826)
//   n1mm    UDP raw ADIF record                       (N1MM+ :2333)
class QslUploader : public QObject {
    Q_OBJECT
public:
    // secrets: where passwords/keys come from (the system keychain).
    QslUploader(LogDb* db, OrionUploadSecrets* secrets, QObject* parent = nullptr);

    void pushQso(qint64 id);           // called right after a QSO is logged
    void sweepSoon(int delayMs = 3000);
    void retryFailedNow() { sweep(); } // the Online Logs window's button

    // cqrlog's "auto down": pull LoTW's QSL report (since the stored
    // watermark) and mark matching QSOs confirmed. Runs by itself a
    // little after startup when LoTW is enabled; the Online Logs window
    // has a Sync QSLs button for on-demand runs.
    void syncLotwQsls();

    // Setup-window Test buttons. Each answers via serviceResult(svc,...).
    void testLotwDownload();
    void testTqsl();
    void testEqsl();
    void testQrz();
    void testHrdlogNet();
    void testLoggerPush(const QString& svc);   // "hrd" | "n1mm"

    // The scrolling upload-traffic print (GridTracker's model, operator's
    // spec): every outcome as a timestamped line, newest last, last 100.
    QStringList recentTraffic() const { return traffic_; }

signals:
    // ok=false always carries a human-readable reason; shown by the Setup
    // window's Result column and the status bar.
    void serviceResult(const QString& svc, bool ok, const QString& detail);
    void trafficLine(const QString& line);   // preformatted, for the print

private:
    void sweep();                      // retry everything pending
    void pushHttp(const Qso& q, const QString& svc);
    void runTqslBatch();
    void mirrorToLoggers(const Qso& q);
    QString adifFor(const Qso& q, const QString& svc) const;
    QString sec(const QString& key) const;   // a secret, from the keychain store

    LogDb* db_;
    OrionUploadSecrets* secrets_;
    QNetworkAccessManager* net_;
    QStringList traffic_;
    bool tqslRunning_ = false;
};

} // namespace AetherSDR::orion::logbook
