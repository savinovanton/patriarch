<#
.SYNOPSIS
    Formats (or checks) all first-party C++ sources with clang-format.

.PARAMETER Check
    Do not modify files; fail (exit code 1) if any file needs formatting.

.EXAMPLE
    tools\format.ps1            # reformat in place
    tools\format.ps1 -Check     # CI mode
#>
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Find-ClangFormat {
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $roots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    )
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem -Path $r -Recurse -Filter clang-format.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\Llvm\\(x64\\)?bin\\' } |
            Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$clangFormat = Find-ClangFormat
if (-not $clangFormat) {
    Write-Error 'clang-format not found. Install LLVM or the VS "C++ Clang tools" component.'
    exit 2
}

$dirs = 'src', 'tests', 'bench', 'tools' | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }
$files = foreach ($d in $dirs) {
    Get-ChildItem -Path $d -Recurse -File -Include *.cpp, *.hpp, *.h, *.cc
}

if (-not $files) {
    Write-Host 'No C++ files found.'
    exit 0
}

[string[]]$fmtArgs = if ($Check) { @('--dry-run', '--Werror') } else { @('-i') }
$failed = $false
foreach ($f in $files) {
    & $clangFormat @fmtArgs --style=file $f.FullName
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}

if ($failed) {
    Write-Host 'Formatting problems found.' -ForegroundColor Red
    exit 1
}
Write-Host ("clang-format OK ({0} files, {1})" -f @($files).Count, $(if ($Check) { 'check' } else { 'formatted' }))
