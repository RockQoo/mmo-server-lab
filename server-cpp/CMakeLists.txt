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

# VS 솔루션 탐색기에서 프로젝트를 폴더(Core, Servers, Tests)로 묶는다. Ninja 빌드에는 영향 없음.
# CMake가 만드는 ALL_BUILD, ZERO_CHECK, RUN_TESTS는 _CMake 폴더로 몰아 둔다.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER _CMake)

enable_testing()
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Core/ 아래 라이브러리는 Core/를 include 기준으로 쓴다.
# 그래서 #include "Base/BinaryWriter.h"처럼 경로가 namespace(Base::)와 일치한다.
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core)

# ======== Core 라이브러리 (정적 라이브러리, 게임을 모른다) ========

# ---- Base: 직렬화(BinaryWriter, BinaryReader), 빌드 정보
# 헤더도 적어야 VS 프로젝트에 보인다(컴파일은 .cpp만 한다).
add_library(Base STATIC
    Core/Base/Assert.cpp
    Core/Base/Assert.h
    Core/Base/BinaryReader.cpp
    Core/Base/BinaryReader.h
    Core/Base/BinaryTraits.h
    Core/Base/BinaryWriter.cpp
    Core/Base/BinaryWriter.h
    Core/Base/BuildInfo.cpp
    Core/Base/BuildInfo.h
    Core/Base/Utf8.cpp
    Core/Base/Utf8.h
)
target_include_directories(Base PUBLIC ${CORE_DIR})
set_target_properties(Base PROPERTIES FOLDER Core)

add_executable(BaseTests
    Core/Base/Tests/AssertTest.cpp
    Core/Base/Tests/BuildInfoTest.cpp
    Core/Base/Tests/BinaryRWTest.cpp
)
target_link_libraries(BaseTests PRIVATE Base GTest::gtest_main)
set_target_properties(BaseTests PROPERTIES FOLDER Tests)
# 예외 잡기를 끈다: 크래시를 gtest가 삼키지 않고 실제 종료 코드로 남긴다(ASSERT 예외 코드 확인).
# ctest는 테스트마다 프로세스를 따로 띄우므로 하나가 죽어도 나머지는 돈다.
gtest_discover_tests(BaseTests EXTRA_ARGS --gtest_catch_exceptions=0)

# ======== 실행 파일 ========

# ---- GameServer: 게임 서버 (Core 라이브러리에만 의존)
add_executable(GameServer
    GameServer/main.cpp
)
target_link_libraries(GameServer PRIVATE Base)
set_target_properties(GameServer PROPERTIES FOLDER Servers)

# VS로 열면 GameServer가 시작 프로젝트(F5로 실행할 대상)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT GameServer)
