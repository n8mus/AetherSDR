#pragma once

// N8EM fork: the Orion console's CW reader, offered beside AetherSDR's own
// in the CW window's decoder picker (step 10). Two brains, both already in
// the tree for the skimmer:
//
//   "orion-fldigi"  the engine ported from fldigi (its tracker and SOM
//                   fuzzy character matching)
//   "orion-bayes"   the console's experimental Bayesian decoder
//
// This is a thin CwRxBackend over skim::CwDecoder, which does its own tone
// extraction and envelope from the sample stream; AetherSDR's fixed 24 kHz
// decoder PCM goes in as real-valued IQ, exactly as the bench tool feeds it
// (tests/orion_cw_bench.cpp). No calibrated cost, so decoded text rides
// unscoredTextDecoded. See FORK-NOTES.md.

#include <memory>

#include <QString>

#include "models/CwRxModel.h"

namespace AetherSDR::orion {

namespace skim {
class CwDecoder;
}

class OrionCwRxBackend final : public CwRxBackend {
    Q_OBJECT

public:
    // `bayes` picks the experimental brain; otherwise the fldigi engine.
    explicit OrionCwRxBackend(bool bayes, QObject* parent = nullptr);
    ~OrionCwRxBackend() override;

    void start() override;
    void stop() override;
    void reset() override;
    void feedFixed24(const DecoderPcmBlock& block) override;
    [[nodiscard]] bool isRunning() const override { return m_running; }
    [[nodiscard]] float estimatedPitch() const override { return m_pitchHz; }
    [[nodiscard]] float estimatedSpeed() const override { return m_wpm; }
    [[nodiscard]] QString status() const override;
    [[nodiscard]] QString detail() const override;

private:
    std::unique_ptr<skim::CwDecoder> m_decoder;
    bool m_bayes = false;
    bool m_running = false;
    float m_pitchHz = 0.0f;
    float m_wpm = 0.0f;
};

}  // namespace AetherSDR::orion
