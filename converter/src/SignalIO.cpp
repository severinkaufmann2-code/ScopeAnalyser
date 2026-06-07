#include "scope/converter/SignalIO.h"

#include "scope/core/Hdf5Session.h"
#include "scope/core/Mdf4Session.h"

#include <nlohmann/json.hpp>

#include <QFile>
#include <QStringList>
#include <QTextStream>

#include <array>

namespace scope::converter {

using scope::core::Signal;
using scope::core::TimestampNs;

namespace {

QString lowerExt(const std::filesystem::path& path) {
    return QString::fromStdString(path.extension().string()).toLower();
}

QChar autoDetectDelim(const QString& headerLine) {
    const std::array<QChar, 4> candidates{QChar(','), QChar(';'),
                                          QChar('\t'), QChar('|')};
    QChar best = ',';
    int bestCount = -1;
    for (QChar c : candidates) {
        const int n = headerLine.count(c);
        if (n > bestCount) { bestCount = n; best = c; }
    }
    return best;
}

void splitNameUnit(const QString& header, QString* name, QString* unit) {
    QString h = header.trimmed();
    const int open  = h.lastIndexOf('[');
    const int close = h.lastIndexOf(']');
    if (open > 0 && close > open && close == h.size() - 1) {
        *name = h.left(open).trimmed();
        *unit = h.mid(open + 1, close - open - 1).trimmed();
    } else {
        *name = h;
        *unit = QString();
    }
}

bool loadCsvChart(const std::filesystem::path& path,
                  std::vector<std::shared_ptr<Signal>>* out,
                  QString* errorOut) {
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = "Couldn't open file.";
        return false;
    }
    QTextStream ts(&f);

    // ---- Optional scope-csv metadata block --------------------------
    // A line like  "# scope-csv: {...json...}"  at the top of the file
    // is authoritative — column roles, units, names and refX come from
    // it and the header-string heuristics below are skipped.
    QString metaJson;
    QString headerLine;
    while (true) {
        headerLine = ts.readLine();
        if (headerLine.isNull()) break;
        const QString trimmed = headerLine.trimmed();
        if (trimmed.isEmpty()) continue;
        if (trimmed.startsWith("# scope-csv:")) {
            metaJson = trimmed.mid(QString("# scope-csv:").size()).trimmed();
            continue;
        }
        break;   // first non-blank, non-metadata line is the column header
    }
    if (headerLine.isNull()) {
        if (errorOut) *errorOut = "Empty file.";
        return false;
    }

    const QChar sep = autoDetectDelim(headerLine);
    const QStringList headers = headerLine.split(sep);
    if (headers.size() < 2) {
        if (errorOut) *errorOut = "Need at least one X column and one value column.";
        return false;
    }

    struct Target { QString name, unit; int tCol{-1}, vCol{-1};
                    Signal::Domain domain{Signal::Domain::Time}; };
    std::vector<Target> targets;

    if (!metaJson.isEmpty()) {
        // ---- Metadata-driven mapping (authoritative) ----------------
        try {
            const auto root = nlohmann::json::parse(metaJson.toStdString());
            const auto& arr = root.value("columns", nlohmann::json::array());
            std::vector<std::string> roles;
            std::vector<std::string> units;
            std::vector<std::string> names;
            std::vector<int>         refX;
            roles.reserve(arr.size());
            units.reserve(arr.size());
            names.reserve(arr.size());
            refX.reserve(arr.size());
            for (const auto& jc : arr) {
                roles.push_back(jc.value("role", std::string("signal")));
                units.push_back(jc.value("unit", std::string{}));
                names.push_back(jc.value("name", std::string{}));
                refX.push_back(jc.value("refX", -1));
            }
            for (std::size_t i = 0; i < roles.size(); ++i) {
                if (roles[i] != "signal") continue;
                Target t;
                t.name = QString::fromStdString(names[i]);
                t.unit = QString::fromStdString(units[i]);
                // Fall back to the column header's name if metadata
                // didn't carry one (e.g. third-party file).
                if (t.name.isEmpty() && i < static_cast<std::size_t>(headers.size())) {
                    QString hName, hUnit;
                    splitNameUnit(headers[i], &hName, &hUnit);
                    if (t.name.isEmpty()) t.name = hName;
                    if (t.unit.isEmpty()) t.unit = hUnit;
                }
                t.vCol = static_cast<int>(i);
                t.tCol = refX[i];
                if (t.tCol >= 0 && t.tCol < static_cast<int>(roles.size())) {
                    t.domain = (roles[t.tCol] == "x_frequency")
                        ? Signal::Domain::Frequency
                        : Signal::Domain::Time;
                }
                if (!t.name.isEmpty() && t.tCol >= 0) targets.push_back(std::move(t));
            }
        } catch (...) {
            // Malformed metadata → silently fall through to heuristics.
            targets.clear();
        }
    }

