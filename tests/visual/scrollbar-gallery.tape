# Each area uses the same ordinary content; show vertical and horizontal movement.
Output build/scrollbar-gallery.gif
Output build/scrollbar-gallery-frames/
Set Shell powershell
Set Width 900
Set Height 460
Set FontSize 16
Set Theme "nord"
Set CursorBlink false
Set TypingSpeed 0 ms
Set WaitTimeout 10 s
Hide
Type "& 'build/windows-msvc-release/portals_docs_scrollbar_gallery.exe'; Write-Output ('SCROLLBARS_EXIT:' + $LASTEXITCODE)"
Enter
Wait+Screen /Scrollbars on frames/
Show
Sleep 2 s
Down 4
Sleep 1500 ms
Right 6
Sleep 1500 ms
Tab
Down 4
Sleep 1500 ms
Right 6
Sleep 1500 ms
Tab
Down 4
Sleep 1500 ms
Right 6
Sleep 1500 ms
Tab
Down 4
Sleep 1500 ms
Right 6
Sleep 1500 ms
Up 4
Left 6
Sleep 2 s
Hide
Escape@0ms
Type@0ms "[21~"
Wait+Screen /SCROLLBARS_EXIT:0/
