#include "scope/plot/PlotLayout.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace scope::plot {

using json = nlohmann::json;

namespace {

PlotLayoutAxis parseAxis(const json& ja) {
    PlotLayoutAxis a;
    a.label = QString::fromStdString(ja.value("label", std::string{}));
    a.side  = QString::fromStdString(ja.value("side",  std::string{"left"}));
    if (ja.contains("min") && ja.contains("max")) {
        a.min = ja.value("min", 0.0);
        a.max = ja.value("max", 0.0);
        a.hasRange = a.max > a.min;
    }
    return a;
}

PlotLayoutChannel parseChannel(const json& jc) {
    PlotLayoutChannel c;
    c.name      = QString::fromStdString(jc.value("name",    std::string{}));
    c.axisIndex = jc.value("axis", 0);
    c.formula   = QString::fromStdString(jc.value("formula", std::string{}));
    c.domain    = QString::fromStdString(jc.value("domain",  std::string{"time"}));
    return c;
}

json axisToJson(const PlotLayoutAxis& a) {
    json ja;
    ja["label"] = a.label.toStdString();
    ja["side"]  = a.side.toStdString();
    if (a.hasRange) {
        ja["min"] = a.min;
        ja["max"] = a.max;
    }
    return ja;
}

json channelToJson(const PlotLayoutChannel& c) {
    json jc;
    jc["name"] = c.name.toStdString();
    jc["axis"] = c.axisIndex;
    if (!c.formula.isEmpty()) jc["formula"] = c.formula.toStdString();
    if (!c.domain.isEmpty() && c.domain != "time")
        jc["domain"] = c.domain.toStdString();
    return jc;
}

// Read a PlotLayout out of a parsed JSON object. Splits the load path
// so file IO and embed-in-CSV IO share the same v1/v2 decode logic.
PlotLayout layoutFromJson(const json& j) {
    PlotLayout p;
    p.version  = j.value("version", 1);
    p.viewMode = QString::fromStdString(
        j.value("viewMode", std::string{"time"}));
    p.xyChannel = QString::fromStdString(
        j.value("xyChannel", std::string{}));

    if (j.contains("axes")) {
        for (const auto& ja : j["axes"])
            p.axes.append(parseAxis(ja));
    }
    if (j.contains("channels")) {
        for (const auto& jc : j["channels"])
            p.channels.append(parseChannel(jc));
    }

    if (j.contains("axesByDomain") && j["axesByDomain"].is_object()) {
        for (const auto& [k, v] : j["axesByDomain"].items()) {
            QList<PlotLayoutAxis> list;
            for (const auto& ja : v) list.append(parseAxis(ja));
            p.axesByDomain.insert(QString::fromStdString(k), list);
        }
    }
    if (j.contains("channelsByDomain") && j["channelsByDomain"].is_object()) {
        for (const auto& [k, v] : j["channelsByDomain"].items()) {
            QList<PlotLayoutChannel> list;
            for (const auto& jc : v) list.append(parseChannel(jc));
            p.channelsByDomain.insert(QString::fromStdString(k), list);
        }
    }

    // v1 fallback: if we read a v1 blob (no per-domain maps), seed
    // axesByDomain["time"] / channelsByDomain["time"] from the legacy
    // flat lists so callers always see a uniform shape.
    if (p.axesByDomain.isEmpty() && !p.axes.isEmpty()) {
        p.axesByDomain.insert("time", p.axes);
    }
    if (p.channelsByDomain.isEmpty() && !p.channels.isEmpty()) {
        p.channelsByDomain.insert("time", p.channels);
    }
    return p;
}

// Build a JSON object from a PlotLayout. Mirror of layoutFromJson.
json layoutToJson(const PlotLayout& p) {
    json j;
    j["version"]   = p.version;
    j["viewMode"]  = p.viewMode.toStdString();
    if (!p.xyChannel.isEmpty()) j["xyChannel"] = p.xyChannel.toStdString();

    // Legacy flat lists — always written so a v1 reader sees data.
    // Source of truth: the active domain's per-domain list when v2
    // semantics are in play; otherwise the caller-supplied flat list.
    const QString activeDomain =
        (p.viewMode == "frequency") ? "frequency" : "time";
    const auto& flatAxes =
        p.axesByDomain.contains(activeDomain)
            ? p.axesByDomain.value(activeDomain) : p.axes;
    const auto& flatChans =
        p.channelsByDomain.contains(activeDomain)
            ? p.channelsByDomain.value(activeDomain) : p.channels;
    j["axes"]     = json::array();
    for (const auto& a : flatAxes) j["axes"].push_back(axisToJson(a));
    j["channels"] = json::array();
    for (const auto& c : flatChans) j["channels"].push_back(channelToJson(c));

    // v2 per-domain maps.
    json axesBy = json::object();
    for (auto it = p.axesByDomain.constBegin(); it != p.axesByDomain.constEnd(); ++it) {
        json arr = json::array();
        for (const auto& a : it.value()) arr.push_back(axisToJson(a));
        axesBy[it.key().toStdString()] = std::move(arr);
    }
    json chansBy = json::object();
    for (auto it = p.channelsByDomain.constBegin(); it != p.channelsByDomain.constEnd(); ++it) {
        json arr = json::array();
        for (const auto& c : it.value()) arr.push_back(channelToJson(c));
        chansBy[it.key().toStdString()] = std::move(arr);
    }
    j["axesByDomain"]     = std::move(axesBy);
    j["channelsByDomain"] = std::move(chansBy);
    return j;
}

}  // namespace

