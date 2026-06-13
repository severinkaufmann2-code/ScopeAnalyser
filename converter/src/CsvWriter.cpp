#include "scope/converter/CsvWriter.h"

#include <nlohmann/json.hpp>

#include <QFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace scope::converter {

using scope::core::Signal;
using scope::core::TimestampNs;

namespace {

QString fmtDouble(double v, const QString& decSep, int digits) {
    if (std::isnan(v)) return QString();
    QString s = (digits < 0)
                  ? QString::number(v, 'g', 15)
                  : QString::number(v, 'f', digits);
    if (decSep != ".") s.replace('.', decSep);
    return s;
}

// Linear interpolation at time t. Returns NaN when t falls outside the
// signal's range (so the CSV gets an empty cell rather than an extrapolated
// value).
double linearInterp(const TimestampNs* ts,
                    const std::vector<double>& vals,
                    std::size_t n,
                    TimestampNs t) {
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    if (t < ts[0] || t > ts[n - 1]) return std::numeric_limits<double>::quiet_NaN();
    auto it = std::lower_bound(ts, ts + n, t);
    const std::size_t hi = it - ts;
    if (hi >= n) return vals.back();
    if (ts[hi] == t) return vals[hi];
    if (hi == 0)    return vals[0];
    const std::size_t lo = hi - 1;
    const double dt = static_cast<double>(ts[hi] - ts[lo]);
    if (dt == 0) return vals[lo];
    const double frac = static_cast<double>(t - ts[lo]) / dt;
    return vals[lo] + frac * (vals[hi] - vals[lo]);
}

QString columnHeader(const std::shared_ptr<Signal>& s) {
    const auto& m = s->meta();
    if (!m.unit.isEmpty()) return QString("%1 [%2]").arg(m.name, m.unit);
    return m.name;
}

bool isFreq(const std::shared_ptr<Signal>& s) {
    return s->meta().domain == Signal::Domain::Frequency;
}
QString sharedXLabel(Signal::Domain d, const QString& timeUnit) {
    return d == Signal::Domain::Frequency ? QStringLiteral("f [Hz]")
                                          : QStringLiteral("t [%1]").arg(timeUnit);
}
QString perSigXHeader(const std::shared_ptr<Signal>& s, const QString& timeUnit) {
    const QString prefix = isFreq(s) ? QStringLiteral("f_") : QStringLiteral("t_");
    const QString unit   = isFreq(s) ? QStringLiteral("Hz") : timeUnit;
    return QStringLiteral("%1%2 [%3]").arg(prefix, s->meta().name, unit);
}

}  // namespace

