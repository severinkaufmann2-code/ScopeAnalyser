#include "scope/converter/CsvSource.h"
#include "scope/core/Signal.h"

#include <QAbstractTableModel>
#include <QFile>
#include <QHash>
#include <QTextStream>

#include <chrono>
#include <unordered_map>

namespace scope::converter {

using scope::core::Signal;
using scope::core::DataType;
using scope::core::TimestampNs;

namespace {

// Simple CSV row splitter that handles double-quoted fields. The delimiter
// must be a single character (the only kind of column separator CSV files use
// in practice).
QStringList splitCsv(const QString& line, QChar delim) {
    QStringList out;
    QString    cur;
    bool       inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else { inQuotes = false; }
            } else {
                cur += c;
            }
        } else {
            if (c == delim) { out << cur; cur.clear(); }
            else if (c == '"') { inQuotes = true; }
            else { cur += c; }
        }
    }
    out << cur;
    return out;
}

class CsvTableModel : public QAbstractTableModel {
public:
    explicit CsvTableModel(const CsvSource& src, QObject* parent = nullptr)
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
        // Use spreadsheet-style letters: A, B, ..., Z, AA, AB, ...
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
    const CsvSource& src_;
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

CsvSource::CsvSource(const std::filesystem::path& path,
                     QString columnDelimiter,
                     QString rowDelimiter,
                     QChar decimal)
    : path_(path),
      columnDelimiter_(std::move(columnDelimiter)),
      rowDelimiter_(std::move(rowDelimiter)),
      decimal_(decimal) {

    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly)) return;

    const QChar colChar = columnDelimiter_.isEmpty() ? QChar(',') : columnDelimiter_[0];

    QTextStream ts(&f);
    if (rowDelimiter_ == "\n" || rowDelimiter_ == "\r\n") {
        // Standard line-based parsing — QTextStream::readLine handles both
        // \n and \r\n. Empty trailing lines are ignored.
        while (!ts.atEnd()) {
            const QString line = ts.readLine();
            if (line.isEmpty() && ts.atEnd()) break;
            rows_.push_back(splitCsv(line, colChar));
        }
    } else {
        // Custom row delimiter — read whole file then split.
        const QString all = ts.readAll();
        const auto parts = all.split(rowDelimiter_, Qt::SkipEmptyParts);
        for (const auto& part : parts) {
            rows_.push_back(splitCsv(part, colChar));
        }
    }
}

CsvSource::~CsvSource() = default;

int CsvSource::rowCount() const { return static_cast<int>(rows_.size()); }
int CsvSource::columnCount() const {
    int maxCols = 0;
    for (const auto& r : rows_) maxCols = std::max(maxCols, static_cast<int>(r.size()));
    return maxCols;
}
QString CsvSource::cell(int row, int col) const {
    if (row < 0 || row >= rowCount()) return {};
    const auto& r = rows_[row];
    if (col < 0 || col >= r.size()) return {};
    return r[col];
}
QStringList CsvSource::headerRow(int rowIndex) const {
    if (rowIndex < 0 || rowIndex >= rowCount()) return {};
    return rows_[rowIndex];
}

std::unique_ptr<QAbstractItemModel> CsvSource::previewModel(const QString& /*region*/) {
    return std::make_unique<CsvTableModel>(*this);
}

std::vector<std::shared_ptr<Signal>> CsvSource::apply(
    const ConverterProfile& profile, QString* errorOut) {

    std::vector<std::shared_ptr<Signal>> out;
    if (rows_.empty()) {
        if (errorOut) *errorOut = "CSV file is empty";
        return out;
    }

    // Index X-axis columns by letter. There can be multiple.
    std::unordered_map<QString, ColumnMapping> xByLabel;
    QString firstXLabel;
    std::vector<std::pair<int, ColumnMapping>> ySpecs;
    for (const auto& c : profile.columns) {
        const int col = labelToCol(c.columnId);
        if (col < 0) continue;
        if (c.role == ColumnMapping::Role::XTime) {
            xByLabel[c.columnId] = c;
            if (firstXLabel.isEmpty()) firstXLabel = c.columnId;
        }
        if (c.role == ColumnMapping::Role::Signal) ySpecs.emplace_back(col, c);
    }
    if (ySpecs.empty()) {
        if (errorOut) *errorOut = "Profile has no Y-signal channels";
        return out;
    }

    auto toVal = [&](const QString& s) -> double {
        QString t = s;
        if (profile.decimalSeparator != ".") t.replace(profile.decimalSeparator, ".");
        bool ok = false;
        const double v = t.toDouble(&ok);
        return ok ? v : 0.0;
    };

    auto toNs = [&](const QString& s, const QString& unit) -> TimestampNs {
        const double v = toVal(s);
        if (unit.compare("ms", Qt::CaseInsensitive) == 0) return static_cast<TimestampNs>(v * 1e6);
        if (unit.compare("us", Qt::CaseInsensitive) == 0
            || unit == QString::fromUtf8("µs")) return static_cast<TimestampNs>(v * 1e3);
        if (unit.compare("ns", Qt::CaseInsensitive) == 0) return static_cast<TimestampNs>(v);
        return static_cast<TimestampNs>(v * 1e9);  // default: seconds
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

        if (useRate) {
            const double dtNs = (rateHz > 0) ? 1e9 / rateHz : 0;
            for (int row = yLo; row <= yHi; ++row) {
                const auto& r = rows_[row];
                if (yCol >= r.size()) continue;
                ts.push_back(static_cast<TimestampNs>((row - yLo) * dtNs));
                vs.push_back(toVal(r[yCol]));
            }
        } else {
            const auto [xLo, xHi] = (xMap.rowStart >= 0 || xMap.rowEnd >= 0)
                                    ? resolveRange(xMap)
                                    : std::pair{yLo, yHi};
            const int lo = std::max(xLo, yLo);
            const int hi = std::min(xHi, yHi);
            for (int row = lo; row <= hi; ++row) {
                const auto& r = rows_[row];
                if (yCol >= r.size() || xCol >= r.size()) continue;
                ts.push_back(toNs(r[xCol], xMap.unit));
                vs.push_back(toVal(r[yCol]));
            }
        }

        // Optional per-channel time-origin reset: shift every sample so
        // ts[0] == 0. Useful when mixing rate-based and column-based
        // imports — both end up anchored at t=0.
        if (ySpec.resetTimeToZero && !ts.empty()) {
            const TimestampNs origin = ts.front();
            for (auto& t : ts) t -= origin;
        }

        Signal::Meta m;
        m.name = ySpec.signalName.isEmpty()
                 ? QString("Col%1").arg(colLabel(yCol))
                 : ySpec.signalName;
        m.unit = ySpec.unit;
        m.dataType = DataType::Float64;
        m.sourceSymbol = QString::fromStdString(path_.filename().string())
                       + ":" + colLabel(yCol);
        if (useRate) m.sampleRateHz = rateHz;
        auto sig = std::make_shared<Signal>(m);
        sig->append(ts.data(),
                    reinterpret_cast<const std::byte*>(vs.data()),
                    vs.size());
        out.push_back(std::move(sig));
    }

    return out;
}

}  // namespace scope::converter
