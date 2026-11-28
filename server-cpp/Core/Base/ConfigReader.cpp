#include "Base/ConfigReader.h"

#include <cstddef>
#include <fstream>
#include <iterator>

namespace Base::Detail
{
    namespace
    {
        // "a, b, c" 의 다음 이름을 잘라 낸다. pos 는 다음 이름의 시작으로 옮겨진다.
        std::string_view NextName(const std::string_view names, std::size_t& pos)
        {
            while (pos < names.size() && (names[pos] == ' ' || names[pos] == ','))
            {
                ++pos;
            }
            const auto start = pos;
            while (pos < names.size() && names[pos] != ',' && names[pos] != ' ')
            {
                ++pos;
            }
            return names.substr(start, pos - start);
        }

        std::string_view ShowPath(const std::string& path)
        {
            return path.empty() ? std::string_view{"/"} : std::string_view{path};
        }
    } // namespace

    std::expected<nlohmann::json, std::string> ParseConfigJson(const std::string_view text)
    {
        // 콜백 없음, 예외 끔(실패하면 discarded 값), 주석 허용. 끝 쉼표는 기본으로 금지다.
        auto json = nlohmann::json::parse(text, nullptr, false, true);
        if (json.is_discarded())
        {
            return std::unexpected(std::string{"invalid JSON (trailing commas are not allowed)"});
        }
        return json;
    }

    std::expected<std::string, std::string> ReadConfigFile(const std::filesystem::path& path)
    {
        std::ifstream file{path, std::ios::binary};
        if (!file)
        {
            return std::unexpected(std::format("{}: cannot open file", path.string()));
        }
        std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        if (file.bad())
        {
            return std::unexpected(std::format("{}: read failed", path.string()));
        }
        return text;
    }

    std::string CheckConfigKeys(const nlohmann::json& json, const std::string& path, const std::string_view names)
    {
        if (!json.is_object())
        {
            return ConfigTypeError(json, path, "object");
        }
        for (const auto& [key, value] : json.items())
        {
            auto known = false;
            auto pos = std::size_t{};
            for (auto name = NextName(names, pos); !name.empty(); name = NextName(names, pos))
            {
                if (name == key)
                {
                    known = true;
                    break;
                }
            }
            if (!known)
            {
                return std::format("unknown key {}/{}", path, key);
            }
        }
        return {};
    }

    std::string_view ConfigFieldName(const std::string_view names, const std::size_t index)
    {
        auto pos = std::size_t{};
        auto name = NextName(names, pos);
        for (auto i = std::size_t{}; i < index; ++i)
        {
            name = NextName(names, pos);
        }
        return name;
    }

    std::string ConfigTypeError(const nlohmann::json& json, const std::string& path, const std::string_view expected)
    {
        return std::format("{} expected {}, got {}", ShowPath(path), expected, json.dump());
    }
} // namespace Base::Detail
