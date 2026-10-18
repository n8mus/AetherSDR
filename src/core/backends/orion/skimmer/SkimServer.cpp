// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/SkimServer.cpp), as is apart from the
// namespace and include paths, for the AetherSDR Orion fork (skimmer, step 8).
#include "core/backends/orion/skimmer/SkimServer.h"

#include <QDateTime>
#include <QHostAddress>
#include <QTcpSocket>

namespace AetherSDR::orion::skim {

SkimServer::SkimServer(QObject* parent) : QObject(parent) {
    connect(&srv_, &QTcpServer::newConnection, this,
            &SkimServer::onNewConnection);
}

bool SkimServer::start(quint16 port) {
    if (srv_.isListening()) return true;
    return srv_.listen(QHostAddress::LocalHost, port);
}

SkimServer::~SkimServer() {
    // Member destruction order bites here: the QTcpServer (and its child
    // sockets) outlive clients_, so a socket's disconnected handler would
    // touch the already-destroyed vector. Detach everything first.
    stop();
}

void SkimServer::stop() {
    srv_.close();
    for (QTcpSocket* c : clients_) {
        c->disconnect(this);
        c->deleteLater();
    }
    clients_.clear();
    lastSent_.clear();
}

void SkimServer::onNewConnection() {
    while (QTcpSocket* c = srv_.nextPendingConnection()) {
        clients_.push_back(c);
        connect(c, &QTcpSocket::disconnected, this, [this, c] {
            clients_.removeAll(c);
            c->deleteLater();
        });
        // Cluster-style login chatter; the reply is accepted verbatim and
        // otherwise ignored — this node has exactly one feed to offer.
        sendLine(c, QStringLiteral(
            "Welcome to the %1 console CW skimmer (local RBN)").arg(spotter_));
        c->write("Please enter your call: ");
        connect(c, &QTcpSocket::readyRead, this, [this, c] {
            const QByteArray in = c->readAll();   // consume login/commands
            if (!c->property("greeted").toBool()) {
                c->setProperty("greeted", true);
                const QString who =
                    QString::fromLatin1(in).trimmed().section(' ', 0, 0);
                // "HELLO" is Not1MM's connected-handshake magic word: its
                // cluster client only flips to line-based reads and sends
                // its filter setup after seeing it (cqrlog doesn't care).
                sendLine(c, QStringLiteral(
                    "HELLO %1 de %2-# >  spots follow as the skimmer "
                    "hears them")
                        .arg(who.isEmpty() ? QStringLiteral("OM") : who,
                             spotter_));
                // Backlog replay: the last hour of everything this node
                // carries, so the band map fills on connect instead of
                // starting blank — the sh/dx page real nodes serve.
                for (const Kept& k : history_) sendLine(c, k.line);
            }
        });
    }
}

void SkimServer::sendLine(QTcpSocket* c, const QString& line) {
    c->write((line + QStringLiteral("\r\n")).toLatin1());
}

void SkimServer::broadcast(const QString& line) {
    // History first (even with zero clients — the login replay is only as
    // good as what accumulated while nobody watched), then the live fans.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    history_.push_back({now, line});
    constexpr qint64 kKeepSecs = 3600;     // "about an hour", like a node
    constexpr int    kKeepMax  = 500;      // contest-weekend cap
    while (!history_.isEmpty()
           && (now - history_.front().atSecs > kKeepSecs
               || history_.size() > kKeepMax))
        history_.pop_front();
    for (QTcpSocket* c : clients_) sendLine(c, line);
}

void SkimServer::relayLine(const QString& line) {
    broadcast(line);
}

void SkimServer::announcePota(const QString& call, qint64 hz,
                              const QString& park) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString key = QStringLiteral("P:") + call;   // own throttle lane
    if (now - lastSent_.value(key, 0) < 600) return;
    lastSent_[key] = now;
    broadcast(QStringLiteral("DX de %1-#:  %2  %3 POTA %4  %5Z")
                  .arg(spotter_)
                  .arg(hz / 1000.0, 8, 'f', 1)
                  .arg(call, -12)
                  .arg(park)
                  .arg(QDateTime::currentDateTimeUtc().toString("hhmm")));
}

void SkimServer::announce(const QString& call, qint64 hz, int wpm) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now - lastSent_.value(call, 0) < 120) return;
    lastSent_[call] = now;
    // Classic layout, column-compatible with what cluster parsers expect:
    // "DX de SPOTTER-#:  FREQ_KHZ  CALL  comment  HHMMZ"
    broadcast(QStringLiteral("DX de %1-#:  %2  %3 CW %4  %5Z")
                  .arg(spotter_)
                  .arg(hz / 1000.0, 8, 'f', 1)
                  .arg(call, -12)
                  .arg(wpm > 0 ? QString("%1 WPM").arg(wpm, 2)
                               : QStringLiteral("      "))
                  .arg(QDateTime::currentDateTimeUtc().toString("hhmm")));
}

} // namespace AetherSDR::orion::skim
