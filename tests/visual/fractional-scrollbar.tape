# Set PORTALS_FRACTIONAL_SCROLL_EXE to the built native sample before recording.
Output docs/images/fractional-scrollbar.gif
Set Shell powershell
Set Width 1080
Set Height 640
Set FontSize 22
Set FontFamily "Consolas"
Set Theme "nord"
Set CursorBlink false
Set TypingSpeed 0 ms
Set WaitTimeout 15 s
Hide
Type "& $env:PORTALS_FRACTIONAL_SCROLL_EXE; Write-Output ('FRACTIONAL_EXIT:' + $LASTEXITCODE)"
Enter
Wait+Screen /Fractional thumb/
Show
Sleep 1500 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Down 1
Sleep 600 ms
Right 1
Sleep 600 ms
Right 1
Sleep 600 ms
Right 1
Sleep 600 ms
Right 1
Sleep 600 ms
Right 1
Sleep 600 ms
Right 1
Sleep 600 ms
Right 1
Sleep 600 ms
Escape@0ms
Type@0ms "[H"
Sleep 1 s
Hide
Escape@0ms
Type@0ms "[21~"
Wait+Screen /FRACTIONAL_EXIT:0/
