// SPDX-License-Identifier: GPL-2.0-or-later
// The band map and the waterfall canvas follow the Orion console's
// SkimmerWindow and SkimViewWindow (src/ui/), re-expressed in AetherSDR's
// widgets and colours for the AetherSDR Orion fork (skimmer stage 2).
#include "gui/orion/OrionSkimmerUi.h"

#include "core/AppSettings.h"
#include "core/backends/orion/OrionBackend.h"
#include "core/backends/orion/OrionSkimmer.h"
#include "core/backends/orion/skimmer/SkimStft.h"
#include "core/backends/orion/skimmer/SkimmerEngine.h"
#include "gui/SpectrumWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#ifdef AETHER_ORION_LOGBOOK
#include "gui/orion/OrionLogbookUi.h"
#endif

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

namespace AetherSDR {

using orion::OrionBackend;
using orion::OrionSkimmer;
using orion::skim::SkimStft;
using orion::skim::SkimmerEngine;

namespace {

// The skimmer of the connected Orion, if it runs (with the RSP2).
OrionSkimmer* currentSkimmer(RadioModel* model)
{
    auto* backend = model ? dynamic_cast<OrionBackend*>(model->backend()) : nullptr;
    return backend ? backend->skimmer() : nullptr;
}

// Worked-before colour for a call (the logbook's, shared with the spots);
// the skimmer's violet when the log has nothing to say.
QColor callColour(const QString& call, qint64 hz)
{
#ifdef AETHER_ORION_LOGBOOK
    const QColor c = orionWorkedSpotColour(call, hz / 1e6);
    if (c.isValid()) return c;
#else
    Q_UNUSED(call);
    Q_UNUSED(hz);
#endif
    return QColor(205, 140, 255);
}

// The skimmer's on-switch, in the window itself (it is also on Radio Setup
// -> Orion): decoding is off until the operator turns it on, as in the
// console, and the waterfall runs either way.
QCheckBox* decodeSwitch(RadioModel* model, QWidget* parent)
{
    auto* box = new QCheckBox(QObject::tr("Decode the CW segment"), parent);
    box->setToolTip(QObject::tr("Run the skimmer's decoder channels (paused outside CW and "
                                "while transmitting)"));
    box->setChecked(AppSettings::instance().value(QStringLiteral("OrionSkimmerEnabled"), false).toBool());
    QObject::connect(box, &QCheckBox::toggled, box, [model](bool on) {
        if (!model || !model->backendDeclaresExtension(QStringLiteral("orion"))) return;
        QVariantMap f;
        f.insert(QStringLiteral("enabled"), on);
        model->invokeBackendExtension(QStringLiteral("orion"), QStringLiteral("skimmer.set"), 0, f);
    });
    return box;
}

void tuneSlice(const std::function<SliceModel*()>& activeSlice, qint64 hz)
{
    if (SliceModel* s = activeSlice ? activeSlice() : nullptr) {
        s->setFrequency(double(hz) / 1e6);
    }
}

// ---- band map ----------------------------------------------------------------

class BandMapWindow : public QDialog {
public:
    BandMapWindow(RadioModel* model, std::function<SliceModel*()> activeSlice, QWidget* parent)
        : QDialog(parent)
        , m_model(model)
        , m_activeSlice(std::move(activeSlice))
    {
        setWindowTitle(tr("Skimmer band map"));
        resize(440, 500);
        auto* lay = new QVBoxLayout(this);
        lay->addWidget(decodeSwitch(model, this));
        m_table = new QTableWidget(0, 4, this);
        m_table->setHorizontalHeaderLabels({tr("kHz"), tr("Call"), tr("WPM"), tr("Age")});
        m_table->horizontalHeader()->setStretchLastSection(true);
        m_table->verticalHeader()->setVisible(false);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setColumnWidth(0, 90);
        m_table->setColumnWidth(1, 120);
        m_table->setColumnWidth(2, 56);
        lay->addWidget(m_table, 1);
        m_info = new QLabel(this);
        m_info->setWordWrap(true);
        lay->addWidget(m_info);
        connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) {
            const auto* f = m_table->item(row, 0);
            if (!f) return;
            const qint64 hz = f->data(Qt::UserRole).toLongLong();
            if (hz > 0) tuneSlice(m_activeSlice, hz);
        });
        m_tick = new QTimer(this);
        m_tick->setInterval(2000);
        connect(m_tick, &QTimer::timeout, this, [this] { refresh(); });
    }

protected:
    void showEvent(QShowEvent* e) override
    {
        refresh();
        m_tick->start();
        QDialog::showEvent(e);
    }
    void hideEvent(QHideEvent* e) override
    {
        m_tick->stop();
        QDialog::hideEvent(e);
    }

private:
    void refresh()
    {
        OrionSkimmer* sk = currentSkimmer(m_model);
        if (!sk) {
            m_table->setRowCount(0);
            m_info->setText(tr("The skimmer runs with the Orion's RSP2 panadapter. Switch it on "
                               "in Settings -> Radio Setup -> Orion."));
            return;
        }
        SkimmerEngine* eng = sk->engine();
        struct Row {
            qint64 hz;
            QString call;
            int wpm;
            qint64 atSecs;   // 0 = channel still hunting
        };
        std::vector<Row> rows;
        for (const auto& s : eng->spots()) rows.push_back({s.hz, s.call, s.wpm, s.atSecs});
        int hunting = 0;
        for (const auto& c : eng->channelInfo()) {
            if (!c.active || !c.call.isEmpty()) continue;
            ++hunting;
            rows.push_back({c.hz, QStringLiteral("?"), c.wpm, 0});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.hz < b.hz; });
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
        m_table->setRowCount(int(rows.size()));
        for (int i = 0; i < int(rows.size()); ++i) {
            const Row& r = rows[size_t(i)];
            auto* f = new QTableWidgetItem(QString::number(r.hz / 1000.0, 'f', 1));
            f->setData(Qt::UserRole, r.hz);
            auto* c = new QTableWidgetItem(r.call);
            auto* w = new QTableWidgetItem(r.wpm > 0 ? QString::number(r.wpm) : QStringLiteral("--"));
            auto* a = new QTableWidgetItem(r.atSecs > 0 ? tr("%1 min").arg((now - r.atSecs) / 60)
                                                        : QStringLiteral("..."));
            c->setForeground(r.call == QLatin1String("?") ? dim : callColour(r.call, r.hz));
            QFont bold = c->font();
            bold.setBold(true);
            c->setFont(bold);
            m_table->setItem(i, 0, f);
            m_table->setItem(i, 1, c);
            m_table->setItem(i, 2, w);
            m_table->setItem(i, 3, a);
        }
        QString text = tr("%1 station(s), %2 channel(s) hunting; click a row to tune")
                           .arg(int(rows.size()) - hunting).arg(hunting);
        if (!sk->isActive()) {
            text += AppSettings::instance().value(QStringLiteral("OrionSkimmerEnabled"), false).toBool()
                ? tr(" (decoding is paused: outside CW or transmitting)")
                : tr(" (decoding is off: tick \"Decode the CW segment\" above)");
        }
        m_info->setText(text);
    }

    RadioModel* m_model;
    std::function<SliceModel*()> m_activeSlice;
    QTableWidget* m_table = nullptr;
    QLabel* m_info = nullptr;
    QTimer* m_tick = nullptr;
};

