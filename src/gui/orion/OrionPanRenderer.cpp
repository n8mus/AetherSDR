// AetherSDR Orion fork: the Orion console's 2D panadapter render math. See
// the header for what is and is not ported.
#include "gui/orion/OrionPanRenderer.h"

#include "gui/FftTraceDecomb.h"
#include "gui/FftTraceSmoothing.h"
#include "gui/SpectrumWidget.h"      // WfGradientStop
#include "core/dsp/SpectralEnvelope.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace AetherSDR::orion {

namespace {

// The console's constants (PanadapterWidget.cpp / .h).
constexpr float kPeakDecayDbPer50Ms = 0.15f;
constexpr float kNoRow = -300.0f;
constexpr QRgb  kBaseFill = qRgb(12, 16, 22);           // #0C1016
constexpr QRgb  kPeakColor = qRgba(235, 235, 245, 150);  // #EBEBF5 @150
constexpr QRgb  kDbLabel   = qRgba(255, 216, 50, 200);   // #FFD832 @200

inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

OrionPanRenderer::OrionPanRenderer()
{
    // A palette until the widget hands one over: the console's Enhanced.
    static const std::pair<float, QRgb> kEnhanced[] = {
        {0.00f, qRgb(0, 0, 0)},       {0.12f, qRgb(25, 0, 60)},
        {0.25f, qRgb(0, 30, 160)},    {0.40f, qRgb(0, 120, 230)},
        {0.52f, qRgb(0, 200, 180)},   {0.62f, qRgb(40, 210, 50)},
        {0.72f, qRgb(230, 230, 40)},  {0.82f, qRgb(255, 140, 20)},
        {0.92f, qRgb(255, 40, 30)},   {1.00f, qRgb(255, 255, 255)},
    };
    m_stops.assign(std::begin(kEnhanced), std::end(kEnhanced));
    rebuildPaletteLut();
}

// ── Palette ──────────────────────────────────────────────────────────────

void OrionPanRenderer::setPalette(const WfGradientStop* stops, int count)
{
    if (!stops || count < 2) {
        return;
    }
    std::vector<std::pair<float, QRgb>> next;
    next.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        next.emplace_back(stops[i].pos, qRgb(stops[i].r, stops[i].g, stops[i].b));
    }
    if (next == m_stops) {
        return;
    }
    m_stops = std::move(next);
    rebuildPaletteLut();
    // A palette change recolours every waterfall row, not just new ones.
    m_wfRenderedValid = false;
    touch();
}

QRgb OrionPanRenderer::paletteColor(float t) const
{
    t = clamp01(t);
    const auto& s = m_stops;
    if (t <= s.front().first) {
        return s.front().second;
    }
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (t <= s[i].first) {
            const float span = std::max(1e-6f, s[i].first - s[i - 1].first);
            const float f = (t - s[i - 1].first) / span;
            const QRgb a = s[i - 1].second, b = s[i].second;
            return qRgb(static_cast<int>(qRed(a)   + (qRed(b)   - qRed(a))   * f),
                        static_cast<int>(qGreen(a) + (qGreen(b) - qGreen(a)) * f),
                        static_cast<int>(qBlue(a)  + (qBlue(b)  - qBlue(a))  * f));
        }
    }
    return qRgb(255, 255, 255);   // past the last stop: white, as the console
}

void OrionPanRenderer::rebuildPaletteLut()
{
    m_lut.resize(256);
    for (int i = 0; i < 256; ++i) {
        m_lut[static_cast<std::size_t>(i)] = paletteColor(static_cast<float>(i) / 255.0f);
    }
}

// ── Settings ─────────────────────────────────────────────────────────────

void OrionPanRenderer::setTraceWindow(float refDb, float rangeDb)
{
    rangeDb = std::max(10.0f, rangeDb);
    if (refDb == m_refDb && rangeDb == m_rangeDb) {
        return;
    }
    m_refDb = refDb;
    m_rangeDb = rangeDb;
    m_wfRenderedValid = false;   // the waterfall shares rangeDb
    touch();
}

