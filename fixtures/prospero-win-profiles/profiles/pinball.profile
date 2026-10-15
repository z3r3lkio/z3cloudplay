; Space Cadet Pinball through Wine. Supply your own copy of the game: it goes
; in the prefix at C:\Games\Pinball. Uses input/pinball.input.
[application]
id = pinball
name = Space Cadet Pinball
executable = C:\Games\Pinball\PINBALL.EXE
working_directory = C:\Games\Pinball
prefix = default
runtime = wine-wow64
architecture = pe32
graphics = gdi

[display]
; Wine's desktop, scaled to the whole screen keeping its aspect ratio.
desktop = 800x600
scaling = fit

[input]
; Flippers, plunger and nudges are shared with other Pinball profiles.
preset = pinball
