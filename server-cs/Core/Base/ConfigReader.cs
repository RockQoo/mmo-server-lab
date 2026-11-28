using System.Collections;
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
//   - null            RespectNullableAnnotations(nullable 이 아닌 속성에 null 이면 실패) + 컬렉션 원소 null 검사(아래)
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
            // BCL 이 만든 오류 글에는 위치가 들어 있다. 우리가 던진 것(MakeAllRequired)은 Path 만 채워지므로 붙인다.
            var where = e.Path is { } path && !e.Message.Contains(path, StringComparison.Ordinal) ? $" Path: {path}" : "";
            throw new InvalidDataException($"{sourceName}: {e.Message}{where}", e);
        }
        return value ?? throw new InvalidDataException($"{sourceName}: config is null");
    }

    // 클래스의 모든 속성을 필수로 만든다(C++ CONFIG_FIELDS 와 같이 빠진 키는 실패).
    // 그리고 속성에 넣기 전에 BCL 이 통과시키는 두 가지를 거절한다(속성 setter 를 감싼다).
    //   - 컬렉션 원소 null: RespectNullableAnnotations 는 컬렉션 원소를 검사하지 않는다(MS 문서에 적힌 제한, Codex 리뷰 R4)
    //   - 무한대: float 에 1e100, double 에 1e400 을 넣으면 오류 없이 ∞ 가 된다(2026-11-28 직접 확인. C++ 리뷰 R3 과 같은 입력)
    // setter 를 거치므로 설정 클래스는 init 속성으로 쓴다(생성자 매개변수는 거치지 않는다).
    private static void MakeAllRequired(JsonTypeInfo typeInfo)
    {
        if (typeInfo.Kind != JsonTypeInfoKind.Object)
        {
            return;
        }
        foreach (var property in typeInfo.Properties)
        {
            property.IsRequired = true;
            if (property.Set is { } set)
            {
                property.Set = (target, value) =>
                {
                    if (FindProblem(value) is { } problem)
                    {
                        throw new JsonException(problem);
                    }
                    set(target, value);
                };
            }
        }
    }

    // 넣으면 안 되는 값이면 이유, 괜찮으면 null. 컬렉션은 원소까지 본다(list 안 list 포함).
    private static string? FindProblem(object? value)
    {
        switch (value)
        {
            case float f when !float.IsFinite(f):
            case double d when !double.IsFinite(d):
                return "value does not fit (not a finite number)";
            case string:
                return null;
            case IEnumerable items:
                foreach (var item in items)
                {
                    if (item is null)
                    {
                        return "null element";
                    }
                    if (FindProblem(item) is { } problem)
                    {
                        return problem;
                    }
                }
                return null;
            default:
                return null;
        }
    }
}