PlotLayout PlotLayout::loadFromFile(const std::filesystem::path& path,
                                    QString* errorOut) {
    PlotLayout p;
    try {
        std::ifstream f(path);
        json j; f >> j;
        p = layoutFromJson(j);
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
    }
    return p;
}

bool PlotLayout::saveToFile(const std::filesystem::path& path,
                            QString* errorOut) const {
    try {
        std::ofstream f(path);
        f << layoutToJson(*this).dump(2);
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        return false;
    }
}

QString PlotLayout::toJsonString() const {
    // Compact (no indent) — this gets embedded into a single-line
    // `# scope-csv: {...}` header where pretty-printing would just bloat
    // the file.
    return QString::fromStdString(layoutToJson(*this).dump());
}

PlotLayout PlotLayout::fromJsonString(const QString& js, QString* errorOut) {
    PlotLayout p;
    try {
        const auto j = json::parse(js.toStdString());
        p = layoutFromJson(j);
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
    }
    return p;
}

PlotLayout PlotLayout::restrictedToDomain(const QString& domain) const {
    PlotLayout r;
    r.version = version;

    if (axesByDomain.contains(domain))
        r.axesByDomain.insert(domain, axesByDomain.value(domain));
    if (channelsByDomain.contains(domain))
        r.channelsByDomain.insert(domain, channelsByDomain.value(domain));

    // Keep the XY view only if its X channel survives into this domain;
    // otherwise the file has nothing to drive an XY from, so fall back to
    // the domain's own straight view.
    bool keepXy = false;
    if (viewMode == "xy" && !xyChannel.isEmpty()) {
        for (const auto& c : r.channelsByDomain.value(domain)) {
            if (c.name == xyChannel) { keepXy = true; break; }
        }
    }
    if (keepXy) {
        r.viewMode  = "xy";
        r.xyChannel = xyChannel;
    } else {
        r.viewMode = domain;  // "time" | "frequency"
        r.xyChannel.clear();
    }

    // Rebuild the v1 flat mirror from the kept domain.
    r.axes     = r.axesByDomain.value(domain);
    r.channels = r.channelsByDomain.value(domain);
    return r;
}

void PlotLayout::stripFormulas() {
    for (auto& c : channels) c.formula.clear();
    for (auto& list : channelsByDomain)
        for (auto& c : list) c.formula.clear();
}

void PlotLayout::stripLayout() {
    axes.clear();
    axesByDomain.clear();
    for (auto& c : channels) c.axisIndex = 0;
    for (auto& list : channelsByDomain)
        for (auto& c : list) c.axisIndex = 0;
    viewMode = "time";
    xyChannel.clear();
}

}  // namespace scope::plot
