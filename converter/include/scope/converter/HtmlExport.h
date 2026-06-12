#pragma once

#include <QString>

namespace scope::core { class SignalStore; }

namespace scope::converter {

// Write a fully self-contained interactive HTML chart of every channel in
// the store: a Time chart (seconds, same origin as the app's time axis)
// and — if frequency-domain channels exist — a Frequency chart (Hz).
// The vendored uPlot library (MIT, ~50 KB) is embedded, so the file works
// offline in any browser: scroll to zoom, drag to box-zoom, double-click
// to reset, click legend entries to show/hide channels.
//
// Channels with different sample grids are merged onto the union grid
// with gaps (null) where a channel has no sample; values are written with
// full double precision.
bool exportInteractiveHtml(const QString& path,
                           const scope::core::SignalStore& store,
                           QString* errorOut = nullptr);

}  // namespace scope::converter
