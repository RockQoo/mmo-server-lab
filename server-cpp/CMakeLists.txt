cmake_minimum_required(VERSION 3.28)
project(mmo_server_lab LANGUAGES CXX)

# C++23. MSVC에서는 CMake가 /std:c++latest로 바꿔 준다.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 경고 = 빌드 에러(컴파일 /WX, 링크 /WX). 모든 대상에 걸린다.
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
set(CMAKE_LINK_WARNING_AS_ERROR ON)

# 모듈(import)을 쓰지 않고 헤더 방식으로 간다(2026-11-17 결정).
# 켜 두면 빌드마다 소스를 훑어 모듈 의존성을 찾는 단계가 추가된다.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
    # /W4: 경고 많이, /permissive-: 표준 엄격, /utf-8: 소스와 실행 문자셋 UTF-8
    # /Zc:__cplusplus: __cplusplus 매크로가 실제 표준 값을 갖게 함(없으면 199711L 고정)
    add_compile_options(/permissive- /utf-8 /Zc:__cplusplus)

    # 경고는 가장 높게(/Wall), 경고가 하나라도 나오면 빌드 실패(아래 COMPILE/LINK_WARNING_AS_ERROR = /WX).
    # <...>로 include한 헤더(표준, vcpkg의 gtest)는 경고를 보지 않는다(/external). 우리 코드가 만든 템플릿 인스턴스의
    # 경고는 그대로 본다.
    add_compile_options(/Wall /external:anglebrackets /external:W0)
    # /Wall 에는 "문제"가 아니라 "컴파일러가 이렇게 했다"는 안내도 섞여 있다. 그런 것만 끈다(나머지는 전부 에러).
    #   C4514 쓰지 않는 inline 함수를 지웠다          C4710 inline 하지 않았다   C4711 자동으로 inline 했다
    #   C4820 구조체에 패딩을 넣었다                   C5045 Spectre 완화 코드를 넣을 수 있다(/Qspectre 안내)
    add_compile_options(/wd4514 /wd4710 /wd4711 /wd4820 /wd5045)
    # 정적 분석(/analyze): 컴파일하면서 널 역참조·버퍼 넘침·초기화 안 된 값 같은 버그를 찾는다. 경고와 같이 에러가 된다.
    add_compile_options(/analyze /analyze:external-)

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
# 그래서 #include "Base/ByteWriter.h"처럼 경로가 namespace(Base::)와 일치한다.
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Core)

# ======== Core 라이브러리 (정적 라이브러리, 게임을 모른다) ========

# ---- Base: 직렬화(ByteWriter, ByteReader), 빌드 정보, ID 타입(StrongId), 고유 ID(Ruid)
# 헤더도 적어야 VS 프로젝트에 보인다(컴파일은 .cpp만 한다).
add_library(Base STATIC
    Core/Base/Assert.cpp
    Core/Base/Assert.h
    Core/Base/BuildInfo.cpp
    Core/Base/BuildInfo.h
    Core/Base/ByteReader.cpp
    Core/Base/ByteReader.h
    Core/Base/ByteTraits.h
    Core/Base/ByteWriter.cpp
    Core/Base/ByteWriter.h
    Core/Base/Ruid.h
    Core/Base/RuidGenerator.cpp
    Core/Base/RuidGenerator.h
    Core/Base/StrongId.h
    Core/Base/Utf8.cpp
    Core/Base/Utf8.h
)
target_include_directories(Base PUBLIC ${CORE_DIR})
set_target_properties(Base PROPERTIES FOLDER Core)

add_executable(BaseTests
    Core/Base/Tests/AssertTest.cpp
    Core/Base/Tests/BuildInfoTest.cpp
    Core/Base/Tests/ByteFormatTest.cpp
    Core/Base/Tests/ByteRWTest.cpp
    Core/Base/Tests/RuidGeneratorTest.cpp
    Core/Base/Tests/RuidTest.cpp
    Core/Base/Tests/StrongIdTest.cpp
)
target_link_libraries(BaseTests PRIVATE Base GTest::gtest_main)
# C# 테스트와 같이 쓰는 정답 파일 위치(저장소 최상위 testdata/)
target_compile_definitions(BaseTests PRIVATE MMO_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata")
set_target_properties(BaseTests PROPERTIES FOLDER Tests)
# gtest 의 EXPECT_*/ASSERT_* 매크로가 안에서 상수끼리 비교해 /analyze 가 C6326 을 낸다(gtest 오탐). 테스트에서만 끈다.
if(MSVC)
    target_compile_options(BaseTests PRIVATE /wd6326)
endif()
# 예외 잡기를 끈다: 크래시를 gtest가 삼키지 않고 실제 종료 코드로 남긴다(ASSERT 예외 코드 확인).
# ctest는 테스트마다 프로세스를 따로 띄우므로 하나가 죽어도 나머지는 돈다.
gtest_discover_tests(BaseTests EXTRA_ARGS --gtest_catch_exceptions=0)

# ---- Messaging: 일을 받아 순서대로 처리하는 스레드(Lane). 스레드를 만들고 돌리는 것은 Base가 아니라 여기에 둔다.
add_library(Messaging STATIC
    Core/Messaging/Lane.cpp
    Core/Messaging/Lane.h
)
target_link_libraries(Messaging PUBLIC Base)
set_target_properties(Messaging PROPERTIES FOLDER Core)

add_executable(MessagingTests
    Core/Messaging/Tests/LaneTest.cpp
)
target_link_libraries(MessagingTests PRIVATE Messaging GTest::gtest_main)
set_target_properties(MessagingTests PROPERTIES FOLDER Tests)
if(MSVC)
    target_compile_options(MessagingTests PRIVATE /wd6326) # BaseTests와 같은 gtest 오탐
endif()
gtest_discover_tests(MessagingTests EXTRA_ARGS --gtest_catch_exceptions=0)

# ======== 실행 파일 (분산 존 구조, docs/decisions/001-서버-구조.md) ========
# 서버는 Core 라이브러리에만 의존한다. 서버끼리는 서로 링크하지 않는다.

# ---- Gateway: 클라 소켓을 들고, 세션별 경로표로 Zone에 넘기고, Zone이 보낸 패킷을 세션별로 모아 보낸다
add_executable(Gateway
    Gateway/main.cpp
)
target_link_libraries(Gateway PRIVATE Messaging)
set_target_properties(Gateway PROPERTIES FOLDER Servers)

# ---- ZoneServer: 전투·이동. 프로세스 하나에 존 여러 개, 존마다 전용 스레드와 틱
add_executable(ZoneServer
    ZoneServer/main.cpp
)
target_link_libraries(ZoneServer PRIVATE Messaging)
set_target_properties(ZoneServer PROPERTIES FOLDER Servers)

# VS로 열면 ZoneServer가 시작 프로젝트(F5로 실행할 대상)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ZoneServer)
