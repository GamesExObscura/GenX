# GenX FAQ

- [Getting started](#getting-started)
- [Games and BIOS files](#games-and-bios-files)
- [Controls](#controls)
- [Netplay](#netplay)
- [Saves](#saves)
- [Problems](#problems)
- [About the project](#about-the-project)

---

## Getting started

### Windows says "Windows protected your PC" when I run genx.exe

Windows shows this for any program that isn't signed with a paid code-signing
certificate, and GenX isn't. Click **More info → Run anyway**.

Some antivirus programs also flag unsigned emulators for the same reason. If
you'd rather not trust a downloaded exe, the full source is in this repository
and you can [build it yourself](README.md#building-from-source).

### What do I need to run it?

- 64-bit Windows 10 or 11.
- Nothing else for the release download. Unzip it and run `genx.exe`.

If you build GenX yourself with the default settings and get *"MSVCP140.dll was
not found"*, install Microsoft's
[Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe),
or build with the static runtime option shown in the README.

### Does it run on Linux or macOS?

Not natively. GenX is built directly on the Windows APIs. Wine may work, but it
hasn't been tested.

### A 32X game runs slowly

The 32X has two SH-2 processors on top of a whole Genesis, which makes it the
heaviest system GenX emulates. Turn on **View → FPS counter** to check. A fast
desktop CPU holds full speed; an older laptop may not.

## Games and BIOS files

### Where do I get games and BIOS files?

GenX doesn't include or link to any, and the project can't help you find them.
Use dumps of cartridges, discs and consoles you own.

### Which BIOS files do I need?

| System | BIOS |
|---|---|
| Genesis / Mega Drive | None |
| Game Gear / Master System | None |
| 32X | Optional. GenX has its own start-up code. |
| Sega CD / Mega-CD | **Required**, one per region |

**32X:**
- To use the real 32X BIOS, put all three files in one folder and choose it in
  **Configure → BIOS**.
- GenX recognises the files by size, so their names don't matter:
  - 256 bytes: Genesis
  - 2,048 bytes: master SH-2
  - 1,024 bytes: slave SH-2

**Sega CD:**
- A disc only boots with the BIOS for its own region: a USA disc needs a USA
  BIOS, and so on.
- If no folder is set in **Configure → BIOS**, GenX looks for a BIOS folder
  near your games.

### My Sega CD game won't start

Go through these in order:

1. **Open the `.cue` file, not a `.bin`.** The `.cue` lists every track. A
   lone `.bin` only holds the data track, so the music is lost.
2. **Check the BIOS region.** **Configure → BIOS** shows which file each region
   will boot from.
3. **Try the US v2.21 BIOS.** It's the most tested version.
4. **Check the file names.** If the disc came as several `.bin` files, keep
   them together and named as the `.cue` expects. GenX tries to match renamed
   files, but it can only guess.

### Sega CD 32X games won't start

Sega CD 32X discs include Night Trap, Corpse Killer, Supreme Warrior and
Fahrenheit. Some of them need the real 32X BIOS as well as the Sega CD BIOS.
Set both folders in **Configure → BIOS**.

### What file types can I open?

| System | File types |
|---|---|
| Genesis / Mega Drive | `.md`, `.gen`, `.smd`, `.bin`, `.zip` |
| 32X | `.32x`, `.bin`, `.zip` |
| Sega CD | `.cue` (with its `.bin` files), `.iso`, `.chd` |
| Game Gear / Master System | `.gg`, `.sms`, `.zip` |

## Controls

### How do I set up a controller?

1. Open **Emulation → Configure → Input**.
2. Pick the controller type: 3 button, 6 button, Justifier, Menacer or Miracle
   Piano.
3. Pick the device: Keyboard, XInput or DirectInput.
   - XInput covers Xbox-style pads, including most modern controllers.
   - DirectInput covers older or unusual pads.
4. Click a button's box and press what you want for it. Right-click a box to
   clear it.
5. **Save as…** keeps the setup as a profile, so you can switch back to it
   later.

### My controller isn't detected, or it stopped responding

- **Check the status line.** Under the controller picture, the Input tab shows
  "Controller N: connected" or "not found". If it says not found, try the
  other device type (XInput or DirectInput).
- **Check other programs.** Steam Input, DS4Windows and similar tools can take
  a controller over or move it to a different slot. GenX follows a pad that
  reappears in a new slot, but closing those tools rules them out.
- **Write a diagnostic log.** If nothing works at all, press **Ctrl+L**. The
  log names which window had your key presses and what happened to the last
  16 of them.

### Esc closed GenX

**Esc** is the default Quit hotkey. You can rebind or clear it in **Configure →
Hotkeys**. Every hotkey can be changed there, and **Restore defaults** puts
them back.

### A keyboard key does something I didn't bind

- **Hotkeys come first.** Every default hotkey is listed in the README. A key
  used by a hotkey won't reach the game.
- **Plain number keys load save states.** Keys 1 to 0 load states 1 to 10, and
  Shift plus a number saves.
- **Gamepad players don't use keyboard keys.** If a player is set to a
  gamepad, only the gamepad's buttons control that player.

### How do the light guns work?

You aim with the mouse in both cases. Trigger and reload can be any mouse
button or key.

**Menacer:**
- Always plugs into port 2, as the real receiver did.
- Every Menacer game draws its own crosshair.

**Justifier:**
- Shows its own crosshair.
- For two players, set both players to Justifier. Each player gets their own
  mouse, claimed in the order the mice are first moved.
- **Emulation → Re-pair light guns** starts the claiming over.

### How do I play two-player Game Gear games?

**Multiplayer → Gear Link** opens a second Game Gear beside the first, joined
by the link cable, with both running the same game.

Each console takes its own controller. **Multiplayer → Show both consoles /
Show console 1 only / Show console 2 only** chooses which consoles are on
screen.

### Can I use cheats?

Yes. **Cheats → Game Genie codes** accepts Game Genie codes for the Genesis,
Game Gear and Master System. Codes are saved, and you can show only the ones
for the loaded game.

### How do fast forward, rewind and turbo work?

Hold the hotkey for each:

| Action | Default key |
|---|---|
| Fast forward | `.` |
| Rewind | `,` |
| Turbo (auto-fire) | `/` |

## Netplay

### How do I play with a friend online?

Both players load **the same game file** first.

**Host:**
1. Choose **Multiplayer → Start netplay**.
2. GenX shows your address, the port (47215 by default) and a new passphrase.
3. Send all three to the other player.
4. To play as player 2, tick **I play as player 2**.

**Joiner:**
1. Choose **Multiplayer → Join netplay**.
2. Enter the host's address, port and passphrase.

**Input delay:**
- The input delay is in frames, from 1 to 20. The default is 4.
- Raise it if the game stutters, or lower it for sharper controls on a good
  connection.

**Game Gear:** both players open **Multiplayer → Gear Link** with the same game
first. Then one player starts netplay and the other joins, and each of you
plays your own console.

### It won't connect

- **On the same network:** use the local address the host's dialog shows.
- **Over the internet:** the host's router must forward **TCP port 47215** (or
  whichever port you chose) to the host's PC. If you'd rather not touch the
  router, a virtual LAN tool that puts both PCs on one private network works
  too. Examples are Tailscale, ZeroTier and Radmin VPN.
- **Firewall:** allow GenX through Windows Firewall when it asks, on the host.
- **Matching files:** both players need the same game file. For Sega CD and
  32X games, they also need the same BIOS. GenX refuses to connect if they
  differ.

### Is netplay safe?

Here is what netplay does and doesn't do:

- **No passphrase, no session.** Nothing gets in without it. GenX makes a new
  passphrase for every session it hosts.
- **The connection is encrypted** with AES-256-GCM, using a key made from the
  passphrase.
- **Incoming game states are checked first.** When you join, you receive the
  host's game state. GenX checks its sizes and value ranges before using it,
  and save-state files from anyone get the same checks. That code was tested
  by feeding it thousands of deliberately broken states.
- **GenX never runs anything it receives.** Only the game state and button
  presses travel between players.

As with any online game, only share your passphrase with the person you're
playing with.

### Will joining a game overwrite my saves?

No. While you're joined you play on the host's save data, and your own comes
back when the session ends.

### The two games drift apart (desync)

Netplay keeps both PCs in lockstep, so drift means something differs between
the two ends:

- Both players should run **the same GenX release**.
- Both should use the same game file and, for Sega CD and 32X, the same BIOS.

If it still drifts, open an issue with the game name and roughly when it
happened.

## Saves

### Where are my saves, save states and settings?

Everything is in `%APPDATA%\GenX`. **File → Open GenX folder** takes you there.

In portable mode, everything is in the `User` folder next to `genx.exe`
instead. Portable mode is on whenever that folder exists.

| Folder or file | What it holds |
|---|---|
| `saves\` | In-game battery saves (`.srm`, `.eep`) and Sega CD backup RAM (`.brm`, one per region) |
| `states\` | Save states, 10 slots per game |
| `profiles\` | Controller profiles |
| `logs\` | Diagnostic logs |
| `settings.txt` | Your settings, as plain text |

To keep battery saves somewhere else, set a save folder in **Configure →
General**. That's useful for sharing them with another emulator.

### Why do all my Sega CD games share one save file?

Because the real console works that way. The Sega CD's backup memory is inside
the console, and every game saves into it. GenX keeps one backup file per
region, so the BIOS's memory manager shows all of them together, just like the
hardware.

## Problems

### How do I report a bug?

Open an [issue](https://github.com/GamesExObscura/GenX/issues) and include:

- The game, its region, and the file type (`.cue`, `.chd`, `.zip` and so on).
- What you expected and what happened, plus a screenshot if it's visual.
- A diagnostic log. Press **Ctrl+L** after the problem appears, then attach the
  newest file from **File → Open log folder**.

**Check the log before posting it.** It includes file paths from your PC,
which may contain your Windows user name.

### The picture looks blurry, or too sharp

Try these View menu options:

- **View → Pixel smoothing** switches between smooth scaling and sharp pixels.
- **View → 1x–4x** sets the window size.
- **View → CRT filter** adds scanlines.

## About the project

### Why build GenX?

Games Ex Obscura is about game preservation, and about making every game
playable. GenX is part of that. I wanted to build a new open-source Sega
emulator for Windows that could match the capabilities of others out there,
such as Kega Fusion, and give the community one place to build on it and
improve it. Most of all, I want fans to be able to play their favorite games,
preserve them, rekindle those memories from childhood and pass them on.

— Games Ex Obscura (Geo)

### Does GenX contain code from other emulators?

No. GenX was written from scratch. Other open-source emulators were read as
references for how the hardware behaves, and the source comments say where a
fact was checked against one. The [credits](README.md#credits) name them.

### Why does the code say "Mercury"?

Mercury was the project's working name. The emulation library kept it, and
only the program you run is called GenX.

### What license is GenX under?

The GNU General Public License, version 3. You can use, study, share and modify
GenX. If you distribute a modified version, it has to stay under the same
license, with its source available. See [LICENSE](LICENSE).
