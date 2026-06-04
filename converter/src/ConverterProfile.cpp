#include "scope/converter/ConverterProfile.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace scope::converter {

using json = nlohmann::json;

namespace {
const char* roleName(ColumnMapping::Role r) {
    switch (r) {
        case ColumnMapping::Role::Ignore: return "ignore";
        case ColumnMapping::Role::XTime:  return "x_time";
        case ColumnMapping::Role::Signal: return "signal";
    }
    return "ignore";
}
ColumnMapping::Role roleFromString(const std::string& s) {
    if (s == "x_time") return ColumnMapping::Role::XTime;
    if (s == "signal") return ColumnMapping::Role::Signal;
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
        if (j.contains("columns")) {
            for (const auto& jc : j["columns"]) {
                ColumnMapping c;
                c.columnId   = QString::fromStdString(jc.value("col", std::string{}));
                c.role       = roleFromString(jc.value("role", std::string{"ignore"}));
                c.signalName = QString::fromStdString(jc.value("name", std::string{}));
                c.unit       = QString::fromStdString(jc.value("unit", std::string{}));
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
        j["columns"] = json::array();
        for (const auto& c : columns) {
            json jc;
            jc["col"]  = c.columnId.toStdString();
            jc["role"] = roleName(c.role);
            jc["name"] = c.signalName.toStdString();
            jc["unit"] = c.unit.toStdString();
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
