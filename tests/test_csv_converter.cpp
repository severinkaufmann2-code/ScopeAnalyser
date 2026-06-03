#include "scope/converter/CsvSource.h"
#include "scope/converter/ConverterProfile.h"
#include "scope/core/Signal.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace scope::converter;
using namespace scope::core;

TEST(CsvConverter, ImportsHeaderedFile) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv.csv";
    {
        std::ofstream f(path);
        f << "t_s,speed,torque\n";
        f << "0.0,0.0,0.0\n";
        f << "0.1,10.0,1.5\n";
        f << "0.2,20.0,3.0\n";
        f << "0.3,30.0,4.5\n";
    }

    CsvSource src(path);
    EXPECT_EQ(src.rowCount(), 5);
    EXPECT_EQ(src.columnCount(), 3);

    ConverterProfile p;
    p.sourceType = "csv";
    p.headerRow  = 1;
    p.decimalSeparator = ".";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",       "s"},
        {"B", ColumnMapping::Role::Signal, "Speed",  "rpm"},
        {"C", ColumnMapping::Role::Signal, "Torque", "Nm"},
    };

    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 2u) << err.toStdString();

    auto findByName = [&](const QString& name) -> std::shared_ptr<Signal> {
        for (const auto& s : sigs) if (s->meta().name == name) return s;
        return nullptr;
    };

    auto speed = findByName("Speed");
    auto torque = findByName("Torque");
    ASSERT_TRUE(speed);
    ASSERT_TRUE(torque);
    EXPECT_EQ(speed->sampleCount(), 4u);
    EXPECT_EQ(torque->sampleCount(), 4u);
    EXPECT_EQ(speed->meta().unit, "rpm");
    EXPECT_EQ(torque->meta().unit, "Nm");

    auto vs = speed->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[3], 30.0);

    std::filesystem::remove(path);
}

TEST(CsvConverter, HandlesEuropeanDecimals) {
    auto path = std::filesystem::temp_directory_path() / "scope_test_csv_eu.csv";
    {
        std::ofstream f(path);
        f << "tijd;snelheid\n";
        f << "0,0;0,0\n";
        f << "0,1;1,5\n";
    }
    CsvSource src(path, ';');
    EXPECT_EQ(src.columnCount(), 2);

    ConverterProfile p;
    p.headerRow = 1;
    p.decimalSeparator = ",";
    p.columns = {
        {"A", ColumnMapping::Role::XTime,  "",         "s"},
        {"B", ColumnMapping::Role::Signal, "Snelheid", "kph"},
    };
    QString err;
    auto sigs = src.apply(p, &err);
    ASSERT_EQ(sigs.size(), 1u) << err.toStdString();
    auto vs = sigs[0]->readAsDouble();
    EXPECT_DOUBLE_EQ(vs[1], 1.5);

    std::filesystem::remove(path);
}
