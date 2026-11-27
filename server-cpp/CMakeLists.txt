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

    # release 도 심볼(.pdb)을 만든다. 없으면 크래시 덤프가 함수 이름·줄 번호 없이 주소만 남는다(performance.html 사례 14).
    # 최적화는 그대로다: /Zi 는 pdb 만 따로 쓰고, /OPT:REF·/OPT:ICF 는 /DEBUG 가 끄는 링크 최적화를 다시 켠다.
    # 배포할 때는 빌드마다 exe 와 pdb 를 같이 보관한다.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

    # VS Code clangd용. cl은 INCLUDE 환경 변수(VS 개발자 환경)로 표준 헤더를 찾지만
    # clangd는 그 경로를 모른다. compile_commands.json에 경로를 직접 넣어 준다.
    if(CMAKE_EXPORT_COMPILE_COMMANDS AND DEFINED ENV{INCLUDE})
        set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES $ENV{INCLUDE})
    endif()
endif()

# VS 솔루션 탐색기의 폴더를 디스크 폴더와 같게 한다(Core/Base, Core/Base/Tests, Gateway …). Ninja 빌드에는 영향 없음.
# 대상을 추가하면 FOLDER 를 그 대상의 디스크 폴더(server-cpp/ 기준)로 준다. 필터(.slnf)는 이 폴더로 나뉜다(tools/Make-SolutionFilters.ps1).
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
    Core/Base/CrashDump.cpp
    Core/Base/CrashDump.h
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
target_link_libraries(Base PUBLIC dbghelp) # MiniDumpWriteDump(CrashDump)
set_target_properties(Base PROPERTIES FOLDER Core/Base)