    if (targets.empty()) {
        // ---- Heuristic mapping (legacy / third-party files) ----------
        auto isSharedX = [](const QString& h){
            const QString t = h.trimmed();
            return t == "t" || t == "f"
                || t.startsWith("t ") || t.startsWith("f ")
                || t.startsWith("t[") || t.startsWith("f[");
        };
        const bool shared = isSharedX(headers[0])
                         && (headers.size() < 3
                             || (!headers[2].trimmed().startsWith("t_")
                              && !headers[2].trimmed().startsWith("f_")));

        auto isFreqHeader = [](const QString& h){
            const QString t = h.trimmed();
            return t == "f" || t.startsWith("f ") || t.startsWith("f[")
                || t.startsWith("f_");
        };

        if (shared) {
            // Walk left→right, remember the most-recent X column. Each
            // Y inherits its domain from that X. This correctly handles
            // mixed files written before metadata existed where the
            // layout is "t [s], time_signals…, f [Hz], freq_signals…".
            int curX = -1;
            Signal::Domain curD = Signal::Domain::Time;
            for (int i = 0; i < headers.size(); ++i) {
                if (isSharedX(headers[i])) {
                    curX = i;
                    curD = isFreqHeader(headers[i]) ? Signal::Domain::Frequency
                                                    : Signal::Domain::Time;
                    continue;
                }
                if (curX < 0) continue;   // Y before any X column
                Target t;
                splitNameUnit(headers[i], &t.name, &t.unit);
                t.tCol   = curX;
                t.vCol   = i;
                t.domain = curD;
                if (!t.name.isEmpty()) targets.push_back(std::move(t));
            }
        } else {
            for (int i = 0; i + 1 < headers.size(); i += 2) {
                Target t;
                splitNameUnit(headers[i + 1], &t.name, &t.unit);
                t.tCol = i;
                t.vCol = i + 1;
                t.domain = isFreqHeader(headers[i])
                    ? Signal::Domain::Frequency : Signal::Domain::Time;
                if (!t.name.isEmpty()) targets.push_back(std::move(t));
            }
        }
    }

    if (targets.empty()) {
        if (errorOut) *errorOut = "No data columns found.";
        return false;
    }

    std::vector<std::vector<TimestampNs>> tsBufs(targets.size());
    std::vector<std::vector<double>>      vsBufs(targets.size());

    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.trimmed().isEmpty()) continue;
        const QStringList parts = line.split(sep);
        for (std::size_t k = 0; k < targets.size(); ++k) {
            const Target& tg = targets[k];
            if (tg.tCol >= parts.size() || tg.vCol >= parts.size()) continue;
            QString tStr = parts[tg.tCol].trimmed();
            QString vStr = parts[tg.vCol].trimmed();
            if (tStr.isEmpty() || vStr.isEmpty()) continue;
            tStr.replace(',', '.');
            vStr.replace(',', '.');
            bool okT = false, okV = false;
            const double xSec = tStr.toDouble(&okT);
            const double v    = vStr.toDouble(&okV);
            if (!okT || !okV) continue;
            tsBufs[k].push_back(static_cast<TimestampNs>(xSec * 1e9));
            vsBufs[k].push_back(v);
        }
    }

    for (std::size_t k = 0; k < targets.size(); ++k) {
        if (tsBufs[k].empty()) continue;
        Signal::Meta m;
        m.name = targets[k].name;
        m.unit = targets[k].unit;
        m.dataType = scope::core::DataType::Float64;
        m.domain = targets[k].domain;
        m.sourceSymbol = QString::fromStdString(path.filename().string());
        auto sig = std::make_shared<Signal>(m);
        sig->append(tsBufs[k].data(),
                    reinterpret_cast<const std::byte*>(vsBufs[k].data()),
                    tsBufs[k].size());
        out->push_back(std::move(sig));
    }
    return true;
}

bool writeHdf5(const std::filesystem::path& path,
               const std::vector<std::shared_ptr<Signal>>& chans,
               QString* errorOut) {
    QString err;
    auto session = scope::core::Hdf5Session::create(path, &err);
    if (!session) {
        if (errorOut) *errorOut = err.isEmpty() ? "Couldn't create file." : err;
        return false;
    }
    // Two passes — same shape as the MDF4 path. HDF5 itself doesn't care
    // about ordering here, but keeping the loops symmetrical makes the
    // dispatch table easier to reason about.
    for (const auto& s : chans) {
        if (!s) continue;
        session->addChannel(s->meta(), &err);
    }
    for (const auto& s : chans) {
        if (!s) continue;
        auto v = s->snapshotForRead();
        if (v.count > 0) {
            session->appendSamples(s->meta().name,
                                   v.timestamps, v.values, v.count, &err);
        }
    }
    session->flush();
    return true;
}

