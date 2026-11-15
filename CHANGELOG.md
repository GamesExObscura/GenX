# Changelog

## Unreleased

### Added

- **Light gun games get their gun by themselves.** Open a Mega Drive or
  Sega CD game made for a light gun and GenX plugs that gun into port 2 for
  as long as the game runs, aimed with the mouse: the Menacer for the Menacer
  6-Game Cartridge, T2: The Arcade Game, Body Count, Corpse Killer, Mad Dog
  McCree, Mad Dog II, Who Shot Johnny Rock?, Crime Patrol, Space Pirates and
  Drug Wars; the Justifier for Lethal Enforcers and Lethal Enforcers II
  (cartridge and CD). Player 1 keeps the pad for the menus. Nothing changes
  if a player has already chosen a gun, and player 2's own controller comes
  back when the game closes. **Emulation → Plug in light guns by game** turns
  it off.
- **A sight for the Sega CD Menacer games that don't draw one.** Mad Dog
  McCree and the other American Laser Games discs, and Corpse Killer, take
  the Menacer but show no crosshair. GenX now draws its own sight for them
  instead of hiding the pointer.
- **Master System Light Phaser and Paddle Control** (issue #5). Light Phaser
  games are aimed with the mouse: the pointer becomes a sight, and the left
  button (or the pad's button 1) pulls the trigger. Paddle games follow the
  mouse left and right across the picture, and the pad's left and right turn
  the knob too; the left button is the paddle's button. GenX plugs in the
  right device by itself for the games that need one: Gangster Town, Safari
  Hunt, Shooting Gallery, Marksman Shooting, Operation Wolf, Rambo III,
  Wanted, Assault City, Laser Ghost, Space Gun, Rescue Mission, Missile
  Defense 3-D and 3-D Gunner with the Phaser; Galactic Protector, Woody Pop,
  Alex Kidd BMX Trial and Megumi Rescue with the paddle. **Emulation →
  Master System controller** chooses something else, for example the pad
  for Hang-On on the Hang-On / Safari Hunt cartridge. Both devices are also
  in player 1's list in **Configure → Input**, with their own pictures and
  binding rows: Trigger on the Phaser; Turn left, Turn right and Button on
  the paddle. Mega Drive games keep player 1's pad whichever is chosen.
- **Sega CD → 32X plugged in.** Untick it to run Sega CD discs on a console
  with no 32X attached. Fahrenheit's first disc then plays its Sega CD version
  instead of asking for disc 2, and Surgical Strike skips its "Sega CD or 32X"
  question. The game restarts when you change it. The setting is saved.

### Fixed

- **Pier Solar and the Great Architects now plays, and saves** (issue #7). It
  stopped for good on its opening disclaimer screen. Pier Solar's cartridge is
  WaterMelon's own design: a 64-megabit board with its own bank switching, a
  64 KB save chip, and a copy-protection trick that real cartridges pass and
  plain ROM images fail. GenX now emulates the whole board. The game gets
  past the disclaimer and asks for its language on the first run. It plays,
  and its saves are written to the game's save file like any other
  cartridge's.
- **Trouble Shooter and Decap Attack: the buttons now do what they should**
  (issue #9). C was read as Start and B as A, and the real A and Start buttons
  did nothing. Both games read A and Start a moment after letting go of the
  controller's select line, before the line has risen on real hardware. GenX
  treated the line as risen at once, and now waits for it the way a real
  controller does.
- **Trouble Shooter: the intro's centre portraits are back.** Madison's and
  Crystal's large portraits, with Madison's mouth and Crystal's eyes
  animating, were missing from the middle of the screen. Only those animated
  parts showed, floating between the smaller copies. The intro uses a
  background size real hardware doesn't support, which makes its rows wrap
  around, and GenX didn't wrap them. This also removes a strip of stray
  tiles at the top-left of the court in Barkley Shut Up and Jam!.
- **Madou Monogatari I (Game Gear): no more broken line under Arle's
  picture** (issue #16). In the bedroom scene and others like it, the last
  line of the picture above the text box showed blocks of the wrong graphics.
  The game switches to the text box's sprites partway down the screen, and
  changes the sprite size a moment before it changes which sprites to use. On
  a real Game Gear the size change arrives too late to affect the picture's
  last line. GenX applied it to that line, which drew it with the wrong sprite
  size. GenX now settles each line's sprites just before the line starts, as
  the console does.
- **Master System timing now matches the console in FluBBa's VDP Test**
  (issue #3). The test's timing page now passes in full:
  - The H counter (port $7F) keeps its value until something latches it.
  - Writing port $3F latches it, as on a console.
  - It counts in the console's exact pattern.
  - The line interrupt, the line counter's change and the frame-interrupt flag
    land at the same points in the line as on a console.

  A horizontal scroll change made in the last moments of a line now takes
  effect a line later, as on a console, so the test's scroll column is
  straight. Three of the test's sprite checks still differ:
  - Two are quirks of the first-model Master System.
  - The third, the exact moment of the collision flag, needs GenX to draw each
    line later in the line than it does now. Fantastic Dizzy's status bar
    relies on the two matching.

  The test's Game Gear and Mega Drive pages check those consoles, not a Master
  System. Game Gear games are unchanged.
- **The 1536-colour demo shows all its colours** (issue #22). Charles
  MacDonald's TEST1536 showed flat bands of the same few colours instead of
  its smooth gradients. It sends four new colours to the video chip on every
  line, and tells the chip where to read them from only once per frame. On a
  real console, the chip remembers where each transfer stopped, so the next
  one picks up from there. GenX started every transfer from the same place,
  so every line got the same four colours. Copies inside the video memory
  now carry on the same way.
- **QuackShot (Rev A): no more black screen** (issue #11). The 512 KB dump of
  this cartridge stores its two ROM chips back to back. The game expects the
  second chip at a higher address, so loaded as-is it never started. GenX now
  rebuilds the layout the game expects, the same as the 1.25 MB dump.
- **Triple Play Gold Edition and Triple Play 96: the Game Setup menu is back**
  (issue #8). The menu showed only its heading and an empty box. These games
  slide the menu into view with a chain of mid-screen interrupts, and they
  count on one extra interrupt that GenX's line counter left out. The chain
  ran one step behind, so the menu never scrolled into place. Both games now
  use the counting rule they need. The credits on the title screen, missing
  for the same reason, are back too.
- **Fantastic Dizzy: no more buzzing in the inventory menu** (issue #20). On
  a real console, the FM sound chip is reset whenever the game resets the
  sound processor. Dizzy does that when the menu opens, which silences the
  music. GenX didn't reset the chip, so a note was left hanging at full volume.
  Other games that reset the sound processor between scenes now cut off their
  old notes at that point too, as on a console.
- **OutRunners: no more white dots in the sky** (issue #23). Every line, the
  game sends the next sky colour to the video chip. On the line after its
  colour list ends, it sends a "read" command and then writes anyway. Real
  hardware ignores that write. GenX stored it in the graphics the sky is
  built from, which put one dot in every 8×8 block of sky. GenX now ignores
  it too. This also removes a stray brown square from the top-left corner of
  Mad Dog II's menu (Sega CD).
- **Toy Story: no more stray line across the floor** (issue #21). A one-pixel
  strip near the bottom of the screen slid against the background. The game
  switches the floor's scrolling partway down the screen, and on real hardware
  the line where it switches still scrolls the old way. GenX switched that line
  too early and read a leftover value for it. Burning Force and Deadly Moves
  switch the same way and now match the hardware on that line too.
- **Combat Cars: no more ghost image in two-player mode** (issue #19). The
  game times its split screen by watching the TV line it's on. In its
  high-resolution two-player mode, the console reports that line in a
  different form, and GenX didn't. The split happened in the wrong place on
  every other frame, so two different pictures were laid over each other.
- **ESPN Sunday Night NFL no longer freezes at the coin toss** (issue #13).
  GenX always plugged a controller into port 2, even when player 2 had no
  buttons set. The game then gave the away team to a second player and
  waited for that player to call heads or tails. Port 2 is now empty unless
  player 2 has a controller or at least one button set in **Configure →
  Input**, as on a console with one controller. The computer then plays the
  away team and calls the toss.
- **Sega CD 32X discs no longer show a black screen without the 32X BIOS.**
  With no 32X BIOS, GenX now runs a disc as on a Sega CD with no 32X. Discs
  that need one (Corpse Killer, Night Trap, Slam City, Supreme Warrior) show
  their own "this game requires a 32X" message, and the status bar says the
  32X BIOS is missing. Fahrenheit's first disc and Surgical Strike play their
  Sega CD versions.

## v1.1.3 (2026-11-11)

### Added

- **Play Sega CD discs from your PC's CD/DVD drive.** Choose **File → Open ROM
  → Sega CD from drive** and pick the drive with the disc in it. GenX reads the
  game and its CD music straight from the disc, with a read-ahead cache so the
  drive's seeks don't hold the game up.
  - **Sega CD → Swap to disc in drive** changes discs mid-game. Putting a new
    disc into the drive that's playing swaps it automatically, like opening a
    real Sega CD's tray.
  - The recent files list remembers the drive, and `genx.exe E:\` starts the
    disc in drive E: from the command line or a front end.
  - Save states and logs for a disc in a drive are named after the game's
    title.

## v1.1.2 (2026-11-10)

### Fixed

- **Crash when opening a Sega CD disc.** GenX closed without a message when
  a disc sat near a file or folder whose name has certain characters, such as
  an accent stored as a separate mark (some "Pokémon" names) or Japanese text.
  Names of any kind now work, everywhere GenX reads them.
- **Japanese and Southeast Asian Sega CD BIOSes now start.** GenX always
  presented itself as a US or European console, so these BIOSes stopped on an
  error such as "This is a PAL-compatible Mega CD for exclusive use in
  Southeast Asia". GenX now works out which console a BIOS belongs to and
  becomes that console, including the Southeast Asian model's 50 Hz.
- **Japanese discs get a Japanese BIOS.** When a BIOS folder holds both, a
  Japanese disc now gets a Japanese BIOS rather than the Southeast Asian one.
  Some BIOS packs name the Southeast Asian BIOS `jp_mcd1_9112.bin`, which GenX
  used to pick first.
- The **BIOS** tab in **Configure** now shows the BIOS each region's discs will
  really use.

## v1.1.1 (2026-10-22)

### Added

- **Version for front ends.** `genx.exe -v` (or `--version`) prints the
  version, such as `GenX 1.1.1`, and exits without opening a window. The
  version is also in the exe's **Properties → Details**. Front ends such as
  LaunchBox can use either one to see which version is installed.

## v1.1.0 (2026-10-22)

### Added

- **Portable mode.** Make a folder named `User` next to `genx.exe`, and GenX
  keeps its settings, saves, save states, controller profiles, cheats and logs
  in it instead of in `%APPDATA%\GenX`. The whole thing can then run from a USB
  stick. Without the folder, nothing changes. To bring your current setup with
  you, copy what's in `%APPDATA%\GenX` into the `User` folder.
- **Update check.** When GenX starts, it asks GitHub whether there's a newer
  release. If there is, one message shows your version and the new one, and
  offers to open the download page.
  - **Help → Check for updates** checks at any time.
  - To stop the startup check, untick **Check for updates when GenX starts** in
    **Emulation → Configure → General**.
  - GenX doesn't check when a front end such as LaunchBox starts it with a
    game, and the message never appears over a game.
  - The check sends nothing about you or your games.

## v1.0.0 (2026-10-04)

The first public release.