add_executable(BaseTests
    Core/Base/Tests/AssertTest.cpp
    Core/Base/Tests/BuildInfoTest.cpp
    Core/Base/Tests/CrashDumpTest.cpp
    Core/Base/Tests/ByteFormatTest.cpp
    Core/Base/Tests/ByteRWTest.cpp
    Core/Base/Tests/RuidGeneratorTest.cpp
    Core/Base/Tests/RuidTest.cpp
    Core/Base/Tests/StrongIdTest.cpp
)
target_link_libraries(BaseTests PRIVATE Base GTest::gtest_main)
# C# 테스트와 같이 쓰는 정답 파일 위치(저장소 최상위 testdata/)
target_compile_definitions(BaseTests PRIVATE MMO_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata")
set_target_properties(BaseTests PROPERTIES FOLDER Core/Base/Tests)
# gtest 의 EXPECT_*/ASSERT_* 매크로가 안에서 상수끼리 비교해 /analyze 가 C6326 을 낸다(gtest 오탐). 테스트에서만 끈다.
if(MSVC)
    target_compile_options(BaseTests PRIVATE /wd6326)
endif()
# 예외 잡기를 끈다: 크래시를 gtest가 삼키지 않고 실제 종료 코드로 남긴다(ASSERT 예외 코드 확인).
# ctest는 테스트마다 프로세스를 따로 띄우므로 하나가 죽어도 나머지는 돈다.
gtest_discover_tests(BaseTests EXTRA_ARGS --gtest_catch_exceptions=0)

# ---- Messaging: 일을 받아 순서대로 처리하는 스레드(Lane), 키로 Lane을 고르는 묶음(LaneGroup). 스레드를 만들고 돌리는 것은 Base가 아니라 여기에 둔다.
add_library(Messaging STATIC
    Core/Messaging/Lane.cpp
    Core/Messaging/Lane.h
    Core/Messaging/LaneGroup.cpp
    Core/Messaging/LaneGroup.h
)
target_link_libraries(Messaging PUBLIC Base)
set_target_properties(Messaging PROPERTIES FOLDER Core/Messaging)

add_executable(MessagingTests
    Core/Messaging/Tests/LaneGroupTest.cpp
    Core/Messaging/Tests/LaneTest.cpp
)
target_link_libraries(MessagingTests PRIVATE Messaging GTest::gtest_main)
set_target_properties(MessagingTests PROPERTIES FOLDER Core/Messaging/Tests)
if(MSVC)
    target_compile_options(MessagingTests PRIVATE /wd6326) # BaseTests와 같은 gtest 오탐
endif()
gtest_discover_tests(MessagingTests EXTRA_ARGS --gtest_catch_exceptions=0)

# ---- Memory: 버퍼 재사용(BufferPool, 빌린 버퍼 핸들 PooledBuffer), 객체 재사용(ObjectPool, 핸들 PooledPtr)
add_library(Memory STATIC
    Core/Memory/BufferPool.cpp
    Core/Memory/BufferPool.h
    Core/Memory/ObjectPool.h
)
target_link_libraries(Memory PUBLIC Base)
set_target_properties(Memory PROPERTIES FOLDER Core/Memory)

add_executable(MemoryTests
    Core/Memory/Tests/BufferPoolTest.cpp
    Core/Memory/Tests/ObjectPoolTest.cpp
)
target_link_libraries(MemoryTests PRIVATE Memory GTest::gtest_main)
set_target_properties(MemoryTests PROPERTIES FOLDER Core/Memory/Tests)
if(MSVC)
    target_compile_options(MemoryTests PRIVATE /wd6326) # BaseTests와 같은 gtest 오탐
endif()
gtest_discover_tests(MemoryTests EXTRA_ARGS --gtest_catch_exceptions=0)

# ---- Log: 비동기 로그(로그 전용 Lane), 카테고리 enum(magic_enum 으로 이름) + 키·값. C# 은 Serilog
find_package(magic_enum CONFIG REQUIRED)
add_library(Log STATIC
    Core/Log/Log.cpp
    Core/Log/Log.h
)
target_link_libraries(Log PUBLIC Messaging magic_enum::magic_enum)
set_target_properties(Log PROPERTIES FOLDER Core/Log)

add_executable(LogTests
    Core/Log/Tests/LogTest.cpp
)
target_link_libraries(LogTests PRIVATE Log GTest::gtest_main)
set_target_properties(LogTests PROPERTIES FOLDER Core/Log/Tests)
if(MSVC)
    target_compile_options(LogTests PRIVATE /wd6326) # BaseTests와 같은 gtest 오탐
endif()
gtest_discover_tests(LogTests EXTRA_ARGS --gtest_catch_exceptions=0)

# ======== 벤치마크 (Google Benchmark) ========
# 설계 선택의 근거를 재는 실행 파일. 결과는 docs/decisions/ 에 남긴다. Release 로 돌린다.
#   build/release/Bench.exe --benchmark_filter=BufferPool
find_package(benchmark CONFIG REQUIRED)
add_executable(Bench
    Bench/BufferPoolBench.cpp
    Bench/CrashDumpBench.cpp
    Bench/LogBench.cpp
    Bench/MessageQueueBench.cpp
    Bench/ObjectPoolBench.cpp
    Bench/RentReturnGrid.h
    Bench/ThreadTeam.h
)
target_link_libraries(Bench PRIVATE Log Memory Messaging Base benchmark::benchmark benchmark::benchmark_main)
set_target_properties(Bench PROPERTIES FOLDER Bench)
if(MSVC)
    target_compile_options(Bench PRIVATE /wd6326) # 테스트와 같은 매크로 오탐 대비
endif()

# ======== 실행 파일 (분산 존 구조, docs/decisions/001-server-architecture.md) ========
# 서버는 Core 라이브러리에만 의존한다. 서버끼리는 서로 링크하지 않는다.

# ---- Gateway: 클라 소켓을 들고, 세션별 경로표로 Zone에 넘기고, Zone이 보낸 패킷을 세션별로 모아 보낸다
add_executable(Gateway
    Gateway/main.cpp
)
target_link_libraries(Gateway PRIVATE Messaging)
set_target_properties(Gateway PROPERTIES FOLDER Gateway)

# ---- ZoneServer: 전투·이동. 프로세스 하나에 존 여러 개, 존마다 전용 스레드와 틱
add_executable(ZoneServer
    ZoneServer/main.cpp
)
target_link_libraries(ZoneServer PRIVATE Messaging)
set_target_properties(ZoneServer PROPERTIES FOLDER ZoneServer)

# VS로 열면 ZoneServer가 시작 프로젝트(F5로 실행할 대상)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ZoneServer)
