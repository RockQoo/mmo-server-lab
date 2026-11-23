# 코딩 규칙 검사: 서식(clang-format)과 이름·버그 검사(clang-tidy). 설정은 저장소 최상위 .clang-format, .clang-tidy.
# 사용: powershell -File server-cpp/lint.ps1 [-Config debug|release] [-Fix]
#   먼저 build.ps1 로 같은 Config 를 한 번 빌드해 둔다(clang-tidy 가 build/<Config>/compile_commands.json 을 쓴다).
#   -Fix: 서식을 고쳐 쓰고, clang-tidy 가 고칠 수 있는 것(이름 등)도 고친다. 고친 뒤 다시 빌드해서 확인한다.
# 하나라도 걸리면 종료 코드 1. 마지막 줄 "lint: N problem(s)" 을 qa.json 이 읽는다.
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'release',
    [switch]$Fix
)
$ErrorActionPreference = 'Stop'

# LLVM 은 PATH 에 있으면 그것을, 없으면 기본 설치 위치를 쓴다.
$llvm = Split-Path (Get-Command clang-tidy.exe -ErrorAction SilentlyContinue).Source -ErrorAction SilentlyContinue
if (-not $llvm) { $llvm = "$env:ProgramFiles\LLVM\bin" }
$format = Join-Path $llvm 'clang-format.exe'
$tidy = Join-Path $llvm 'clang-tidy.exe'
foreach ($tool in $format, $tidy) {
    if (-not (Test-Path $tool)) { throw "LLVM tool not found: $tool (install LLVM)" }
}

$db = Join-Path $PSScriptRoot "build\$Config"
if (-not (Test-Path (Join-Path $db 'compile_commands.json'))) {
    throw "compile_commands.json not found in $db. Run build.ps1 -Config $Config first."
}

# build/ 아래(생성 파일, vcpkg)는 뺀다
$files = Get-ChildItem $PSScriptRoot -Recurse -Include *.h, *.cpp |
    Where-Object { $_.FullName -notmatch '\\build\\' } | ForEach-Object FullName
$sources = $files | Where-Object { $_ -like '*.cpp' }
$problems = 0
# 아래 도구는 진행 상황을 stderr 로 쓴다. PowerShell 5.1 은 Stop 이면 그것을 오류로 보고 멈추므로 결과는 종료 코드로 본다.
$ErrorActionPreference = 'Continue'

# 1) 서식
if ($Fix) {
    & $format -i $files
}
foreach ($file in $files) {
    $out = & $format --dry-run --Werror $file 2>&1
    if ($LASTEXITCODE) {
        $problems++
        Write-Output "format: $file"
        $out | Select-Object -First 6 | ForEach-Object { Write-Output "  $_" }
    }
}

# 2) 이름·버그 검사. 헤더는 .clang-tidy 의 HeaderFilterRegex 로 .cpp 를 통해 같이 본다.
$tidyArgs = @('-p', $db, '--quiet')
if ($Fix) { $tidyArgs += '--fix' }
$out = & $tidy @tidyArgs $sources 2>&1 | ForEach-Object { "$_" }
$found = $out | Where-Object { $_ -match '(warning|error): .*\[[a-z]' } | Sort-Object -Unique
foreach ($line in $found) {
    $problems++
    Write-Output "tidy: $line"
}

Write-Output "lint: $problems problem(s)"
if ($problems) { exit 1 }
