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

    # VS Code clangd용. cl은 INCLUDE 환경 변수(VS 개발자 환경)로 표준 헤더를 찾지만
    # clangd는 그 경로를 모른다. compile_commands.json에 경로를 직접 넣어 준다.
    if(CMAKE_EXPORT_COMPILE_COMMANDS AND DEFINED ENV{INCLUDE})
        set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES $ENV{INCLUDE})
    endif()
endif()

enable_testing()
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Core/ 아래 라이브러리는 Core/를 include 기준으로 쓴다.
# 그래서 #include "Base/Stream.h"처럼 경로가 namespace(Base::)와 일치한다.
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core)

# ======== Core 라이브러리 (정적 라이브러리, 게임을 모른다) ========

# ---- Base: 직렬화(Stream), 빌드 정보
add_library(Base STATIC
    Core/Base/BuildInfo.cpp
    Core/Base/Stream.cpp
)
target_include_directories(Base PUBLIC ${CORE_DIR})

add_executable(BaseTests
    Core/Base/Tests/BuildInfoTest.cpp
    Core/Base/Tests/StreamTest.cpp
)
target_link_libraries(BaseTests PRIVATE Base GTest::gtest_main)
gtest_discover_tests(BaseTests)

# ======== 실행 파일 ========

# ---- GameServer: 게임 서버 (Core 라이브러리에만 의존)
add_executable(GameServer
    GameServer/main.cpp
)
target_link_libraries(GameServer PRIVATE Base)
