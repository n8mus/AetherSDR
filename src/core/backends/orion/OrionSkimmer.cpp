#include "core/backends/orion/OrionSkimmer.h"

#include "core/CtyDatParser.h"
#include "core/backends/orion/OrionSkimmerFeed.h"
#include "core/backends/orion/skimmer/SkimServer.h"
#include "core/backends/orion/skimmer/SkimmerEngine.h"
#ifdef AETHER_ORION_SDRPLAY
#include "core/backends/orion/skimmer/SkimStft.h"
#endif

#include <QDir>
#include <QFile>
#include <QLoggingCategory>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(lcOrion)

namespace AetherSDR::orion {

namespace {

// MASTER.SCP, as the console loads it: cqrlog's copy, then the system one.
QSet<QString> loadMasterScp(const QString& preferred)
{
    QSet<QString> out;
    QStringList paths;
    if (!preferred.isEmpty()) {
        paths << preferred;
    } else {
        paths << QDir::homePath() + QStringLiteral("/.config/cqrlog/MASTER.SCP")
              << QStringLiteral("/usr/share/cqrlog/ctyfiles/MASTER.SCP");
    }
    for (const QString& p : paths) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) {
                continue;
            }
            out.insert(QString::fromLatin1(line).toUpper());
        }
        break;
    }
    return out;
}

}  // namespace

OrionSkimmer::OrionSkimmer(double inputRate, const Settings& settings, QObject* parent)
    : QObject(parent)
    , m_engine(std::make_unique<skim::SkimmerEngine>(inputRate,
                                                     std::clamp(settings.channels, 4, 64)))
    , m_cty(std::make_unique<CtyDatParser>())
    , m_settings(settings)
{
    m_feed = std::make_unique<OrionSkimmerFeed>(m_engine.get());

    // Calls whose prefix maps to no country are decode artifacts (the
    // console's rule, with AetherSDR's own cty.dat).
    m_ctyLoaded = settings.ctyPath.startsWith(QLatin1Char(':'))
        ? m_cty->loadFromResource(settings.ctyPath)
        : m_cty->loadFromFile(settings.ctyPath);
    if (m_ctyLoaded) {
        CtyDatParser* cty = m_cty.get();
        m_engine->setCallValidator([cty](const QString& call) {
            return cty->entityForCallsign(call) != nullptr;
        });
    } else {
        qCWarning(lcOrion) << "Skimmer: no cty.dat at" << settings.ctyPath
                           << "- calls are checked by shape and repetition only";
    }
    const QSet<QString> known = loadMasterScp(settings.masterScpPath);
    m_knownCalls = known.size();
    m_engine->setKnownCalls(known);
    m_engine->setBayes(settings.bayes);

    connect(m_engine.get(), &skim::SkimmerEngine::spotFound, this, &OrionSkimmer::spotFound);
    m_feed->start();
#ifdef AETHER_ORION_SDRPLAY
    // The SKIM waterfall's STFT, on its own feed thread (stage 2).
    m_stft = new skim::SkimStft(inputRate, this);
    skim::SkimStft* stft = m_stft;
    m_viewFeed = std::make_unique<OrionSkimmerFeed>(
        [stft](const std::complex<float>* d, std::size_t n) { stft->processIq(d, n); },
        /*gateOnLayoutLo=*/false);
    m_viewFeed->setPaused(true);
    m_viewFeed->start();
#endif
}

void OrionSkimmer::setViewActive(bool active)
{
    if (!m_stft || !m_viewFeed) {
        return;
    }
    m_stft->setEnabled(active);
    m_viewFeed->setPaused(!active);
}

OrionSkimmer::~OrionSkimmer()
{
    // The feeds' workers call into the engine and the STFT: they stop first.
    m_feed->stop();
    if (m_viewFeed) {
        m_viewFeed->stop();
    }
    if (m_server) {
        m_server->stop();
    }
}

bool OrionSkimmer::startServer(QString* error)
{
    if (m_settings.telnetPort <= 0) {
        return true;
    }
    m_server = std::make_unique<skim::SkimServer>();
    if (!m_settings.spotterCall.isEmpty()) {
        m_server->setSpotterCall(m_settings.spotterCall);
    }
    if (!m_server->start(static_cast<quint16>(m_settings.telnetPort))) {
        if (error) {
            *error = tr("cannot listen on localhost:%1").arg(m_settings.telnetPort);
        }
        m_server.reset();
        return false;
    }
    connect(m_engine.get(), &skim::SkimmerEngine::spotFound, m_server.get(),
            &skim::SkimServer::announce);
    return true;
}

quint16 OrionSkimmer::serverPort() const
{
    return m_server && m_server->active() ? m_server->port() : 0;
}

int OrionSkimmer::serverClients() const
{
    return m_server ? m_server->clientCount() : 0;
}

void OrionSkimmer::setActive(bool active)
{
    if (active == m_engine->enabled()) {
        return;
    }
    m_engine->setEnabled(active);   // off frees every channel
    m_feed->setPaused(!active);
    if (!active) {
        m_feed->setLayoutLo(0);     // nothing passes until the next layout
    }
}

bool OrionSkimmer::isActive() const
{
    return m_engine->enabled();
}

void OrionSkimmer::onFullSpectrum(const std::vector<float>& db, int spanHz, qint64 loHz,
                                  qint64 dialHz)
{
    if (!m_engine->enabled()) {
        return;
    }
    m_engine->updateFromSpectrum(db, spanHz, dialHz, static_cast<int>(loHz - dialHz));
    m_feed->setLayoutLo(loHz);
}

int OrionSkimmer::activeChannels() const
{
    int n = 0;
    for (const auto& c : m_engine->channelInfo()) {
        n += c.active ? 1 : 0;
    }
    return n;
}

int OrionSkimmer::channelCount() const
{
    return m_engine->channelCount();
}

quint64 OrionSkimmer::droppedSamples() const
{
    return m_feed->droppedSamples();
}

}  // namespace AetherSDR::orion
