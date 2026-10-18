// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/SkimStft.h) for the AetherSDR Orion
// fork (skimmer stage 2): namespace, and FFTW in place of the console's Fft.
#pragma once
#include <QObject>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <memory>

namespace AetherSDR::orion::skim {

// High-resolution STFT over a slice of the SDR capture, sized so CW
// KEYING is visible: 4096-point FFT at ~125 ksps (~30 Hz/bin) hopping
// 1024 samples (~8 ms/column) — a dit at 25 WPM spans ~6 columns, so the
// SKIM view's waterfall shows the dits, dahs and spaces of every station
// the way the ear hears them. The panadapter's 16384-bin / 20 fps
// spectrum can't do this: its rows are max-merged display frames with no
// element-rate time detail.
//
// processIq runs on the SDR streaming thread: recurrence-phasor mix of
// the slice center to DC, windowed-sinc FIR decimation (1 MS/s -> ~125
// ksps), then the hopped FFT. Finished columns (dB quantized to half-dB
// bytes) queue under a mutex; the GUI drains them with takeColumns() on
// a timer. Disabled, the whole thing costs one branch per block.
class SkimStft : public QObject {
    Q_OBJECT
public:
    explicit SkimStft(double inputRate, QObject* parent = nullptr);
    ~SkimStft() override;

    void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Move the slice: center offset relative to the SDR LO (negative =
    // below it). Thread-safe; the SDR thread applies it at the next block
    // and drops queued columns. Returns the generation stamped on columns
    // produced under the new offset — the widget ignores older ones.
    uint32_t setViewOffset(double offsetHz);

    // Capture-file replay at a different rate (see CwDecoder::setInputRate).
    // Before streaming starts only.
    void setInputRate(double rate);

    static constexpr int   kFft  = 4096;
    static constexpr int   kHop  = 1024;
    static constexpr float kDbMin  = -140.0f;   // byte 0
    static constexpr float kDbStep = 0.5f;      // per byte count

    double binHz() const { return outRate_ / kFft; }
    double colSecs() const { return kHop / outRate_; }
    double inputRate() const { return inputRate_; }

    // One finished column: kFft bins, fftshifted (bin kFft/2 = slice
    // center), each byte = (dB - kDbMin) / kDbStep.
    struct Column {
        uint32_t gen = 0;
        std::vector<uint8_t> db;
    };

    void processIq(const std::complex<float>* d, size_t n);  // SDR thread
    std::vector<Column> takeColumns();                       // GUI thread

private:
    void configureRates();

    std::atomic<bool> enabled_{false};
    std::atomic<double> pendingOffset_{0.0};
    std::atomic<bool> retunePending_{false};
    std::atomic<uint32_t> gen_{0};
    uint32_t curGen_ = 0;

    double inputRate_;
    int    decim_ = 8;
    double outRate_ = 125000.0;

    // mixer (recurrence phasor, renormalized periodically)
    std::complex<double> lo_{1.0, 0.0}, loStep_{1.0, 0.0};
    int renorm_ = 0;

    // FIR decimator: windowed sinc, computed once per rate
    std::vector<float> taps_;
    std::vector<std::complex<float>> dline_;     // power-of-2 ring
    size_t dpos_ = 0;
    int    phase_ = 0;

    // hopped FFT over a ring of decimated samples (FFTW, in place)
    struct Fft;
    std::unique_ptr<Fft> fft_;
    std::vector<float> window_;                  // Hann
    std::vector<std::complex<float>> ring_;      // kFft newest samples
    size_t rpos_ = 0;
    size_t primed_ = 0;                          // samples until ring full
    int    hopFill_ = 0;
    std::vector<std::complex<float>> frame_;

    std::mutex mux_;
    std::deque<Column> cols_;                    // bounded, drop-oldest
};

} // namespace AetherSDR::orion::skim
