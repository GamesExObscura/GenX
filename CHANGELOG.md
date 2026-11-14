# Changelog

## Unreleased

### Added

- **Sega CD → 32X plugged in.** Untick it to run Sega CD discs on a console
  with no 32X attached. Fahrenheit's first disc then plays its Sega CD version
  instead of asking for disc 2, and Surgical Strike skips its "Sega CD or 32X"
  question. The game restarts when you change it. The setting is saved.

### Fixed

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
