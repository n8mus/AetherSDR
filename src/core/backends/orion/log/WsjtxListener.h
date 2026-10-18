// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/log/WsjtxListener.h) for the AetherSDR Orion
// fork (logbook, step 9): namespace and include paths, AetherSDR settings in
// place of QSettings, and AetherSDR's cty.dat parser in place of the console's.
#pragma once
#include <QObject>
#include <QString>

class QUdpSocket;

namespace AetherSDR {
class CtyDatParser;
}

namespace AetherSDR::orion::logbook {

class LogDb;

// WSJT-X's UDP status broadcast, listened to the way GridTracker does:
// join the multicast group (default 224.0.0.1:2237, matching WSJT-X's own
// default) and pick the "Logged ADIF" message (type 12) out of the stream —
// a plain ADIF record inside the binary envelope. Every FT8/FT4 QSO WSJT-X
// logs then lands in the console's logbook automatically, so the
// worked-before colors and needed-columns know about digital contacts.
//
// The console's own uploader pushes these QSOs to the online logs
// (log/wsjtxPush, default true — the operator runs WITHOUT GridTracker).
// Set it false to hand pushing back to GridTracker; QSOs then arrive
// stamped already-uploaded so nothing double-posts.
class WsjtxListener : public QObject {
    Q_OBJECT
public:
    WsjtxListener(LogDb* db, const CtyDatParser* cty,
                  QObject* parent = nullptr);
    void start();                          // bind + join per settings

signals:
    void qsoLogged(qint64 id, const QString& call, bool pushWanted);
    // The DX call WSJT-X is working changed (a decode was clicked, or a
    // transmission started) — feeds the New QSO window and the globe.
    // mode is WSJT-X's own ("FT8"), which outranks the rig's USB.
    void dxChanged(const QString& call, const QString& grid,
                   const QString& mode);

private:
    void onDatagram();

    LogDb* db_;
    const CtyDatParser* cty_;
    QUdpSocket* sock_ = nullptr;
    QString lastDx_;
};

} // namespace AetherSDR::orion::logbook
