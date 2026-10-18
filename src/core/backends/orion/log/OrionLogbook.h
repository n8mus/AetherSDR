#pragma once

// The station logbook (step 9, stage 1), as the Orion console keeps it: its
// own SQLite log (the console's LogDb, ported), ADIF import/export, and the
// cqrlog bridge both ways: each logged QSO is mirrored to cqrlog's bridge
// (UDP, default localhost:2334) and QSOs that land in cqrlog first are
// pulled in while cqrlog runs (CqrlogSync). cqrlog stays the award log.
//
// AetherSDR keeps its own log file (OrionLog/dbPath, default
// ~/.local/share/AetherSDR/logbook.sqlite), separate from the console's.
// Owned by the GUI (the logbook has nothing to do with which radio is
// connected); everything runs on the GUI thread.

#include "core/backends/orion/log/LogDb.h"

#include <QObject>
#include <QString>

#include <memory>

class QUdpSocket;

namespace AetherSDR {
class CtyDatParser;
}

namespace AetherSDR::orion::logbook {

class CqrlogSync;

class OrionLogbook : public QObject {
    Q_OBJECT

public:
    struct Country {
        QString name;
        int cqz = 0;
        int ituz = 0;
        bool known = false;
    };

    explicit OrionLogbook(QObject* parent = nullptr);
    ~OrionLogbook() override;

    // Opens the log (empty path: the setting, else the default) and loads
    // cty.dat (empty: AetherSDR's built-in copy). False with a reason.
    bool open(const QString& path = QString(), const QString& ctyPath = QString(),
              QString* error = nullptr);
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString path() const;
    LogDb* db() { return &m_db; }

    // Starts the cqrlog pull (only does anything while cqrlog runs).
    void startCqrlogSync();

    // Logs one QSO and mirrors it to cqrlog. Returns the new id, -1 on error.
    qint64 logQso(Qso q);

    [[nodiscard]] Country country(const QString& call) const;

    int importAdif(const QString& file, QString* error = nullptr);
    bool exportAdif(const QString& file, QString* error = nullptr) const;

    // The UDP port of cqrlog's bridge, and whether QSOs are mirrored there.
    [[nodiscard]] static quint16 bridgePort();
    [[nodiscard]] static bool mirrorsToCqrlog();

signals:
    void qsoLogged(qint64 id, const QString& call);
    void pulledFromCqrlog(int added);

private:
    void mirror(const Qso& q);

    LogDb m_db;
    std::unique_ptr<CtyDatParser> m_cty;
    std::unique_ptr<CqrlogSync> m_sync;
    QUdpSocket* m_udp = nullptr;
};

}  // namespace AetherSDR::orion::logbook
