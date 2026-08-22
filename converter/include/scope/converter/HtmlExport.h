#pragma once

#include "scope/core/Signal.h"

#include <QList>
#include <QString>

#include <memory>
#include <vector>

namespace scope::core { class SignalStore; }

namespace scope::converter {

// Describes how the exported page should look, mirroring the Analyser:
// Y axes (label / side / colour) and per-channel axis assignment,
// visibility, display order, and trace colour.
struct HtmlAxis {
    QString label;
    bool    right{false};
    QString color;            // "#rrggbb"
};

struct HtmlChannel {
    QString name;             // store channel name
    int     axisIndex{0};
    bool    visible{true};
    QString color;            // "#rrggbb"
};

struct HtmlChartView {
    QList<HtmlAxis>    axes;       // index 0 = Y1; empty → single default axis
    QList<HtmlChannel> channels;   // display order; empty → all store channels
};

struct HtmlExportView {
    HtmlChartView time;
    HtmlChartView frequency;
    // The view the page opens in: "time", "frequency" or "xy" (with
    // xyChannel naming the X channel). Falls back to the first domain
    // that has channels.
    QString initialView{"time"};
    QString xyChannel;

    // Compact PlotLayout JSON to embed in a *storable* export so re-opening
    // restores Y axes / view mode / channel assignments — exactly like the
    // HDF5 / MDF4 / CSV save paths. Consumed only by exportStorableHtml();
    // the visualisation-only exportInteractiveHtml() ignores it. Empty →
    // nothing embedded (e.g. the Converter, which has no plot).
    QString layoutJson;
};

// Write a fully self-contained interactive HTML chart that mirrors the
// Analyser's layout: a View selector (Time / Frequency / XY with an
// X-channel picker), a channel panel on the left (checkbox + colour swatch
// + live value under the cursor), a toolbar with Fit / Zoom button groups,
// a Δ Measure tool and the Line / Points / Line+points selector, and the
// same multi-Y-axis arrangement (labels, left/right sides, colours).
// The XY view pairs channels exactly like the app: index-paired on shared
// timestamps, otherwise Y linearly interpolated onto X's sample times,
// with gaps (never fabricated points) outside Y's recorded range.
//
// The vendored uPlot library (MIT, ~50 KB) is embedded, so the file works
// offline in any browser. Channels with different sample grids are merged
// onto the union grid with gaps (null) where a channel has no sample;
// values are written with full (shortest round-trip) double precision.
//
// The store-only overload derives a default view: one Y axis per domain,
// every channel visible, palette colours in store order.
bool exportInteractiveHtml(const QString& path,
                           const scope::core::SignalStore& store,
                           QString* errorOut = nullptr);
bool exportInteractiveHtml(const QString& path,
                           const scope::core::SignalStore& store,
                           const HtmlExportView& view,
                           QString* errorOut = nullptr);

// ---------- Storable HTML (data island + re-import) --------------------
//
// Like exportInteractiveHtml(), but writes the channel data ONCE, as a
// single canonical JSON island (<script type="application/json"
// id="scope-data">). The page renders from that island (no second copy of
// the data) and ScopeAnalyser can re-open the file: HTML becomes a peer of
// .h5 / .mf4 / .csv — both saveable and openable. Timestamps are stored as
// exact int64-ns strings, units / domains are explicit, and an optional
// PlotLayout (view.layoutJson) is embedded for axis/view restore.
//
// The original exportInteractiveHtml() above is kept untouched as the
// visualisation-only "Export HTML…" path; its output has no data island and
// is NOT re-loadable. Note it is not the *small* option — its data is
// plaintext, so past a few thousand samples it is markedly LARGER than the
// compressed island below.
bool exportStorableHtml(const QString& path,
                        const scope::core::SignalStore& store,
                        QString* errorOut = nullptr);
bool exportStorableHtml(const QString& path,
                        const scope::core::SignalStore& store,
                        const HtmlExportView& view,
                        QString* errorOut = nullptr);

// ---------- Chart-only HTML (smaller, one-way) -------------------------
//
// The same compressed page as exportStorableHtml(), written for looking at
// rather than for keeping: sample values are rounded to `digits` significant
// figures and the re-import marker is left out, so loadStorableHtml()
// refuses the file.
//
// Rounding is where the size actually goes. Full-precision doubles from real
// (noisy) measurements are near-incompressible; at 6 significant figures —
// far more than a chart can resolve — a 200k-sample channel lands at roughly
// 60% of the re-openable file. Dropping the island alone saves only a few
// percent, which is why this mode trades precision instead.
//
// Because the values written are display-quality, this is NOT a backup of
// the data. Use exportStorableHtml() (or .h5 / .mf4 / .json) to keep it.
constexpr int kChartOnlyDefaultDigits = 6;

bool exportChartOnlyHtml(const QString& path,
                         const scope::core::SignalStore& store,
                         int digits = kChartOnlyDefaultDigits,
                         QString* errorOut = nullptr);
bool exportChartOnlyHtml(const QString& path,
                         const scope::core::SignalStore& store,
                         const HtmlExportView& view,
                         int digits = kChartOnlyDefaultDigits,
                         QString* errorOut = nullptr);

// Re-import a storable HTML's embedded "scope-data" island. Rebuilds one
// Signal per channel (name / unit / domain / values / timestamps restored)
// and, when present, returns the embedded PlotLayout JSON via layoutJsonOut.
// Returns false (with *errorOut set) when the file carries no island — e.g.
// an old visualisation-only export, or any non-scope HTML.
bool loadStorableHtml(const QString& path,
                      std::vector<std::shared_ptr<scope::core::Signal>>* channelsOut,
                      QString* layoutJsonOut = nullptr,
                      QString* errorOut = nullptr);

}  // namespace scope::converter