bool writeMdf4(const std::filesystem::path& path,
               const std::vector<std::shared_ptr<Signal>>& chans,
               QString* errorOut) {
    QString err;
    auto session = scope::core::Mdf4Session::create(path, &err);
    if (!session) {
        if (errorOut) *errorOut = err.isEmpty() ? "Couldn't create file." : err;
        return false;
    }
    // mdflib won't accept new DataGroups after InitMeasurement has run,
    // and Mdf4Session lazily runs it on the first appendSamples. So we have
    // to register ALL channels before writing samples for any of them —
    // interleaving silently drops every channel past the first.
    for (const auto& s : chans) {
        if (!s) continue;
        session->addChannel(s->meta(), &err);
    }
    for (const auto& s : chans) {
        if (!s) continue;
        auto v = s->snapshotForRead();
        if (v.count > 0) {
            session->appendSamples(s->meta().name,
                                   v.timestamps, v.values, v.count, &err);
        }
    }
    return session->finalize(errorOut);
}

}  // namespace

FileFormat detectFormatFromExtension(const std::filesystem::path& path) {
    const QString e = lowerExt(path);
    if (e == ".h5" || e == ".hdf5") return FileFormat::Hdf5;
    if (e == ".mf4")                return FileFormat::Mdf4;
    if (e == ".csv" || e == ".txt") return FileFormat::Csv;
    return FileFormat::Auto;
}

QStringList nameFilters(FileFormat fmt) {
    switch (fmt) {
        case FileFormat::Csv:  return {"CSV (*.csv)", "Text (*.txt)", "All files (*)"};
        case FileFormat::Hdf5: return {"HDF5 (*.h5)", "All files (*)"};
        case FileFormat::Mdf4: return {"MDF4 (*.mf4)", "All files (*)"};
        case FileFormat::Auto:
        default:
            return {"Scope chart (*.h5 *.mf4 *.csv *.txt)",
                    "HDF5 (*.h5)",
                    "MDF4 (*.mf4)",
                    "CSV / text (*.csv *.txt)",
                    "All files (*)"};
    }
}

QString defaultSuffix(FileFormat fmt) {
    switch (fmt) {
        case FileFormat::Csv:  return "csv";
        case FileFormat::Hdf5: return "h5";
        case FileFormat::Mdf4: return "mf4";
        case FileFormat::Auto:
        default:               return "h5";
    }
}

LoadResult loadFile(const std::filesystem::path& path, FileFormat fmt) {
    LoadResult r;
    if (fmt == FileFormat::Auto) fmt = detectFormatFromExtension(path);
    QString err;
    switch (fmt) {
        case FileFormat::Hdf5: {
            auto s = scope::core::Hdf5Session::openForRead(path, &err);
            if (!s) { r.error = err.isEmpty() ? "Couldn't open file." : err; return r; }
            r.channels = s->loadAllSignals(&err);
            r.error = err;
            r.ok = !r.channels.empty() || err.isEmpty();
            return r;
        }
        case FileFormat::Mdf4: {
            auto s = scope::core::Mdf4Session::openForRead(path, &err);
            if (!s) { r.error = err.isEmpty() ? "Couldn't open file." : err; return r; }
            r.channels = s->loadAllSignals(&err);
            r.error = err;
            r.ok = !r.channels.empty() || err.isEmpty();
            return r;
        }
        case FileFormat::Csv:
            r.ok = loadCsvChart(path, &r.channels, &r.error);
            return r;
        case FileFormat::Auto:
        default:
            r.error = "Unsupported file format (couldn't detect from extension).";
            return r;
    }
}

bool saveFile(const std::filesystem::path& path,
              const std::vector<std::shared_ptr<Signal>>& channels,
              FileFormat fmt,
              const SaveOptions& opts,
              QString* errorOut) {
    if (fmt == FileFormat::Auto) fmt = detectFormatFromExtension(path);
    switch (fmt) {
        case FileFormat::Hdf5: return writeHdf5(path, channels, errorOut);
        case FileFormat::Mdf4: return writeMdf4(path, channels, errorOut);
        case FileFormat::Csv:  return writeCsv (path, channels, opts.csv, errorOut);
        case FileFormat::Auto:
        default:
            if (errorOut) *errorOut = "Unsupported file format (couldn't detect from extension).";
            return false;
    }
}

namespace {

bool isDerivedChannel(const std::shared_ptr<Signal>& s) {
    if (!s) return false;
    const QString src = s->meta().sourceSymbol;
    const int eq = src.indexOf('=');
    if (eq <= 0) return false;
    return src.left(eq).trimmed() == s->meta().name;
}

QString withSuffixBeforeExt(const QString& p, const QString& suffix) {
    const int dot = p.lastIndexOf('.');
    return (dot > p.lastIndexOf('/') && dot > p.lastIndexOf('\\'))
        ? p.left(dot) + suffix + p.mid(dot)
        : p + suffix;
}

}  // namespace

