using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace Base;

// 서버 설정(JSON) 읽기. 시작할 때 한 번 읽어 타입이 정해진 클래스로 바꾼다. 쓰기와 실행 중 다시 읽기는 없다.
// C++ Base::ConfigReader 와 같은 역할이고 같은 파일을 읽는다(키는 camelCase, 속성은 PascalCase).
//
// 엄격하게 읽는다(안전 > 편의). C++ 과 같은 것을 실패로 본다. 모두 BCL System.Text.Json 옵션으로 된다.
//   - 빠진 키         모든 속성을 필수로 바꾼다(아래 MakeAllRequired). `required` 를 적지 않아도 빠지면 실패
//   - 알 수 없는 키    UnmappedMemberHandling.Disallow
//   - 담을 수 없는 값  기본 동작(70000 → ushort, -1 → 부호 없는 정수, 1.5 → int, true → int 모두 실패)
//   - 타입이 다름      기본 동작(문자열 "7000" → int 도 실패: NumberHandling 기본값 Strict)
//   - null            RespectNullableAnnotations(nullable 이 아닌 속성에 null 이면 실패)
//   - 문법 오류        끝 쉼표 금지(기본). 주석(// /* */)은 허용한다
//
// C++ 은 실패를 std::expected 로 돌려주지만, C# 은 시작할 때 한 번이라 예외가 자연스럽다.
// 실패하면 InvalidDataException(글 앞에 파일 경로), 파일이 없으면 FileNotFoundException.
//
//     public sealed class NetworkConfig
//     {
//         public ushort Port { get; init; }
//         public int LaneCount { get; init; }
//     }
//
//     var config = ConfigReader.Read<NetworkConfig>("zone.json");
public static class ConfigReader
{
    private static readonly JsonSerializerOptions s_options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        ReadCommentHandling = JsonCommentHandling.Skip,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectNullableAnnotations = true,
        TypeInfoResolver = new DefaultJsonTypeInfoResolver { Modifiers = { MakeAllRequired } },
    };

    // 파일 → T. 오류 글 앞에 파일 경로가 붙는다.
    public static T Read<T>(string path)
    {
        var text = File.ReadAllText(path);
        return Parse<T>(text, path);
    }

    // JSON 글 → T. sourceName 은 오류 글 앞에 붙는 이름(파일 경로 등).
    public static T Parse<T>(string text, string sourceName)
    {
        T? value;
        try
        {
            value = JsonSerializer.Deserialize<T>(text, s_options);
        }
        catch (JsonException e)
        {
            throw new InvalidDataException($"{sourceName}: {e.Message}", e);
        }
        return value ?? throw new InvalidDataException($"{sourceName}: config is null");
    }

    // 클래스의 모든 속성을 필수로 만든다(C++ CONFIG_FIELDS 와 같이 빠진 키는 실패).
    private static void MakeAllRequired(JsonTypeInfo typeInfo)
    {
        if (typeInfo.Kind != JsonTypeInfoKind.Object)
        {
            return;
        }
        foreach (var property in typeInfo.Properties)
        {
            property.IsRequired = true;
        }
    }
}