// ---- SKIM waterfall ----------------------------------------------------------

// A "nice" tick step (1/2/5 x 10^k) giving ~6 divisions across the view.
double niceStep(double span)
{
    const double raw = span / 6.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (raw <= m * mag) return m * mag;
    return 10.0 * mag;
}

constexpr size_t kHistCols = 4096;   // ~34 s of raw columns for re-render
constexpr float kRangeDb = 45.0f;    // floor..top of the colour ramp
constexpr float kFloorPad = 3.0f;    // keep the noise just above the floor colour

// The console's SkimViewCanvas, with AetherSDR's waterfall scheme and theme
// in place of the console's colours.
class SkimCanvas : public QWidget {
public:
    SkimCanvas(RadioModel* model, QMainWindow* mainWindow,
               std::function<void(qint64)> onTune, QWidget* parent)
        : QWidget(parent)
        , m_model(model)
        , m_mainWindow(mainWindow)
        , m_onTune(std::move(onTune))
    {
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        m_colsPerPx = std::clamp(AppSettings::instance()
                                     .value(QStringLiteral("OrionSkimView/speed"), 1).toInt(), 1, 4);
        m_drain = new QTimer(this);
        m_drain->setInterval(33);
        connect(m_drain, &QTimer::timeout, this, [this] { drainColumns(); });
        m_tune = new QTimer(this);
        m_tune->setInterval(300);
        connect(m_tune, &QTimer::timeout, this, [this] { checkTuning(); });
    }

