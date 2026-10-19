#include "core/backends/orion/OrionCwRxBackend.h"

#include <complex>
#include <vector>

#include "core/AppSettings.h"
#include "core/backends/orion/skimmer/CwDecoder.h"

namespace AetherSDR::orion {

namespace {
// AetherSDR hands every decoder its PCM at this rate (DecoderPcmBlock).
constexpr double kRateHz = static_cast<double>(DecoderPcmBlock::kSampleRateHz);

// Where in that stream the tone sits. Real-valued samples put the signal at
// +/- the audio pitch, so the reader listens at the operator's sidetone
// pitch — the note they tune a station onto.
double pitchHz()
{
    const int hz = AppSettings::instance()
                       .value(QStringLiteral("CwSidetonePitch"), 600)
                       .toInt();
    return hz >= 100 && hz <= 3000 ? static_cast<double>(hz) : 600.0;
}
}  // namespace

OrionCwRxBackend::OrionCwRxBackend(bool bayes, QObject* parent)
    : CwRxBackend(parent)
    , m_bayes(bayes)
{
    m_decoder = std::make_unique<skim::CwDecoder>(kRateHz, pitchHz());
    m_decoder->setBrain(bayes ? skim::CwDecoder::Brain::Bayes
                              : skim::CwDecoder::Brain::Engine);
    m_decoder->setSom(true);
    m_decoder->setAttack(1);
    m_decoder->setDecay(1);
    m_decoder->setSquelch(12.0);
    m_pitchHz = static_cast<float>(pitchHz());

    // The loose stream is the one for human eyes: SOM's closest letter
    // rather than the call miner's honest '*'. No calibrated cost here, so
    // it rides the unscored signal.
    connect(m_decoder.get(), &skim::CwDecoder::textLoose, this,
            [this](const QString& text) {
                if (!text.isEmpty()) {
                    emit unscoredTextDecoded(text);
                }
            });
    connect(m_decoder.get(), &skim::CwDecoder::wpmEstimated, this, [this](int wpm) {
        if (wpm > 0) {
            m_wpm = static_cast<float>(wpm);
            emit statsUpdated(m_pitchHz, m_wpm);
        }
    });
}

OrionCwRxBackend::~OrionCwRxBackend() = default;

void OrionCwRxBackend::start()
{
    // The pitch may have moved since this backend was built (the operator
    // keeps the reader on the note they listen at).
    const double hz = pitchHz();
    if (!qFuzzyCompare(hz, static_cast<double>(m_pitchHz))) {
        m_pitchHz = static_cast<float>(hz);
        m_decoder->retune(hz);
    }
    m_decoder->setEnabled(true);
    m_running = true;
    emit statusChanged();
}

void OrionCwRxBackend::stop()
{
    m_decoder->setEnabled(false);
    m_running = false;
    emit statusChanged();
}

void OrionCwRxBackend::reset()
{
    m_decoder->retune(pitchHz());
    m_wpm = 0.0f;
}

void OrionCwRxBackend::feedFixed24(const DecoderPcmBlock& block)
{
    if (!m_running || block.samples.isEmpty()) {
        return;
    }
    // Real-valued IQ, the way the bench feeds it: the decoder's own front
    // end does the mixing, filtering and envelope from here.
    std::vector<std::complex<float>> iq(static_cast<std::size_t>(block.samples.size()));
    for (int i = 0; i < block.samples.size(); ++i) {
        iq[static_cast<std::size_t>(i)] = {block.samples[i], 0.0f};
    }
    m_decoder->processIq(iq.data(), iq.size());
}

QString OrionCwRxBackend::status() const
{
    if (!m_running) {
        return tr("stopped");
    }
    return m_wpm > 0.0f ? tr("%1 WPM").arg(static_cast<int>(m_wpm)) : tr("listening");
}

QString OrionCwRxBackend::detail() const
{
    return m_bayes ? tr("Orion Bayes decoder (experimental), at %1 Hz")
                         .arg(static_cast<int>(m_pitchHz))
                   : tr("Orion fldigi engine, at %1 Hz").arg(static_cast<int>(m_pitchHz));
}

}  // namespace AetherSDR::orion
