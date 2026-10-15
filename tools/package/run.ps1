# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0

<#
.SYNOPSIS
Runs an example from an extracted Windows runnable bundle.
.DESCRIPTION
Finds the selected executable in bin relative to this launcher and forwards the
remaining arguments. This source file is installed at the bundle root.
.PARAMETER Example
Example name: dashboard (default), gallery, custom_widget, editor_history or
click_handlers. The name is positional; unknown values fail parameter validation.
.PARAMETER ExampleArguments
Remaining arguments forwarded to the example. Default: none. For named launcher
arguments use -Example and -ExampleArguments explicitly when needed.
.EXAMPLE
Get-Help ./run.ps1 -Full
.EXAMPLE
./run.ps1 gallery
.OUTPUTS
The example's output. Exits with its exit code. A missing executable throws.
.NOTES
Runs an interactive terminal application; example arguments may request replay or
other application-specific behavior. Does not build or download anything.
#>
[CmdletBinding()]
param(
    [ValidateSet('dashboard', 'gallery', 'custom_widget', 'editor_history', 'click_handlers')]
    [string] $Example = 'dashboard',
    [Parameter(ValueFromRemainingArguments)] [string[]] $ExampleArguments
)
$programs = @{
    dashboard = 'portals_dashboard.exe'
    gallery = 'portals_gallery.exe'
    custom_widget = 'portals_custom_widget.exe'
    editor_history = 'portals_editor_history.exe'
    click_handlers = 'portals_click_handlers.exe'
}
$program = Join-Path $PSScriptRoot ('bin/' + $programs[$Example])
if (-not (Test-Path -LiteralPath $program -PathType Leaf)) { throw "Example executable is missing: $program" }
& $program @ExampleArguments
exit $LASTEXITCODE
