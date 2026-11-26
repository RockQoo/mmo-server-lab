# 솔루션(.slnx)을 읽어 같은 폴더에 솔루션 필터(.slnf) 세 개를 만든다. C++·C# build.ps1이 부른다.
# 사용: powershell -File tools/Make-SolutionFilters.ps1 -Solution <.slnx> [-AlwaysInclude <프로젝트 경로>...]
#
# 솔루션 폴더 = 디스크 폴더라서, 폴더 이름만 보고 나눈다(프로젝트를 추가해도 손댈 곳이 없다).
#   Core.slnf     /Core/ 아래, 테스트 제외
#   Servers.slnf  Core + 서버(테스트, 벤치마크, _CMake 제외)
#   Tests.slnf    모든 테스트(/…/Tests/) + Core + 테스트가 시험하는 폴더(ZoneServer/Tests 가 있으면 ZoneServer/)
# -AlwaysInclude: 모든 필터에 넣을 프로젝트(C++ 의 ZERO_CHECK: VS 안에서 빌드할 때 솔루션을 갱신한다).
# 내용이 같으면 파일을 다시 쓰지 않는다(VS 다시 로드 창이 뜨지 않게).
param(
    [Parameter(Mandatory)][string]$Solution,
    [string[]]$AlwaysInclude = @()
)
$ErrorActionPreference = 'Stop'

$solutionPath = (Resolve-Path $Solution).Path
$directory = Split-Path $solutionPath
[xml]$xml = Get-Content -Raw -Encoding UTF8 $solutionPath

$projects = @(
    foreach ($project in $xml.Solution.Project) { [pscustomobject]@{ Folder = '/'; Path = $project.Path } }
    foreach ($folder in $xml.Solution.Folder) {
        foreach ($project in $folder.Project) { [pscustomobject]@{ Folder = $folder.Name; Path = $project.Path } }
    }
)

function Test-IsTest($p) { $p.Folder -like '*/Tests/' }
function Test-IsCore($p) { $p.Folder -like '/Core/*' }
function Test-IsCMake($p) { $p.Folder -like '/_CMake/*' }
function Test-IsBench($p) { $p.Folder -like '/Bench/*' }

# 테스트가 시험하는 폴더: /ZoneServer/Tests/ → /ZoneServer/
$testedFolders = @($projects | Where-Object { Test-IsTest $_ } | ForEach-Object { $_.Folder -replace 'Tests/$', '' })

$filters = [ordered]@{
    Core    = @($projects | Where-Object { (Test-IsCore $_) -and -not (Test-IsTest $_) })
    Servers = @($projects | Where-Object { -not (Test-IsTest $_) -and -not (Test-IsCMake $_) -and -not (Test-IsBench $_) })
    Tests   = @($projects | Where-Object { (Test-IsTest $_) -or (Test-IsCore $_) -or ($testedFolders -contains $_.Folder) })
}
$always = @($projects | Where-Object { $AlwaysInclude -contains $_.Path })

foreach ($name in $filters.Keys) {
    $paths = @(($filters[$name] + $always) | ForEach-Object { $_.Path -replace '/', '\' } | Sort-Object -Unique)
    $json = [ordered]@{
        solution = [ordered]@{ path = (Split-Path -Leaf $solutionPath); projects = $paths }
    } | ConvertTo-Json -Depth 4
    $out = Join-Path $directory "$name.slnf"
    if (-not (Test-Path $out) -or (Get-Content -Raw $out) -ne $json) {
        [IO.File]::WriteAllText($out, $json, (New-Object Text.UTF8Encoding $false))
    }
}
