# Drive the unchanged native command example; pause on selection before activation.
Output build/commands.gif
Output build/commands-frames/
Set Shell powershell
Set Width 900
Set Height 340
Set FontSize 16
Set Theme "nord"
Set CursorBlink false
Set TypingSpeed 0 ms
Set WaitTimeout 10 s

Hide
Type "& 'build/windows-msvc-release/examples/commands/portals_commands_example.exe'; Write-Output ('COMMANDS_EXIT:' + $LASTEXITCODE)"
Enter
Wait+Screen /Command desk/
Show
Sleep 1500 ms

# Focus the menu, show Clear, then select Save and keep it highlighted.
Tab
Sleep 1 s
Down
Sleep 1500 ms
Up
Sleep 1500 ms
Enter
Wait+Screen /Remember the milk/
Sleep 1500 ms

# Clear the draft; the saved caption remains unchanged.
Down
Sleep 1500 ms
Enter
Sleep 1500 ms
Shift+Tab
Type@120ms "A fresh note"
Sleep 1 s
Tab
Up
Sleep 1500 ms
Enter
Wait+Screen /A fresh note/
Sleep 2 s

Hide
Escape@0ms
Type@0ms "[21~"
Wait+Screen /COMMANDS_EXIT:0/
