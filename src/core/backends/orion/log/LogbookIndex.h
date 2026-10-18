// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/util/LogbookIndex.h) for the AetherSDR Orion
// fork (logbook, step 9): namespace and include paths, AetherSDR settings in
// place of QSettings, and AetherSDR's cty.dat parser in place of the console's.
#pragma once
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

class QProcess;
class QTimer;

namespace AetherSDR {
class CtyDatParser;
}

namespace AetherSDR::orion::logbook {

class LogDb;

// Worked-before view for spot coloring, merged from two sources:
//
//  - cqrlog (Linux): three summaries through the mysql CLI over cqrlog's
//    own embedded server socket (~/.config/cqrlog/database/sock) — no Qt
//    SQL plugin, no schema coupling beyond cqrlog_main's stable columns.
//    That server only runs while cqrlog is open, so every good load is
//    cached to disk and the cache restored at startup; a periodic retry
//    keeps the live view fresh whenever cqrlog is up. Confirmation = any
//    of card (qsl_r 'Q'), LoTW (lotw_qslr 'L') or eQSL (eqsl_qsl_rcvd 'E').
//
//  - the console's own LogDb (attachDb): rebuilt whenever the log changes.
//    This is the only source on Windows, and the only source for the
//    country/band/mode "needed" dimensions below (cqrlog's summary has no
//    mode or country column; the console's log stores both).
class LogbookIndex : public QObject {
    Q_OBJECT
public:
    explicit LogbookIndex(QObject* parent = nullptr);

    void start();                          // restore cache + begin refresh
    void refreshSoon(int delayMs = 2500);  // e.g. right after LOG sends a QSO
    void attachDb(LogDb* db);              // console log as a second source
    void attachCty(const CtyDatParser* cty);  // for the country dimension

    // Spot status: 'N' never worked, 'B' worked but not this band,
    // 'W' worked this band, 'C' confirmed this band, '?' no data yet.
    QChar status(const QString& call, const QString& band) const;
    bool  parkHunted(const QString& park) const;
    bool  ready() const { return haveData_; }

    // HRD-style needed-status, country-centric: for the spot's COUNTRY,
    // is the country / this band / this mode confirmed 'C', worked 'W',
    // or needed 'N'?  '?' = can't say (no console-log data, or unknown
    // country). Fed by the console's LogDb only.
    struct Need { QChar country{'?'}, band{'?'}, mode{'?'}; };
    Need need(const QString& call, const QString& band,
              const QString& mode) const;

    static QString bandForHz(qint64 hz);   // cqrlog band text ("20M")

signals:
    void updated();                        // sets rebuilt (live or cache)

private:
    void refresh();                        // spawn the mysql query
    void refreshDb();                      // rebuild the LogDb-side sets
    void rebuildMerged();
    void parseRows(const QString& tsv);    // "T<TAB>call<TAB>band" lines
    QString cachePath() const;
    QString countryOf(const QString& call) const;   // memoized cty lookup

    bool haveData_ = false;
    // Merged view (what status()/parkHunted() read).
    QSet<QString> workedCall_;             // "N8EM"
    QSet<QString> workedCallBand_;         // "N8EM|20M"
    QSet<QString> confCallBand_;
    QSet<QString> parks_;                  // "US-1234"
    // cqrlog-side staging (parseRows).
    QSet<QString> cqrWorkedCall_, cqrWorkedCallBand_, cqrConfCallBand_,
                  cqrParks_;
    // LogDb-side staging (refreshDb), plus the needed-status dimensions.
    QSet<QString> dbWorkedCall_, dbWorkedCallBand_, dbConfCallBand_,
                  dbParks_;
    bool haveDbData_ = false;
    QSet<QString> workedCountry_, confCountry_;          // "Sweden"
    QSet<QString> workedCountryBand_, confCountryBand_;  // "Sweden|20M"
    QSet<QString> workedCountryMode_, confCountryMode_;  // "Sweden|CW"

    LogDb* db_ = nullptr;
    const CtyDatParser* cty_ = nullptr;
    QHash<QString, QString> dbCallCountry_;              // logged call -> country
    mutable QHash<QString, QString> ctyMemo_;            // call -> country
    QProcess* proc_ = nullptr;
    QTimer*   timer_ = nullptr;
};

} // namespace AetherSDR::orion::logbook
