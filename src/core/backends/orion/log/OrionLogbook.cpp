#include "core/backends/orion/log/OrionLogbook.h"

#include "core/AppSettings.h"
#include "core/CtyDatParser.h"
#include "core/backends/orion/log/CqrlogSync.h"
#include "core/backends/orion/log/LogbookIndex.h"
#include "core/backends/orion/log/WsjtxListener.h"

#include <QFile>
#include <QHostAddress>
#include <QUdpSocket>

namespace AetherSDR::orion::logbook {

OrionLogbook::OrionLogbook(QObject* parent)
    : QObject(parent)
    , m_cty(std::make_unique<CtyDatParser>())
{
    m_udp = new QUdpSocket(this);
}

OrionLogbook::~OrionLogbook() = default;

bool OrionLogbook::open(const QString& path, const QString& ctyPath, QString* error)
{
    const QString cty = ctyPath.isEmpty() ? QStringLiteral(":/cty.dat") : ctyPath;
    if (cty.startsWith(QLatin1Char(':'))) {
        m_cty->loadFromResource(cty);
    } else {
        m_cty->loadFromFile(cty);
    }
    if (!m_db.open(path)) {
        if (error) {
            *error = tr("cannot open the log at %1").arg(m_db.path());
        }
        return false;
    }
    m_sync = std::make_unique<CqrlogSync>(&m_db, m_cty.get());
    connect(m_sync.get(), &CqrlogSync::pulled, this, &OrionLogbook::pulledFromCqrlog);
    m_index = std::make_unique<LogbookIndex>();
    m_index->attachCty(m_cty.get());
    m_index->attachDb(&m_db);   // rebuilt whenever the log changes
    connect(m_index.get(), &LogbookIndex::updated, this, &OrionLogbook::workedIndexUpdated);
    return true;
}

void OrionLogbook::startWsjtx()
{
    if (!m_db.isOpen() || m_wsjtx) {
        return;
    }
    m_wsjtx = std::make_unique<WsjtxListener>(&m_db, m_cty.get());
    connect(m_wsjtx.get(), &WsjtxListener::qsoLogged, this,
            [this](qint64 id, const QString& call, bool) { emit wsjtxLogged(id, call); });
    connect(m_wsjtx.get(), &WsjtxListener::dxChanged, this, &OrionLogbook::wsjtxDx);
    m_wsjtx->start();
}

QChar OrionLogbook::workedStatus(const QString& call, qint64 hz) const
{
    if (!m_index) {
        return QChar('?');
    }
    return m_index->status(call, LogDb::bandForHz(hz));
}

bool OrionLogbook::isOpen() const
{
    return m_db.isOpen();
}

QString OrionLogbook::path() const
{
    return m_db.path();
}

void OrionLogbook::startCqrlogSync()
{
    if (m_sync) {
        m_sync->start();
    }
    // cqrlog's own worked/confirmed summary (cached between runs).
    if (m_index) {
        m_index->start();
    }
}

qint64 OrionLogbook::logQso(Qso q)
{
    if (q.band.isEmpty() && q.freqHz > 0) {
        q.band = LogDb::bandForHz(q.freqHz);
    }
    if (q.country.isEmpty()) {
        const Country c = country(q.call);
        if (c.known) {
            q.country = c.name;
            if (q.cqz == 0) q.cqz = c.cqz;
            if (q.ituz == 0) q.ituz = c.ituz;
        }
    }
    const qint64 id = m_db.addQso(q);
    if (id < 0) {
        return id;
    }
    mirror(m_db.qso(id));
    emit qsoLogged(id, q.call.trimmed().toUpper());
    return id;
}

OrionLogbook::Country OrionLogbook::country(const QString& call) const
{
    Country c;
    if (const DxccEntity* e = m_cty->entityForCallsign(call.trimmed().toUpper())) {
        c.name = e->name;
        c.cqz = e->cqZone;
        c.ituz = e->ituZone;
        c.known = true;
    }
    return c;
}

int OrionLogbook::importAdif(const QString& file, QString* error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("cannot read %1").arg(file);
        return -1;
    }
    return m_db.importAdif(f, m_cty.get(), error);
}

bool OrionLogbook::exportAdif(const QString& file, QString* error) const
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = tr("cannot write %1").arg(file);
        return false;
    }
    return m_db.exportAdif(f);
}

quint16 OrionLogbook::bridgePort()
{
    return quint16(AppSettings::instance().value(QStringLiteral("OrionLog/port"), 2334).toInt());
}

bool OrionLogbook::mirrorsToCqrlog()
{
    return AppSettings::instance().value(QStringLiteral("OrionLog/mirrorCqrlog"), true).toBool();
}

// One QSO -> cqrlog's bridge, as the console's mirrorToCqrlog does (the
// bridge drops anything that does not start with <CALL).
void OrionLogbook::mirror(const Qso& q)
{
    if (q.id < 0 || !mirrorsToCqrlog()) {
        return;
    }
    m_udp->writeDatagram(CqrlogSync::bridgeDatagram(q), QHostAddress::LocalHost, bridgePort());
}

}  // namespace AetherSDR::orion::logbook
