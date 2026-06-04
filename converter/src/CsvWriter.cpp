#include "scope/converter/CsvWriter.h"

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

    // Pre-cache each signal's values + view for performance.
    struct Cache {
        Signal::ReadView    view;
        std::vector<double> vals;
    };
    std::vector<Cache> caches;
    caches.reserve(channels.size());
    for (const auto& s : channels) {
        caches.push_back({s->snapshotForRead(), s->readAsDouble()});
    }

    // Time origin = earliest first-sample across all channels.
    TimestampNs origin = std::numeric_limits<TimestampNs>::max();
    for (const auto& c : caches)
        if (c.view.count > 0) origin = std::min(origin, c.view.timestamps[0]);
    if (origin == std::numeric_limits<TimestampNs>::max()) origin = 0;

    if (opts.timeMode == CsvExportOptions::TimeMode::Shared) {
        // ---- Build union timestamp grid ----
        std::set<TimestampNs> uniq;
        for (const auto& c : caches)
            for (std::size_t i = 0; i < c.view.count; ++i)
                uniq.insert(c.view.timestamps[i]);
        std::vector<TimestampNs> grid(uniq.begin(), uniq.end());

        // ---- Header ----
        if (opts.includeHeader) {
            out << "t [s]";
            for (const auto& s : channels) {
                out << sep << columnHeader(s);
            }
            out << row;
        }

        // ---- Rows ----
        for (TimestampNs t : grid) {
            const double tSec = (t - origin) / 1e9;
            out << fmtDouble(tSec, dec, opts.decimalDigits);
            for (const auto& c : caches) {
                const double v = linearInterp(c.view.timestamps, c.vals,
                                              c.view.count, t);
                out << sep << fmtDouble(v, dec, opts.decimalDigits);
            }
            out << row;
        }
    } else {
        // ---- Per-signal: (t_i, v_i) columns side-by-side ----
        std::size_t maxN = 0;
        for (const auto& c : caches) maxN = std::max(maxN, c.view.count);

        if (opts.includeHeader) {
            bool first = true;
            for (const auto& s : channels) {
                if (!first) out << sep;
                out << QString("t_%1 [s]").arg(s->meta().name)
                    << sep << columnHeader(s);
                first = false;
            }
            out << row;
        }

        for (std::size_t r = 0; r < maxN; ++r) {
            bool first = true;
            for (const auto& c : caches) {
                if (!first) out << sep;
                if (r < c.view.count) {
                    const double tSec = (c.view.timestamps[r] - origin) / 1e9;
                    out << fmtDouble(tSec, dec, opts.decimalDigits)
                        << sep
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
