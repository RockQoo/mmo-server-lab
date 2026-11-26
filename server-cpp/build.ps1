# VS 개발자 환경(cl, ninja, cmake)을 켜고 프리셋으로 빌드한다.
# 사용: powershell -File server-cpp/build.ps1 [-Config debug|release] [-Test]
#   -Test: 빌드 후 ctest로 단위 테스트까지 돌린다(qa.json이 쓴다).
#   -Sln:  VS 솔루션(build/vs)과 필터(Core/Servers/Tests.slnf)를 만들고 Visual Studio로 연다. 빌드는 VS에서 한다(OpenVS.bat이 쓴다).
#   -Filter: -Sln 으로 열 때 필터. All(전체 솔루션), Core, Servers, Tests
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'debug',
    [switch]$Test,
    [switch]$Sln,
    [ValidateSet('All', 'Core', 'Servers', 'Tests')]
    [string]$Filter = 'All'
)
$ErrorActionPreference = 'Stop'

# VS 솔루션을 만든 뒤 필터를 만든다. ZERO_CHECK 는 모든 필터에 넣는다(VS 안에서 빌드할 때 솔루션을 갱신한다).
function Update-SolutionFilters {
    $solution = Get-ChildItem build/vs -File -Filter *.slnx | Select-Object -First 1
    if (-not $solution) { throw 'build/vs에 솔루션 파일이 없다.' }
    & "$PSScriptRoot/../tools/Make-SolutionFilters.ps1" -Solution $solution.FullName -AlwaysInclude ZERO_CHECK.vcxproj
    return $solution
}

# 설치된 VS 중 C++ 도구가 있는 최신 버전을 찾는다.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'C++ 도구가 설치된 Visual Studio를 찾지 못했다.' }

# 이 셸에 개발자 환경을 적용한다(64비트, 현재 폴더 유지).
# 환경 스크립트가 내부에서 vswhere를 PATH로 찾으므로 먼저 넣어 준다.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

# 컴파일러(cl) 출력을 영어로 고정한다. ninja 는 cl /showIncludes 출력에서 "Note: including file:" 줄을 찾아 헤더 의존을
# 기록하는데, 한국어 VS 는 "참고: 포함 파일:"로 출력해 ninja 가 알아보지 못한다. 그러면 헤더만 바꿨을 때 다시 컴파일하지
# 않는다(2026-11-26 BufferPool.h 상수를 바꿨는데 옛 값으로 빌드된 채 측정됨).
$env:VSLANG = '1033'

Push-Location $PSScriptRoot
try {
    if ($Sln) {
        # CMakeLists.txt를 고쳤으면 다시 실행한다. VS 안에서 빌드해도 ZERO_CHECK가 솔루션을 갱신한다.
        cmake --preset vs
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
        $solution = Update-SolutionFilters
        $open = if ($Filter -eq 'All') { $solution.FullName } else { Join-Path $solution.DirectoryName "$Filter.slnf" }
        # 개발자 환경(VCPKG_ROOT 등)을 물려받도록 이 셸에서 연다.
        Start-Process "$vs\Common7\IDE\devenv.exe" -ArgumentList "`"$open`""
        exit 0
    }

    cmake --preset $Config
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    # VS 솔루션을 한 번이라도 만들었으면(OpenVS.bat) 같이 갱신한다. 안 하면 CMakeLists.txt에 추가한 라이브러리가
    # VS에 보이지 않는다(2026-11-24 Messaging). CMake는 내용이 바뀐 파일만 다시 써서, VS 다시 로드 창도 그때만 뜬다.
    if (Test-Path build/vs/CMakeCache.txt) {
        cmake --preset vs | Out-Null
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
        Update-SolutionFilters | Out-Null
    }
    cmake --build --preset $Config
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($Test) {
        ctest --preset $Config
    }
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
