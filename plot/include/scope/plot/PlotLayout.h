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
    // For formula-derived channels (Analyser): the right-hand-side
    // expression that produced the channel, e.g. "Filter(speed, 0.05)".
    // Empty for recorded / imported channels — they re-appear in the
    // store on their own when the source file is reloaded. When a layout
    // is loaded and the channel name isn't in the store yet but `formula`
    // is non-empty, the host re-evaluates it through the FormulaEngine.
    QString formula;
    // "time" or "frequency". Hint for the Analyser's View combo so a
    // round-tripped layout puts the channel back in the right view.
    // Empty (or "time") on read = treat as time-domain.
    QString domain{"time"};
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
