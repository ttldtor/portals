# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0

<#
.SYNOPSIS
Builds a runnable x64 Release bundle from a committed Git revision.
.DESCRIPTION
Passes packaging settings to bundle.cmake. Builds examples, documentation and an
installed SDK, then checks the staged bundle. Working-tree edits are excluded.
.PARAMETER SourceDir
Git checkout to archive. Default: repository root, two levels above this script.
.PARAMETER Revision
Committed Git revision to archive. Default: HEAD.
.PARAMETER WorkDir
Required fresh staging directory outside SourceDir. Preserved after failure.
.PARAMETER OutputArchive
Required new archive path outside WorkDir: .zip on Windows, .tar.gz on Linux.
.PARAMETER Generator
CMake generator. Default: Ninja.
.PARAMETER CxxCompiler
Optional C++ compiler path forwarded as CMAKE_CXX_COMPILER; otherwise CMake chooses.
.PARAMETER CCompiler
Optional C compiler path forwarded as CMAKE_C_COMPILER; otherwise CMake chooses.
.PARAMETER PythonExecutable
Optional interpreter for extra HTML-link checks; omission skips that gate.
The documentation build independently requires Python.
.PARAMETER Runtime
Windows Release runtime: MultiThreaded (default) or MultiThreadedDLL.
.PARAMETER Parallel
Build parallelism from 1 through 1024. Default: 2.
.PARAMETER CMake
CMake executable command or path. Default: cmake; requires version 3.24 or newer.
.EXAMPLE
Get-Help ./tools/package/bundle.ps1 -Full
.EXAMPLE
./tools/package/bundle.ps1 -WorkDir C:/staging/new-bundle -OutputArchive C:/artifacts/portals.zip -Parallel 4
.OUTPUTS
CMake/build/check output, staging files and the requested archive. Success returns
normally. A nonzero CMake exit throws and preserves staging for diagnosis.
.NOTES
Requires Git, native x64 tools, documentation tools and package-check dependencies.
Uses subprocesses, creates staging/output files and runs bundle checks. Does not
publish a release or change Git history. See bundle.cmake -DHELP=ON usage for the
underlying interface. Get-Help only reads this comment-based help.
#>
[CmdletBinding()]
param(
    [string] $SourceDir = (Join-Path $PSScriptRoot '../..'),
    [string] $Revision = 'HEAD',
    [Parameter(Mandatory)] [string] $WorkDir,
    [Parameter(Mandatory)] [string] $OutputArchive,
    [string] $Generator = 'Ninja',
    [string] $CxxCompiler,
    [string] $CCompiler,
    [string] $PythonExecutable,
    [ValidateSet('MultiThreaded', 'MultiThreadedDLL')] [string] $Runtime = 'MultiThreaded',
    [ValidateRange(1, 1024)] [int] $Parallel = 2,
    [string] $CMake = 'cmake'
)
$ErrorActionPreference = 'Stop'
$arguments = @(
    "-DSOURCE_DIR=$([IO.Path]::GetFullPath($SourceDir))",
    "-DSOURCE_REVISION=$Revision",
    "-DWORK_DIR=$([IO.Path]::GetFullPath($WorkDir))",
    "-DOUTPUT_ARCHIVE=$([IO.Path]::GetFullPath($OutputArchive))",
    "-DGENERATOR=$Generator",
    "-DMSVC_RUNTIME_LIBRARY=$Runtime",
    "-DPARALLEL=$Parallel"
)
if ($CxxCompiler) { $arguments += "-DCXX_COMPILER=$CxxCompiler" }
if ($CCompiler) { $arguments += "-DC_COMPILER=$CCompiler" }
if ($PythonExecutable) { $arguments += "-DPYTHON_EXECUTABLE=$PythonExecutable" }
$arguments += @('-P', (Join-Path $PSScriptRoot 'bundle.cmake'))
& $CMake @arguments
if ($LASTEXITCODE -ne 0) { throw "Bundle creation failed with exit code $LASTEXITCODE. Staging was preserved." }