void OrionPanRenderer::setWaterfallRefDb(float refDb)
{
    refDb = std::clamp(refDb, -200.0f, 50.0f);
    if (refDb == m_wfRefDb) {
        return;
    }
    m_wfRefDb = refDb;
    m_wfRenderedValid = false;
    touch();
}

void OrionPanRenderer::setRowPeriodMs(int ms)
{
    m_rowPeriodMs = std::max(10, ms);
}

void OrionPanRenderer::setAvgFrames(int frames)
{
    m_avgFrames = std::clamp(frames, 1, 64);
}

void OrionPanRenderer::setPeakHold(bool on)
{
    if (on == m_peakHold) {
        return;
    }
    m_peakHold = on;
    if (on) {
        m_peaks = m_trace;   // seed from the live trace, as the console does
    }
    touch();
}

void OrionPanRenderer::setLineWidth(int px)
{
    m_lineWidth = std::clamp(px, 1, 4);
    touch();
}

void OrionPanRenderer::setSmoothPx(int px)
{
    m_smoothPx = std::clamp(px, 0, 16);
    touch();
}

void OrionPanRenderer::setDecombHz(int hz)
{
    m_decombHz = std::clamp(hz, 0, kFftDecombMaxHz);
    touch();
}

// ── Data ─────────────────────────────────────────────────────────────────

void OrionPanRenderer::reset()
{
    m_trace.clear();
    m_peaks.clear();
    m_wfAccum.clear();
    m_wfAccumMs = 0.0;
    m_wfRows.clear();
    m_wfRowMs.clear();
    m_wfHead = 0;
    m_wfCount = 0;
    m_wfPushed = 0;
    m_wfRenderedValid = false;
    m_lastFrameMs = 0;
    touch();
}

void OrionPanRenderer::ingest(const QVector<float>& binsDbm, qint64 nowMs)
{
    const int n = binsDbm.size();
    if (n < 2) {
        return;
    }
    // 50 ms is the console's reference frame interval; k scales the per-frame
    // decay so peak hold behaves the same at any frame rate.
    float dtMs = 50.0f;
    if (m_lastFrameMs > 0) {
        dtMs = std::clamp(static_cast<float>(nowMs - m_lastFrameMs), 1.0f, 250.0f);
    }
    m_lastFrameMs = nowMs;
    const float k = dtMs / 50.0f;

    // The trace: the console's EMA, a = 1 - (1 - 1/avgFrames)^k, so the time
    // constant is avgFrames * 50 ms at any frame rate.
    const bool sizeChanged = static_cast<int>(m_trace.size()) != n;
    if (sizeChanged || m_avgFrames <= 1) {
        m_trace.assign(binsDbm.begin(), binsDbm.end());
    } else {
        const float a = 1.0f - std::pow(1.0f - 1.0f / static_cast<float>(m_avgFrames), k);
        for (int i = 0; i < n; ++i) {
            const auto u = static_cast<std::size_t>(i);
            m_trace[u] += (binsDbm[i] - m_trace[u]) * a;
        }
    }
    if (m_peakHold) {
        if (sizeChanged || m_peaks.size() != m_trace.size()) {
            m_peaks = m_trace;
        } else {
            const float decay = kPeakDecayDbPer50Ms * k;
            for (int i = 0; i < n; ++i) {
                const auto u = static_cast<std::size_t>(i);
                m_peaks[u] = std::max(m_peaks[u] - decay, m_trace[u]);
            }
        }
    }

    // Waterfall: frames are max-merged into one row per row period, so a
    // short burst between rows is still seen.
    if (sizeChanged || static_cast<int>(m_wfAccum.size()) != n) {
        m_wfAccum.assign(static_cast<std::size_t>(n), kNoRow);
        m_wfAccumMs = 0.0;
        m_wfRows.clear();
        m_wfRowMs.clear();
        m_wfHead = 0;
        m_wfCount = 0;
        m_wfPushed = 0;
        m_wfRenderedValid = false;
    }
    for (int i = 0; i < n; ++i) {
        m_wfAccum[static_cast<std::size_t>(i)] =
            std::max(m_wfAccum[static_cast<std::size_t>(i)], binsDbm[i]);
    }
    m_wfAccumMs += dtMs;
    const double rowMs = m_rowPeriodMs;
    if (m_wfAccumMs >= rowMs - 0.5 * dtMs) {
        if (m_wfRows.size() < static_cast<std::size_t>(kWfHistRows)) {
            m_wfRows.push_back(m_wfAccum);
            m_wfRowMs.push_back(nowMs);
            m_wfHead = static_cast<int>(m_wfRows.size()) - 1;
        } else {
            m_wfHead = (m_wfHead + 1) % kWfHistRows;
            m_wfRows[static_cast<std::size_t>(m_wfHead)] = m_wfAccum;
            m_wfRowMs[static_cast<std::size_t>(m_wfHead)] = nowMs;
        }
        m_wfCount = std::min(m_wfCount + 1, kWfHistRows);
        ++m_wfPushed;
        std::fill(m_wfAccum.begin(), m_wfAccum.end(), kNoRow);
        m_wfAccumMs = std::min(m_wfAccumMs - rowMs, rowMs);
    }

    ++m_generation;
    m_specDirty = true;
    m_wfDirty = true;
}

