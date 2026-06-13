#include "scope/converter/TabularSource.h"
#include "scope/core/Signal.h"

#include <QAbstractTableModel>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace scope::converter {

using scope::core::Signal;
using scope::core::DataType;
using scope::core::TimestampNs;

namespace {

// Preview model over any TabularSource's grid. Columns are labelled with
// spreadsheet letters (A, B, …, Z, AA, …) — the same identifiers the
// ConverterProfile / mapping panel reference.
class TabularTableModel : public QAbstractTableModel {
public:
    explicit TabularTableModel(const TabularSource& src, QObject* parent = nullptr)
        : QAbstractTableModel(parent), src_(src) {}

    int rowCount(const QModelIndex& = {}) const override { return src_.rowCount(); }
    int columnCount(const QModelIndex& = {}) const override { return src_.columnCount(); }
    QVariant data(const QModelIndex& idx, int role) const override {
        if (role != Qt::DisplayRole || !idx.isValid()) return {};
        return src_.cell(idx.row(), idx.column());
    }
    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (role != Qt::DisplayRole) return {};
        if (o == Qt::Vertical)   return section + 1;
        QString s;
        int n = section;
        while (true) {
            s.prepend(QChar('A' + (n % 26)));
            n = n / 26 - 1;
            if (n < 0) break;
        }
        return s;
    }
private:
    const TabularSource& src_;
};

QString colLabel(int section) {
    QString s;
    int n = section;
    while (true) {
        s.prepend(QChar('A' + (n % 26)));
        n = n / 26 - 1;
        if (n < 0) break;
    }
    return s;
}

int labelToCol(const QString& label) {
    int n = 0;
    for (QChar c : label) {
        if (!c.isLetter()) return -1;
        n = n * 26 + (c.toUpper().unicode() - 'A' + 1);
    }
    return n - 1;
}

}  // namespace

int TabularSource::rowCount() const { return static_cast<int>(rows_.size()); }
int TabularSource::columnCount() const {
    int maxCols = 0;
    for (const auto& r : rows_) maxCols = std::max(maxCols, static_cast<int>(r.size()));
    return maxCols;
}
QString TabularSource::cell(int row, int col) const {
    if (row < 0 || row >= rowCount()) return {};
    const auto& r = rows_[row];
    if (col < 0 || col >= r.size()) return {};
    return r[col];
}
QStringList TabularSource::headerRow(int rowIndex) const {
    if (rowIndex < 0 || rowIndex >= rowCount()) return {};
    return rows_[rowIndex];
}

std::unique_ptr<QAbstractItemModel> TabularSource::previewModel(const QString& /*region*/) {
    return std::make_unique<TabularTableModel>(*this);
}

