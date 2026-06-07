#include "scope/converter/ConverterProfile.h"

#include <nlohmann/json.hpp>

#include <fstream>

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

}  // namespace scope::converter
