#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Base/ConfigReader.h"

namespace
{
    using Base::ConfigReader;

    struct LaneConfig
    {
        std::string name{};
        int32_t count{};
        CONFIG_FIELDS(LaneConfig, name, count)
    };

    struct TestConfig
    {
        uint16_t port{};
        bool verbose{};
        float tickSeconds{};
        std::vector<LaneConfig> lanes{};
        CONFIG_FIELDS(TestConfig, port, verbose, tickSeconds, lanes)
    };

    constexpr auto Valid = R"({
        // 주석은 허용한다
        "port": 7000,
        "verbose": true,
        "tickSeconds": 0.1, /* 100ms */
        "lanes": [ { "name": "zone", "count": 4 }, { "name": "db", "count": 2 } ]
    })";

    // 오류 글에 기대한 조각이 들어 있는지 본다(어디가 틀렸는지 알려 주는가).
    void ExpectError(const std::string_view text, const std::string_view expected)
    {
        const auto config = ConfigReader::Parse<TestConfig>(text, "test.json");
        ASSERT_FALSE(config.has_value()) << "should fail: " << text;
        EXPECT_NE(config.error().find("test.json: "), std::string::npos) << config.error();
        EXPECT_NE(config.error().find(expected), std::string::npos) << config.error();
    }
} // namespace

TEST(ConfigReader, ReadsNestedStructsWithComments)
{
    const auto config = ConfigReader::Parse<TestConfig>(Valid, "test.json");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->port, 7000);
    EXPECT_TRUE(config->verbose);
    EXPECT_FLOAT_EQ(config->tickSeconds, 0.1f); // float 로 담으면 0.1 과 조금 다르지만 같은 값으로 본다
    ASSERT_EQ(config->lanes.size(), 2u);
    EXPECT_EQ(config->lanes[1].name, "db");
    EXPECT_EQ(config->lanes[1].count, 2);
}

TEST(ConfigReader, IntegerForFloatFieldIsFine)
{
    const auto config = ConfigReader::Parse<TestConfig>(
        R"({ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [] })", "test.json");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_FLOAT_EQ(config->tickSeconds, 1.0f);
}

TEST(ConfigReader, MissingKeyFails)
{
    ExpectError(R"({ "port": 1, "verbose": false, "lanes": [] })", "tickSeconds");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [ { "name": "zone" } ] })", "count");
}

TEST(ConfigReader, UnknownKeyFails)
{
    ExpectError(R"({ "port": 1, "prot": 2, "verbose": false, "tickSeconds": 1, "lanes": [] })", "unknown key /prot");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1,
                     "lanes": [ { "name": "zone", "count": 4, "cuont": 5 } ] })",
                "unknown key /lanes/0/cuont");
}

TEST(ConfigReader, ValueThatDoesNotFitFails)
{
    ExpectError(R"({ "port": 70000, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port value 70000");
    ExpectError(R"({ "port": -1, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port value -1");
    ExpectError(R"({ "port": 1.5, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port value 1.5");
    ExpectError(R"({ "port": true, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port value true");
}

TEST(ConfigReader, WrongTypeFails)
{
    ExpectError(R"({ "port": "7000", "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port");
    ExpectError(R"({ "port": 1, "verbose": 1, "tickSeconds": 1, "lanes": [] })", "/verbose");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": {} })", "/lanes");
}

TEST(ConfigReader, SyntaxErrorAndTrailingCommaFail)
{
    ExpectError(R"({ "port": 1, )", "invalid JSON");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [], })", "invalid JSON");
}

TEST(ConfigReader, ReadsFileAndReportsMissingFile)
{
    const auto path = std::filesystem::temp_directory_path() / "mmo_server_lab_config_reader_test.json";
    {
        std::ofstream file{path, std::ios::binary};
        file << Valid;
    }
    const auto config = ConfigReader::Read<TestConfig>(path);
    std::filesystem::remove(path);
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->port, 7000);

    const auto missing = ConfigReader::Read<TestConfig>(path);
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().find("cannot open file"), std::string::npos) << missing.error();
}
