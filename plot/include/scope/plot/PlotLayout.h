#pragma once

#include <QList>
#include <QString>

#include <filesystem>

namespace scope::plot {

struct PlotLayoutAxis {
    QString label;
    QString side{"left"};   // "left" or "right"
    bool    hasRange{false};
    double  min{0};
    double  max{0};
};

struct PlotLayoutChannel {
    QString name;
    int     axisIndex{0};
};

// Persistable plot configuration: the set of Y axes (label / side / optional
// range) and the channel→axis assignment. Visibility is intentionally not
// persisted — it's purely a transient UI toggle.
struct PlotLayout {
    int version{1};
    QList<PlotLayoutAxis>    axes;
    QList<PlotLayoutChannel> channels;

    static PlotLayout loadFromFile(const std::filesystem::path& path,
                                   QString* errorOut = nullptr);
    bool saveToFile(const std::filesystem::path& path,
                    QString* errorOut = nullptr) const;
};

}  // namespace scope::plot