// Per-column normalised levels (0 = bottom of the window, 1 = top), the
// console's columnLevels(): MAX of the bins under a column when zoomed out,
// linear interpolation between bins when zoomed in; de-comb, then smoothing,
// then the clamp to the window.
void OrionPanRenderer::columnLevels(int w, std::vector<float>& outT) const
{
    const int nv = static_cast<int>(m_trace.size());
    outT.assign(static_cast<std::size_t>(std::max(0, w)), 0.0f);
    if (nv < 2 || w <= 0) {
        return;
    }
    static thread_local std::vector<float> col;
    static thread_local std::vector<float> bins;
    static thread_local std::vector<double> linScratch;
    static thread_local SpectralEnvelope env;
    static thread_local DecombScratch decombScratch;

    col.resize(static_cast<std::size_t>(w));
    const bool zoomedOut = nv >= w;

    if (zoomedOut) {
        for (int x = 0; x < w; ++x) {
            const int b0 = static_cast<int>(static_cast<qint64>(x) * nv / w);
            int b1 = static_cast<int>(static_cast<qint64>(x + 1) * nv / w);
            b1 = std::max(b1, b0 + 1);
            float m = -std::numeric_limits<float>::infinity();
            for (int b = b0; b < b1 && b < nv; ++b) {
                m = std::max(m, m_trace[static_cast<std::size_t>(b)]);
            }
            col[static_cast<std::size_t>(x)] = m;
        }
        if (m_decombHz > 0 && m_binHz > 0.0) {
            const double hzPerPx = m_binHz * nv / w;
            const double pts = decombWindowPoints(hzPerPx, m_decombHz);
            if (pts > 0.0) {
                decombTrace(std::span<float>(col), pts, env, decombScratch);
            }
        }
    } else {
        bins.assign(m_trace.begin(), m_trace.end());
        if (m_decombHz > 0 && m_binHz > 0.0) {
            const double pts = decombWindowPoints(m_binHz, m_decombHz);
            if (pts > 0.0) {
                decombTrace(std::span<float>(bins), pts, env, decombScratch);
            }
        }
        for (int x = 0; x < w; ++x) {
            const double pos = static_cast<double>(x) * (nv - 1) / std::max(1, w - 1);
            const int i0 = std::clamp(static_cast<int>(pos), 0, nv - 2);
            const float f = static_cast<float>(pos - i0);
            const float a = bins[static_cast<std::size_t>(i0)];
            const float b = bins[static_cast<std::size_t>(i0 + 1)];
            col[static_cast<std::size_t>(x)] = a + (b - a) * f;
        }
    }
    if (m_smoothPx > 0) {
        smoothFftTraceLinearPower(std::span<const float>(col), std::span<float>(col),
                                  m_smoothPx, linScratch);
    }
    const float dbMin = m_refDb - m_rangeDb;
    for (int x = 0; x < w; ++x) {
        const float v = col[static_cast<std::size_t>(x)];
        outT[static_cast<std::size_t>(x)] =
            std::isfinite(v) ? clamp01((v - dbMin) / m_rangeDb) : 0.0f;
    }
}

