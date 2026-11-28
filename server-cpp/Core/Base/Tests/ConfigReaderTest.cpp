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
        CONFIG_FIELDS(name, count)
    };

    // 64비트 끝값(리뷰 R1: nlohmann 의 부호 섞인 비교는 -1 과 UINT64_MAX 를 같다고 본다)
    struct WideConfig
    {
        uint64_t u{};
        int64_t i{};
        CONFIG_FIELDS(u, i)
    };

    struct TestConfig
    {
        uint16_t port{};
        bool verbose{};
        float tickSeconds{};
        std::vector<LaneConfig> lanes{};
        CONFIG_FIELDS(port, verbose, tickSeconds, lanes)
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
    ExpectError(R"({ "port": 1, "verbose": false, "lanes": [] })", "/tickSeconds is missing");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [ { "name": "zone" } ] })",
                "/lanes/0/count is missing");
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
    ExpectError(R"({ "port": 70000, "verbose": false, "tickSeconds": 1, "lanes": [] })",
                "/port value 70000 is out of range");
    ExpectError(R"({ "port": -1, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port value -1 is out of range");
    ExpectError(R"({ "port": 1.5, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port expected integer");
    ExpectError(R"({ "port": true, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port expected integer");
    // 리뷰 R2: 범위 밖 실수 → 정수는 바꾸기 전에 거절한다(static_cast 하면 정의되지 않은 동작)
    ExpectError(R"({ "port": 1e100, "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port expected integer");
    // 리뷰 R3: float 에 담을 수 없는 값(바꾸면 무한대)
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1e100, "lanes": [] })", "/tickSeconds value");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": -1e100, "lanes": [] })", "/tickSeconds value");
}

TEST(ConfigReader, SixtyFourBitLimits)
{
    const auto edge =
        ConfigReader::Parse<WideConfig>(R"({ "u": 18446744073709551615, "i": -9223372036854775808 })", "test.json");
    ASSERT_TRUE(edge.has_value()) << edge.error();
    EXPECT_EQ(edge->u, UINT64_MAX);
    EXPECT_EQ(edge->i, INT64_MIN);

    const auto negativeUnsigned = ConfigReader::Parse<WideConfig>(R"({ "u": -1, "i": 0 })", "test.json");
    ASSERT_FALSE(negativeUnsigned.has_value());
    EXPECT_NE(negativeUnsigned.error().find("/u value -1 is out of range"), std::string::npos)
        << negativeUnsigned.error();

    const auto tooBigSigned = ConfigReader::Parse<WideConfig>(R"({ "u": 0, "i": 18446744073709551615 })", "test.json");
    ASSERT_FALSE(tooBigSigned.has_value());
    EXPECT_NE(tooBigSigned.error().find("/i value 18446744073709551615 is out of range"), std::string::npos)
        << tooBigSigned.error();
}

TEST(ConfigReader, WrongTypeFails)
{
    ExpectError(R"({ "port": "7000", "verbose": false, "tickSeconds": 1, "lanes": [] })", "/port expected integer");
    ExpectError(R"({ "port": 1, "verbose": 1, "tickSeconds": 1, "lanes": [] })", "/verbose expected bool");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": {} })", "/lanes expected array");
    ExpectError(R"({ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [ null ] })", "/lanes/0 expected object");
    ExpectError(R"([ 1 ])", "/ expected object");
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
