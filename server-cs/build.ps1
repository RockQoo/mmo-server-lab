# C# 서버를 빌드한다(server-cpp/build.ps1과 같은 사용법).
# 사용: powershell -File server-cs/build.ps1 [-Config debug|release] [-Test]
#   -Test: 빌드 후 dotnet test로 단위 테스트까지 돌린다(qa.json이 쓴다).
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'debug',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'

# 출력 언어를 영어로 고정한다. qa.json이 결과 줄을 정규식으로 찾는다.
$env:DOTNET_CLI_UI_LANGUAGE = 'en'

# dotnet은 Debug/Release처럼 첫 글자가 대문자인 이름을 쓴다.
$configuration = (Get-Culture).TextInfo.ToTitleCase($Config)
$solution = Join-Path $PSScriptRoot 'mmo_server_lab.slnx'

# dotnet은 현재 폴더에서 위로 global.json을 찾는다(SDK 버전, 테스트 실행 방식).
# 저장소 루트에서 불려도 server-cs/global.json을 읽도록 이 폴더에서 실행한다.
Push-Location $PSScriptRoot
try {
    dotnet build $solution -c $configuration --nologo
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($Test) {
        dotnet test --solution $solution -c $configuration --no-build
    }
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
