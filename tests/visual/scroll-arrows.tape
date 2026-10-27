# Deliver SGR mouse press/release to the native guide. Hold DOWN, release, hold UP.
Output build/scroll-arrows.gif
Output build/scroll-arrows-frames/
Set Shell powershell
Set Width 760
Set Height 340
Set FontSize 16
Set Theme "nord"
Set CursorBlink false
Set TypingSpeed 0 ms
Set WaitTimeout 15 s
Hide
Type "& 'build/windows-msvc-release/portals_docs_scroll_controls.exe'; Write-Output ('ARROWS_EXIT:' + $LASTEXITCODE)"
Enter
Wait+Screen /Choose a section/
Show
Sleep 2 s
Escape@0ms
Type@0ms "[<0;28;6M"
Sleep 900 ms
Escape@0ms
Type@0ms "[<0;28;6m"
Sleep 2 s
Escape@0ms
Type@0ms "[<0;28;3M"
Sleep 700 ms
Escape@0ms
Type@0ms "[<0;28;3m"
Sleep 2 s
Hide
Escape@0ms
Type@0ms "[21~"
Wait+Screen /ARROWS_EXIT:0/
