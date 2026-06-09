#include "scope/converter/ConverterProfile.h"

#include <nlohmann/json.hpp>

#include <QFile>
#include <QStringList>
#include <QTextStream>

#include <array>
#include <fstream>
#include <optional>

namespace scope::converter {

using json = nlohmann::json;

namespace {
const char* roleName(ColumnMapping::Role r) {
    switch (r) {
        case ColumnMapping::Role::Ignore:     return "ignore";
        case ColumnMapping::Role::XTime:      return "x_time";
        case ColumnMapping::Role::XFrequency: return "x_frequency";
        case ColumnMapping::Role::Signal:     return "signal";
    }
    return "ignore";
}
ColumnMapping::Role roleFromString(const std::string& s) {
    if (s == "x_time")      return ColumnMapping::Role::XTime;
    if (s == "x_frequency") return ColumnMapping::Role::XFrequency;
    if (s == "signal")      return ColumnMapping::Role::Signal;
    return ColumnMapping::Role::Ignore;
}
}

ConverterProfile ConverterProfile::loadFromFile(const std::filesystem::path& path,
                                                QString* errorOut) {
    ConverterProfile p;
    try {
        std::ifstream f(path);
        json j; f >> j;
        p.version = j.value("version", 1);
        p.sourceType = QString::fromStdString(j.value("source", std::string{}));
        p.sheet = QString::fromStdString(j.value("sheet", std::string{}));
        p.range = QString::fromStdString(j.value("range", std::string{}));
        p.headerRow = j.value("headerRow", 1);
        p.decimalSeparator = QString::fromStdString(j.value("decimal", std::string{"."}));
        p.columnDelimiter = QString::fromStdString(j.value("columnDelimiter", std::string{","}));
        p.rowDelimiter    = QString::fromStdString(j.value("rowDelimiter",    std::string{"\n"}));
        p.useSampleRate   = j.value("useSampleRate", false);
        p.sampleRateHz    = j.value("sampleRateHz", 0.0);
        p.sampleRateDisplayUnit = QString::fromStdString(
            j.value("sampleRateUnit", std::string{"ms"}));
        if (j.contains("columns")) {
            for (const auto& jc : j["columns"]) {
                ColumnMapping c;
                c.columnId   = QString::fromStdString(jc.value("col", std::string{}));
                c.role       = roleFromString(jc.value("role", std::string{"ignore"}));
                c.signalName = QString::fromStdString(jc.value("name", std::string{}));
                c.unit       = QString::fromStdString(jc.value("unit", std::string{}));
                c.rowStart   = jc.value("rowStart", -1);
                c.rowEnd     = jc.value("rowEnd", -1);
                c.xSourceColumn         = QString::fromStdString(jc.value("xSourceColumn", std::string{}));
                c.useSampleRate         = jc.value("useSampleRate", false);
                c.sampleRateHz          = jc.value("sampleRateHz", 0.0);
                c.sampleRateDisplayUnit = QString::fromStdString(
                    jc.value("sampleRateUnit", std::string{"ms"}));
                c.resetTimeToZero       = jc.value("resetTimeToZero", false);
                c.timeOffsetSec         = jc.value("timeOffsetSec", 0.0);
                const auto cdStr = QString::fromStdString(
                    jc.value("collapseDuplicates", std::string{"none"}));
                if      (cdStr == "first") c.collapseDuplicates = ColumnMapping::CollapseMode::First;
                else if (cdStr == "last")  c.collapseDuplicates = ColumnMapping::CollapseMode::Last;
                else if (cdStr == "mean")  c.collapseDuplicates = ColumnMapping::CollapseMode::Mean;
                else                       c.collapseDuplicates = ColumnMapping::CollapseMode::None;
                const auto vpStr = QString::fromStdString(
                    jc.value("collapseValuePlateaus", std::string{"none"}));
                if      (vpStr == "first") c.collapseValuePlateaus = ColumnMapping::ValuePlateauMode::KeepFirst;
                else if (vpStr == "last")  c.collapseValuePlateaus = ColumnMapping::ValuePlateauMode::KeepLast;
                else                       c.collapseValuePlateaus = ColumnMapping::ValuePlateauMode::None;
                p.columns.push_back(std::move(c));
            }
        }
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
    }
    return p;
}

bool ConverterProfile::saveToFile(const std::filesystem::path& path,
                                  QString* errorOut) const {
    try {
        json j;
        j["version"] = version;
        j["source"]  = sourceType.toStdString();
        j["sheet"]   = sheet.toStdString();
        j["range"]   = range.toStdString();
        j["headerRow"] = headerRow;
        j["decimal"] = decimalSeparator.toStdString();
        j["columnDelimiter"] = columnDelimiter.toStdString();
        j["rowDelimiter"]    = rowDelimiter.toStdString();
        j["useSampleRate"]   = useSampleRate;
        j["sampleRateHz"]    = sampleRateHz;
        j["sampleRateUnit"]  = sampleRateDisplayUnit.toStdString();
        j["columns"] = json::array();
        for (const auto& c : columns) {
            json jc;
            jc["col"]  = c.columnId.toStdString();
            jc["role"] = roleName(c.role);
            jc["name"] = c.signalName.toStdString();
            jc["unit"] = c.unit.toStdString();
            jc["rowStart"] = c.rowStart;
            jc["rowEnd"]   = c.rowEnd;
            jc["xSourceColumn"]   = c.xSourceColumn.toStdString();
            jc["useSampleRate"]   = c.useSampleRate;
            jc["sampleRateHz"]    = c.sampleRateHz;
            jc["sampleRateUnit"]  = c.sampleRateDisplayUnit.toStdString();
            jc["resetTimeToZero"] = c.resetTimeToZero;
            jc["timeOffsetSec"]   = c.timeOffsetSec;
            const char* cd = "none";
            switch (c.collapseDuplicates) {
                case ColumnMapping::CollapseMode::First: cd = "first"; break;
                case ColumnMapping::CollapseMode::Last:  cd = "last";  break;
                case ColumnMapping::CollapseMode::Mean:  cd = "mean";  break;
                default: break;
            }
            jc["collapseDuplicates"] = cd;
            const char* vp = "none";
            switch (c.collapseValuePlateaus) {
                case ColumnMapping::ValuePlateauMode::KeepFirst: vp = "first"; break;
                case ColumnMapping::ValuePlateauMode::KeepLast:  vp = "last";  break;
                default: break;
            }
            jc["collapseValuePlateaus"] = vp;
            j["columns"].push_back(std::move(jc));
        }
        std::ofstream f(path);
        f << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
//
// Convert a 0-based column index to its A / B / … / AA / AB / … letter,
// matching the convention CsvSource / MappingPanel use everywhere else.
namespace {
QString columnLetter(int index) {
    QString s;
    int n = index;
    while (true) {
        s.prepend(QChar('A' + (n % 26)));
        n = n / 26 - 1;
        if (n < 0) break;
    }
    return s;
}
}

bool profileFromScopeMetadata(const std::filesystem::path& path,
                              ConverterProfile* out,
                              QString* errorOut) {
    if (!out) return false;
    try {
        std::ifstream f(path);
        if (!f) {
            if (errorOut) *errorOut = "Couldn't open file.";
            return false;
        }
        std::string line;
        // First non-blank line wins. We keep this loop simple: peek up
        // to ~5 lines so a leading BOM line or stray empty line doesn't
        // throw us off, then give up.
        std::string firstNonBlank;
        for (int i = 0; i < 5 && std::getline(f, line); ++i) {
            std::string trimmed = line;
            while (!trimmed.empty()
                   && (trimmed.back() == '\r' || trimmed.back() == ' '
                    || trimmed.back() == '\t' || trimmed.back() == '\n'))
                trimmed.pop_back();
            std::size_t start = 0;
            while (start < trimmed.size()
                   && (trimmed[start] == ' ' || trimmed[start] == '\t'))
                ++start;
            if (start < trimmed.size()) {
                firstNonBlank = trimmed.substr(start);
                break;
            }
        }
        static const std::string kPrefix = "# scope-csv:";
        if (firstNonBlank.size() < kPrefix.size()
            || firstNonBlank.compare(0, kPrefix.size(), kPrefix) != 0) {
            return false;   // no metadata, caller falls back to manual mapping
        }
        std::string jsonText = firstNonBlank.substr(kPrefix.size());
        // Strip leading whitespace inside the comment.
        std::size_t js = 0;
        while (js < jsonText.size()
               && (jsonText[js] == ' ' || jsonText[js] == '\t')) ++js;
        const auto root = json::parse(jsonText.substr(js));

        ConverterProfile prof;
        prof.sourceType = "csv";
        prof.headerRow  = 1;
        prof.decimalSeparator = ".";
        prof.columnDelimiter  = ",";
        prof.rowDelimiter     = "\n";

        const auto& arr = root.value("columns", json::array());

        // Two-pass build: pass 1 figures out each column's letter and
        // role/unit/name (signals carry a `refX` index pointing back at
        // their X column); pass 2 fills in xSourceColumn now that we
        // have the letter table.
        struct Tmp {
            QString letter;
            ColumnMapping::Role role{ColumnMapping::Role::Ignore};
            QString unit;
            QString signalName;
            int     refX{-1};
            QString sourceSymbol;
        };
        std::vector<Tmp> tmps;
        tmps.reserve(arr.size());
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const auto& jc = arr[i];
            Tmp t;
            t.letter = columnLetter(static_cast<int>(i));
            t.role   = roleFromString(jc.value("role", std::string("signal")));
            t.unit   = QString::fromStdString(jc.value("unit", std::string{}));
            t.signalName  = QString::fromStdString(jc.value("name", std::string{}));
            t.sourceSymbol= QString::fromStdString(jc.value("src", std::string{}));
            t.refX        = jc.value("refX", -1);
            tmps.push_back(std::move(t));
        }
        for (const auto& t : tmps) {
            ColumnMapping cm;
            cm.columnId   = t.letter;
            cm.role       = t.role;
            cm.unit       = t.unit;
            cm.signalName = t.signalName;
            if (cm.role == ColumnMapping::Role::Signal
                && t.refX >= 0
                && t.refX < static_cast<int>(tmps.size())) {
                cm.xSourceColumn = tmps[t.refX].letter;
            }
            prof.columns.push_back(std::move(cm));
        }
        *out = std::move(prof);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        return false;
    }
}

// ---- TwinCAT Scope CSV ------------------------------------------------------
namespace {

bool looksNumeric(const QString& cell) {
    QString t = cell.trimmed();
    if (t.isEmpty()) return false;
    t.replace(',', '.');   // accept either decimal mark while sniffing
    bool ok = false;
    t.toDouble(&ok);
    return ok;
}

// TwinCAT writes "(None)" for a unitless channel; treat that as no unit.
QString stripTwinCatUnit(const QString& u) {
    const QString t = u.trimmed();
    if (t.isEmpty() || t.compare("(None)", Qt::CaseInsensitive) == 0)
        return QString();
    return t;
}

}  // namespace

std::optional<TwinCatScopeLayout> detectTwinCatScopeCsv(
    const std::filesystem::path& path) {
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;
    QTextStream ts(&f);

    // The preamble + per-channel header block is ~25 rows; the first data
    // row always lands within the first few dozen lines. Read 100 to be safe.
    QStringList raw;
    raw.reserve(100);
    for (int i = 0; i < 100 && !ts.atEnd(); ++i) raw.push_back(ts.readLine());
    if (raw.isEmpty()) return std::nullopt;

    // Pick the delimiter that exposes the channel "Name" row — col0 == "Name"
    // && col2 == "Name" (the repeated per-channel key pattern, distinct from
    // the single "Name<sep><export-name>" preamble line). Take the delimiter
    // giving the widest such row.
    const std::array<QChar, 3> delims{QChar('\t'), QChar(';'), QChar(',')};
    QChar delim('\t');
    int   nameRowIdx = -1;
    int   bestCols   = 0;
    for (QChar d : delims) {
        for (int i = 0; i < raw.size(); ++i) {
            const QStringList c = raw[i].split(d);
            if (c.size() >= 4 && c[0].trimmed() == "Name"
                              && c[2].trimmed() == "Name"
                              && c.size() > bestCols) {
                bestCols   = c.size();
                delim      = d;
                nameRowIdx = i;
            }
        }
    }
    if (nameRowIdx < 0) return std::nullopt;

    std::vector<QStringList> rows;
    rows.reserve(raw.size());
    for (const auto& line : raw) rows.push_back(line.split(delim));

    auto findKeyRow = [&](const QString& key) -> int {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (!rows[i].isEmpty() && rows[i][0].trimmed() == key) return i;
        return -1;
    };
    const int sampleRowIdx = findKeyRow("SampleTime[ms]");
    if (sampleRowIdx < 0) return std::nullopt;   // confirms TwinCAT Scope
    const int unitRowIdx = findKeyRow("Unit");

    // Data starts at the first row after the name row whose first cell is a
    // number (every header row carries a text key in column 0).
    int dataStartRow = -1;
    for (int i = nameRowIdx + 1; i < static_cast<int>(rows.size()); ++i) {
        if (rows[i].isEmpty()) continue;
        if (looksNumeric(rows[i][0])) { dataStartRow = i; break; }
    }
    if (dataStartRow < 0) return std::nullopt;

    // Decimal mark: a comma in a value cell of the first data row (when the
    // delimiter isn't itself a comma) means a comma-decimal export.
    QChar decimal('.');
    if (delim != QChar(',')) {
        for (const QString& cell : rows[dataStartRow])
            if (cell.contains(',')) { decimal = QChar(','); break; }
    }

    TwinCatScopeLayout layout;
    layout.delimiter    = delim;
    layout.decimal      = decimal;
    layout.nameRow      = nameRowIdx;
    layout.dataStartRow = dataStartRow;

    const QStringList& nameRow = rows[nameRowIdx];
    const int nChannels = nameRow.size() / 2;
    for (int j = 0; j < nChannels; ++j) {
        const int vcol = 2 * j + 1;
        TwinCatScopeChannel ch;
        ch.name = (vcol < nameRow.size()) ? nameRow[vcol].trimmed() : QString();
        if (ch.name.isEmpty()) continue;   // unnamed pair → skip
        ch.timeCol  = 2 * j;
        ch.valueCol = vcol;
        if (unitRowIdx >= 0 && vcol < rows[unitRowIdx].size())
            ch.unit = stripTwinCatUnit(rows[unitRowIdx][vcol]);
        if (vcol < rows[sampleRowIdx].size()) {
            bool ok = false;
            const double smp = rows[sampleRowIdx][vcol].trimmed().toDouble(&ok);
            if (ok) ch.sampleTimeMs = smp;
        }
        layout.channels.push_back(std::move(ch));
    }
    if (layout.channels.empty()) return std::nullopt;
    return layout;
}

bool profileFromTwinCatScope(const std::filesystem::path& path,
                             ConverterProfile* out,
                             QString* errorOut) {
    if (!out) return false;
    auto layout = detectTwinCatScopeCsv(path);
    if (!layout) {
        if (errorOut) *errorOut = "Not a TwinCAT Scope export.";
        return false;
    }
    ConverterProfile prof;
    prof.sourceType       = "csv";
    prof.columnDelimiter  = QString(layout->delimiter);
    // Keep CsvSource's line-by-line branch (so blank lines stay as rows and
    // the data-start index lines up); readLine tolerates LF or CRLF.
    prof.rowDelimiter     = "\r\n";
    prof.decimalSeparator = QString(layout->decimal);
    prof.headerRow        = layout->dataStartRow;   // 0-based first data row
    for (const auto& ch : layout->channels) {
        // Pin each column's data range explicitly to the first data row so it
        // shows as e.g. "25:*" in the mapping table (not the misleading
        // "all", which only skips the header block via the Header-row
        // default). Keeping the range in the visible per-column mapping also
        // means the generic CsvSource import doesn't depend on any
        // out-of-band header-row state.
        ColumnMapping x;
        x.columnId = columnLetter(ch.timeCol);
        x.role     = ColumnMapping::Role::XTime;
        x.unit     = "ms";
        x.rowStart = layout->dataStartRow;
        prof.columns.push_back(std::move(x));

        ColumnMapping y;
        y.columnId      = columnLetter(ch.valueCol);
        y.role          = ColumnMapping::Role::Signal;
        y.signalName    = ch.name;
        y.unit          = ch.unit;
        y.xSourceColumn = columnLetter(ch.timeCol);
        y.rowStart      = layout->dataStartRow;
        prof.columns.push_back(std::move(y));
    }
    *out = std::move(prof);
    return true;
}

}  // namespace scope::converter
