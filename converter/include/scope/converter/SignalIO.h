#pragma once

#include "CsvWriter.h"

#include "scope/core/Signal.h"
#include "scope/core/SignalStore.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace scope::converter {

// Disk file formats supported by the shared open/save shim. Used by both
// the Analyser ("Open chart" / "Save chart") and the Converter ("Open .h5"
// / "Save .h5" / "Save .csv"). Centralises the dispatch so each tool no
// longer has its own hand-rolled HDF5/MDF4/CSV ladder.
enum class FileFormat {
    Auto,    // pick from extension
    Csv,
    Hdf5,
    Mdf4,
    Svdx,    // TwinCAT Scope View export (read-only)
};

// Pick a format from a path's extension. ".h5"/".hdf5" → Hdf5, ".mf4" →
// Mdf4, ".csv"/".txt" → Csv. Returns Auto if nothing matches.
FileFormat detectFormatFromExtension(const std::filesystem::path& path);

// File-dialog name filters and the default suffix for a given format.
// Centralises the strings so the Analyser and Converter Save dialogs stay
// in sync.
QStringList nameFilters(FileFormat fmt);
QString     defaultSuffix(FileFormat fmt);

// "Save chart" / "Save .csv" options. Only the field for the chosen
// format is consulted; other fields are ignored.
struct SaveOptions {
    CsvExportOptions csv{};

    // Format-agnostic PlotLayout JSON to embed in the saved file. Empty →
    // nothing embedded (foreign / non-Analyser saves are unaffected). For
    // HDF5 / MDF4 it's written to a file-level metadata slot; for CSV it's
    // spliced into the scope-csv header (saveFile bridges it into
    // CsvExportOptions::layoutJson). Stored as a string to keep nlohmann
    // and PlotLayout out of this public header.
    QString layoutJson;

    // Per-domain restricted layouts, consulted ONLY when a split export
    // writes a single-domain file: key "time" / "frequency" picks which
    // JSON that file carries instead of the full `layoutJson`. Absent key
    // → falls back to `layoutJson`. Lets each split file embed only the
    // axes / channels for the domain it actually contains.
    QHash<QString, QString> layoutJsonByDomain;
};

struct LoadResult {
    // Named "channels" not "signals" because Qt's moc treats the lowercase
    // identifier `signals` as a keyword (alias for `public:`), so member
    // access like `result.signals` is a syntax error in Qt translation units.
    std::vector<std::shared_ptr<scope::core::Signal>> channels;
    QString error;
    bool ok{false};

    // Compact PlotLayout JSON pulled from a CSV's "# scope-csv: ..."
    // metadata block when its `layout` key is present. Empty otherwise
    // (non-CSV sources, foreign CSVs without the key). The host (e.g.
    // AnalyserPlot::openChartDialog) feeds this into
    // PlotLayout::fromJsonString to restore axes / view mode / channel
    // assignments.
    QString layoutJson;
};

// Load all signals from a single file. CSV import here handles the
// "Save chart"-style layout (single shared time column with "<name> [unit]"
// headers, or alternating "t_<name>" / "<name>" pairs). The Converter's
// profile-driven CSV import in CsvSource is unaffected — that one stays
// in place because it needs the row-skip / header-row / column-mapping
// machinery a generic chart loader can't supply.
LoadResult loadFile(const std::filesystem::path& path,
                    FileFormat fmt = FileFormat::Auto);

// Write a set of channels to a file. For HDF5 / MDF4 the SaveOptions field
// is ignored; for CSV opts.csv controls header / separator / time mode.
bool saveFile(const std::filesystem::path& path,
              const std::vector<std::shared_ptr<scope::core::Signal>>& channels,
              FileFormat fmt = FileFormat::Auto,
              const SaveOptions& opts = {},
              QString* errorOut = nullptr);

// ---------- Save-chart-from-store helper -------------------------------
//
// What the Analyser's `Save chart…` and the Converter's `Save chart…`
// share: pull channels from a SignalStore, apply the standard filters
// (per-domain include, derived-channel toggle, time range), trim, split
// into separate files per domain if requested, and dispatch to
// saveFile(). Centralising it keeps both tools in lockstep.

struct ChartSaveFilters {
    bool   includeTime{true};
    bool   includeFrequency{true};
    bool   includeDerived{true};
    // When true, write <base>_time.<ext> and <base>_frequency.<ext> as
    // separate files. Only honoured when both domains have data after
    // filtering — a single-domain export goes to <base> regardless.
    bool   splitDomainsIntoTwoFiles{false};
    // Inclusive time range filter applied to time-domain signals only.
    // Frequency signals' "timestamps" carry Hz so seconds bounds don't
    // make sense for them. The default sentinels mean "no trim".
    bool   useCustomRange{false};
    double fromSec{-1e18};
    double toSec  { 1e18};
};

enum class ChartSaveResult {
    Ok,
    NothingMatchedFilters,
    Failed,
};

// `basePath` is the user-chosen target. On a split write the helper
// inserts `_time` / `_frequency` before the extension.
// On Ok: `messagesOut` holds one human-readable line per file written.
// On Failed: `errorOut` carries the message.
ChartSaveResult saveChartFromStore(
    const QString& basePath,
    const scope::core::SignalStore& store,
    FileFormat fmt,
    const ChartSaveFilters& filters,
    const SaveOptions& opts,
    QStringList* messagesOut = nullptr,
    QString* errorOut = nullptr);

}  // namespace scope::converter
