# Record the ordinary guide; focus changes make the active framed button visible.
Output build/frame-styles.gif
Output build/frame-styles-frames/
Set Shell powershell
Set Width 780
Set Height 520
Set FontSize 16
Set Theme "nord"
Set CursorBlink false
Set TypingSpeed 0 ms
Set WaitTimeout 10 s
Hide
Type "& 'build/windows-msvc-release/portals_docs_frame_styles.exe'; Write-Output ('FRAMES_EXIT:' + $LASTEXITCODE)"
Enter
Wait+Screen /Select a frame with Enter/
Show
Sleep 2 s
Enter
Wait+Screen /Frame: single/
Sleep 1 s
Tab
Enter
Wait+Screen /Frame: rounded/
Sleep 1 s
Tab
Enter
Wait+Screen /Frame: double/
Sleep 1 s
Tab
Enter
Wait+Screen /Frame: heavy/
Sleep 1 s
Tab
Enter
Wait+Screen /Frame: ascii/
Sleep 2 s
Hide
Escape@0ms
Type@0ms "[21~"
Wait+Screen /FRAMES_EXIT:0/
