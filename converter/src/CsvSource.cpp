#include "scope/converter/CsvSource.h"
#include "scope/core/Signal.h"

#include <QAbstractTableModel>
#include <QFile>
#include <QTextStream>

#include <chrono>

namespace scope::converter {

using scope::core::Signal;
using scope::core::DataType;
using scope::core::TimestampNs;

namespace {

// Simple CSV row splitter that handles double-quoted fields.
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

CsvSource::CsvSource(const std::filesystem::path& path, QChar delimiter, QChar decimal)
    : path_(path), delimiter_(delimiter), decimal_(decimal) {
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.isEmpty() && ts.atEnd()) break;
        rows_.push_back(splitCsv(line, delimiter_));
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

    const int dataStartRow = std::max(0, profile.headerRow);

    // Identify columns
    int xCol = -1;
    QString xUnit;
    std::vector<std::pair<int, ColumnMapping>> ySpecs;
    for (const auto& c : profile.columns) {
        const int col = labelToCol(c.columnId);
        if (col < 0) continue;
        if (c.role == ColumnMapping::Role::XTime) { xCol = col; xUnit = c.unit; }
        if (c.role == ColumnMapping::Role::Signal) ySpecs.emplace_back(col, c);
    }
    if (xCol < 0) {
        if (errorOut) *errorOut = "Profile has no X-time column mapping";
        return out;
    }

    auto toNs = [&](const QString& s) -> TimestampNs {
        QString t = s;
        if (profile.decimalSeparator != ".") t.replace(profile.decimalSeparator, ".");
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (!ok) return 0;
        if (xUnit.compare("ms", Qt::CaseInsensitive) == 0) return static_cast<TimestampNs>(v * 1e6);
        if (xUnit.compare("us", Qt::CaseInsensitive) == 0
            || xUnit == QString::fromUtf8("µs")) return static_cast<TimestampNs>(v * 1e3);
        if (xUnit.compare("ns", Qt::CaseInsensitive) == 0) return static_cast<TimestampNs>(v);
        // default: seconds
        return static_cast<TimestampNs>(v * 1e9);
    };

    auto toVal = [&](const QString& s) -> double {
        QString t = s;
        if (profile.decimalSeparator != ".") t.replace(profile.decimalSeparator, ".");
        bool ok = false;
        const double v = t.toDouble(&ok);
        return ok ? v : 0.0;
    };

    std::vector<TimestampNs> ts;
    ts.reserve(rows_.size());
    std::vector<std::vector<double>> ys(ySpecs.size());
    for (auto& y : ys) y.reserve(rows_.size());

    for (int row = dataStartRow; row < rowCount(); ++row) {
        const auto& r = rows_[row];
        if (xCol >= r.size()) continue;
        ts.push_back(toNs(r[xCol]));
        for (std::size_t k = 0; k < ySpecs.size(); ++k) {
            const int col = ySpecs[k].first;
            ys[k].push_back(col < r.size() ? toVal(r[col]) : 0.0);
        }
    }

    for (std::size_t k = 0; k < ySpecs.size(); ++k) {
        Signal::Meta m;
        m.name = ySpecs[k].second.signalName.isEmpty()
                 ? QString("Col%1").arg(colLabel(ySpecs[k].first))
                 : ySpecs[k].second.signalName;
        m.unit = ySpecs[k].second.unit;
        m.dataType = DataType::Float64;
        m.sourceSymbol = QString::fromStdString(path_.filename().string())
                       + ":" + colLabel(ySpecs[k].first);
        auto sig = std::make_shared<Signal>(m);
        sig->append(ts.data(),
                    reinterpret_cast<const std::byte*>(ys[k].data()),
                    ys[k].size());
        out.push_back(std::move(sig));
    }

    return out;
}

}  // namespace scope::converter
