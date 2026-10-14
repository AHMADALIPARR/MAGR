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
    Verify the vendored Netlib BLAS sources are byte-identical to upstream.

.DESCRIPTION
    Re-computes SHA-256 for every file listed in blas-3.12.0.sha256 and compares
    against the recorded digest.

    This is the guard on the one provenance claim in this repository that can
    actually be verified without a compiler. The eight `source_digest` values in
    fortran-functors/descriptors/functors.json were produced by an external
    build; this script confirms they describe the bytes on disk.

    Exits non-zero on any mismatch. A mismatch means one of three things, and
    the operator has to work out which:
      - a legitimate local patch was made (must be recorded separately, per
        Netlib's terms, with the routine renamed and the change commented)
      - the file was corrupted in transit
      - somebody silently "fixed" a routine that was supposed to be untouched

.PARAMETER Manifest
    Path to the digest manifest. Default: fortran-functors/upstream/blas-3.12.0.sha256

.EXAMPLE
    ./tools/verify_upstream.ps1
#>
[CmdletBinding()]
param([string] $Manifest)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $Manifest) {
    $Manifest = Join-Path $Root "fortran-functors\upstream\blas-3.12.0.sha256"
}
if (-not (Test-Path -LiteralPath $Manifest)) {
    Write-Error "digest manifest not found: $Manifest"
    exit 1
}

$Base = Join-Path $Root "fortran-functors\upstream"
$checked = 0
$missing = @()
$mismatch = @()

foreach ($line in Get-Content -LiteralPath $Manifest) {
    if ($line -match '^\s*([0-9a-fA-F]{64})\s+(.+?)\s*$') {
        $expected = $Matches[1].ToLower()
        $rel      = $Matches[2] -replace '/', '\'
        $path     = Join-Path $Base $rel
        $checked++

        if (-not (Test-Path -LiteralPath $path)) {
            $missing += $rel
            continue
        }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
        if ($actual -ne $expected) {
            $mismatch += [pscustomobject]@{ File = $rel; Expected = $expected; Actual = $actual }
        }
    }
}

Write-Host ""
Write-Host "upstream verification: $checked file(s) from $(Split-Path -Leaf $Manifest)" -ForegroundColor Cyan
Write-Host ""

if ($missing.Count -gt 0) {
    Write-Host "MISSING ($($missing.Count)):" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}
if ($mismatch.Count -gt 0) {
    Write-Host "MODIFIED ($($mismatch.Count)):" -ForegroundColor Red
    $mismatch | ForEach-Object {
        Write-Host "  $($_.File)" -ForegroundColor Red
        Write-Host "    expected $($_.Expected)" -ForegroundColor DarkYellow
        Write-Host "    actual   $($_.Actual)"   -ForegroundColor DarkYellow
    }
}

if ($missing.Count -eq 0 -and $mismatch.Count -eq 0) {
    Write-Host "all $checked files are byte-identical to the recorded digests" -ForegroundColor Green
    Write-Host "the vendored sources are unmodified Netlib BLAS 3.12.0" -ForegroundColor DarkGray
    exit 0
}

Write-Host ""
Write-Host "The vendored sources must stay unmodified. If you changed one" -ForegroundColor Yellow
Write-Host "deliberately, record it in docs/ and update the manifest with a" -ForegroundColor Yellow
Write-Host "written note -- silently changing vendored code breaks the only" -ForegroundColor Yellow
Write-Host "provenance claim this repository can independently verify." -ForegroundColor Yellow
exit 1