bool writeCsv(const std::filesystem::path& path,
              const std::vector<std::shared_ptr<Signal>>& channels,
              const CsvExportOptions& opts,
              QString* errorOut) {

    if (channels.empty()) {
        if (errorOut) *errorOut = "No channels to write";
        return false;
    }

    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = "Cannot open file for writing: " + f.errorString();
        return false;
    }
    QTextStream out(&f);
    out.setRealNumberPrecision(15);

    const QString sep = opts.columnDelimiter.isEmpty() ? QString(",") : opts.columnDelimiter;
    const QString row = opts.rowDelimiter.isEmpty()    ? QString("\n") : opts.rowDelimiter;
    const QString dec = opts.decimalSeparator.isEmpty() ? QString(".") : opts.decimalSeparator;

    // Time-X formatting. EpochNs writes the absolute timestamp as integer ns
    // (lossless); RelativeSeconds rebases to the earliest sample as a double.
    const bool    epochNs  = (opts.timeAxis == CsvExportOptions::TimeAxis::EpochNs);
    const QString timeUnit = epochNs ? QStringLiteral("ns") : QStringLiteral("s");

    // Per-signal cache: view + values pulled once.
    struct Cache {
        std::shared_ptr<Signal> sig;
        Signal::ReadView        view;
        std::vector<double>     vals;
    };
    std::vector<Cache> caches;
    caches.reserve(channels.size());
    for (const auto& s : channels) {
        caches.push_back({s, s->snapshotForRead(), s->readAsDouble()});
    }

    // Split by domain.
    std::vector<const Cache*> timeC, freqC;
    for (const auto& c : caches) {
        if (isFreq(c.sig)) freqC.push_back(&c);
        else               timeC.push_back(&c);
    }

    // Time-domain origin = earliest first-sample across time signals.
    // Frequency bins are absolute Hz so no shift.
    TimestampNs timeOriginNs = std::numeric_limits<TimestampNs>::max();
    for (const auto* c : timeC)
        if (c->view.count > 0)
            timeOriginNs = std::min(timeOriginNs, c->view.timestamps[0]);
    if (timeOriginNs == std::numeric_limits<TimestampNs>::max()) timeOriginNs = 0;

    auto fmtTimeX = [&](TimestampNs t) -> QString {
        return epochNs
            ? QString::number(static_cast<qlonglong>(t))
            : fmtDouble((t - timeOriginNs) / 1e9, dec, opts.decimalDigits);
    };

    // ----- Build the column-descriptor list ---------------------------
    //
    // One entry per column we're about to write, in column order. The
    // metadata line carries this as authoritative info on read so the
    // loader doesn't need to guess from header strings.
    struct ColDesc {
        std::string role;       // "x_time" | "x_frequency" | "signal"
        std::string unit;
        std::string name;       // signal-only
        std::string src;        // signal-only (sourceSymbol)
        int         refX{-1};   // signal-only: index of its X column
    };
    std::vector<ColDesc> cols;
    if (opts.timeMode == CsvExportOptions::TimeMode::Shared) {
        if (!timeC.empty()) {
            cols.push_back({"x_time", timeUnit.toStdString(), "", "", -1});
            const int xIdx = static_cast<int>(cols.size()) - 1;
            for (const auto* c : timeC) {
                cols.push_back({"signal",
                                c->sig->meta().unit.toStdString(),
                                c->sig->meta().name.toStdString(),
                                c->sig->meta().sourceSymbol.toStdString(),
                                xIdx});
            }
        }
        if (!freqC.empty()) {
            cols.push_back({"x_frequency", "Hz", "", "", -1});
            const int xIdx = static_cast<int>(cols.size()) - 1;
            for (const auto* c : freqC) {
                cols.push_back({"signal",
                                c->sig->meta().unit.toStdString(),
                                c->sig->meta().name.toStdString(),
                                c->sig->meta().sourceSymbol.toStdString(),
                                xIdx});
            }
        }
    } else {
        for (const auto& c : caches) {
            const bool freq = isFreq(c.sig);
            cols.push_back({freq ? "x_frequency" : "x_time",
                            freq ? std::string("Hz") : timeUnit.toStdString(),
                            "", "", -1});
            const int xIdx = static_cast<int>(cols.size()) - 1;
            cols.push_back({"signal",
                            c.sig->meta().unit.toStdString(),
                            c.sig->meta().name.toStdString(),
                            c.sig->meta().sourceSymbol.toStdString(),
                            xIdx});
        }
    }

    if (opts.includeMetadata) {
        nlohmann::json meta;
        meta["v"] = 1;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : cols) {
            nlohmann::json j;
            j["role"] = c.role;
            j["unit"] = c.unit;
            if (c.role == "signal") {
                if (!c.name.empty()) j["name"] = c.name;
                if (!c.src.empty())  j["src"]  = c.src;
                j["refX"] = c.refX;
            }
            arr.push_back(std::move(j));
        }
        meta["columns"] = std::move(arr);
        if (!opts.layoutJson.isEmpty()) {
            // Splice the layout in as a nested object rather than as an
            // opaque string so a reader can `root["layout"]` it directly.
            // Malformed input falls through silently — we'd rather emit a
            // chart without the optional layout than refuse to save.
            try {
                meta["layout"] = nlohmann::json::parse(
                    opts.layoutJson.toStdString());
            } catch (...) {
                // intentionally swallowed — see comment above
            }
        }
        out << "# scope-csv: "
            << QString::fromStdString(meta.dump())
            << row;
    }

    if (opts.timeMode == CsvExportOptions::TimeMode::Shared) {
        // ----- Build per-domain shared grids -----
        auto buildGrid = [](const std::vector<const Cache*>& cs) {
            std::set<TimestampNs> uniq;
            for (const auto* c : cs)
                for (std::size_t i = 0; i < c->view.count; ++i)
                    uniq.insert(c->view.timestamps[i]);
            return std::vector<TimestampNs>(uniq.begin(), uniq.end());
        };
        const auto timeGrid = timeC.empty() ? std::vector<TimestampNs>{} : buildGrid(timeC);
        const auto freqGrid = freqC.empty() ? std::vector<TimestampNs>{} : buildGrid(freqC);
        const bool mixed = !timeC.empty() && !freqC.empty();

        // ----- Header -----
        if (opts.includeHeader) {
            bool firstCol = true;
            auto colSep = [&]{ if (!firstCol) out << sep; firstCol = false; };
            if (!timeC.empty()) {
                colSep(); out << sharedXLabel(Signal::Domain::Time, timeUnit);
                for (const auto* c : timeC) { colSep(); out << columnHeader(c->sig); }
            }
            if (!freqC.empty()) {
                colSep(); out << sharedXLabel(Signal::Domain::Frequency, timeUnit);
                for (const auto* c : freqC) { colSep(); out << columnHeader(c->sig); }
            }
            out << row;
        }

        // ----- Rows -----
        const std::size_t rowCount = std::max(timeGrid.size(), freqGrid.size());
        for (std::size_t r = 0; r < rowCount; ++r) {
            bool firstCol = true;
            auto colSep = [&]{ if (!firstCol) out << sep; firstCol = false; };

            if (!timeC.empty()) {
                if (r < timeGrid.size()) {
                    const TimestampNs t = timeGrid[r];
                    colSep();
                    out << fmtTimeX(t);
                    for (const auto* c : timeC) {
                        colSep();
                        out << fmtDouble(
                            linearInterp(c->view.timestamps, c->vals, c->view.count, t),
                            dec, opts.decimalDigits);
                    }
                } else if (mixed) {
                    // Pad time block with empties so the frequency block stays aligned.
                    colSep();                       // empty t cell
                    for (std::size_t k = 0; k < timeC.size(); ++k) { colSep(); }
                }
            }
            if (!freqC.empty()) {
                if (r < freqGrid.size()) {
                    const TimestampNs fns = freqGrid[r];
                    colSep();
                    out << fmtDouble(fns / 1e9, dec, opts.decimalDigits);
                    for (const auto* c : freqC) {
                        colSep();
                        out << fmtDouble(
                            linearInterp(c->view.timestamps, c->vals, c->view.count, fns),
                            dec, opts.decimalDigits);
                    }
                } else if (mixed) {
                    colSep();
                    for (std::size_t k = 0; k < freqC.size(); ++k) { colSep(); }
                }
            }
            out << row;
        }
    } else {
        // ---- Per-signal: (X_i, V_i) columns side-by-side, X label per signal ----
        std::size_t maxN = 0;
        for (const auto& c : caches) maxN = std::max(maxN, c.view.count);

        if (opts.includeHeader) {
            bool first = true;
            for (const auto& c : caches) {
                if (!first) out << sep;
                out << perSigXHeader(c.sig, timeUnit) << sep << columnHeader(c.sig);
                first = false;
            }
            out << row;
        }

        for (std::size_t r = 0; r < maxN; ++r) {
            bool first = true;
            for (const auto& c : caches) {
                if (!first) out << sep;
                if (r < c.view.count) {
                    const TimestampNs xRaw = c.view.timestamps[r];
                    const QString xCell = isFreq(c.sig)
                        ? fmtDouble(xRaw / 1e9, dec, opts.decimalDigits)
                        : fmtTimeX(xRaw);
                    out << xCell << sep
                        << fmtDouble(c.vals[r], dec, opts.decimalDigits);
                } else {
                    out << sep;  // two empty cells for this signal
                }
                first = false;
            }
            out << row;
        }
    }

    return true;
}

}  // namespace scope::converter
