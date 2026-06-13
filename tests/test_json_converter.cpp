#include "scope/converter/JsonSource.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/core/Signal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace scope::converter;
using namespace scope::core;

namespace {
void writeFile(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p);
    f << s;
}
std::shared_ptr<Signal> byName(const std::vector<std::shared_ptr<Signal>>& v,
                               const QString& n) {
    for (const auto& s : v) if (s->meta().name == n) return s;
    return nullptr;
}
}  // namespace

// An array of record objects flattens to union-of-keys columns, in file order
// (ordered_json), and maps exactly like a CSV.
TEST(JsonConverter, RecordsArray) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_records.json";
    writeFile(path, R"([
        {"t": 0.0, "speed": 0.0,  "torque": 1.0},
        {"t": 0.1, "speed": 10.0, "torque": 1.5},
        {"t": 0.2, "speed": 20.0, "torque": 2.0}
    ])");

    JsonSource src(path);
    EXPECT_EQ(src.columnCount(), 3);
    EXPECT_EQ(src.rowCount(), 4);   // header + 3 data rows

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",       "s"},
        {"B", ColumnMapping::Role::Signal, "Speed",  "rpm"},
        {"C", ColumnMapping::Role::Signal, "Torque", "Nm"},
    };

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 2u) << err.toStdString();
    auto speed = byName(sigs, "Speed");
    ASSERT_TRUE(speed);
    auto v = speed->snapshotForRead();
    ASSERT_EQ(v.count, 3u);
    EXPECT_EQ(v.timestamps[1], 100000000LL);   // 0.1 s → ns
    EXPECT_DOUBLE_EQ(speed->readAsDouble()[2], 20.0);
    EXPECT_EQ(speed->meta().unit, "rpm");
}

// A columnar object of equal-length arrays flattens to one column per key.
TEST(JsonConverter, ColumnarObject) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_columnar.json";
    writeFile(path, R"({"t":[0.0,0.1,0.2],"a":[1.0,2.0,3.0]})");

    JsonSource src(path);
    EXPECT_EQ(src.columnCount(), 2);
    EXPECT_EQ(src.rowCount(), 4);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",  "s"},
        {"B", ColumnMapping::Role::Signal, "A", ""},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto v = sigs[0]->snapshotForRead();
    ASSERT_EQ(v.count, 3u);
    EXPECT_EQ(v.timestamps[2], 200000000LL);
    EXPECT_DOUBLE_EQ(sigs[0]->readAsDouble()[1], 2.0);
}

// An array of objects nested one level under a key is found and flattened.
TEST(JsonConverter, NestedRecordsUnderKey) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_nested.json";
    writeFile(path, R"({"meta":"x","data":[
        {"t":0.0,"v":5.0},
        {"t":0.5,"v":6.0}
    ]})");

    JsonSource src(path);
    EXPECT_EQ(src.columnCount(), 2);
    EXPECT_EQ(src.rowCount(), 3);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ".";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",  "s"},
        {"B", ColumnMapping::Role::Signal, "V", ""},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    EXPECT_EQ(sigs[0]->snapshotForRead().timestamps[1], 500000000LL);
}

// Native scope-json: profileFromScopeJson pre-fills the mapping, and feeding
// that profile back through JsonSource reproduces the signal (raw ns kept via
// the "ns" X-unit).
TEST(JsonConverter, NativePrefillRoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_native.json";
    writeFile(path, R"({"scope-json":1,"signals":[
        {"name":"Speed","unit":"rpm","domain":"time",
         "t":[0,100000000,200000000],"v":[0.0,10.0,20.0]}
    ]})");

    ConverterProfile prof;
    QString perr;
    ASSERT_TRUE(profileFromScopeJson(path, &prof, &perr)) << perr.toStdString();
    ASSERT_EQ(prof.columns.size(), 2u);
    EXPECT_EQ(prof.columns[0].role, ColumnMapping::Role::XTime);
    EXPECT_EQ(prof.columns[0].unit, "ns");
    EXPECT_EQ(prof.columns[1].role, ColumnMapping::Role::Signal);
    EXPECT_EQ(prof.columns[1].signalName, "Speed");
    EXPECT_EQ(prof.columns[1].xSourceColumn, "A");

    JsonSource src(path);
    QString err;
    auto sigs = src.apply(prof, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto v = sigs[0]->snapshotForRead();
    ASSERT_EQ(v.count, 3u);
    EXPECT_EQ(v.timestamps[1], 100000000LL);   // ns preserved through "ns" unit
    EXPECT_DOUBLE_EQ(sigs[0]->readAsDouble()[2], 20.0);
    EXPECT_EQ(sigs[0]->meta().name, "Speed");
    EXPECT_EQ(sigs[0]->meta().unit, "rpm");
}

// A non-native object whose values aren't arrays has no table → throws.
TEST(JsonConverter, RejectsNonTabular) {
    auto path = std::filesystem::temp_directory_path() / "scope_json_bad.json";
    writeFile(path, R"({"hello":"world","n":42})");
    EXPECT_THROW(JsonSource src(path), std::exception);
}
