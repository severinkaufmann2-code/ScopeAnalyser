#pragma once

#include "CsvWriter.h"

#include "scope/core/Signal.h"

#include <QString>

#include <filesystem>
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
};

struct LoadResult {
    // Named "channels" not "signals" because Qt's moc treats the lowercase
    // identifier `signals` as a keyword (alias for `public:`), so member
    // access like `result.signals` is a syntax error in Qt translation units.
    std::vector<std::shared_ptr<scope::core::Signal>> channels;
    QString error;
    bool ok{false};
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

}  // namespace scope::converter