std::vector<std::shared_ptr<Signal>> TabularSource::apply(
    const ConverterProfile& profile, QString* errorOut,
    QStringList* warningsOut) {

    std::vector<std::shared_ptr<Signal>> out;
    if (rows_.empty()) {
        if (errorOut) *errorOut = "Source is empty";
        return out;
    }

    // Index X-axis columns by letter. There can be multiple, of either
    // time or frequency role. The same xByLabel table services both
    // kinds — they just convert to the int64 storage differently below.
    std::unordered_map<QString, ColumnMapping> xByLabel;
    QString firstXLabel;
    bool haveTimeX = false, haveFreqX = false;
    std::vector<std::pair<int, ColumnMapping>> ySpecs;
    for (const auto& c : profile.columns) {
        const int col = labelToCol(c.columnId);
        if (col < 0) continue;
        if (c.role == ColumnMapping::Role::XTime
            || c.role == ColumnMapping::Role::XFrequency) {
            xByLabel[c.columnId] = c;
            if (firstXLabel.isEmpty()) firstXLabel = c.columnId;
            haveTimeX = haveTimeX || c.role == ColumnMapping::Role::XTime;
            haveFreqX = haveFreqX || c.role == ColumnMapping::Role::XFrequency;
        }
        if (c.role == ColumnMapping::Role::Signal) ySpecs.emplace_back(col, c);
    }
    if (ySpecs.empty()) {
        if (errorOut) *errorOut = "Profile has no Y-signal channels";
        return out;
    }
    // Mixed time + frequency X columns: forcing a fallback would silently
    // assign one of them to every Y signal that doesn't pin xSourceColumn,
    // and which one is "first" depends on column-letter sort order. Refuse
    // the ambiguity outright — the user pins each Y to its X explicitly.
    if (haveTimeX && haveFreqX) {
        for (const auto& [_, ySpec] : ySpecs) {
            if (ySpec.xSourceColumn.isEmpty() && !ySpec.useSampleRate) {
                if (errorOut) *errorOut = QString(
                    "Channel '%1' must pin its X column when the profile mixes "
                    "time- and frequency-axis X columns.").arg(ySpec.signalName);
                return {};
            }
        }
    }

    // Validate X-column units against the recognised ladder. The parser
    // silently treats unknown units as seconds (XTime) or Hz (XFrequency)
    // for back-compat with legacy/hand-rolled profiles; warn the caller
    // so a user-facing tool can surface it. The new dropdown UI in
    // ChannelEditDialog can't produce an off-ladder value, so this fires
    // only for old `.scaconv` files or hand-edited JSON.
    if (warningsOut) {
        static const QStringList kTimeLadder = {"s","ms","us","µs","ns"};
        static const QStringList kFreqLadder = {"Hz","kHz","MHz"};
        auto inLadder = [](const QString& u, const QStringList& ladder) {
            for (const auto& v : ladder)
                if (u.compare(v, Qt::CaseInsensitive) == 0) return true;
            return false;
        };
        for (const auto& [_, x] : xByLabel) {
            if (x.role == ColumnMapping::Role::XTime
                && !x.unit.isEmpty() && !inLadder(x.unit, kTimeLadder)) {
                *warningsOut << QString(
                    "Column %1: unit \"%2\" is not a recognised time unit "
                    "(s / ms / µs / ns). The parser will treat values as "
                    "seconds.").arg(x.columnId, x.unit);
            }
            if (x.role == ColumnMapping::Role::XFrequency
                && !x.unit.isEmpty() && !inLadder(x.unit, kFreqLadder)) {
                *warningsOut << QString(
                    "Column %1: unit \"%2\" is not a recognised frequency unit "
                    "(Hz / kHz / MHz). The parser will treat values as Hz.")
                    .arg(x.columnId, x.unit);
            }
        }
    }

    // Parses a cell as a double. Returns whether the cell was actually
    // numeric — the value is still written to `*out` (0.0 on failure)
    // so callers can keep the silent-zero behaviour while telling us
    // when they did. Empty cells are treated as "skip, don't flag":
    // they're a common sparse-data convention, not a parse error.
    auto tryVal = [&](const QString& s, double* out, bool* wasNumeric) -> bool {
        const QString trimmed = s.trimmed();
        if (trimmed.isEmpty()) { *out = 0.0; *wasNumeric = true; return true; }
        QString t = trimmed;
        if (profile.decimalSeparator != ".") t.replace(profile.decimalSeparator, ".");
        bool ok = false;
        const double v = t.toDouble(&ok);
        *out = ok ? v : 0.0;
        *wasNumeric = ok;
        return ok;
    };

    auto tryNs = [&](const QString& s, const QString& unit,
                     TimestampNs* out, bool* wasNumeric) -> bool {
        // Integer "ns" literal → exact int64 (no double round-trip), so an
        // epoch-ns CSV column re-imports bit-exact.
        if (unit.compare("ns", Qt::CaseInsensitive) == 0) {
            const QString t = s.trimmed();
            if (!t.isEmpty() && !t.contains('.') && !t.contains('e')
                && !t.contains('E') && !t.contains(profile.decimalSeparator)) {
                bool okI = false;
                const qlonglong iv = t.toLongLong(&okI);
                if (okI) {
                    *out = static_cast<TimestampNs>(iv);
                    *wasNumeric = true;
                    return true;
                }
            }
        }
        double v = 0.0;
        const bool ok = tryVal(s, &v, wasNumeric);
        if (unit.compare("ms", Qt::CaseInsensitive) == 0)
            *out = static_cast<TimestampNs>(v * 1e6);
        else if (unit.compare("us", Qt::CaseInsensitive) == 0
            || unit == QString::fromUtf8("µs"))
            *out = static_cast<TimestampNs>(v * 1e3);
        else if (unit.compare("ns", Qt::CaseInsensitive) == 0)
            *out = static_cast<TimestampNs>(v);
        else
            *out = static_cast<TimestampNs>(v * 1e9);  // default: seconds
        return ok;
    };

    // Frequency-axis values are stored in the same int64 "ns" field the
    // Signal uses for timestamps, scaled as Hz × 1e9 — same convention
    // the in-app FFT output uses. Unit ladders: Hz / kHz / MHz, default Hz.
    auto tryFreqStorage = [&](const QString& s, const QString& unit,
                              TimestampNs* out, bool* wasNumeric) -> bool {
        double v = 0.0;
        const bool ok = tryVal(s, &v, wasNumeric);
        if (unit.compare("kHz", Qt::CaseInsensitive) == 0)
            *out = static_cast<TimestampNs>(v * 1e12);
        else if (unit.compare("MHz", Qt::CaseInsensitive) == 0)
            *out = static_cast<TimestampNs>(v * 1e15);
        else
            *out = static_cast<TimestampNs>(v * 1e9);  // default: Hz
        return ok;
    };

    const int defaultStart = std::max(0, profile.headerRow);
    const int defaultEnd   = rowCount() - 1;
    auto resolveRange = [&](const ColumnMapping& m) {
        const int lo = (m.rowStart < 0) ? defaultStart : m.rowStart;
        const int hi = (m.rowEnd   < 0) ? defaultEnd   : m.rowEnd;
        return std::pair{std::max(lo, 0), std::min(hi, rowCount() - 1)};
    };

    // Decide an X strategy per Y channel:
    //   per-channel useSampleRate → rate path
    //   per-channel xSourceColumn → use that X column
    //   else profile.useSampleRate → profile rate (legacy)
    //   else firstXLabel != ""    → use first X column found (legacy)
    //   else error
    auto resolveXStrategy = [&](const ColumnMapping& y, bool* useRateOut,
                                double* rateHzOut,
                                ColumnMapping* xMapOut, int* xColOut,
                                QString* errOut) -> bool {
        if (y.useSampleRate) {
            *useRateOut = true;
            *rateHzOut  = y.sampleRateHz;
            return true;
        }
        const QString xLabel = !y.xSourceColumn.isEmpty()
                                ? y.xSourceColumn
                                : firstXLabel;
        if (!xLabel.isEmpty()) {
            auto it = xByLabel.find(xLabel);
            if (it == xByLabel.end()) {
                if (errOut) *errOut = QString("Y signal references X column '%1' "
                                              "that is not in the profile").arg(xLabel);
                return false;
            }
            *useRateOut = false;
            *xMapOut    = it->second;
            *xColOut    = labelToCol(it->second.columnId);
            return true;
        }
        if (profile.useSampleRate) {
            *useRateOut = true;
            *rateHzOut  = profile.sampleRateHz;
            return true;
        }
        if (errOut) *errOut = "No X-time channel and no sample rate set "
                              "for this Y signal";
        return false;
    };

    // Aggregates "this column's cell wasn't a number, parsed as 0" events
    // so we can show one warning per X column instead of N (once per Y
    // signal sharing it). Keyed by X column letter.
    struct BadCellStats {
        int     count{0};
        QString firstExample;
        int     firstRow{-1};
    };
    std::unordered_map<QString, BadCellStats> badXStats;

    auto bumpBad = [](BadCellStats& s, const QString& example, int row) {
        if (s.count == 0) {
            s.firstExample = example;
            s.firstRow     = row;
        }
        ++s.count;
    };

    for (std::size_t k = 0; k < ySpecs.size(); ++k) {
        const auto& ySpec = ySpecs[k].second;
        const auto [yLo, yHi] = resolveRange(ySpec);
        const int yCol = ySpecs[k].first;
        if (yLo > yHi) continue;

        bool useRate = false;
        double rateHz = 0;
        ColumnMapping xMap;
        int xCol = -1;
        QString xErr;
        if (!resolveXStrategy(ySpec, &useRate, &rateHz, &xMap, &xCol, &xErr)) {
            if (errorOut) *errorOut = xErr;
            return {};
        }

        std::vector<TimestampNs> ts;
        std::vector<double> vs;
        ts.reserve(yHi - yLo + 1);
        vs.reserve(yHi - yLo + 1);

        BadCellStats yBad;   // per Y signal — no cross-signal sharing

        if (useRate) {
            const double dtNs = (rateHz > 0) ? 1e9 / rateHz : 0;
            for (int row = yLo; row <= yHi; ++row) {
                const auto& r = rows_[row];
                if (yCol >= r.size()) continue;
                ts.push_back(static_cast<TimestampNs>((row - yLo) * dtNs));
                double v = 0.0; bool ok = true;
                tryVal(r[yCol], &v, &ok);
                vs.push_back(v);
                if (!ok) bumpBad(yBad, r[yCol].trimmed(), row);
            }
        } else {
            const auto [xLo, xHi] = (xMap.rowStart >= 0 || xMap.rowEnd >= 0)
                                    ? resolveRange(xMap)
                                    : std::pair{yLo, yHi};
            const int lo = std::max(xLo, yLo);
            const int hi = std::min(xHi, yHi);
            const bool freqX = xMap.role == ColumnMapping::Role::XFrequency;
            auto& xBad = badXStats[xMap.columnId];
            for (int row = lo; row <= hi; ++row) {
                const auto& r = rows_[row];
                if (yCol >= r.size() || xCol >= r.size()) continue;
                // A blank X cell means this channel has no sample on this row
                // (a common ragged / sparse-column layout). Skip it rather
                // than synthesise a spurious (t=0, v=0) point. This is a
                // generic rule — the importer stays unaware of any specific
                // source format.
                if (r[xCol].trimmed().isEmpty()) continue;
                TimestampNs t = 0; double v = 0.0;
                bool xOk = true, yOk = true;
                if (freqX) tryFreqStorage(r[xCol], xMap.unit, &t, &xOk);
                else       tryNs        (r[xCol], xMap.unit, &t, &xOk);
                tryVal(r[yCol], &v, &yOk);
                ts.push_back(t);
                vs.push_back(v);
                if (!xOk) bumpBad(xBad, r[xCol].trimmed(), row);
                if (!yOk) bumpBad(yBad, r[yCol].trimmed(), row);
            }
        }

        if (yBad.count > 0 && warningsOut) {
            *warningsOut << QString(
                "Column %1 (Y signal '%2'): %3 non-numeric cell(s) in the parsed range; "
                "first was \"%4\" at preview row %5. Parsed as 0.")
                .arg(colLabel(yCol))
                .arg(ySpec.signalName.isEmpty()
                        ? QString("Col%1").arg(colLabel(yCol))
                        : ySpec.signalName)
                .arg(yBad.count)
                .arg(yBad.firstExample)
                .arg(yBad.firstRow + 1);
        }

        // Per-channel time-axis post-processing:
        //   1. resetTimeToZero  — subtract this channel's first timestamp
        //   2. timeOffsetSec    — add a fixed shift (seconds → ns)
        // Order matters: reset first, then offset. So reset+offset=5 puts
        // the first sample at exactly t = 5 s.
        if (ySpec.resetTimeToZero && !ts.empty()) {
            const TimestampNs t0 = ts.front();
            for (auto& t : ts) t -= t0;
        }
        if (ySpec.timeOffsetSec != 0.0 && !ts.empty()) {
            const TimestampNs off = static_cast<TimestampNs>(
                std::llround(ySpec.timeOffsetSec * 1e9));
            for (auto& t : ts) t += off;
        }

        // Collapse runs of equal timestamps per the chosen mode. The
        // column-X path can produce duplicates when the CSV repeats the
        // same X for multiple rows (oversampled logs, redundant exports,
        // wide-to-long reshapes). Sample-rate mode is monotonic so this
        // is a no-op there.
        if (ySpec.collapseDuplicates != ColumnMapping::CollapseMode::None
            && !ts.empty()) {
            std::vector<TimestampNs> tsOut;
            std::vector<double> vsOut;
            tsOut.reserve(ts.size());
            vsOut.reserve(ts.size());
            std::size_t i = 0;
            while (i < ts.size()) {
                std::size_t j = i + 1;
                while (j < ts.size() && ts[j] == ts[i]) ++j;
                tsOut.push_back(ts[i]);
                switch (ySpec.collapseDuplicates) {
                    case ColumnMapping::CollapseMode::Last:
                        vsOut.push_back(vs[j - 1]);
                        break;
                    case ColumnMapping::CollapseMode::Mean: {
                        double acc = 0.0;
                        for (std::size_t k = i; k < j; ++k) acc += vs[k];
                        vsOut.push_back(acc / static_cast<double>(j - i));
                        break;
                    }
                    case ColumnMapping::CollapseMode::First:
                    default:
                        vsOut.push_back(vs[i]);
                        break;
                }
                i = j;
            }
            ts.swap(tsOut);
            vs.swap(vsOut);
        }

        // Collapse runs of equal *value* (distinct timestamps, same Y).
        // This is the staircase pattern of a CSV logged faster than
        // the underlying value updates — common for integer-sensor
        // values stored as REAL/LREAL. KeepFirst keeps the timestamp
        // at which the value first became X; KeepLast keeps the last
        // confirmation. Applied AFTER the timestamp-dedup step.
        if (ySpec.collapseValuePlateaus != ColumnMapping::ValuePlateauMode::None
            && !vs.empty()) {
            std::vector<TimestampNs> tsOut;
            std::vector<double> vsOut;
            tsOut.reserve(vs.size());
            vsOut.reserve(vs.size());
            std::size_t i = 0;
            while (i < vs.size()) {
                std::size_t j = i + 1;
                while (j < vs.size() && vs[j] == vs[i]) ++j;
                if (ySpec.collapseValuePlateaus
                    == ColumnMapping::ValuePlateauMode::KeepLast) {
                    tsOut.push_back(ts[j - 1]);
                } else {
                    tsOut.push_back(ts[i]);  // KeepFirst
                }
                vsOut.push_back(vs[i]);  // any sample in the run carries the value
                i = j;
            }
            ts.swap(tsOut);
            vs.swap(vsOut);
        }

        Signal::Meta m;
        m.name = ySpec.signalName.isEmpty()
                 ? QString("Col%1").arg(colLabel(yCol))
                 : ySpec.signalName;
        m.unit = ySpec.unit;
        m.dataType = DataType::Float64;
        m.sourceSymbol = sourceLabel_ + ":" + colLabel(yCol);
        if (useRate) m.sampleRateHz = rateHz;
        if (!useRate && xMap.role == ColumnMapping::Role::XFrequency)
            m.domain = Signal::Domain::Frequency;
        auto sig = std::make_shared<Signal>(m);
        sig->append(ts.data(),
                    reinterpret_cast<const std::byte*>(vs.data()),
                    vs.size());
        out.push_back(std::move(sig));
    }

    // Emit one warning per X column that had any non-numeric cells, after
    // every Y signal has had a chance to drive its parse loop. Keyed by
    // column letter so a shared X column is mentioned once, not once per
    // signal that referenced it.
    if (warningsOut) {
        for (const auto& [colId, bad] : badXStats) {
            if (bad.count == 0) continue;
            *warningsOut << QString(
                "Column %1 (X-axis): %2 non-numeric cell(s) in the parsed range; "
                "first was \"%3\" at preview row %4. Parsed as 0.")
                .arg(colId)
                .arg(bad.count)
                .arg(bad.firstExample)
                .arg(bad.firstRow + 1);
        }
    }

    return out;
}

}  // namespace scope::converter