    void start()
    {
        buildLut();
        attach();
        checkTuning();
        m_drain->start();
        m_tune->start();
    }
    void stop()
    {
        m_drain->stop();
        m_tune->stop();
        if (OrionSkimmer* sk = currentSkimmer(m_model)) sk->setViewActive(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_wfDirty) rebuildImage();
        QPainter p(this);
        p.fillRect(rect(), m_lut[0]);
        if (!m_wfImg.isNull()) p.drawImage(0, 0, m_wfImg);
        drawScale(p);
        drawDial(p);
        drawLabels(p);
        drawHover(p);
        drawInfo(p);
    }
    void resizeEvent(QResizeEvent*) override { m_wfDirty = true; }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_pressPos = e->pos();
        m_pressView0 = m_viewLo;
        m_dragging = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        m_hover = e->pos();
        if ((e->buttons() & Qt::LeftButton) && m_stft) {
            if ((e->pos() - m_pressPos).manhattanLength() > 4) m_dragging = true;
            if (m_dragging && height() > 0) {
                const double span = m_viewHi - m_viewLo;
                const double dHz = (e->pos().y() - m_pressPos.y()) * span / height();
                setView(m_pressView0 + dHz, m_pressView0 + dHz + span);
            }
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || m_dragging) return;
        for (const auto& [r, cpp] : m_speedHits) {
            if (r.contains(e->pos())) {
                if (cpp != m_colsPerPx) {
                    m_colsPerPx = cpp;
                    AppSettings::instance().setValue(QStringLiteral("OrionSkimView/speed"), cpp);
                    AppSettings::instance().save();
                    m_accumN = 0;
                    m_wfDirty = true;
                }
                update();
                return;
            }
        }
        for (const auto& L : m_labelHits) {
            if (L.rect.contains(e->pos())) {
                m_onTune(L.hz);
                return;
            }
        }
        const qint64 hz = hzOfY(e->pos().y());
        if (hz > 0 && m_stft) m_onTune((hz + 5) / 10 * 10);
    }
    void wheelEvent(QWheelEvent* e) override
    {
        if (!m_stft) return;
        const double span = m_viewHi - m_viewLo;
        const double f = e->angleDelta().y() > 0 ? 1.0 / 1.25 : 1.25;
        const double ns = std::clamp(span * f, 1500.0, displaySpan());
        const double hzC = double(hzOfY(int(e->position().y())));
        const double top = hzC + (m_viewHi - hzC) * ns / span;
        setView(top - ns, top);
        m_pressPos = e->position().toPoint();
        m_pressView0 = m_viewLo;
        e->accept();
    }

