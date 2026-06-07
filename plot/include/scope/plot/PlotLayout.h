#pragma once

#include <QHash>
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

// Persistable plot configuration. Two flavours:
//   v1 — single `axes` + `channels` list. The Analyser had one shared set
//        of Y axes regardless of view.
//   v2 — adds:
//        * viewMode + xyChannel — restore the View combo's selection on
//          reload, including which channel drives the XY view's X axis.
//        * axesByDomain / channelsByDomain — per-domain Y-axis sets
//          (the time-domain view and the frequency-domain view each
//          remember their own axes + channel→axis assignment).
//
// The top-level `axes` / `channels` fields stay populated even on v2 saves
// (mirrored from the active domain) so a v1 reader still sees the most
// relevant entries.
//
// Visibility is intentionally not persisted — it's purely a transient
// UI toggle.
struct PlotLayout {
    int     version{2};

    // v2+ — restored if the file is v2; otherwise default to "time" / "".
    QString viewMode{"time"};   // "time" | "frequency" | "xy"
    QString xyChannel;          // meaningful only when viewMode == "xy"

    // v2+ — per-domain axis sets + channel assignments. Key is "time" or
    // "frequency". An XY view borrows whichever domain matches its X
    // channel's domain (no separate "xy" entry).
    QHash<QString, QList<PlotLayoutAxis>>    axesByDomain;
    QHash<QString, QList<PlotLayoutChannel>> channelsByDomain;

    // v1 — single flat list. On v1 read this is the only data. On v2
    // read this is mirrored from the active domain so old tooling still
    // sees something coherent.
    QList<PlotLayoutAxis>    axes;
    QList<PlotLayoutChannel> channels;

    static PlotLayout loadFromFile(const std::filesystem::path& path,
                                   QString* errorOut = nullptr);
    bool saveToFile(const std::filesystem::path& path,
                    QString* errorOut = nullptr) const;

    // Embed-friendly variants. Used when a layout is round-tripped inside
    // another payload (e.g. the "layout" key in a chart CSV's scope-csv
    // metadata block). On malformed input, fromJsonString returns a
    // default-constructed PlotLayout and writes the error to *errorOut.
    QString          toJsonString() const;
    static PlotLayout fromJsonString(const QString& js,
                                     QString* errorOut = nullptr);

    // Return a copy that keeps only the axes / channels belonging to
    // `domain` ("time" | "frequency"). Used when a split chart export
    // writes a single-domain file per domain: each file then carries
    // only the layout that concerns the signals actually in it.
    //
    // viewMode is coerced to `domain` (a frequency-only file can't show
    // a time view, etc.), except an XY view is preserved when its X
    // channel still lives in the kept domain. The flat v1 mirror is
    // rebuilt from the kept domain.
    PlotLayout restrictedToDomain(const QString& domain) const;
};

}  // namespace scope::plot
