# Readable native choices demonstration. The application is unchanged; this fixture drives keyboard input.
# Leave the highlighted row visible before Enter and the accepted value visible afterward.
Output build/choices.gif
Output build/choices-frames/
Set Shell powershell
Set Width 1100
Set Height 440
Set FontSize 16
Set Theme "nord"
Set CursorBlink false
Set TypingSpeed 0 ms
Set WaitTimeout 10 s

Hide
Type "& 'build/windows-msvc-release/examples/choices/portals_choices_example.exe'; Write-Output ('CHOICES_EXIT:' + $LASTEXITCODE)"
Enter
Wait+Screen /Shipping desk/
Show
Sleep 1500 ms

# Type a query, highlight Prague, then confirm with Enter.
Type@180ms "Pra"
Wait+Screen /Prague/
Sleep 1200 ms
Down
Sleep 1800 ms
Enter
Wait+Screen /City: Prague/
Sleep 2 s

# Open delivery choices, move to Pickup point, then confirm.
Tab
Sleep 500 ms
Down
Wait+Screen /Pickup point/
Sleep 1200 ms
Down
Sleep 1800 ms
Enter
Wait+Screen /Delivery: Pickup point/
Sleep 2 s

# Show the highlighted palette before applying it.
Tab
Sleep 500 ms
Down
Wait+Screen /Catppuccin Latte/
Sleep 1 s
Down
Sleep 1800 ms
Enter
Wait+Screen /Palette +Gruvbox +\[ v \]/
Sleep 2 s

Down
Wait+Screen /Catppuccin Latte/
Sleep 1 s
Down
Sleep 1800 ms
Enter
Wait+Screen /Palette +Catppuccin Latte +\[ v \]/
Wait+Screen /City: Prague/
Sleep 2500 ms
Hide
Escape@0ms
Type@0ms "[21~"
Wait+Screen /CHOICES_EXIT:0/