private:
    struct LabelHit {
        QRect rect;
        qint64 hz;
    };

    // The waterfall's colours: the operator's AetherSDR waterfall scheme.
    void buildLut()
    {
        int scheme = 0;
        if (m_mainWindow) {
            if (auto* sw = m_mainWindow->findChild<SpectrumWidget*>()) scheme = sw->wfColorScheme();
        }
        int count = 0;
        const WfGradientStop* stops = wfSchemeStops(static_cast<WfColorScheme>(scheme), count);
        for (int i = 0; i < 256; ++i) {
            const float t = i / 255.0f;
            if (!stops || count < 2) {
                m_lut[i] = qRgb(i, i, i);
                continue;
            }
            int k = 1;
            while (k < count - 1 && stops[k].pos < t) ++k;
            const WfGradientStop& a = stops[k - 1];
            const WfGradientStop& b = stops[k];
            const float f = std::clamp((t - a.pos) / std::max(1e-6f, b.pos - a.pos), 0.0f, 1.0f);
            m_lut[i] = qRgb(int(a.r + f * (b.r - a.r)), int(a.g + f * (b.g - a.g)),
                            int(a.b + f * (b.b - a.b)));
        }
        m_wfDirty = true;
    }

    // Follow the skimmer of the current connection (it is rebuilt with the RSP2).
    void attach()
    {
        OrionSkimmer* sk = currentSkimmer(m_model);
        SkimStft* stft = sk ? sk->stft() : nullptr;
        if (stft != m_stft) {
            m_stft = stft;
            m_raw.clear();
            m_accumN = 0;
            m_sliceOff = 1e18;
            m_sliceCenterAbs = 0;
            m_segLo = m_segHi = 0;
            m_floorDb = -999.0f;
            m_wfDirty = true;
        }
        m_engine = sk ? sk->engine() : nullptr;
        if (sk) sk->setViewActive(true);
    }

    double displaySpan() const
    {
        // The FIR's skirts make the outer ~8% of the slice dishonest.
        return m_stft ? 0.92 * m_stft->binHz() * SkimStft::kFft : 24000.0;
    }
    double yOfHz(double hz) const { return (m_viewHi - hz) / (m_viewHi - m_viewLo) * height(); }
    qint64 hzOfY(int y) const
    {
        if (height() <= 0) return 0;
        return qint64(std::llround(m_viewHi - double(y) * (m_viewHi - m_viewLo) / height()));
    }
    double binOfHz(double hz) const
    {
        return SkimStft::kFft / 2.0 + (hz - double(m_sliceCenterAbs)) / m_stft->binHz();
    }

    void setView(double lo, double hi)
    {
        const double half = displaySpan() / 2.0;
        const double lim0 = double(m_sliceCenterAbs) - half;
        const double lim1 = double(m_sliceCenterAbs) + half;
        const double span = std::min(hi - lo, lim1 - lim0);
        lo = std::clamp(lo, lim0, lim1 - span);
        m_viewLo = lo;
        m_viewHi = lo + span;
        m_wfDirty = true;
        update();
    }

    void checkTuning()
    {
        attach();
        auto* backend = m_model ? dynamic_cast<OrionBackend*>(m_model->backend()) : nullptr;
        if (!m_stft || !backend) {
            update();
            return;
        }
        qint64 dial = 0, loAbs = 0;
        backend->skimmerTuning(dial, loAbs);
        if (dial <= 0 || loAbs <= 0) return;
        m_dial = dial;
        qint64 segLo = 0, segHi = 0;
        if (!SkimmerEngine::cwSegment(dial, segLo, segHi)) {
            segLo = dial - 12000;   // off the CW segment: follow the dial, +-12 kHz
            segHi = dial + 12000;
        }
        double off = double((segLo + segHi) / 2 - loAbs);
        const double lim = m_stft->inputRate() / 2.0 - m_stft->binHz() * SkimStft::kFft / 2.0;
        off = std::clamp(off, -lim, lim);
        if (std::abs(off - m_sliceOff) > 0.5 || loAbs != m_loAbs) {
            const qint64 center = loAbs + qint64(std::llround(off));
            m_sliceOff = off;
            m_loAbs = loAbs;
            m_curGen = m_stft->setViewOffset(off);
            m_accumN = 0;
            // The stored columns are slice-centre relative: keep them when
            // only the LO moved under a fixed slice.
            if (center != m_sliceCenterAbs) {
                m_sliceCenterAbs = center;
                m_raw.clear();
                m_wfDirty = true;
            }
            if (segLo != m_segLo || segHi != m_segHi) {
                m_segLo = segLo;
                m_segHi = segHi;
                setView(double(segLo) - 500.0, double(segHi) + 500.0);
            }
        }
        update();
    }

    void drainColumns()
    {
        if (!m_stft) return;
        auto batch = m_stft->takeColumns();
        std::vector<std::vector<uint8_t>> px;
        for (auto& c : batch) {
            if (c.gen != m_curGen) continue;   // pre-retune leftovers
            if (m_accumN == 0) m_accum = c.db;
            else
                for (size_t i = 0; i < m_accum.size(); ++i) m_accum[i] = std::max(m_accum[i], c.db[i]);
            m_raw.push_back(std::move(c.db));
            if (m_raw.size() > kHistCols) m_raw.pop_front();
            if (++m_accumN >= m_colsPerPx) {
                px.push_back(m_accum);
                m_accumN = 0;
            }
            if (m_floorDb < -998.0f || ++m_floorTick >= 8) {
                m_floorTick = 0;
                updateFloor(m_raw.back());
            }
        }
        if (px.empty()) return;
        if (!m_wfDirty && !m_wfImg.isNull()) {
            const int w = m_wfImg.width();
            const int k = std::min(int(px.size()), w);
            scrollLeft(k);
            for (int i = 0; i < k; ++i) renderPixelColumn(w - k + i, px[px.size() - size_t(k) + size_t(i)].data());
        }
        update();
    }

    void updateFloor(const std::vector<uint8_t>& col)
    {
        int b0 = int(binOfHz(m_viewLo)), b1 = int(binOfHz(m_viewHi));
        b0 = std::clamp(b0, 0, SkimStft::kFft - 1);
        b1 = std::clamp(b1, b0 + 1, SkimStft::kFft);
        std::vector<uint8_t> s;
        s.reserve(256);
        const int step = std::max(1, (b1 - b0) / 256);
        for (int b = b0; b < b1; b += step) s.push_back(col[size_t(b)]);
        if (s.size() < 8) return;
        std::nth_element(s.begin(), s.begin() + long(s.size() / 2), s.end());
        const float med = SkimStft::kDbMin + s[s.size() / 2] * SkimStft::kDbStep;
        if (m_floorDb < -998.0f) m_floorDb = med;
        else m_floorDb += 0.05f * (med - m_floorDb);
    }

    void scrollLeft(int k)
    {
        const int w = m_wfImg.width();
        if (k >= w) return;
        for (int y = 0; y < m_wfImg.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(m_wfImg.scanLine(y));
            std::memmove(line, line + k, size_t(w - k) * sizeof(QRgb));
        }
    }

    void renderPixelColumn(int x, const uint8_t* col)
    {
        // Per-column floor: a lightning crash lifts its own column, so the
        // stripe vanishes while narrowband keying keeps full contrast.
        float floor = m_floorDb < -998.0f ? -120.0f : m_floorDb;
        {
            int b0 = int(binOfHz(m_viewLo)), b1 = int(binOfHz(m_viewHi));
            b0 = std::clamp(b0, 0, SkimStft::kFft - 1);
            b1 = std::clamp(b1, b0 + 1, SkimStft::kFft);
            if (b1 - b0 >= 32) {
                uint8_t s[64];
                const int step = std::max(1, (b1 - b0) / 64);
                int n = 0;
                for (int b = b0; b < b1 && n < 64; b += step) s[n++] = col[b];
                std::nth_element(s, s + n / 2, s + n);
                floor = SkimStft::kDbMin + s[n / 2] * SkimStft::kDbStep;
            }
        }
        for (int y = 0; y < m_wfImg.height(); ++y) {
            const auto& [b0, b1] = m_yBins[size_t(y)];
            float v;
            if (b1 > b0) {
                uint8_t m = 0;
                for (int b = b0; b < b1; ++b) m = std::max(m, col[b]);
                v = SkimStft::kDbMin + m * SkimStft::kDbStep;
            } else {
                const double fb = std::clamp(
                    binOfHz(m_viewHi - (y + 0.5) * (m_viewHi - m_viewLo) / height()), 0.0,
                    double(SkimStft::kFft - 1.001));
                const int b = int(fb);
                const float f = float(fb - b);
                v = SkimStft::kDbMin + ((1.0f - f) * col[b] + f * col[b + 1]) * SkimStft::kDbStep;
            }
            const float t = (v - floor - kFloorPad) / kRangeDb;
            const int i = std::clamp(int(t * 255.0f), 0, 255);
            reinterpret_cast<QRgb*>(m_wfImg.scanLine(y))[x] = m_lut[i];
        }
    }

    void rebuildImage()
    {
        m_wfDirty = false;
        const int w = std::max(1, width()), h = std::max(1, height());
        m_wfImg = QImage(w, h, QImage::Format_RGB32);
        m_wfImg.fill(m_lut[0]);
        if (!m_stft) return;
        m_yBins.resize(size_t(h));
        for (int y = 0; y < h; ++y) {
            const double hzHi = m_viewHi - y * (m_viewHi - m_viewLo) / h;
            const double hzLo = m_viewHi - (y + 1) * (m_viewHi - m_viewLo) / h;
            int b0 = int(std::ceil(binOfHz(hzLo)));
            int b1 = int(std::ceil(binOfHz(hzHi)));
            b0 = std::clamp(b0, 0, SkimStft::kFft);
            b1 = std::clamp(b1, 0, SkimStft::kFft);
            m_yBins[size_t(y)] = {b0, b1};
        }
        m_accumN = 0;
        const size_t groups = m_raw.size() / size_t(m_colsPerPx);
        const size_t useGroups = std::min(groups, size_t(w));
        size_t idx = m_raw.size() - useGroups * size_t(m_colsPerPx);
        int x = w - int(useGroups);
        std::vector<uint8_t> merged;
        for (size_t g = 0; g < useGroups; ++g, ++x) {
            merged = m_raw[idx++];
            for (int k = 1; k < m_colsPerPx; ++k, ++idx)
                for (size_t i = 0; i < merged.size(); ++i) merged[i] = std::max(merged[i], m_raw[idx][i]);
            renderPixelColumn(x, merged.data());
        }
    }

    // ---- overlays (AetherSDR's theme colours) ----
    QColor textColour() const { return palette().color(QPalette::WindowText); }

    void drawScale(QPainter& p)
    {
        if (!m_stft || m_viewHi <= m_viewLo) return;
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);
        const double step = niceStep(m_viewHi - m_viewLo);
        for (double hz = std::ceil(m_viewLo / step) * step; hz <= m_viewHi; hz += step) {
            const int y = int(yOfHz(hz));
            QColor grid = textColour();
            grid.setAlpha(30);
            p.setPen(grid);
            p.drawLine(50, y, width(), y);
            p.setPen(textColour());
            p.drawText(QRect(0, y - 8, 46, 16), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(hz / 1000.0, 'f', step >= 1000.0 ? 0 : 1));
        }
    }

    void drawDial(QPainter& p)
    {
        if (m_dial <= 0 || !m_stft) return;
        const int y = int(yOfHz(double(m_dial)));
        if (y < -2 || y > height() + 2) return;
        const QColor accent = palette().color(QPalette::Highlight);
        p.setPen(QPen(accent, 1, Qt::DashLine));
        p.drawLine(0, y, width(), y);
        p.setPen(accent);
        p.drawText(QRect(width() - 130, y - 16, 124, 14), Qt::AlignRight | Qt::AlignBottom,
                   tr("VFO A"));
    }

    void drawLabels(QPainter& p)
    {
        m_labelHits.clear();
        if (!m_engine || !m_stft) return;
        struct Row {
            double y;
            QString head;
            QString tail;
            QColor colour;
            qint64 hz;
        };
        std::vector<Row> rows;
        QSet<QString> live;
        const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
        // Live channels print their copy as it happens (CW Skimmer style);
        // the confirmed call rides in front in its worked-before colour.
        for (const auto& c : m_engine->channelInfo()) {
            if (!c.active || c.hz < m_viewLo || c.hz > m_viewHi) continue;
            Row r{yOfHz(double(c.hz)), c.call.isEmpty() ? QStringLiteral("?") : c.call,
                  c.text.right(18), c.call.isEmpty() ? dim : callColour(c.call, c.hz), c.hz};
            if (!c.call.isEmpty()) live.insert(c.call);
            rows.push_back(std::move(r));
        }
        // Spots whose channel moved on (kept 10 min): call and WPM.
        for (const auto& s : m_engine->spots()) {
            if (live.contains(s.call) || s.hz < m_viewLo || s.hz > m_viewHi) continue;
            QString head = s.call;
            if (s.wpm > 0) head += QStringLiteral("  %1").arg(s.wpm);
            rows.push_back({yOfHz(double(s.hz)), head, QString(), callColour(s.call, s.hz), s.hz});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.y < b.y; });
        QFont fb = font();
        fb.setBold(true);
        const QFont fr = font();
        const QFontMetrics fmB(fb), fmR(fr);
        QColor boxBg = palette().color(QPalette::Window);
        boxBg.setAlpha(200);
        double nextFree = -1e9;
        for (const auto& r : rows) {
            const double y = std::max(r.y, nextFree);
            nextFree = y + fmB.height() + 1;
            if (y > height() + 8) break;
            const int hw = fmB.horizontalAdvance(r.head);
            const int tw = r.tail.isEmpty() ? 0 : fmR.horizontalAdvance(r.tail) + 6;
            const int bh = fmB.height();
            const QRect box(width() - hw - tw - 26, int(y) - bh / 2, hw + tw + 8, bh);
            p.fillRect(box, boxBg);
            p.setPen(QPen(r.colour.darker(140), 1));
            p.drawLine(box.right() + 2, box.center().y(), width() - 4, int(r.y));
            p.setFont(fb);
            p.setPen(r.colour);
            p.drawText(QRect(box.left() + 4, box.top(), hw, bh), Qt::AlignLeft | Qt::AlignVCenter,
                       r.head);
            if (tw > 0) {
                p.setFont(fr);
                p.setPen(textColour());
                p.drawText(QRect(box.left() + 4 + hw + 6, box.top(), tw - 6, bh),
                           Qt::AlignLeft | Qt::AlignVCenter, r.tail);
            }
            m_labelHits.push_back({box, r.hz});
        }
    }

    void drawHover(QPainter& p)
    {
        if (!underMouse() || m_hover.y() < 0 || !m_stft) return;
        QColor line = textColour();
        line.setAlpha(70);
        p.setPen(line);
        p.drawLine(0, m_hover.y(), width(), m_hover.y());
        p.setPen(textColour());
        p.drawText(QPoint(54, m_hover.y() - 4),
                   tr("%1 kHz").arg(hzOfY(m_hover.y()) / 1000.0, 0, 'f', 2));
    }

    void drawInfo(QPainter& p)
    {
        p.setPen(textColour());
        QString t;
        if (!m_stft) {
            t = tr("The skimmer is not running: it needs the Orion's RSP2 panadapter "
                   "(switch it on in Settings -> Radio Setup -> Orion).");
        } else if (!m_engine || !currentSkimmer(m_model) || !currentSkimmer(m_model)->isActive()) {
            t = tr("decoding is off or paused (outside CW, transmitting): no calls are read");
        } else if (m_raw.empty()) {
            t = tr("waiting for IQ...");
        } else {
            t = tr("%1 - %2 kHz, %3 Hz/bin, %4 ms/px")
                    .arg(m_viewLo / 1000.0, 0, 'f', 1)
                    .arg(m_viewHi / 1000.0, 0, 'f', 1)
                    .arg(m_stft->binHz(), 0, 'f', 0)
                    .arg(m_stft->colSecs() * 1000.0 * m_colsPerPx, 0, 'f', 0);
        }
        p.drawText(QRect(54, 2, width() - 160, 18), Qt::AlignLeft | Qt::AlignVCenter, t);
        // Scroll speed (x1 fastest: a dit at 25 WPM is ~6 px).
        m_speedHits.clear();
        static const int kSpeeds[] = {1, 2, 4};
        int x = width() - 4;
        for (int i = 2; i >= 0; --i) {
            const int w = 28;
            x -= w + 4;
            const QRect r(x, 3, w, 16);
            const bool on = m_colsPerPx == kSpeeds[i];
            p.fillRect(r, on ? palette().color(QPalette::Highlight) : palette().color(QPalette::Button));
            p.setPen(on ? palette().color(QPalette::HighlightedText) : palette().color(QPalette::ButtonText));
            p.drawText(r, Qt::AlignCenter, QStringLiteral("x%1").arg(kSpeeds[i]));
            m_speedHits.push_back({r, kSpeeds[i]});
        }
    }

    RadioModel* m_model;
    QPointer<QMainWindow> m_mainWindow;
    std::function<void(qint64)> m_onTune;
    SkimStft* m_stft = nullptr;
    SkimmerEngine* m_engine = nullptr;
    QTimer* m_drain = nullptr;
    QTimer* m_tune = nullptr;
    qint64 m_dial = 0, m_loAbs = 0, m_sliceCenterAbs = 0;
    qint64 m_segLo = 0, m_segHi = 0;
    double m_sliceOff = 1e18;
    uint32_t m_curGen = ~0u;
    double m_viewLo = 0.0, m_viewHi = 1.0;
    std::deque<std::vector<uint8_t>> m_raw;
    std::vector<uint8_t> m_accum;
    int m_accumN = 0;
    int m_colsPerPx = 1;
    std::vector<std::pair<QRect, int>> m_speedHits;
    QImage m_wfImg;
    bool m_wfDirty = true;
    std::vector<std::pair<int, int>> m_yBins;
    float m_floorDb = -999.0f;
    int m_floorTick = 0;
    QRgb m_lut[256] = {};
    QPoint m_hover{-1, -1};
    QPoint m_pressPos;
    double m_pressView0 = 0.0;
    bool m_dragging = false;
    std::vector<LabelHit> m_labelHits;
};

