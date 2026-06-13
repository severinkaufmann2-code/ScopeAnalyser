#pragma once

#include "scope/converter/TabularSource.h"

#include <filesystem>

namespace scope::converter {

// JSON file source for the Converter's "edit / convert" flow. Flattens the
// file into the shared tabular grid so it maps exactly like a CSV. Handles:
//   • native scope-json  ({"signals":[{name,unit,domain,t[],v[]}]})  → one
//     t_/value column pair per signal (pre-fillable via profileFromScopeJson),
//   • array of record objects  ([{…},{…}])  → union-of-keys columns,
//   • columnar object of arrays  ({"t":[…],"a":[…]}),  (also when the array
//     of records is nested one level under a key),
//   • array of arrays / array of scalars.
// Throws std::runtime_error if the file isn't valid JSON or holds no table.
class JsonSource : public TabularSource {
public:
    explicit JsonSource(const std::filesystem::path& path);
    ~JsonSource() override;

    QString id() const override { return "json"; }
    QString displayName() const override { return "JavaScript Object Notation (.json)"; }
};

}  // namespace scope::converter
