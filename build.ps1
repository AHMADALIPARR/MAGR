# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 MAGR contributors
#
# This file is part of MAGR (Merkle Array Graph Runtime).
#
# MAGR is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, version 3.
#
# MAGR is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with MAGR. If not, see <https://www.gnu.org/licenses/>.
#
# Note for AGPL section 13: MAGR is designed to be used over a
# network. If you run a modified version as a service, section 13
# requires you to offer those users the corresponding source of
# your modified version. See docs/licensing.md.

<#
.SYNOPSIS
    MAGR build + test driver for Windows.

.DESCRIPTION
    Wraps CMake so that the toolchain requirements are checked BEFORE anything
    is compiled, and so that a missing compiler produces an honest message
    rather than a wall of CMake errors.

    This script reports what it verified. It does not report success for work it
    did not do: if the Fortran backend is missing, it says so and exits non-zero
    for the suites that need it, rather than skipping quietly and printing an
    encouraging summary.

.PARAMETER BuildDir
    CMake binary directory. Default: .\build

.PARAMETER NoFortran
    Skip the Fortran backend. The C core, the Array ABI tests, and the merkle
    suites are all meaningful without it, and a hard requirement would make the
    whole tree unbuildable on a machine that could verify a good deal of it.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -NoFortran
#>
[CmdletBinding()]
param(
    [string] $BuildDir = "build",
    [switch] $NoFortran,
    [switch] $NoTest
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Say([string]$msg, [string]$colour = "Gray") {
    Write-Host $msg -ForegroundColor $colour
}

Say ""
Say "MAGR build driver" "Cyan"
Say "=================" "Cyan"
Say ""

# ---------------------------------------------------------------- preflight

$cc  = Get-Command cl.exe    -ErrorAction SilentlyContinue
$gcc = Get-Command gcc.exe   -ErrorAction SilentlyContinue
$clang = Get-Command clang.exe -ErrorAction SilentlyContinue
$gfortran = Get-Command gfortran.exe -ErrorAction SilentlyContinue
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$python = Get-Command python.exe -ErrorAction SilentlyContinue

Say "Toolchain detection"
Say "  cmake     : $(if ($cmake)    { $cmake.Source }    else { 'NOT FOUND' })"
Say "  C compiler: $(if ($gcc)      { "gcc $($gcc.Source)" } elseif ($clang) { "clang $($clang.Source)" } elseif ($cc) { "msvc $($cc.Source)" } else { 'NOT FOUND' })"
Say "  gfortran  : $(if ($gfortran) { $gfortran.Source } else { 'NOT FOUND' })"
Say "  python    : $(if ($python)   { $python.Source }   else { 'NOT FOUND' })"
Say ""

$missing = @()
if (-not $cmake) { $missing += "cmake" }
if (-not ($gcc -or $clang -or $cc)) { $missing += "a C compiler (cl, gcc, or clang)" }
if (-not $NoFortran -and -not $gfortran) { $missing += "gfortran (or pass -NoFortran)" }

if ($missing.Count -gt 0) {
    Say "MISSING: $($missing -join ', ')" "Red"
    Say ""
    Say "Nothing has been built. To fix this on Windows:" "Yellow"
    Say "  # C compiler + gfortran + cmake, via the MSYS2 toolchain (recommended)"
    Say "  winget install -e --id MSYS2.MSYS2"
    Say "  # then in an MSYS2 UCRT64 shell:"
    Say "  pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gcc-fortran mingw-w64-ucrt-x86_64-cmake"
    Say ""
    Say "  # or, for the full Unix toolchain including Chapel:"
    Say "  wsl --install -d Ubuntu"
    Say "  #   then inside WSL:  apt install gcc gfortran cmake make python3"
    Say ""
    Say "Alternatively, the suites that need no compiler at all can be run now:"
    Say "  python tests\merkle-integrity\test_merkle_integrity.py"
    Say "  python tools\check_descriptors.py"
    exit 1
}

# ---------------------------------------------------------------- configure

$cmakeArgs = @("-S", $Root, "-B", (Join-Path $Root $BuildDir))
if ($NoFortran) { $cmakeArgs += "-DMAGR_WITH_FORTRAN=OFF" }
$cmakeArgs += @("-DCMAKE_BUILD_TYPE=Release")

Say "Configuring..." "Yellow"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Say "Configure failed." "Red"; exit $LASTEXITCODE }

Say ""
Say "Building..." "Yellow"
& cmake --build (Join-Path $Root $BuildDir) --config Release
if ($LASTEXITCODE -ne 0) { Say "Build failed." "Red"; exit $LASTEXITCODE }

# ---------------------------------------------------------------- stamp

# Hash the artefacts that actually exist now. This is what makes a descriptor's
# binary digest honest: it is computed from the file on disk, in this build, and
# it is null before the build rather than a plausible-looking constant.
Say ""
Say "Stamping binary digests" "Yellow"
$stampScript = Join-Path $Root "tools\stamp_digests.py"
if (Test-Path $stampScript) {
    & $python.Source $stampScript --build-dir (Join-Path $Root $BuildDir)
} else {
    Say "  tools\stamp_digests.py not present; descriptors keep binary.digest = null" "DarkGray"
}

# ---------------------------------------------------------------- test

if ($NoTest) { Say "Skipping tests (-NoTest)."; exit 0 }

Say ""
Say "Running tests" "Yellow"
& cmake --build (Join-Path $Root $BuildDir) --config Release --target test
$testExit = $LASTEXITCODE

Say ""
if ($testExit -eq 0) {
    Say "All registered tests passed." "Green"
    Say "Reminder: passing tests establish the properties those tests assert."
    Say "It does not make this a verified numerical runtime" "DarkGray"
} else {
    Say "Some tests FAILED. See the output above." "Red"
}
exit $testExit