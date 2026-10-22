# Changelog

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
