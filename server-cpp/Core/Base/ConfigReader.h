#pragma once

#include <concepts>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

// 서버 설정(JSON) 읽기. 시작할 때 한 번 읽어 타입이 정해진 구조체로 바꾼다. 쓰기와 실행 중 다시 읽기는 없다.
//
// 엄격하게 읽는다(안전 > 편의). 아래는 모두 실패로 돌려준다(오류 글에 /lanes/0/count 같은 위치가 붙는다).
//   - 빠진 키         구조체 필드가 파일에 없다
//   - 알 수 없는 키    파일에 있는데 구조체에 없다(오타)
//   - 담을 수 없는 값  70000 → uint16_t, -1 → 부호 없는 정수, 1.5·true → 정수, 1e100 → float
//   - 타입이 다름      문자열 자리에 숫자, 객체 자리에 null 등
//   - 문법 오류        끝 쉼표 포함. 주석(// /* */)은 허용한다
//
// 값을 넣기 **전에** JSON 값의 종류와 범위를 검사한다. 범위 밖 실수를 정수로 바꾸는 것은 C++ 에서 정의되지 않은
// 동작이라 넣은 뒤 비교하는 방식으로는 막을 수 없다(Codex 리뷰 R1~R3, 2026-11-28).
//
// 구조체는 쓰는 쪽(Network, ZoneServer ...)이 CONFIG_FIELDS 로 정한다.
// 필드 타입: bool, 정수, float/double, std::string, std::vector<필드 타입>, CONFIG_FIELDS 를 가진 구조체.
//
//     struct NetworkConfig
//     {
//         uint16_t port{};
//         int32_t laneCount{};
//         CONFIG_FIELDS(port, laneCount)
//     };
//
//     auto config = Base::ConfigReader::Read<NetworkConfig>("zone.json");
//     if (!config) { /* config.error() 를 로그에 남기고 종료 */ }
namespace Base
{
    template <typename T>
    concept ConfigStruct = requires(T& value, const nlohmann::json& json, const std::string& path) {
        { value.ReadConfig(json, path) } -> std::same_as<std::string>;
    };

    namespace Detail
    {
        // 글 → JSON. 주석 허용, 끝 쉼표 금지. 실패하면 오류 글.
        std::expected<nlohmann::json, std::string> ParseConfigJson(std::string_view text);

        // 파일 전체를 글로 읽는다. 실패하면 오류 글.
        std::expected<std::string, std::string> ReadConfigFile(const std::filesystem::path& path);

        // json 이 객체이고, 키가 모두 names("a, b, c": CONFIG_FIELDS 가 적은 그대로) 안에 있는지 본다. 문제가 없으면 빈
        // 글.
        std::string CheckConfigKeys(const nlohmann::json& json, const std::string& path, std::string_view names);

        // names 의 index 번째 이름.
        std::string_view ConfigFieldName(std::string_view names, std::size_t index);

        std::string ConfigTypeError(const nlohmann::json& json, const std::string& path, std::string_view expected);

        template <typename T>
        inline constexpr bool IsConfigVector = false;
        template <typename T, typename Alloc>
        inline constexpr bool IsConfigVector<std::vector<T, Alloc>> = true;

