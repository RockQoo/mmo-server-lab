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
$found = Get-Command clang-tidy.exe -ErrorAction SilentlyContinue
$llvm = if ($found) { Split-Path $found.Source } else { "$env:ProgramFiles\LLVM\bin" }
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
$tidyExit = $LASTEXITCODE
$diagnostics = $out | Where-Object { $_ -match '(warning|error): .*\[[a-z]' } | Sort-Object -Unique
foreach ($line in $diagnostics) {
    $problems++
    Write-Output "tidy: $line"
}
# 설정 오류·옵션 오류처럼 진단 형식이 아닌 실패도 놓치지 않는다: 종료 코드가 0이 아니면 출력을 보여 주고 실패로 센다.
if ($tidyExit -and -not $diagnostics) {
    $problems++
    Write-Output "tidy: clang-tidy failed (exit $tidyExit)"
    $out | Where-Object { $_ -notmatch 'Processing file' } | Select-Object -First 20 | ForEach-Object { Write-Output "  $_" }
}

# 3) 도구가 못 보는 프로젝트 규칙(docs/coding-style.md "정수 타입").
#   - 고정 폭 정수는 std:: 없이(int32_t, std::int32_t 아님)
#   - 정수는 모두 고정 폭: int·unsigned·long·short 를 쓰지 않는다. auto x = 0 도 int 가 되므로 auto x = int32_t{0}.
#     예외: int main(, 바로 윗줄 주석에 "바깥 API 모양"이 있는 줄(signal·CRT 처리기처럼 모양이 정해진 함수)
#   검사 전에 문자열·문자 리터럴과 // 주석을 지운다(주석에 int 라고 써도 된다).
$rules = @(
    @{ Pattern = 'std::u?int(8|16|32|64)_t'; Message = 'fixed-width integer without std:: (int32_t)' },
    @{ Pattern = '\b(int|unsigned|signed|long|short)\b'; Message = 'use fixed-width integer (int32_t, uint8_t ...)' },
    @{ Pattern = '\bauto\s+\w+\s*=\s*-?\d[\d'']*\s*[;,)]'; Message = 'integer literal makes int: auto x = int32_t{0}' }
)
foreach ($file in $files) {
    $lines = [System.IO.File]::ReadAllLines($file, [System.Text.Encoding]::UTF8)
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $code = $lines[$i] -replace '"(\\.|[^"\\])*"', '""' -replace "(?<![0-9A-Fa-f])'(\\.|[^'\\])*'", "''" -replace '//.*$', ''
        # -cmatch: 대소문자를 구분한다(-match 는 구분하지 않아 Windows 타입 LONG 까지 long 으로 잡는다)
        if ($code -cmatch '\bint main\(') { continue }
        if ($i -gt 0 -and $lines[$i - 1] -match '//.*바깥 API 모양') { continue }
        foreach ($rule in $rules) {
            if ($code -cmatch $rule.Pattern) {
                $problems++
                Write-Output "style: ${file}:$($i + 1): $($rule.Message)"
            }
        }
    }
}

Write-Output "lint: $problems problem(s)"
if ($problems) { exit 1 }
