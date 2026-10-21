// AetherSDR Orion fork: the Orion console's 2D panadapter, as a selectable
// Display mode beside AetherSDR's own 2D and 3D (fork rule 2).
//
// This is the console's render MATH re-expressed on AetherSDR's data: its
// per-column trace shaping, the KE9NS fill (palette keyed to the dB axis,
// gamma 0.6), the trace stroke, peak hold, its dB scale, and a waterfall of
// max-merged raw frames coloured through the same palette on its OWN
// reference level (the console's wfRefDb). Everything the console fuses in
// around that -- VFO roles, zap, locks, map backdrops -- stays out; AetherSDR
// supplies the chrome, the overlays and the gestures.
//
// The renderer owns no Qt widget. SpectrumWidget feeds it frames
// (ingest()) and asks it for two opaque images (renderSpectrum(),
// renderWaterfall()) that it draws into the spectrum and waterfall rects on
// either paint path. See FORK-NOTES.md for the hook-up points.
#pragma once

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <cstdint>
#include <vector>

namespace AetherSDR {

struct WfGradientStop;

namespace orion {

class OrionPanRenderer {
public:
    OrionPanRenderer();

    // The palette, as AetherSDR's own scheme stops (wfSchemeStops()), so the
    // Display panel's one Color Scheme picker drives both halves.
    void setPalette(const WfGradientStop* stops, int count);

    // Trace window: refDb is the top of the spectrum axis, rangeDb its depth.
    // These are AetherSDR's refLevel()/dynamicRange() -- the console's refDb
    // and rangeDb are the same two numbers.
    void setTraceWindow(float refDb, float rangeDb);
    // The waterfall's own top (the console's wfRefDb). It shares rangeDb.
    void setWaterfallRefDb(float refDb);
    [[nodiscard]] float waterfallRefDb() const { return m_wfRefDb; }

    // Waterfall row period in ms (AetherSDR's wfLineDuration()).
    void setRowPeriodMs(int ms);

    void setFillTrace(bool on) { m_fillTrace = on; }
    void setPeakHold(bool on);
    void setLineWidth(int px);
    void setTraceColor(const QColor& c) { m_traceColor = c; }
    void setSmoothPx(int px);
    void setDecombHz(int hz);
    void setShowGrid(bool on) { m_showGrid = on; }
    // Bin spacing in Hz, for de-comb's window.
    void setBinHz(double hz) { m_binHz = hz; }

    // The trace's own time average, the console's avgFrames: 1 = off. The
    // waterfall never sees it; it is built from the raw frames.
    void setAvgFrames(int frames);

    // A new RAW frame: dBm per bin, spanning the display width left to
    // right. The trace is an EMA of these (setAvgFrames); the waterfall is
    // their max over each row period, exactly as the console does it.
    void ingest(const QVector<float>& binsDbm, qint64 nowMs);
    void reset();

    // Opaque images the size asked for. Both are cached and rebuilt only when
    // the data or a setting changed since the last call at that size.
    const QImage& renderSpectrum(const QSize& px);
    const QImage& renderWaterfall(const QSize& px);

    // Rebuild counter: bumps on every ingest and settings change, so a GPU
    // path can skip re-uploading an unchanged image.
    [[nodiscard]] quint64 generation() const { return m_generation; }

    // The console's constants, public so the widget's hit-testing matches.
    static constexpr int   kDbAxisW   = 40;    // left strip: dB labels / drag
    static constexpr float kFillGamma = 0.6f;

private:
    void touch() { ++m_generation; m_specDirty = true; m_wfDirty = true; }
    QRgb paletteColor(float t) const;
    void rebuildPaletteLut();
    void columnLevels(int w, std::vector<float>& outT) const;
    void drawDbScale(QPainter& p, int w, int h) const;
    void renderWaterfallRow(int rowIndex, int w, QRgb* out) const;

    // Palette
    std::vector<std::pair<float, QRgb>> m_stops;
    std::vector<QRgb> m_lut;            // 256 entries

    // Scale
    float m_refDb{-55.0f};
    float m_rangeDb{70.0f};
    float m_wfRefDb{-55.0f};

    // Trace options
    bool  m_fillTrace{true};
    bool  m_peakHold{false};
    int   m_lineWidth{1};
    QColor m_traceColor{0xD2, 0xF5, 0xD7};   // the console's "soft"
    int   m_smoothPx{0};
    int   m_decombHz{0};
    bool  m_showGrid{true};
    double m_binHz{0.0};

    // Data
    int   m_avgFrames{1};
    std::vector<float> m_trace;         // averaged frame (dBm per bin)
    std::vector<float> m_peaks;
    qint64 m_lastFrameMs{0};

    // Waterfall: ring of raw rows, newest last, max-merged per row period
    int    m_rowPeriodMs{100};
    std::vector<float> m_wfAccum;
    double m_wfAccumMs{0.0};
    static constexpr int kWfHistRows = 1024;
    std::vector<std::vector<float>> m_wfRows;   // ring
    int    m_wfHead{0};                         // index of newest row
    int    m_wfCount{0};                        // rows held (<= kWfHistRows)
    quint64 m_wfPushed{0};                      // rows ever pushed
    std::vector<qint64> m_wfRowMs;

    // Caches
    QImage m_spec;
    QImage m_wf;
    bool   m_specDirty{true};
    bool   m_wfDirty{true};
    quint64 m_generation{0};
    // Scrolling waterfall cache: rows already rendered at this width/scale.
    quint64 m_wfRenderedPushed{0};              // m_wfPushed at the last render
    bool   m_wfRenderedValid{false};
};

} // namespace orion
} // namespace AetherSDR