class SkimViewWindow : public QDialog {
public:
    SkimViewWindow(RadioModel* model, QMainWindow* mainWindow,
                   std::function<SliceModel*()> activeSlice, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Skimmer waterfall"));
        resize(920, 640);
        setMinimumSize(480, 320);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* top = new QHBoxLayout;
        top->setContentsMargins(8, 4, 8, 0);
        top->addWidget(decodeSwitch(model, this));
        top->addStretch(1);
        lay->addLayout(top);
        m_canvas = new SkimCanvas(model, mainWindow,
                                  [activeSlice](qint64 hz) { tuneSlice(activeSlice, hz); }, this);
        lay->addWidget(m_canvas, 1);
    }

protected:
    void showEvent(QShowEvent* e) override
    {
        m_canvas->start();
        QDialog::showEvent(e);
    }
    void hideEvent(QHideEvent* e) override
    {
        m_canvas->stop();
        QDialog::hideEvent(e);
    }

private:
    SkimCanvas* m_canvas = nullptr;
};

}  // namespace

void installOrionSkimmerWindows(QMainWindow* window, QMenu* menu, RadioModel* model,
                                std::function<SliceModel*()> activeSlice)
{
    menu->addSeparator();
    auto* mapAct = menu->addAction(QObject::tr("Skimmer band map..."));
    mapAct->setToolTip(QObject::tr("The CW skimmer's finds as a frequency-sorted list; click to tune"));
    auto* viewAct = menu->addAction(QObject::tr("Skimmer waterfall..."));
    viewAct->setToolTip(QObject::tr("The CW segment at keying resolution, with the decoded calls "
                                    "beside their traces; click to tune, wheel to zoom, drag to pan"));
    auto map = std::make_shared<QPointer<BandMapWindow>>();
    QObject::connect(mapAct, &QAction::triggered, window, [=] {
        if (!*map) *map = new BandMapWindow(model, activeSlice, window);
        (*map)->show();
        (*map)->raise();
        (*map)->activateWindow();
    });
    auto view = std::make_shared<QPointer<SkimViewWindow>>();
    QObject::connect(viewAct, &QAction::triggered, window, [=] {
        if (!*view) *view = new SkimViewWindow(model, window, activeSlice, window);
        (*view)->show();
        (*view)->raise();
        (*view)->activateWindow();
    });
}

}  // namespace AetherSDR
