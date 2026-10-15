# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
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
