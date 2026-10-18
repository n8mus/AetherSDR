#pragma once

// The CW skimmer (step 8), as the Orion console runs it: a bank of CW decoder
// channels parked on the strongest signals of the band's CW segment, decoding
// the RSP2's raw IQ, with callsigns mined from the copy. Found calls go out
// as spots (the backend hands them to AetherSDR's spot plumbing) and over a
// DX-cluster telnet server on localhost (default :7300) for cqrlog and
// Not1MM. The engine, decoders and server are the console's, ported as is
// (skimmer/); this class is the AetherSDR-side glue.
//
// Threading: owner thread, apart from feed() which the RSP2 stream callback
// calls through OrionSkimmerFeed.

#include <QObject>
#include <QSet>
#include <QString>

#include <memory>
#include <vector>

namespace AetherSDR {
class CtyDatParser;
}

namespace AetherSDR::orion {

class OrionSkimmerFeed;
namespace skim {
class SkimmerEngine;
class SkimServer;
class SkimStft;
}

class OrionSkimmer : public QObject {
    Q_OBJECT

public:
    struct Settings {
        int channels = 24;                 // the console's default (4..64)
        int telnetPort = 7300;             // 0 = no telnet server
        QString spotterCall;               // "DX de <call>-#:"
        QString ctyPath = QStringLiteral(":/cty.dat");
        QString masterScpPath;             // empty: cqrlog's copy, then the system one
        bool bayes = false;                // the console's experimental brain
    };

    OrionSkimmer(double inputRate, const Settings& settings, QObject* parent = nullptr);
    ~OrionSkimmer() override;

    // The telnet server; false with a reason if the port is taken.
    bool startServer(QString* error);
    [[nodiscard]] quint16 serverPort() const;
    [[nodiscard]] int serverClients() const;

    // Decoding on or off (the operator's switch, the CW-only gate and TX).
    void setActive(bool active);
    [[nodiscard]] bool isActive() const;

    // The RSP2 feeds: hand both to Rsp2Receiver::setSkimmerSink.
    [[nodiscard]] OrionSkimmerFeed* feed() const { return m_feed.get(); }
    [[nodiscard]] OrionSkimmerFeed* viewFeed() const { return m_viewFeed.get(); }

    // For the band map and SKIM waterfall windows (GUI thread).
    [[nodiscard]] skim::SkimmerEngine* engine() const { return m_engine.get(); }
    // The waterfall's fast STFT; null in a build without the RSP2.
    [[nodiscard]] skim::SkimStft* stft() const { return m_stft; }
    // The waterfall window open or closed: the STFT and its feed run only
    // while it is open.
    void setViewActive(bool active);

    // The whole capture's spectrum (dB, DC-centred) at loHz, with the dial
    // (VFO A), which picks the band's CW segment. Lays out the channels and
    // lets the feed through at this LO.
    void onFullSpectrum(const std::vector<float>& db, int spanHz, qint64 loHz, qint64 dialHz);

    [[nodiscard]] int activeChannels() const;
    [[nodiscard]] int channelCount() const;
    [[nodiscard]] bool callValidatorLoaded() const { return m_ctyLoaded; }
    [[nodiscard]] int knownCalls() const { return m_knownCalls; }
    [[nodiscard]] quint64 droppedSamples() const;

signals:
    void spotFound(const QString& call, qint64 hz, int wpm);

private:
    std::unique_ptr<skim::SkimmerEngine> m_engine;
    std::unique_ptr<OrionSkimmerFeed> m_feed;
    // A QObject child (built only with the RSP2): no unique_ptr, so a build
    // without the SDRplay headers needs no SkimStft destructor.
    skim::SkimStft* m_stft = nullptr;
    std::unique_ptr<OrionSkimmerFeed> m_viewFeed;
    std::unique_ptr<skim::SkimServer> m_server;
    std::unique_ptr<CtyDatParser> m_cty;
    Settings m_settings;
    bool m_ctyLoaded = false;
    int m_knownCalls = 0;
};

}  // namespace AetherSDR::orion
