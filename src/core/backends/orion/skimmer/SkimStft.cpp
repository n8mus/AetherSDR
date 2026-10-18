// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/SkimStft.cpp) for the AetherSDR Orion
// fork (skimmer stage 2): namespace, and FFTW in place of the console's Fft.
#include "core/backends/orion/skimmer/SkimStft.h"

#include "core/dsp/FftwPlannerLock.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR::orion::skim {

namespace {
constexpr size_t kMaxQueue = 1024;   // ~8 s of columns if the GUI stalls
} // namespace

// The console's Fft::forward(frame) in place, on FFTW.
struct SkimStft::Fft {
    fftwf_complex* buf = nullptr;
    fftwf_plan plan = nullptr;
    explicit Fft(int n) {
        const auto lock = AetherSDR::fftwfPlannerLock();
        buf = fftwf_alloc_complex(size_t(n));
        plan = fftwf_plan_dft_1d(n, buf, buf, FFTW_FORWARD, FFTW_ESTIMATE);
    }
    ~Fft() {
        const auto lock = AetherSDR::fftwfPlannerLock();
        if (plan) fftwf_destroy_plan(plan);
        fftwf_free(buf);
    }
    void forward(std::vector<std::complex<float>>& v) {
        std::memcpy(buf, v.data(), v.size() * sizeof(fftwf_complex));
        fftwf_execute(plan);
        std::memcpy(v.data(), buf, v.size() * sizeof(fftwf_complex));
    }
};

SkimStft::~SkimStft() = default;

SkimStft::SkimStft(double inputRate, QObject* parent)
    : QObject(parent), inputRate_(inputRate), fft_(std::make_unique<Fft>(kFft)),
      window_(kFft), ring_(kFft), frame_(kFft) {
    for (int i = 0; i < kFft; ++i)
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i
                                             / (kFft - 1)));
    configureRates();
}

void SkimStft::configureRates() {
    decim_ = std::max(1, int(std::lround(inputRate_ / 125000.0)));
    outRate_ = inputRate_ / decim_;
    // Windowed-sinc anti-alias FIR, computed at the decimated instants
    // only (~6 M multiplies/s at 1 MS/s — nothing). 6 taps per decim step
    // and a 0.42*outRate cutoff put the first alias ~50 dB down; edge
    // droop is cosmetic on a waterfall.
    const int n = 6 * decim_;
    taps_.resize(n);
    const float fc = 0.42f * float(outRate_);
    float sum = 0.0f;
    for (int k = 0; k < n; ++k) {
        const float t = k - (n - 1) / 2.0f;
        const float x = 2.0f * float(M_PI) * fc / float(inputRate_) * t;
        const float sinc = std::abs(x) < 1e-6f ? 1.0f : std::sin(x) / x;
        const float w = 0.54f - 0.46f * std::cos(2.0f * float(M_PI) * k
                                                 / (n - 1));
        taps_[k] = sinc * w;
        sum += taps_[k];
    }
    for (float& t : taps_) t /= sum;
    size_t sz = 1;
    while (sz < taps_.size() + 1) sz <<= 1;
    dline_.assign(sz, {0.0f, 0.0f});
    dpos_ = 0;
    phase_ = 0;
}

uint32_t SkimStft::setViewOffset(double offsetHz) {
    pendingOffset_.store(offsetHz, std::memory_order_relaxed);
    const uint32_t g = gen_.fetch_add(1, std::memory_order_relaxed) + 1;
    retunePending_.store(true, std::memory_order_release);
    return g;
}

void SkimStft::setInputRate(double rate) {
    inputRate_ = rate;
    configureRates();
    setViewOffset(pendingOffset_.load(std::memory_order_relaxed));
}

void SkimStft::processIq(const std::complex<float>* d, size_t n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (retunePending_.exchange(false, std::memory_order_acquire)) {
        const double inc =
            -2.0 * M_PI * pendingOffset_.load(std::memory_order_relaxed)
            / inputRate_;
        loStep_ = {std::cos(inc), std::sin(inc)};
        lo_ = {1.0, 0.0};
        std::fill(dline_.begin(), dline_.end(),
                  std::complex<float>{0.0f, 0.0f});
        dpos_ = 0;
        phase_ = 0;
        std::fill(ring_.begin(), ring_.end(),
                  std::complex<float>{0.0f, 0.0f});
        rpos_ = 0;
        primed_ = kFft;
        hopFill_ = 0;
        curGen_ = gen_.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mux_);
        cols_.clear();
    }
    const size_t mask = dline_.size() - 1;
    for (size_t i = 0; i < n; ++i) {
        dline_[dpos_ & mask] =
            std::complex<float>(std::complex<double>(d[i]) * lo_);
        ++dpos_;
        lo_ *= loStep_;
        if (++renorm_ >= 4096) {
            renorm_ = 0;
            lo_ /= std::abs(lo_);
        }
        if (++phase_ < decim_) continue;
        phase_ = 0;
        std::complex<float> acc(0.0f, 0.0f);
        size_t idx = (dpos_ - 1) & mask;
        for (const float t : taps_) {
            acc += t * dline_[idx];
            idx = (idx - 1) & mask;
        }
        ring_[rpos_] = acc;
        rpos_ = (rpos_ + 1) % kFft;
        if (primed_ > 0) --primed_;
        if (++hopFill_ < kHop) continue;
        hopFill_ = 0;
        if (primed_ > 0) continue;           // window not yet full of data
        // Assemble oldest->newest, window, FFT. NO mean subtraction: the
        // mixer already moved the SDR's residual DC line off to −offset
        // (normally outside the slice), so post-mix DC is the SLICE
        // CENTER — subtracting it notched out whatever station sat there
        // (review-found: off-segment the slice centers on the DIAL, and
        // these radios are carrier-at-dial in CW — the display nulled
        // exactly the station being worked).
        for (int k = 0; k < kFft; ++k)
            frame_[k] = ring_[(rpos_ + k) % kFft] * window_[k];
        fft_->forward(frame_);
        Column col;
        col.gen = curGen_;
        col.db.resize(kFft);
        const float norm = 1.0f / float(kFft);
        for (int k = 0; k < kFft; ++k) {
            const std::complex<float> c = frame_[(k + kFft / 2) % kFft];
            const float p =
                (c.real() * c.real() + c.imag() * c.imag()) * norm * norm;
            const float db = 10.0f * std::log10(p + 1e-12f);
            col.db[k] = uint8_t(std::clamp(
                int((db - kDbMin) / kDbStep + 0.5f), 0, 255));
        }
        std::lock_guard<std::mutex> lk(mux_);
        cols_.push_back(std::move(col));
        if (cols_.size() > kMaxQueue) cols_.pop_front();
    }
}

std::vector<SkimStft::Column> SkimStft::takeColumns() {
    std::lock_guard<std::mutex> lk(mux_);
    std::vector<Column> out(std::make_move_iterator(cols_.begin()),
                            std::make_move_iterator(cols_.end()));
    cols_.clear();
    return out;
}

} // namespace AetherSDR::orion::skim
