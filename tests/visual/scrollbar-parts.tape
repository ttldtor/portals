# Record the ordinary guide. SGR presses hold the actual scrollbar arrow buttons.
Output build/scrollbar-parts.gif
Output build/scrollbar-parts-frames/
Set Shell powershell
Set Width 900
Set Height 420
Set FontSize 16
Set Theme "nord"
Set CursorBlink false
Set TypingSpeed 0 ms
Set WaitTimeout 15 s
Hide
Type "& 'build/windows-msvc-release/portals_docs_scrollbar_parts.exe'; Write-Output ('PARTS_EXIT:' + $LASTEXITCODE)"
Enter
Wait+Screen /Independent parts/
Show
Sleep 2 s
Escape@0ms
Type@0ms "[<0;26;10M"
Sleep 900 ms
Escape@0ms
Type@0ms "[<0;26;10m"
Sleep 2 s
Escape@0ms
Type@0ms "[<0;54;10M"
Sleep 900 ms
Escape@0ms
Type@0ms "[<0;54;10m"
Sleep 2 s
Escape@0ms
Type@0ms "[<0;54;3M"
Sleep 700 ms
Escape@0ms
Type@0ms "[<0;54;3m"
Sleep 2 s
Escape@0ms
Type@0ms "[<0;25;11M"
Sleep 700 ms
Escape@0ms
Type@0ms "[<0;25;11m"
Sleep 2 s
Hide
Escape@0ms
Type@0ms "[21~"
Wait+Screen /PARTS_EXIT:0/
