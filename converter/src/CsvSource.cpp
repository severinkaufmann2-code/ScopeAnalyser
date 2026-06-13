#include "scope/converter/CsvSource.h"

#include <QFile>
#include <QTextStream>

namespace scope::converter {

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

}  // namespace

CsvSource::CsvSource(const std::filesystem::path& path,
                     QString columnDelimiter,
                     QString rowDelimiter,
                     QChar /*decimal*/) {
    sourceLabel_ = QString::fromStdString(path.filename().string());

    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly)) return;

    const QChar colChar = columnDelimiter.isEmpty() ? QChar(',') : columnDelimiter[0];

    // Lines starting with `#` are treated as comments and skipped — this
    // matches the `# scope-csv: …` metadata header written by writeCsv
    // and the convention pandas / NumPy / R use. Avoids polluting the
    // mapping panel's preview with a meaningless first row.
    auto isComment = [](const QString& s) {
        const QString t = s.trimmed();
        return !t.isEmpty() && t[0] == QChar('#');
    };

    QTextStream ts(&f);
    if (rowDelimiter == "\n" || rowDelimiter == "\r\n") {
        while (!ts.atEnd()) {
            const QString line = ts.readLine();
            if (line.isEmpty() && ts.atEnd()) break;
            if (isComment(line)) continue;
            rows_.push_back(splitCsv(line, colChar));
        }
    } else {
        const QString all = ts.readAll();
        const auto parts = all.split(rowDelimiter, Qt::SkipEmptyParts);
        for (const auto& part : parts) {
            if (isComment(part)) continue;
            rows_.push_back(splitCsv(part, colChar));
        }
    }
}

CsvSource::~CsvSource() = default;

}  // namespace scope::converter
