#pragma once

#include <QList>
#include <QString>

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

}  // namespace scope::converter
