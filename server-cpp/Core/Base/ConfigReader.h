#pragma once

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

// 서버 설정(JSON) 읽기. 시작할 때 한 번 읽어 타입이 정해진 구조체로 바꾼다. 쓰기와 실행 중 다시 읽기는 없다.
//
// 엄격하게 읽는다(안전 > 편의). 아래는 모두 실패로 돌려준다(오류 글에 위치가 붙는다).
//   - 빠진 키         구조체 필드가 파일에 없다
//   - 알 수 없는 키    파일에 있는데 구조체에 없다(오타)
//   - 담을 수 없는 값  70000 → uint16_t, -1 → 부호 없는 정수, 1.5 → 정수, true → 정수
//   - 타입이 다름      문자열 자리에 숫자 등
//   - 문법 오류        끝 쉼표 포함. 주석(// /* */)은 허용한다
//
// 구조체는 쓰는 쪽(Network, ZoneServer ...)이 CONFIG_FIELDS 로 정한다. 구조체 안에 구조체, vector, string 을 쓸 수
// 있다.
//
//     struct NetworkConfig
//     {
//         uint16_t port{};
//         int32_t laneCount{};
//         CONFIG_FIELDS(NetworkConfig, port, laneCount)
//     };
//
//     auto config = Base::ConfigReader::Read<NetworkConfig>("zone.json");
//     if (!config) { /* config.error() 를 로그에 남기고 종료 */ }
namespace Base
{
    namespace Detail
    {
        // 글 → JSON. 주석 허용, 끝 쉼표 금지. 실패하면 오류 글.
        std::expected<nlohmann::json, std::string> ParseConfigJson(std::string_view text);

        // 파일 전체를 글로 읽는다. 실패하면 오류 글.
        std::expected<std::string, std::string> ReadConfigFile(const std::filesystem::path& path);

        // 원본(source)과, 읽은 구조체를 다시 JSON 으로 만든 것(written)을 비교해 처음 다른 곳을 글로 돌려준다. 같으면
        // 빈 글. 알 수 없는 키와 담을 수 없는 값(잘리거나 바뀐 값)을 이 비교 하나로 찾는다.
        std::string FindConfigMismatch(const nlohmann::json& source, const nlohmann::json& written,
                                       const std::string& path);
    } // namespace Detail

    class ConfigReader
    {
    public:
        // JSON 글 → T. sourceName 은 오류 글 앞에 붙는 이름(파일 경로 등).
        template <typename T>
        static std::expected<T, std::string> Parse(const std::string_view text, const std::string_view sourceName)
        {
            auto json = Detail::ParseConfigJson(text);
            if (!json)
            {
                return std::unexpected(std::format("{}: {}", sourceName, json.error()));
            }
            try
            {
                auto value = json->template get<T>(); // 빠진 키, 타입 다름은 여기서 예외
                const nlohmann::json written = value;
                if (auto mismatch = Detail::FindConfigMismatch(*json, written, ""); !mismatch.empty())
                {
                    return std::unexpected(std::format("{}: {}", sourceName, mismatch));
                }
                return value;
            }
            catch (const nlohmann::json::exception& e)
            {
                return std::unexpected(std::format("{}: {}", sourceName, e.what()));
            }
        }

        // 파일 → T. 오류 글 앞에 파일 경로가 붙는다.
        template <typename T>
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

// 구조체 안에 적는다. 적은 필드를 JSON 의 같은 이름 키와 잇는다(읽기 + 검사용 다시 쓰기).
#define CONFIG_FIELDS(Type, ...) NLOHMANN_DEFINE_TYPE_INTRUSIVE(Type, __VA_ARGS__)
