# Native visual recordings

Public examples remain ordinary applications. Terminal automation and recorded
interaction sequences belong here.

`commands.tape` records `portals_commands_example`: menu selection remains visible
before Enter, Save copies the current editor value, and Clear leaves the saved
caption unchanged. With the same tools and preset described below:

```powershell
vhs validate tests/visual/commands.tape
vhs tests/visual/commands.tape
```

It writes `build/commands.gif` and `build/commands-frames/`, then checks that F10
exits successfully. The public application contains no recording or replay code.

`choices.tape` records the choices example on Windows using optional VHS, ttyd
and FFmpeg tools. Build `portals_choices_example` with the
`windows-msvc-release` preset, put the recording tools on the calling process's
PATH, then run from the repository root:

```powershell
vhs validate tests/visual/choices.tape
vhs tests/visual/choices.tape
```

The fixture writes `build/choices.gif` and PNG frames under
`build/choices-frames/`. Review the frames before copying selected PNG/GIF output into `docs/images/`.
Adjust the executable path and shell when using another build configuration or
platform. The keyboard-only demonstration pauses on each highlighted row before Enter,
then pauses again on the accepted value. Cursor blinking is disabled. It shows
filtered selection, delivery choices and palette
changes, then waits for the shell to report application exit code zero after F10. A recording failure is not proof of
an application defect; retain the command output and check the native behavior.

Some Windows VHS versions can save frames without producing the final GIF.
FFmpeg can assemble those actual frames when needed:

```powershell
ffmpeg -framerate 50 -start_number 1 -i build/choices-frames/frame-text-%05d.png `
  -vf "fps=12,palettegen=stats_mode=diff" build/choices-palette.png
ffmpeg -framerate 50 -start_number 1 -i build/choices-frames/frame-text-%05d.png `
  -i build/choices-palette.png -lavfi "fps=12 [x]; [x][1:v] paletteuse" `
  -loop 0 build/choices.gif
```

The PNG/GIF images show a captured native terminal session rendered by VHS;
they do not establish pixel-identical output on every terminal host. After
reviewing and preserving selected frames, remove the disposable frame directory.
