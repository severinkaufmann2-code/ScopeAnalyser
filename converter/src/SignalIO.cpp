#include "scope/converter/SignalIO.h"

#include "scope/converter/ConverterProfile.h"
#include "scope/converter/HtmlExport.h"
#include "scope/core/Hdf5Session.h"
#include "scope/core/Mdf4Session.h"

#include <nlohmann/json.hpp>

#include <QFile>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>

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

// Parse an X-axis cell into the Signal's int64 storage.
//   Time: an integer "ns" literal is read exactly via toLongLong — no double
//     round-trip — which is what keeps epoch-ns CSVs lossless. Other units
//     (s default, ms, us/µs) and real-valued ns go through a double × scale.
//     A unit-less time column whose value is an unmistakably-nanosecond
//     integer (|v| ≥ 1e15) is treated as ns; everything else stays seconds,
//     preserving legacy / foreign behaviour.
//   Frequency: Hz (default), kHz, MHz — stored as Hz × 1e9.
// `cell` is expected to already have its decimal mark normalised to '.'.
TimestampNs parseXValue(const QString& cell, const QString& unit,
                        Signal::Domain domain, bool* ok) {
    const QString t = cell.trimmed();
    const bool integer = !t.contains('.') && !t.contains('e') && !t.contains('E');
    const QString u = unit.trimmed();

    if (domain == Signal::Domain::Time) {
        bool treatAsNs = (u.compare("ns", Qt::CaseInsensitive) == 0);
        if (!treatAsNs && u.isEmpty() && integer) {
            bool okI = false;
            const qlonglong iv = t.toLongLong(&okI);
            if (okI && (iv >= 1000000000000000LL || iv <= -1000000000000000LL))
                treatAsNs = true;
        }
        if (treatAsNs && integer) {
            bool okI = false;
            const qlonglong iv = t.toLongLong(&okI);
            if (okI) { *ok = true; return static_cast<TimestampNs>(iv); }
        }
        bool okD = false;
        const double d = t.toDouble(&okD);
        *ok = okD;
        double scale = 1e9;  // seconds (default / empty unit)
        if      (u.compare("ms", Qt::CaseInsensitive) == 0) scale = 1e6;
        else if (u.compare("us", Qt::CaseInsensitive) == 0
                 || u == QString::fromUtf8("µs"))           scale = 1e3;
        else if (treatAsNs)                                 scale = 1.0;
        return static_cast<TimestampNs>(d * scale);
    }

    bool okD = false;
    const double d = t.toDouble(&okD);
    *ok = okD;
    double scale = 1e9;  // Hz (default / empty unit)
    if      (u.compare("kHz", Qt::CaseInsensitive) == 0) scale = 1e12;
    else if (u.compare("MHz", Qt::CaseInsensitive) == 0) scale = 1e15;
    return static_cast<TimestampNs>(d * scale);
}

// Stream a TwinCAT Scope export into one Signal per channel. The layout
// (delimiter, decimal, per-channel time/value columns, data-start row) is
// sniffed from the header by detectTwinCatScopeCsv; here we just skip to the
// data and read each row's (time_ms, value) pairs. Time is milliseconds →
// ns. Ragged columns (a channel that ends early leaves empty cells) are
// handled by skipping the pair when either cell is blank. Returns false if
// the file isn't a TwinCAT Scope export (caller falls back to the generic
// CSV path). Streaming keeps memory flat on multi-hundred-MB exports.
bool loadTwinCatScopeCsv(const std::filesystem::path& path,
                         std::vector<std::shared_ptr<Signal>>* out,
                         QString* errorOut) {
    auto layout = detectTwinCatScopeCsv(path);
    if (!layout) return false;

    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut) *errorOut = "Couldn't open file.";
        return false;
    }
    QTextStream ts(&f);
    const QChar sep          = layout->delimiter;
    const bool  commaDecimal = (layout->decimal == QChar(','));
    const std::size_t n      = layout->channels.size();

    std::vector<std::vector<TimestampNs>> tsBufs(n);
    std::vector<std::vector<double>>      vsBufs(n);

    int lineIdx = 0;
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (lineIdx++ < layout->dataStartRow) continue;
        if (line.isEmpty()) continue;
        const QStringList parts = line.split(sep);
        for (std::size_t k = 0; k < n; ++k) {
            const auto& ch = layout->channels[k];
            if (ch.timeCol >= parts.size() || ch.valueCol >= parts.size())
                continue;
            QString tStr = parts[ch.timeCol].trimmed();
            QString vStr = parts[ch.valueCol].trimmed();
            if (tStr.isEmpty() || vStr.isEmpty()) continue;   // ragged tail
            if (commaDecimal) { tStr.replace(',', '.'); vStr.replace(',', '.'); }
            bool okT = false, okV = false;
            const double tMs = tStr.toDouble(&okT);
            const double v   = vStr.toDouble(&okV);
            if (!okT || !okV) continue;
            tsBufs[k].push_back(static_cast<TimestampNs>(tMs * 1e6));
            vsBufs[k].push_back(v);
        }
    }

    bool any = false;
    for (std::size_t k = 0; k < n; ++k) {
        if (tsBufs[k].empty()) continue;
        Signal::Meta m;
        m.name         = layout->channels[k].name;
        m.unit         = layout->channels[k].unit;
        m.dataType     = scope::core::DataType::Float64;
        m.domain       = Signal::Domain::Time;
        m.sourceSymbol = QString::fromStdString(path.filename().string());
        auto sig = std::make_shared<Signal>(m);
        sig->append(tsBufs[k].data(),
                    reinterpret_cast<const std::byte*>(vsBufs[k].data()),
                    tsBufs[k].size());
        out->push_back(std::move(sig));
        any = true;
    }
    if (!any && errorOut)
        *errorOut = "TwinCAT Scope export had no parseable data.";
    return any;
}

