using System.Diagnostics.CodeAnalysis;

namespace Base.Tests;

// C++ ConfigReaderTest 와 같은 내용. 같은 JSON 을 같은 이유로 받아들이거나 거절한다.
public class ConfigReaderTest
{
    private const string Valid = """
        {
            // 주석은 허용한다
            "port": 7000,
            "verbose": true,
            "tickSeconds": 0.1, /* 100ms */
            "lanes": [ { "name": "zone", "count": 4 }, { "name": "db", "count": 2 } ]
        }
        """;

    // CA1812(만들지 않는 클래스): 설정 클래스는 System.Text.Json 이 리플렉션으로 만들어서 분석기가 보지 못한다.
    [SuppressMessage("Performance", "CA1812", Justification = "System.Text.Json 이 리플렉션으로 만든다")]
    private sealed class LaneConfig
    {
        public string Name { get; init; } = "";
        public int Count { get; init; }
    }

    [SuppressMessage("Performance", "CA1812", Justification = "System.Text.Json 이 리플렉션으로 만든다")]
    private sealed class TestConfig
    {
        public ushort Port { get; init; }
        public bool Verbose { get; init; }
        public float TickSeconds { get; init; }
        public IReadOnlyList<LaneConfig> Lanes { get; init; } = [];
    }

    // 실패하고, 오류 글에 이름과 기대한 조각이 들어 있는지 본다(어디가 틀렸는지 알려 주는가).
    private static void ExpectError(string text, string expected)
    {
        var e = Assert.Throws<InvalidDataException>(() => ConfigReader.Parse<TestConfig>(text, "test.json"));
        Assert.StartsWith("test.json: ", e.Message, StringComparison.Ordinal);
        Assert.Contains(expected, e.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ReadsNestedClassesWithComments()
    {
        var config = ConfigReader.Parse<TestConfig>(Valid, "test.json");
        Assert.Equal(7000, config.Port);
        Assert.True(config.Verbose);
        Assert.Equal(0.1f, config.TickSeconds);
        Assert.Equal(2, config.Lanes.Count);
        Assert.Equal("db", config.Lanes[1].Name);
        Assert.Equal(2, config.Lanes[1].Count);
    }

    [Fact]
    public void IntegerForFloatFieldIsFine()
    {
        var config = ConfigReader.Parse<TestConfig>("""{ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [] }""",
            "test.json");
        Assert.Equal(1.0f, config.TickSeconds);
    }

    [Fact]
    public void MissingKeyFails()
    {
        ExpectError("""{ "port": 1, "verbose": false, "lanes": [] }""", "tickSeconds");
        ExpectError("""{ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [ { "name": "zone" } ] }""", "count");
    }

    [Fact]
    public void UnknownKeyFails()
    {
        ExpectError("""{ "port": 1, "prot": 2, "verbose": false, "tickSeconds": 1, "lanes": [] }""", "prot");
        ExpectError("""
            { "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [ { "name": "zone", "count": 4, "cuont": 5 } ] }
            """, "cuont");
    }

    [Fact]
    public void ValueThatDoesNotFitFails()
    {
        ExpectError("""{ "port": 70000, "verbose": false, "tickSeconds": 1, "lanes": [] }""", "$.port");
        ExpectError("""{ "port": -1, "verbose": false, "tickSeconds": 1, "lanes": [] }""", "$.port");
        ExpectError("""{ "port": 1.5, "verbose": false, "tickSeconds": 1, "lanes": [] }""", "$.port");
        ExpectError("""{ "port": true, "verbose": false, "tickSeconds": 1, "lanes": [] }""", "$.port");
    }

    [Fact]
    public void WrongTypeFails()
    {
        ExpectError("""{ "port": "7000", "verbose": false, "tickSeconds": 1, "lanes": [] }""", "$.port");
        ExpectError("""{ "port": 1, "verbose": 1, "tickSeconds": 1, "lanes": [] }""", "$.verbose");
        ExpectError("""{ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": {} }""", "$.lanes");
        ExpectError("""{ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [ { "name": null, "count": 1 } ] }""",
            "name");
    }

    [Fact]
    public void SyntaxErrorAndTrailingCommaFail()
    {
        ExpectError("""{ "port": 1, """, "test.json");
        ExpectError("""{ "port": 1, "verbose": false, "tickSeconds": 1, "lanes": [], }""", "test.json");
    }

    [Fact]
    public void ReadsFileAndReportsMissingFile()
    {
        var path = Path.Combine(Path.GetTempPath(), "mmo_server_lab_config_reader_test_cs.json");
        File.WriteAllText(path, Valid);
        try
        {
            Assert.Equal(7000, ConfigReader.Read<TestConfig>(path).Port);
        }
        finally
        {
            File.Delete(path);
        }
        Assert.Throws<FileNotFoundException>(() => ConfigReader.Read<TestConfig>(path));
    }
}