// ── Spectrum image ───────────────────────────────────────────────────────

void OrionPanRenderer::drawDbScale(QPainter& p, int w, int h) const
{
    const float dbMin = m_refDb - m_rangeDb;
    const int step = m_rangeDb > 80.0f ? 20 : 10;
    const int first = static_cast<int>(std::ceil(dbMin / (step / 2))) * (step / 2);
    QFont f = p.font();
    f.setPixelSize(9);
    p.setFont(f);
    for (int db = first; db <= static_cast<int>(m_refDb); db += step / 2) {
        const int y = static_cast<int>(h * (1.0f - (db - dbMin) / m_rangeDb));
        if (y < 20 || y > h - 6) {
            continue;
        }
        const bool major = (db % step) == 0;
        if (m_showGrid) {
            p.setPen(QColor(255, 255, 255, major ? 26 : 12));
            p.drawLine(0, y, w, y);
        }
        if (major) {
            p.setPen(QColor::fromRgba(kDbLabel));
            p.drawText(4, y - 2, QString::number(db));
        }
    }
}

const QImage& OrionPanRenderer::renderSpectrum(const QSize& px)
{
    const int w = std::max(2, px.width());
    const int h = std::max(2, px.height());
    if (m_spec.size() != QSize(w, h)) {
        m_spec = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        m_specDirty = true;
    }
    if (!m_specDirty) {
        return m_spec;
    }
    m_specDirty = false;

    static thread_local std::vector<float> colT;
    columnLevels(w, colT);

    m_spec.fill(QColor::fromRgb(kBaseFill));

    // KE9NS fill: the palette keyed to the dB AXIS, gamma-compressed, so the
    // bands run horizontally and a signal climbs through them. One ramp
    // column, then every column is a copy cut off at its trace height.
    if (m_fillTrace && !colT.empty()) {
        std::vector<QRgb> ramp(static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y) {
            const float t = 1.0f - static_cast<float>(y) / static_cast<float>(h - 1);
            const int idx = std::clamp(static_cast<int>(std::pow(t, kFillGamma) * 255.0f + 0.5f), 0, 255);
            ramp[static_cast<std::size_t>(y)] = m_lut[static_cast<std::size_t>(idx)];
        }
        for (int y = 0; y < h; ++y) {
            auto* line = reinterpret_cast<QRgb*>(m_spec.scanLine(y));
            const QRgb c = ramp[static_cast<std::size_t>(y)];
            for (int x = 0; x < w; ++x) {
                const int colY = static_cast<int>(h * (1.0f - colT[static_cast<std::size_t>(x)]));
                if (y >= colY) {
                    line[x] = c;
                }
            }
        }
    }

    QPainter p(&m_spec);
    drawDbScale(p, w, h);

    // Peak hold, under the live trace.
    if (m_peakHold && m_peaks.size() == m_trace.size() && m_peaks.size() >= 2) {
        const int nv = static_cast<int>(m_peaks.size());
        const float dbMin = m_refDb - m_rangeDb;
        QPainterPath path;
        for (int i = 0; i < nv; ++i) {
            const float x = static_cast<float>(i) / static_cast<float>(nv - 1) * (w - 1);
            const float y = h * (1.0f - clamp01((m_peaks[static_cast<std::size_t>(i)] - dbMin) / m_rangeDb));
            if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(QColor::fromRgba(kPeakColor), 1));
        p.drawPath(path);
    }

    // The live trace, the console's "classic" stroke.
    if (!colT.empty()) {
        QPainterPath path;
        for (int x = 0; x < w; ++x) {
            const float y = h * (1.0f - colT[static_cast<std::size_t>(x)]);
            if (x == 0) path.moveTo(x + 0.5f, y); else path.lineTo(x + 0.5f, y);
        }
        QPen pen(m_traceColor, m_lineWidth);
        if (m_lineWidth > 1) {
            pen.setJoinStyle(Qt::RoundJoin);
            pen.setCapStyle(Qt::RoundCap);
            p.setRenderHint(QPainter::Antialiasing, true);
        } else {
            p.setRenderHint(QPainter::Antialiasing, false);
        }
        p.setPen(pen);
        p.drawPath(path);
    }
    return m_spec;
}

