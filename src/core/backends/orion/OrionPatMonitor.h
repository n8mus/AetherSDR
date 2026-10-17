#pragma once

// Pat (Winlink) session monitor (step 7). Watches Pat's own websocket,
// read-only, for its Status messages (dialing / connected / remote_addr).
//
// Operator decision: WARN ONLY. During a Winlink session the backend shows
// a notice and logs it; it never blocks keying. (Pat keys through
// AetherSDR's rigctld, which reaches the same keying verb as MOX, so a block
// at the backend would stop Pat itself.)

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <memory>

class QWebSocket;

namespace AetherSDR::orion {

class OrionPatMonitor : public QObject {
    Q_OBJECT

public:
    static constexpr const char* kDefaultUrl = "ws://127.0.0.1:8080/ws";
    static constexpr int kRetryMs = 10'000;

    explicit OrionPatMonitor(QObject* parent = nullptr);
    ~OrionPatMonitor() override;

    // False when this build has no websocket support.
    bool start(const QUrl& url);
    void stop();
    [[nodiscard]] bool sessionActive() const { return m_dialing || m_connected; }
    [[nodiscard]] QString remote() const { return m_remote; }
    [[nodiscard]] bool patUp() const { return m_patUp; }

    // One websocket text message; public so the parsing is testable.
    void handleMessage(const QString& json);

signals:
    void sessionChanged(bool active, const QString& remote);
    void patUpChanged(bool up);

private:
    void connectNow();
    void setPatUp(bool up);

    QUrl m_url;
    bool m_enabled = false;
    bool m_patUp = false;
    bool m_dialing = false;
    bool m_connected = false;
    QString m_remote;
    QTimer m_retry;
#ifdef HAVE_WEBSOCKETS
    std::unique_ptr<QWebSocket> m_socket;
#endif
};

}  // namespace AetherSDR::orion
