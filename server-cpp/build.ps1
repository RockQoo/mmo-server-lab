# VS 개발자 환경(cl, ninja, cmake)을 켜고 프리셋으로 빌드한다.
# 사용: powershell -File server-cpp/build.ps1 [-Config debug|release] [-Test]
#   -Test: 빌드 후 ctest로 단위 테스트까지 돌린다(qa.json이 쓴다).
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'debug',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'

# 설치된 VS 중 C++ 도구가 있는 최신 버전을 찾는다.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'C++ 도구가 설치된 Visual Studio를 찾지 못했다.' }

# 이 셸에 개발자 환경을 적용한다(64비트, 현재 폴더 유지).
# 환경 스크립트가 내부에서 vswhere를 PATH로 찾으므로 먼저 넣어 준다.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

Push-Location $PSScriptRoot
try {
    cmake --preset $Config
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
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