        // 값 하나를 검사하고 넣는다. 문제가 없으면 빈 글(그때만 out 이 바뀐다).
        template <typename T>
        std::string ReadConfigValue(const nlohmann::json& json, T& out, const std::string& path)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                if (!json.is_boolean())
                {
                    return ConfigTypeError(json, path, "bool");
                }
                out = json.get<bool>();
            }
            else if constexpr (std::is_integral_v<T>)
            {
                // 정수 JSON 만 받는다(1.5, 1e100, true 는 여기서 거절). 64비트 값으로 꺼내 범위를 본 뒤에만 바꾼다.
                if (json.is_number_unsigned())
                {
                    const auto value = json.get<uint64_t>();
                    if (value > static_cast<uint64_t>(std::numeric_limits<T>::max()))
                    {
                        return std::format("{} value {} is out of range", path, value);
                    }
                    out = static_cast<T>(value);
                }
                else if (json.is_number_integer()) // 음수(양수는 위에서 unsigned 로 잡힌다)
                {
                    const auto value = json.get<int64_t>();
                    if (std::is_unsigned_v<T> || value < static_cast<int64_t>(std::numeric_limits<T>::min()))
                    {
                        return std::format("{} value {} is out of range", path, value);
                    }
                    out = static_cast<T>(value);
                }
                else
                {
                    return ConfigTypeError(json, path, "integer");
                }
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                if (!json.is_number())
                {
                    return ConfigTypeError(json, path, "number");
                }
                const auto value = json.get<double>();
                if (value > static_cast<double>(std::numeric_limits<T>::max()) ||
                    value < static_cast<double>(std::numeric_limits<T>::lowest()))
                {
                    return std::format("{} value {} is out of range", path, json.dump());
                }
                out = static_cast<T>(value);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                if (!json.is_string())
                {
                    return ConfigTypeError(json, path, "string");
                }
                out = json.get<std::string>();
            }
            else if constexpr (IsConfigVector<T>)
            {
                if (!json.is_array())
                {
                    return ConfigTypeError(json, path, "array");
                }
                T values(json.size());
                for (auto i = std::size_t{}; i < json.size(); ++i)
                {
                    if (auto error = ReadConfigValue(json[i], values[i], std::format("{}/{}", path, i)); !error.empty())
                    {
                        return error;
                    }
                }
                out = std::move(values);
            }
            else if constexpr (ConfigStruct<T>)
            {
                if (auto error = out.ReadConfig(json, path); !error.empty())
                {
                    return error;
                }
            }
            else
            {
                static_assert(
                    false,
                    "ConfigReader: 지원하지 않는 필드 타입(bool, 정수, 실수, string, vector, CONFIG_FIELDS 구조체)");
            }
            return {};
        }

        // CONFIG_FIELDS 가 부른다. 키를 검사하고 필드를 적은 순서대로 읽는다. 첫 오류에서 멈춘다.
        template <typename... Fields>
        std::string ReadConfigFields(const nlohmann::json& json, const std::string& path, const std::string_view names,
                                     Fields&... fields)
        {
            auto error = CheckConfigKeys(json, path, names);
            auto index = std::size_t{};
            const auto readOne = [&](auto& field)
            {
                const auto name = ConfigFieldName(names, index++);
                const auto found = json.find(name);
                if (found == json.end())
                {
                    error = std::format("{}/{} is missing", path, name);
                    return false;
                }
                error = ReadConfigValue(*found, field, std::format("{}/{}", path, name));
                return error.empty();
            };
            if (error.empty())
            {
                (readOne(fields) && ...);
            }
            return error;
        }
    } // namespace Detail

    class ConfigReader
    {
    public:
        // JSON 글 → T. sourceName 은 오류 글 앞에 붙는 이름(파일 경로 등).
        template <ConfigStruct T>
        static std::expected<T, std::string> Parse(const std::string_view text, const std::string_view sourceName)
        {
            const auto json = Detail::ParseConfigJson(text);
            if (!json)
            {
                return std::unexpected(std::format("{}: {}", sourceName, json.error()));
            }
            T value{};
            if (auto error = value.ReadConfig(*json, ""); !error.empty())
            {
                return std::unexpected(std::format("{}: {}", sourceName, error));
            }
            return value;
        }

        // 파일 → T. 오류 글 앞에 파일 경로가 붙는다.
        template <ConfigStruct T>
        static std::expected<T, std::string> Read(const std::filesystem::path& path)
        {
            const auto text = Detail::ReadConfigFile(path);
            if (!text)
            {
                return std::unexpected(text.error());
            }
            return Parse<T>(*text, path.string());
        }
    };
} // namespace Base

// 구조체 안에 적는다. 적은 필드를 JSON 의 같은 이름 키와 잇는다(SERIALIZE_FIELDS 와 같은 모양).
#define CONFIG_FIELDS(...)                                                                                             \
    std::string ReadConfig(const nlohmann::json& json, const std::string& path)                                        \
    {                                                                                                                  \
        return Base::Detail::ReadConfigFields(json, path, #__VA_ARGS__, __VA_ARGS__);                                  \
    }
