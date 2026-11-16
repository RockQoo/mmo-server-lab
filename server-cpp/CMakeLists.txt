cmake_minimum_required(VERSION 3.28)
project(mmo_server_lab LANGUAGES CXX)

# C++23. MSVC에서는 CMake가 /std:c++latest로 바꿔 준다.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 모듈(import)을 쓰지 않고 헤더 방식으로 간다(2026-11-17 결정).
# 켜 두면 빌드마다 소스를 훑어 모듈 의존성을 찾는 단계가 추가된다.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
    # /W4: 경고 많이, /permissive-: 표준 엄격, /utf-8: 소스와 실행 문자셋 UTF-8
    # /Zc:__cplusplus: __cplusplus 매크로가 실제 표준 값을 갖게 함(없으면 199711L 고정)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

# ---- core: 서버 코어 (정적 라이브러리, 게임을 모른다)
add_library(core STATIC
    core/src/util/BuildInfo.cpp
)
target_include_directories(core PUBLIC core/include)

# ---- game: 게임 서버 (core에만 의존)
add_executable(game
    game/src/main.cpp
)
target_link_libraries(game PRIVATE core)
