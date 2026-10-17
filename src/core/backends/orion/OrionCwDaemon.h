#pragma once

// cwdaemon on loopback UDP :6789 (step 7): cqrlog's CW interface set to
// cwdaemon keys the WinKeyer through here, as it did through the reference
// program. Text keys the radio through the WinKeyer's own key line, so the
// backend logs every message it accepts.
//
// cwdaemon's prosign convention is NOT the keyer's, and they collide: '<' is
// SK to cwdaemon but AR on a WinKeyer, and cwdaemon's '*' (AR) maps to null
// on the keyer. So prosigns are translated here into 0x1B merges.
//   * AR   = BT   < SK   ( KN   ! SN   & AS   > BK
// ESC 4 or ESC 0 stops; ESC 2<n> sets the speed (5..60 WPM).

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QUdpSocket>

#include <variant>
#include <vector>

namespace AetherSDR::orion {

class OrionCwDaemon : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 6789;

    // One cwdaemon datagram, decoded.
    struct Text { QString text; };
    struct Prosign { char a; char b; };
    struct Stop {};
    struct Speed { int wpm; };
    using Piece = std::variant<Text, Prosign, Stop, Speed>;
    [[nodiscard]] static std::vector<Piece> decode(const QByteArray& datagram);

    explicit OrionCwDaemon(QObject* parent = nullptr);
    bool listen(quint16 port, QString* error);
    void close();
    [[nodiscard]] quint16 port() const { return m_socket.localPort(); }

signals:
    void textRequested(const QString& text);
    void prosignRequested(char a, char b);
    void stopRequested();
    void speedRequested(int wpm);

private:
    QUdpSocket m_socket;
};

}  // namespace AetherSDR::orion
