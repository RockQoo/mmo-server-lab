#include "Base/ConfigReader.h"

#include <cstddef>
#include <fstream>
#include <iterator>

namespace Base::Detail
{
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

    std::string FindConfigMismatch(const nlohmann::json& source, const nlohmann::json& written, const std::string& path)
    {
        if (source.is_object() && written.is_object())
        {
            // 빠진 키는 get<T>() 가 먼저 막는다. 여기서는 원본에만 있는 키(오타)를 찾는다.
            for (const auto& [key, value] : source.items())
            {
                const auto child = path + "/" + key;
                const auto found = written.find(key);
                if (found == written.end())
                {
                    return std::format("unknown key {}", child);
                }
                if (auto mismatch = FindConfigMismatch(value, *found, child); !mismatch.empty())
                {
                    return mismatch;
                }
            }
            return {};
        }
        if (source.is_array() && written.is_array())
        {
            if (source.size() != written.size())
            {
                return std::format("{} has {} elements, expected {}", path.empty() ? "/" : path, source.size(),
                                   written.size());
            }
            for (auto i = std::size_t{}; i < source.size(); ++i)
            {
                if (auto mismatch = FindConfigMismatch(source[i], written[i], std::format("{}/{}", path, i));
                    !mismatch.empty())
                {
                    return mismatch;
                }
            }
            return {};
        }
        // float 필드: 0.1 은 float 로 담으면 0.100000001 로 돌아온다. float 정밀도에서 같으면 같은 값으로 본다.
        if (source.is_number_float() && written.is_number_float() &&
            static_cast<float>(source.get<double>()) == static_cast<float>(written.get<double>()))
        {
            return {};
        }
        // 숫자끼리는 타입이 달라도 값으로 비교한다(5 == 5.0). 숫자와 bool 은 다른 값이다(1 != true).
        if (source == written)
        {
            return {};
        }
        return std::format("{} value {} does not fit (read as {})", path.empty() ? "/" : path, source.dump(),
                           written.dump());
    }
} // namespace Base::Detail