ChartSaveResult saveChartFromStore(
    const QString& basePath,
    const scope::core::SignalStore& store,
    FileFormat fmt,
    const ChartSaveFilters& filters,
    const SaveOptions& opts,
    QStringList* messagesOut,
    QString* errorOut) {

    using TimestampNs = scope::core::TimestampNs;
    const TimestampNs fromNs = filters.useCustomRange
        ? static_cast<TimestampNs>(filters.fromSec * 1e9)
        : std::numeric_limits<TimestampNs>::min();
    const TimestampNs toNs = filters.useCustomRange
        ? static_cast<TimestampNs>(filters.toSec   * 1e9)
        : std::numeric_limits<TimestampNs>::max();

    std::vector<std::shared_ptr<Signal>> timeChans, freqChans;
    for (const auto& n : store.channelNames()) {
        auto src = store.get(n);
        if (!src) continue;
        const bool freq = (src->meta().domain == Signal::Domain::Frequency);
        if (freq && !filters.includeFrequency) continue;
        if (!freq && !filters.includeTime)      continue;
        if (!filters.includeDerived && isDerivedChannel(src)) continue;
        auto view = src->snapshotForRead();
        auto vs   = src->readAsDouble();
        if (view.count == 0) continue;

        std::vector<TimestampNs> tsBuf;
        std::vector<double>      vsBuf;
        tsBuf.reserve(view.count);
        vsBuf.reserve(view.count);
        for (std::size_t i = 0; i < view.count; ++i) {
            const TimestampNs t = view.timestamps[i];
            if (!freq && (t < fromNs || t > toNs)) continue;
            tsBuf.push_back(t);
            vsBuf.push_back(i < vs.size() ? vs[i] : 0.0);
        }
        if (tsBuf.empty()) continue;
        Signal::Meta meta = src->meta();
        meta.dataType = scope::core::DataType::Float64;
        auto trimmed = std::make_shared<Signal>(meta);
        trimmed->append(tsBuf.data(),
                        reinterpret_cast<const std::byte*>(vsBuf.data()),
                        tsBuf.size());
        if (freq) freqChans.push_back(std::move(trimmed));
        else      timeChans.push_back(std::move(trimmed));
    }

    if (timeChans.empty() && freqChans.empty()) {
        return ChartSaveResult::NothingMatchedFilters;
    }

    const bool haveBoth = !timeChans.empty() && !freqChans.empty();
    const bool reallySplit = filters.splitDomainsIntoTwoFiles && haveBoth;

    auto writeOne = [&](const QString& outPath,
                        const std::vector<std::shared_ptr<Signal>>& chans,
                        QString* err) -> bool {
        return saveFile(std::filesystem::path(outPath.toStdString()),
                        chans, fmt, opts, err);
    };

    QString err;
    if (reallySplit) {
        const QString tPath = withSuffixBeforeExt(basePath, "_time");
        const QString fPath = withSuffixBeforeExt(basePath, "_frequency");
        if (!writeOne(tPath, timeChans, &err)) {
            if (errorOut) *errorOut = err;
            return ChartSaveResult::Failed;
        }
        if (!writeOne(fPath, freqChans, &err)) {
            if (errorOut) *errorOut = err;
            return ChartSaveResult::Failed;
        }
        if (messagesOut) {
            const QString tName = QString::fromStdString(
                std::filesystem::path(tPath.toStdString()).filename().string());
            const QString fName = QString::fromStdString(
                std::filesystem::path(fPath.toStdString()).filename().string());
            *messagesOut << QString("%1 time-domain channel(s) → %2")
                                 .arg(timeChans.size()).arg(tName)
                         << QString("%1 frequency-domain channel(s) → %2")
                                 .arg(freqChans.size()).arg(fName);
        }
        return ChartSaveResult::Ok;
    }

    // Single combined file.
    std::vector<std::shared_ptr<Signal>> all;
    all.reserve(timeChans.size() + freqChans.size());
    for (auto& s : timeChans) all.push_back(s);
    for (auto& s : freqChans) all.push_back(s);
    if (!writeOne(basePath, all, &err)) {
        if (errorOut) *errorOut = err;
        return ChartSaveResult::Failed;
    }
    if (messagesOut) {
        const QString name = QString::fromStdString(
            std::filesystem::path(basePath.toStdString()).filename().string());
        *messagesOut << QString("%1 channel(s) → %2  (time: %3, frequency: %4)")
                            .arg(all.size())
                            .arg(name)
                            .arg(timeChans.size())
                            .arg(freqChans.size());
    }
    return ChartSaveResult::Ok;
}

}  // namespace scope::converter