// ── Waterfall image ──────────────────────────────────────────────────────

// One history row to one pixel line: MAX of the bins under each column,
// coloured on the waterfall's own window (wfRefDb, shared rangeDb). No gamma.
void OrionPanRenderer::renderWaterfallRow(int rowIndex, int w, QRgb* out) const
{
    const auto& row = m_wfRows[static_cast<std::size_t>(rowIndex)];
    const int nv = static_cast<int>(row.size());
    const float dbMin = m_wfRefDb - m_rangeDb;
    for (int x = 0; x < w; ++x) {
        const int b0 = static_cast<int>(static_cast<qint64>(x) * nv / w);
        int b1 = static_cast<int>(static_cast<qint64>(x + 1) * nv / w);
        b1 = std::max(b1, b0 + 1);
        float m = -std::numeric_limits<float>::infinity();
        for (int b = b0; b < b1 && b < nv; ++b) {
            m = std::max(m, row[static_cast<std::size_t>(b)]);
        }
        const float t = std::isfinite(m) ? clamp01((m - dbMin) / m_rangeDb) : 0.0f;
        out[x] = m_lut[static_cast<std::size_t>(static_cast<int>(t * 255.0f + 0.5f))];
    }
}

const QImage& OrionPanRenderer::renderWaterfall(const QSize& px)
{
    const int w = std::max(2, px.width());
    const int h = std::max(2, px.height());
    bool full = false;
    if (m_wf.size() != QSize(w, h)) {
        m_wf = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        full = true;
    }
    if (!m_wfDirty && !full && m_wfRenderedValid) {
        return m_wf;
    }
    m_wfDirty = false;

    if (m_wfCount == 0) {
        m_wf.fill(Qt::black);
        m_wfRenderedValid = false;
        return m_wf;
    }

    // Rows are stored newest at m_wfHead; row k back from newest is at
    // (head - k) mod N. New rows since the last render scroll the image down
    // and are rendered at the top; anything else is a full repaint. Counted
    // on rows ever pushed, not rows held: once the ring is full the latter
    // stops growing, and the waterfall would stop with it.
    const quint64 since = m_wfPushed - m_wfRenderedPushed;
    const int newRows = (full || !m_wfRenderedValid || since >= static_cast<quint64>(h))
        ? h
        : static_cast<int>(since);
    if (newRows >= h) {
        m_wf.fill(Qt::black);
    } else if (newRows > 0) {
        const int bpl = m_wf.bytesPerLine();
        uchar* bits = m_wf.bits();
        std::memmove(bits + static_cast<std::size_t>(newRows) * bpl, bits,
                     static_cast<std::size_t>(h - newRows) * bpl);
    }
    const int toRender = std::min(newRows, std::min(h, m_wfCount));
    for (int y = 0; y < toRender; ++y) {
        const int idx = (m_wfHead - y + kWfHistRows) % kWfHistRows;
        if (idx >= static_cast<int>(m_wfRows.size())) {
            break;
        }
        renderWaterfallRow(idx, w, reinterpret_cast<QRgb*>(m_wf.scanLine(y)));
    }
    m_wfRenderedPushed = m_wfPushed;
    m_wfRenderedValid = true;

    // "WF n dB" when the waterfall sits on a different reference than the
    // trace, so the split is visible rather than mysterious.
    if (std::fabs(m_wfRefDb - m_refDb) > 0.5f) {
        QPainter p(&m_wf);
        QFont f = p.font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        const QString s = QStringLiteral("WF %1 dB").arg(static_cast<int>(std::lround(m_wfRefDb)));
        p.setPen(QColor(0, 0, 0, 160));
        p.drawText(kDbAxisW + 7, 13, s);
        p.setPen(QColor(200, 220, 240, 150));
        p.drawText(kDbAxisW + 6, 12, s);
    }
    return m_wf;
}

} // namespace AetherSDR::orion
