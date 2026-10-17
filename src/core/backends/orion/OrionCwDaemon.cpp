#include "core/backends/orion/OrionCwDaemon.h"

#include <QHash>

namespace AetherSDR::orion {

std::vector<OrionCwDaemon::Piece> OrionCwDaemon::decode(const QByteArray& datagram)
{
    std::vector<Piece> out;
    if (datagram.isEmpty()) {
        return out;
    }
    if (datagram.startsWith('\x1b')) {
        if (datagram.size() < 2) {
            return out;
        }
        if (datagram[1] == '4' || datagram[1] == '0') {
            out.emplace_back(Stop{});
        } else if (datagram[1] == '2') {
            bool ok = false;
            const int wpm = QString::fromLatin1(datagram.mid(2)).trimmed().toInt(&ok);
            if (ok && wpm >= 5 && wpm <= 60) {
                out.emplace_back(Speed{wpm});
            }
        }
        return out;   // other escapes (tone, weight, ...) are not ours
    }
    static const QHash<QChar, QString> kProsigns = {
        {QLatin1Char('*'), QStringLiteral("AR")}, {QLatin1Char('='), QStringLiteral("BT")},
        {QLatin1Char('<'), QStringLiteral("SK")}, {QLatin1Char('('), QStringLiteral("KN")},
        {QLatin1Char('!'), QStringLiteral("SN")}, {QLatin1Char('&'), QStringLiteral("AS")},
        {QLatin1Char('>'), QStringLiteral("BK")},
    };
    QString run;
    for (const QChar c : QString::fromLatin1(datagram).trimmed()) {
        const QString ps = kProsigns.value(c);
        if (ps.isEmpty()) {
            run += c;
            continue;
        }
        if (!run.isEmpty()) {
            out.emplace_back(Text{run});
            run.clear();
        }
        out.emplace_back(Prosign{ps[0].toLatin1(), ps[1].toLatin1()});
    }
    if (!run.isEmpty()) {
        out.emplace_back(Text{run});
    }
    return out;
}

OrionCwDaemon::OrionCwDaemon(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, [this] {
        while (m_socket.hasPendingDatagrams()) {
            QByteArray d(static_cast<qsizetype>(m_socket.pendingDatagramSize()), Qt::Uninitialized);
            m_socket.readDatagram(d.data(), d.size());
            for (const Piece& p : decode(d)) {
                std::visit([this](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, Text>) {
                        emit textRequested(v.text);
                    } else if constexpr (std::is_same_v<T, Prosign>) {
                        emit prosignRequested(v.a, v.b);
                    } else if constexpr (std::is_same_v<T, Stop>) {
                        emit stopRequested();
                    } else if constexpr (std::is_same_v<T, Speed>) {
                        emit speedRequested(v.wpm);
                    }
                }, p);
            }
        }
    });
}

bool OrionCwDaemon::listen(quint16 port, QString* error)
{
    close();
    // Loopback only, like the reference program: cqrlog runs on this host.
    if (!m_socket.bind(QHostAddress::LocalHost, port)) {
        if (error) {
            *error = tr("cannot bind localhost:%1: %2").arg(port).arg(m_socket.errorString());
        }
        return false;
    }
    return true;
}

void OrionCwDaemon::close()
{
    m_socket.close();
}

}  // namespace AetherSDR::orion
