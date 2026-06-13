#include "scope/converter/JsonSource.h"

#include <nlohmann/json.hpp>

#include <QHash>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace scope::converter {

// ordered_json preserves object key order, so the grid's columns follow the
// file's column order (not nlohmann's default alphabetical sort) — what a user
// mapping by column letter expects.
using json = nlohmann::ordered_json;

namespace {

// One grid cell from a JSON scalar. Integers keep their exact digits; floats
// get full double round-trip precision. Non-scalars (null / object / array)
// become empty cells, which the apply() engine treats as "no sample here".
QString cellToStr(const json& v) {
    if (v.is_number_integer())  return QString::number(v.get<std::int64_t>());
    if (v.is_number_unsigned()) return QString::number(v.get<std::uint64_t>());
    if (v.is_number_float())    return QString::number(v.get<double>(), 'g', 17);
    if (v.is_boolean())         return v.get<bool>() ? QStringLiteral("1")
                                                     : QStringLiteral("0");
    if (v.is_string())          return QString::fromStdString(v.get<std::string>());
    return QString();
}

}  // namespace

JsonSource::JsonSource(const std::filesystem::path& path) {
    sourceLabel_ = QString::fromStdString(path.filename().string());

    json root;
    {
        std::ifstream is(path);
        if (!is) throw std::runtime_error("Couldn't open file.");
        is >> root;   // throws nlohmann::json::parse_error on invalid JSON
    }

    // ---- Native scope-json: one t_/value column pair per signal ------
    if (root.is_object() && root.contains("signals")
        && root["signals"].is_array()) {
        QStringList header;
        std::vector<const json*> colArrays;  // alternating t[], v[]
        std::vector<bool> hzScale;           // ÷1e9 a freq t column
        std::size_t maxLen = 0;
        for (const auto& js : root["signals"]) {
            if (!js.is_object()) continue;
            const auto tIt = js.find("t");
            const auto vIt = js.find("v");
            if (tIt == js.end() || vIt == js.end()
                || !tIt->is_array() || !vIt->is_array()) continue;
            const QString name =
                QString::fromStdString(js.value("name", std::string{}));
            const bool freq =
                js.value("domain", std::string("time")) == "frequency";
            // Time keeps raw int64 ns (X-unit "ns" on re-map); frequency is
            // shown in Hz (the stored field is Hz×1e9) so X-unit "Hz" maps it
            // back. Matches profileFromScopeJson's pre-fill.
            header << (freq ? QStringLiteral("f_") + name
                            : QStringLiteral("t_") + name)
                   << name;
            colArrays.push_back(&(*tIt)); hzScale.push_back(freq);
            colArrays.push_back(&(*vIt)); hzScale.push_back(false);
            maxLen = std::max({maxLen, tIt->size(), vIt->size()});
        }
        if (!header.isEmpty()) {
            rows_.push_back(header);
            for (std::size_t i = 0; i < maxLen; ++i) {
                QStringList row;
                for (std::size_t c = 0; c < colArrays.size(); ++c) {
                    const auto& arr = *colArrays[c];
                    if (i >= arr.size()) { row << QString(); continue; }
                    if (hzScale[c] && arr[i].is_number())
                        row << QString::number(arr[i].get<double>() / 1e9, 'g', 17);
                    else
                        row << cellToStr(arr[i]);
                }
                rows_.push_back(row);
            }
            return;
        }
        // "signals" present but unusable → fall through to generic handling.
    }

    // ---- Generic flatteners -----------------------------------------
    auto flattenRecords = [&](const json& arr) {
        QStringList keys;
        QHash<QString, int> keyIndex;
        for (const auto& rec : arr) {
            if (!rec.is_object()) continue;
            for (auto it = rec.begin(); it != rec.end(); ++it) {
                const QString k = QString::fromStdString(it.key());
                if (!keyIndex.contains(k)) { keyIndex.insert(k, keys.size()); keys << k; }
            }
        }
        if (keys.isEmpty()) return false;
        rows_.push_back(keys);
        for (const auto& rec : arr) {
            if (!rec.is_object()) continue;
            QStringList row;
            row.reserve(keys.size());
            for (int i = 0; i < keys.size(); ++i) row << QString();
            for (auto it = rec.begin(); it != rec.end(); ++it) {
                const int idx = keyIndex.value(QString::fromStdString(it.key()), -1);
                if (idx >= 0) row[idx] = cellToStr(it.value());
            }
            rows_.push_back(row);
        }
        return true;
    };

    auto flattenColumnar = [&](const json& obj) {
        QStringList keys;
        std::vector<const json*> arrs;
        std::size_t maxLen = 0;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it.value().is_array()) continue;
            keys << QString::fromStdString(it.key());
            arrs.push_back(&it.value());
            maxLen = std::max(maxLen, it.value().size());
        }
        if (keys.isEmpty()) return false;
        rows_.push_back(keys);
        for (std::size_t i = 0; i < maxLen; ++i) {
            QStringList row;
            row.reserve(static_cast<int>(arrs.size()));
            for (const auto* a : arrs)
                row << (i < a->size() ? cellToStr((*a)[i]) : QString());
            rows_.push_back(row);
        }
        return true;
    };

    bool built = false;
    if (root.is_array()) {
        if (!root.empty() && root.front().is_object()) {
            built = flattenRecords(root);
        } else if (!root.empty() && root.front().is_array()) {
            std::size_t maxCols = 0;
            for (const auto& r : root)
                if (r.is_array()) maxCols = std::max(maxCols, r.size());
            QStringList header;
            for (std::size_t c = 0; c < maxCols; ++c)
                header << QString("col%1").arg(c + 1);
            rows_.push_back(header);
            for (const auto& r : root) {
                if (!r.is_array()) continue;
                QStringList row;
                for (std::size_t c = 0; c < maxCols; ++c)
                    row << (c < r.size() ? cellToStr(r[c]) : QString());
                rows_.push_back(row);
            }
            built = maxCols > 0;
        } else if (!root.empty()) {
            rows_.push_back(QStringList{QStringLiteral("value")});
            for (const auto& v : root) rows_.push_back(QStringList{cellToStr(v)});
            built = true;
        }
    } else if (root.is_object()) {
        // An array-of-objects nested one level under a key (e.g. {"data":[…]}).
        const json* recArr = nullptr;
        for (auto it = root.begin(); it != root.end(); ++it)
            if (it.value().is_array() && !it.value().empty()
                && it.value().front().is_object()) { recArr = &it.value(); break; }
        built = recArr ? flattenRecords(*recArr) : flattenColumnar(root);
    }

    if (!built || rows_.size() < 2)
        throw std::runtime_error(
            "JSON has no recognisable table of values. Expected scope-json, an "
            "array of objects, or an object of equal-length arrays.");
}

JsonSource::~JsonSource() = default;

}  // namespace scope::converter
