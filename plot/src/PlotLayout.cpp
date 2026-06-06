#include "scope/plot/PlotLayout.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace scope::plot {

using json = nlohmann::json;

PlotLayout PlotLayout::loadFromFile(const std::filesystem::path& path,
                                    QString* errorOut) {
    PlotLayout p;
    try {
        std::ifstream f(path);
        json j; f >> j;
        p.version = j.value("version", 1);
        if (j.contains("axes")) {
            for (const auto& ja : j["axes"]) {
                PlotLayoutAxis a;
                a.label = QString::fromStdString(ja.value("label", std::string{}));
                a.side  = QString::fromStdString(ja.value("side",  std::string{"left"}));
                if (ja.contains("min") && ja.contains("max")) {
                    a.min = ja.value("min", 0.0);
                    a.max = ja.value("max", 0.0);
                    a.hasRange = a.max > a.min;
                }
                p.axes.append(std::move(a));
            }
        }
        if (j.contains("channels")) {
            for (const auto& jc : j["channels"]) {
                PlotLayoutChannel c;
                c.name      = QString::fromStdString(jc.value("name",    std::string{}));
                c.axisIndex = jc.value("axis", 0);
                c.formula   = QString::fromStdString(jc.value("formula", std::string{}));
                c.domain    = QString::fromStdString(jc.value("domain",  std::string{"time"}));
                p.channels.append(std::move(c));
            }
        }
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
    }
    return p;
}

bool PlotLayout::saveToFile(const std::filesystem::path& path,
                            QString* errorOut) const {
    try {
        json j;
        j["version"] = version;
        j["axes"] = json::array();
        for (const auto& a : axes) {
            json ja;
            ja["label"] = a.label.toStdString();
            ja["side"]  = a.side.toStdString();
            if (a.hasRange) {
                ja["min"] = a.min;
                ja["max"] = a.max;
            }
            j["axes"].push_back(std::move(ja));
        }
        j["channels"] = json::array();
        for (const auto& c : channels) {
            json jc;
            jc["name"] = c.name.toStdString();
            jc["axis"] = c.axisIndex;
            if (!c.formula.isEmpty()) jc["formula"] = c.formula.toStdString();
            if (!c.domain.isEmpty() && c.domain != "time")
                jc["domain"] = c.domain.toStdString();
            j["channels"].push_back(std::move(jc));
        }
        std::ofstream f(path);
        f << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        return false;
    }
}

}  // namespace scope::plot
