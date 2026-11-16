#pragma once

#include <string>

namespace Core
{
    // 이 바이너리를 만든 컴파일러, C++ 표준, 빌드 구성.
    // 벤치마크 결과마다 측정 조건으로 함께 남긴다.
    std::string BuildInfo();
} // namespace Core
