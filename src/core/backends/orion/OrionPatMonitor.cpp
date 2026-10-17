#include "core/backends/orion/OrionPatMonitor.h"

#include <QJsonDocument>
#include <QJsonObject>

#ifdef HAVE_WEBSOCKETS
#include <QWebSocket>
#endif

namespace AetherSDR::orion {

OrionPatMonitor::OrionPatMonitor(QObject* parent)
    : QObject(parent)
{
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &OrionPatMonitor::connectNow);
}

OrionPatMonitor::~OrionPatMonitor()
{
    stop();
}

bool OrionPatMonitor::start(const QUrl& url)
{
#ifdef HAVE_WEBSOCKETS
    stop();
    m_url = url;
    m_enabled = true;
    connectNow();
    return true;
#else
    Q_UNUSED(url);
    return false;
#endif
}

void OrionPatMonitor::stop()
{
    m_enabled = false;
    m_retry.stop();
#ifdef HAVE_WEBSOCKETS
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket.reset();
    }
#endif
    setPatUp(false);
}

void OrionPatMonitor::connectNow()
{
#ifdef HAVE_WEBSOCKETS
    if (!m_enabled) {
        return;
    }
    if (!m_socket) {
        m_socket = std::make_unique<QWebSocket>(QString(), QWebSocketProtocol::VersionLatest);
        connect(m_socket.get(), &QWebSocket::textMessageReceived, this,
                &OrionPatMonitor::handleMessage);
        connect(m_socket.get(), &QWebSocket::connected, this, [this] { setPatUp(true); });
        connect(m_socket.get(), &QWebSocket::disconnected, this, [this] {
            setPatUp(false);
            if (m_enabled) {
                m_retry.start();
            }
        });
    }
    m_socket->open(m_url);
#endif
}

void OrionPatMonitor::setPatUp(bool up)
{
    if (!up && sessionActive()) {
        // Pat went away mid-session: the session is over as far as we know.
        m_dialing = m_connected = false;
        emit sessionChanged(false, m_remote);
    }
    if (up == m_patUp) {
        return;
    }
    m_patUp = up;
    emit patUpChanged(up);
}

void OrionPatMonitor::handleMessage(const QString& json)
{
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    if (!o.contains(QLatin1String("Status"))) {
        return;
    }
    const QJsonObject s = o.value(QLatin1String("Status")).toObject();
    const bool wasActive = sessionActive();
    m_dialing = s.value(QLatin1String("dialing")).toBool();
    m_connected = s.value(QLatin1String("connected")).toBool();
    const QString remote = s.value(QLatin1String("remote_addr")).toString();
    if (!remote.isEmpty()) {
        m_remote = remote;   // a new session's peer, or the one that is ending
    }
    if (sessionActive() != wasActive) {
        emit sessionChanged(sessionActive(), m_remote);
    }
}

}  // namespace AetherSDR::orion