bool loadCsvChart(const std::filesystem::path& path,
                  std::vector<std::shared_ptr<Signal>>* out,
                  QString* errorOut,
                  QString* layoutJsonOut = nullptr,
                  QString* autoDetectedOut = nullptr) {
    // TwinCAT Scope exports have a multi-row header block + per-channel
    // (time, value) column pairs that the generic header/heuristic path
    // below can't read. Detect + stream them directly so they open without
    // the Converter. Conservative: anything that isn't unmistakably a
    // TwinCAT export falls through to the generic loader unchanged.
    {
        std::vector<std::shared_ptr<Signal>> twChans;
        QString twErr;
        if (loadTwinCatScopeCsv(path, &twChans, &twErr)) {
            *out = std::move(twChans);
            if (autoDetectedOut) *autoDetectedOut = "TwinCAT Scope export";
            return true;
        }
    }

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

    struct Target { QString name, unit, xUnit; int tCol{-1}, vCol{-1};
                    Signal::Domain domain{Signal::Domain::Time}; };
    std::vector<Target> targets;

    if (!metaJson.isEmpty()) {
        // ---- Metadata-driven mapping (authoritative) ----------------
        try {
            const auto root = nlohmann::json::parse(metaJson.toStdString());
            // Hand the embedded PlotLayout up to the host (Analyser's
            // openChartDialog feeds it through PlotLayout::fromJsonString).
            // Missing key → leave layoutJsonOut empty so the host falls
            // back to its existing behaviour.
            if (layoutJsonOut && root.contains("layout")) {
                *layoutJsonOut = QString::fromStdString(root["layout"].dump());
            }
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
                if (t.tCol >= 0 && t.tCol < static_cast<int>(units.size()))
                    t.xUnit = QString::fromStdString(units[t.tCol]);
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
                { QString xn; splitNameUnit(headers[curX], &xn, &t.xUnit); }
                t.tCol   = curX;
                t.vCol   = i;
                t.domain = curD;
                if (!t.name.isEmpty()) targets.push_back(std::move(t));
            }
        } else {
            for (int i = 0; i + 1 < headers.size(); i += 2) {
                Target t;
                splitNameUnit(headers[i + 1], &t.name, &t.unit);
                { QString xn; splitNameUnit(headers[i], &xn, &t.xUnit); }
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
            const TimestampNs x = parseXValue(tStr, tg.xUnit, tg.domain, &okT);
            const double v      = vStr.toDouble(&okV);
            if (!okT || !okV) continue;
            tsBufs[k].push_back(x);
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
               const QString& layoutJson,
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
    if (!layoutJson.isEmpty()) session->writeLayout(layoutJson, &err);
    session->flush();
    return true;
}

bool writeMdf4(const std::filesystem::path& path,
               const std::vector<std::shared_ptr<Signal>>& chans,
               const QString& layoutJson,
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
    // Set header metadata before the first appendSamples — mdflib writes the
    // HD/MD block lazily on InitMeasurement (triggered by the first sample, or
    // by finalize() when there are none), after which it's no longer editable.
    if (!layoutJson.isEmpty()) session->writeLayout(layoutJson, &err);
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

// ---- JSON ------------------------------------------------------------
//
// Native "scope-json" format: one object per signal, timestamps written as
// the Signal's internal int64 field verbatim (ns for time signals, Hz×1e9
// for frequency signals) so a re-open is bit-exact. An optional top-level
// "layout" carries the PlotLayout. The compact dump keeps multi-million-
// sample files from exploding into one-number-per-line.
bool writeJson(const std::filesystem::path& path,
               const std::vector<std::shared_ptr<Signal>>& chans,
               const QString& layoutJson,
               QString* errorOut) {
    if (chans.empty()) {
        if (errorOut) *errorOut = "No channels to write";
        return false;
    }
    nlohmann::json root;
    root["scope-json"] = 1;
    if (!layoutJson.isEmpty()) {
        // Nested object (not an opaque string) so a reader can root["layout"]
        // it directly. Malformed layout is dropped, not fatal — we'd rather
        // save the data without the optional layout than refuse the save.
        try {
            root["layout"] = nlohmann::json::parse(layoutJson.toStdString());
        } catch (...) {
            // intentionally swallowed — see comment above
        }
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : chans) {
        if (!s) continue;
        const auto& m = s->meta();
        auto view = s->snapshotForRead();
        auto vals = s->readAsDouble();
        nlohmann::json js;
        js["name"] = m.name.toStdString();
        if (!m.unit.isEmpty()) js["unit"] = m.unit.toStdString();
        js["domain"] = (m.domain == Signal::Domain::Frequency) ? "frequency" : "time";
        if (!m.sourceSymbol.isEmpty()) js["src"] = m.sourceSymbol.toStdString();
        if (m.sampleRateHz > 0.0)      js["sampleRateHz"] = m.sampleRateHz;
        nlohmann::json jt = nlohmann::json::array();
        nlohmann::json jv = nlohmann::json::array();
        for (std::size_t i = 0; i < view.count; ++i) {
            jt.push_back(static_cast<std::int64_t>(view.timestamps[i]));
            jv.push_back(i < vals.size() ? vals[i] : 0.0);  // NaN → null
        }
        js["t"] = std::move(jt);
        js["v"] = std::move(jv);
        arr.push_back(std::move(js));
    }
    root["signals"] = std::move(arr);

    try {
        std::ofstream os(path, std::ios::binary | std::ios::trunc);
        if (!os) {
            if (errorOut) *errorOut = "Cannot open file for writing.";
            return false;
        }
        os << root.dump();
        if (!os) {
            if (errorOut) *errorOut = "Write failed.";
            return false;
        }
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        return false;
    }
    return true;
}

// Best-effort numeric extraction from a *non-native* JSON table: an array of
// record objects ([{…},{…}]) or a columnar object of arrays
// ({"t":[…],"a":[…]}). The first column whose name looks like a time axis
// (t/time/x/…) — else column 0 — becomes the X axis, read as SECONDS; every
// other numeric column becomes a Time-domain signal. The mapping is a guess:
// the caller surfaces it via autoDetectedFormat and points the user at the
// Converter for exact control. Returns false if no table could be formed.
bool loadForeignJson(const nlohmann::json& root,
                     const std::filesystem::path& path,
                     std::vector<std::shared_ptr<Signal>>* out) {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    std::vector<QString>             colNames;
    std::vector<std::vector<double>> colVals;
    auto colIndex = [&](const QString& name) -> std::size_t {
        for (std::size_t i = 0; i < colNames.size(); ++i)
            if (colNames[i] == name) return i;
        colNames.push_back(name);
        colVals.emplace_back();
        return colNames.size() - 1;
    };
    auto toNum = [&](const nlohmann::json& v) -> double {
        if (v.is_number())  return v.get<double>();
        if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
        return kNaN;
    };

    if (root.is_array()) {
        std::size_t rowIdx = 0;
        for (const auto& rec : root) {
            if (!rec.is_object()) continue;
            for (auto it = rec.begin(); it != rec.end(); ++it) {
                auto& cv = colVals[colIndex(QString::fromStdString(it.key()))];
                cv.resize(rowIdx, kNaN);     // pad rows this column missed
                cv.push_back(toNum(it.value()));
            }
            ++rowIdx;
        }
        for (auto& cv : colVals) cv.resize(rowIdx, kNaN);
    } else if (root.is_object()) {
        // A member that is itself an array of objects is a nested record
        // table — recurse into the first such one.
        for (auto it = root.begin(); it != root.end(); ++it)
            if (it.value().is_array() && !it.value().empty()
                && it.value()[0].is_object())
                return loadForeignJson(it.value(), path, out);
        std::size_t maxLen = 0;
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!it.value().is_array()) continue;
            auto& cv = colVals[colIndex(QString::fromStdString(it.key()))];
            for (const auto& cell : it.value()) cv.push_back(toNum(cell));
            maxLen = std::max(maxLen, cv.size());
        }
        for (auto& cv : colVals) cv.resize(maxLen, kNaN);
    } else {
        return false;
    }
    if (colNames.size() < 2) return false;

    auto looksTime = [](const QString& n) {
        const QString t = n.trimmed().toLower();
        return t == "t" || t == "time" || t == "timestamp" || t == "x"
            || t == "sec" || t == "seconds" || t == "s";
    };
    std::size_t xIdx = 0;
    for (std::size_t i = 0; i < colNames.size(); ++i)
        if (looksTime(colNames[i])) { xIdx = i; break; }

    bool any = false;
    for (std::size_t c = 0; c < colNames.size(); ++c) {
        if (c == xIdx) continue;
        std::vector<TimestampNs> ts;
        std::vector<double>      vs;
        const std::size_t n = std::min(colVals[c].size(), colVals[xIdx].size());
        for (std::size_t i = 0; i < n; ++i) {
            const double x = colVals[xIdx][i];
            if (std::isnan(x)) continue;                      // no X → skip row
            ts.push_back(static_cast<TimestampNs>(x * 1e9));  // seconds → ns
            vs.push_back(colVals[c][i]);
        }
        if (ts.empty()) continue;
        Signal::Meta m;
        m.name         = colNames[c];
        m.dataType     = scope::core::DataType::Float64;
        m.domain       = Signal::Domain::Time;
        m.sourceSymbol = QString::fromStdString(path.filename().string());
        auto sig = std::make_shared<Signal>(m);
        sig->append(ts.data(),
                    reinterpret_cast<const std::byte*>(vs.data()), ts.size());
        out->push_back(std::move(sig));
        any = true;
    }
    return any;
}

bool loadJsonChart(const std::filesystem::path& path,
                   std::vector<std::shared_ptr<Signal>>* out,
                   QString* errorOut,
                   QString* layoutJsonOut = nullptr,
                   QString* autoDetectedOut = nullptr) {
    nlohmann::json root;
    try {
        std::ifstream is(path);
        if (!is) { if (errorOut) *errorOut = "Couldn't open file."; return false; }
        is >> root;
    } catch (const std::exception& e) {
        if (errorOut)
            *errorOut = QString("Invalid JSON: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }

    // Embedded PlotLayout → up to the host (Analyser restores axes / view).
    if (layoutJsonOut && root.is_object() && root.contains("layout"))
        *layoutJsonOut = QString::fromStdString(root["layout"].dump());

    // ---- Native per-signal format (bit-exact) -----------------------
    if (root.is_object() && root.contains("signals")
        && root["signals"].is_array()) {
        for (const auto& js : root["signals"]) {
            if (!js.is_object()) continue;
            Signal::Meta m;
            m.name = QString::fromStdString(js.value("name", std::string{}));
            if (m.name.isEmpty()) continue;
            m.unit     = QString::fromStdString(js.value("unit", std::string{}));
            m.dataType = scope::core::DataType::Float64;
            m.domain   = (js.value("domain", std::string("time")) == "frequency")
                         ? Signal::Domain::Frequency : Signal::Domain::Time;
            m.sourceSymbol = js.contains("src")
                ? QString::fromStdString(js.value("src", std::string{}))
                : QString::fromStdString(path.filename().string());
            m.sampleRateHz = js.value("sampleRateHz", 0.0);

            const auto jt = js.value("t", nlohmann::json::array());
            const auto jv = js.value("v", nlohmann::json::array());
            const std::size_t n = std::min(jt.size(), jv.size());
            if (n == 0) continue;
            std::vector<TimestampNs> ts; ts.reserve(n);
            std::vector<double>      vs; vs.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                ts.push_back(jt[i].is_number_integer()
                             ? jt[i].get<std::int64_t>()
                             : static_cast<TimestampNs>(jt[i].is_number()
                                   ? jt[i].get<double>() : 0.0));
                vs.push_back(jv[i].is_number()
                             ? jv[i].get<double>()
                             : std::numeric_limits<double>::quiet_NaN());
            }
            auto sig = std::make_shared<Signal>(m);
            sig->append(ts.data(),
                        reinterpret_cast<const std::byte*>(vs.data()), ts.size());
            out->push_back(std::move(sig));
        }
        if (out->empty()) {
            if (errorOut) *errorOut = "JSON has no parseable signals.";
            return false;
        }
        return true;
    }

    // ---- Foreign best-effort (records / columnar) -------------------
    if (loadForeignJson(root, path, out)) {
        if (autoDetectedOut) *autoDetectedOut = "JSON (guessed column mapping)";
        return true;
    }
    if (errorOut)
        *errorOut = "Unrecognised JSON. Open it in the Converter to map its "
                    "columns to signals.";
    return false;
}

}  // namespace

FileFormat detectFormatFromExtension(const std::filesystem::path& path) {
    const QString e = lowerExt(path);
    if (e == ".h5" || e == ".hdf5")  return FileFormat::Hdf5;
    if (e == ".mf4")                 return FileFormat::Mdf4;
    if (e == ".csv" || e == ".txt")  return FileFormat::Csv;
    if (e == ".html" || e == ".htm") return FileFormat::Html;
    if (e == ".json")                return FileFormat::Json;
    return FileFormat::Auto;
}

QStringList nameFilters(FileFormat fmt) {
    switch (fmt) {
        case FileFormat::Csv:  return {"CSV (*.csv)", "Text (*.txt)", "All files (*)"};
        case FileFormat::Hdf5: return {"HDF5 (*.h5)", "All files (*)"};
        case FileFormat::Mdf4: return {"MDF4 (*.mf4)", "All files (*)"};
        case FileFormat::Html: return {"Interactive HTML chart (*.html)", "All files (*)"};
        case FileFormat::Json: return {"JSON (*.json)", "All files (*)"};
        case FileFormat::Auto:
        default:
            return {"Scope chart (*.h5 *.mf4 *.csv *.txt *.html *.json)",
                    "HDF5 (*.h5)",
                    "MDF4 (*.mf4)",
                    "CSV / text (*.csv *.txt)",
                    "Interactive HTML chart (*.html *.htm)",
                    "JSON (*.json)",
                    "All files (*)"};
    }
}

QString defaultSuffix(FileFormat fmt) {
    switch (fmt) {
        case FileFormat::Csv:  return "csv";
        case FileFormat::Hdf5: return "h5";
        case FileFormat::Mdf4: return "mf4";
        case FileFormat::Html: return "html";
        case FileFormat::Json: return "json";
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
            r.layoutJson = s->readLayout();
            r.error = err;
            r.ok = !r.channels.empty() || err.isEmpty();
            return r;
        }
        case FileFormat::Mdf4: {
            auto s = scope::core::Mdf4Session::openForRead(path, &err);
            if (!s) { r.error = err.isEmpty() ? "Couldn't open file." : err; return r; }
            r.channels = s->loadAllSignals(&err);
            r.layoutJson = s->readLayout();
            r.error = err;
            r.ok = !r.channels.empty() || err.isEmpty();
            return r;
        }
        case FileFormat::Csv:
            r.ok = loadCsvChart(path, &r.channels, &r.error, &r.layoutJson,
                                &r.autoDetectedFormat);
            return r;
        case FileFormat::Html: {
            r.ok = loadStorableHtml(QString::fromStdString(path.string()),
                                    &r.channels, &r.layoutJson, &err);
            r.error = err;
            return r;
        }
        case FileFormat::Json:
            r.ok = loadJsonChart(path, &r.channels, &r.error, &r.layoutJson,
                                 &r.autoDetectedFormat);
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
        case FileFormat::Hdf5: return writeHdf5(path, channels, opts.layoutJson, errorOut);
        case FileFormat::Mdf4: return writeMdf4(path, channels, opts.layoutJson, errorOut);
        case FileFormat::Csv: {
            // Bridge the format-agnostic layout into the CSV writer's own
            // knob. SaveOptions::layoutJson wins when set so all formats are
            // driven from the same field.
            CsvExportOptions csvOpts = opts.csv;
            if (!opts.layoutJson.isEmpty()) csvOpts.layoutJson = opts.layoutJson;
            return writeCsv(path, channels, csvOpts, errorOut);
        }
        case FileFormat::Html: {
            // Storable HTML stores the data once and re-imports via
            // loadStorableHtml. Stage the (already filtered / trimmed)
            // channels into a scratch store so exportStorableHtml can read
            // them; mirror the live plot layout when the caller supplied one
            // (Analyser), else fall back to a default view (Converter).
            //
            // htmlViewOnly instead rounds the values for display and drops
            // the re-import marker: a much smaller, one-way page.
            scope::core::SignalStore tmp;
            for (const auto& s : channels) if (s) tmp.add(s);
            const QString out = QString::fromStdString(path.string());
            const int digits = kChartOnlyDefaultDigits;
            if (opts.htmlView) {
                HtmlExportView view = *opts.htmlView;
                if (!opts.layoutJson.isEmpty()) view.layoutJson = opts.layoutJson;
                return opts.htmlViewOnly
                    ? exportChartOnlyHtml(out, tmp, view, digits, errorOut)
                    : exportStorableHtml(out, tmp, view, errorOut);
            }
            return opts.htmlViewOnly
                ? exportChartOnlyHtml(out, tmp, digits, errorOut)
                : exportStorableHtml(out, tmp, errorOut);
        }
        case FileFormat::Json: return writeJson(path, channels, opts.layoutJson, errorOut);
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
    // The custom range arrives in chart-relative seconds (the chart and the
    // CSV writer both present time relative to the earliest first sample).
    // Resolve it against that same origin: recordings carry absolute epoch
    // timestamps, where treating the seconds as absolute would silently
    // select nothing.
    //
    // An empty selectedChannels means "everything the flags admit"; anything
    // else restricts to exactly those names. Resolved once here so the range
    // origin and the channel gather below can't disagree about what is in.
    const QSet<QString> picked(filters.selectedChannels.begin(),
                               filters.selectedChannels.end());
    const auto isPicked = [&](const QString& name) {
        return picked.isEmpty() || picked.contains(name);
    };

    TimestampNs originNs = 0;
    if (filters.useCustomRange && filters.includeTime) {
        bool haveOrigin = false;
        for (const auto& n : store.channelNames()) {
            auto src = store.get(n);
            if (!src) continue;
            if (!isPicked(n)) continue;
            if (src->meta().domain == Signal::Domain::Frequency) continue;
            if (!filters.includeDerived && isDerivedChannel(src)) continue;
            auto view = src->snapshotForRead();
            if (view.count == 0) continue;
            if (!haveOrigin || view.timestamps[0] < originNs) {
                originNs = view.timestamps[0];
                haveOrigin = true;
            }
        }
    }
    const TimestampNs fromNs = filters.useCustomRange
        ? originNs + static_cast<TimestampNs>(filters.fromSec * 1e9)
        : std::numeric_limits<TimestampNs>::min();
    const TimestampNs toNs = filters.useCustomRange
        ? originNs + static_cast<TimestampNs>(filters.toSec   * 1e9)
        : std::numeric_limits<TimestampNs>::max();

    std::vector<std::shared_ptr<Signal>> timeChans, freqChans;
    for (const auto& n : store.channelNames()) {
        auto src = store.get(n);
        if (!src) continue;
        if (!isPicked(n)) continue;
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
    // HTML is always a single file: the page has its own Time / Frequency /
    // XY view selector, so a per-domain split makes no sense for it.
    const bool reallySplit = filters.splitDomainsIntoTwoFiles && haveBoth
                          && fmt != FileFormat::Html;

    // Each write gets its own layout string: a split single-domain file
    // carries only that domain's layout (opts.layoutJsonByDomain), while a
    // combined file keeps the full opts.layoutJson. Missing per-domain key
    // falls back to the full layout.
    auto writeOne = [&](const QString& outPath,
                        const std::vector<std::shared_ptr<Signal>>& chans,
                        const QString& layoutForFile,
                        QString* err) -> bool {
        SaveOptions o = opts;
        o.layoutJson = layoutForFile;
        return saveFile(std::filesystem::path(outPath.toStdString()),
                        chans, fmt, o, err);
    };

    QString err;
    if (reallySplit) {
        const QString tPath = withSuffixBeforeExt(basePath, "_time");
        const QString fPath = withSuffixBeforeExt(basePath, "_frequency");
        if (!writeOne(tPath, timeChans,
                      opts.layoutJsonByDomain.value("time", opts.layoutJson),
                      &err)) {
            if (errorOut) *errorOut = err;
            return ChartSaveResult::Failed;
        }
        if (!writeOne(fPath, freqChans,
                      opts.layoutJsonByDomain.value("frequency", opts.layoutJson),
                      &err)) {
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
    if (!writeOne(basePath, all, opts.layoutJson, &err)) {
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
