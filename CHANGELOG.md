# Changelog

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
