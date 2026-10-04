// Resource IDs shared between runtime_win32.rc and runtime_win32.cpp.
//
// Plain #defines rather than constexpr: this header is also read by the
// resource compiler, which only understands the preprocessor.
#ifndef MERCURY_RUNTIME_RESOURCES_HPP
#define MERCURY_RUNTIME_RESOURCES_HPP

#define IDI_APPICON 101

// The logo is embedded as the original PNG and decoded once at startup, rather
// than converted to a bitmap at build time. It carries an alpha channel and has
// to sit on either a dark or a light background, so the transparency has to
// survive into the executable.
#define IDR_LOGO_PNG 201

// Controller artwork for the Input tab, embedded the same way and for the same
// reason: the backgrounds are keyed out, so the alpha has to survive.
// Contiguous, so a controller kind maps to an image by adding an offset.
#define IDR_PAD_3BUTTON 210
#define IDR_PAD_6BUTTON 211
#define IDR_PAD_JUSTIFIER 212
#define IDR_PAD_MENACER 213
#define IDR_PAD_PIANO 214

// Open ROM is a submenu, one entry per system, each starting in that system's
// own games folder. IDM_FILE_OPEN stays the Mega Drive entry so the existing
// Ctrl+O binding and its menu label keep working.
#define IDM_FILE_OPEN 40001
#define IDM_FILE_CLOSE 40002
#define IDM_FILE_EXIT 40003
// 40030 and up, NOT 40011: the recent list owns ten contiguous ids from 40010,
// and the handler turns anything in that range into a recent-file index before
// the switch ever sees it. Five items put at 40011 were silently opening recent
// games instead. Anything added to the File menu goes ABOVE the recent block.
#define IDM_FILE_OPEN_GG 40030
#define IDM_FILE_OPEN_32X 40031
#define IDM_FILE_OPEN_CD 40032
// Straight to the folders GenX keeps its own files in, so a setting or a log
// can be looked at without hunting through AppData for it.
#define IDM_FILE_OPEN_GENX_FOLDER 40033
#define IDM_FILE_OPEN_LOG_FOLDER 40034

// Ten contiguous slots for the recent list, so the handler can turn a command
// into an index by subtracting the first.
#define IDM_FILE_RECENT_FIRST 40010
#define IDM_FILE_RECENT_LAST 40019
#define IDM_FILE_RECENT_CLEAR 40020
#define IDM_FILE_RECENT_EMPTY 40021

#define IDM_EMU_PAUSE 40101
#define IDM_EMU_RESET_SOFT 40102
#define IDM_EMU_RESET_HARD 40103
#define IDM_EMU_POWER_OFF 40104
#define IDM_EMU_CONFIGURE 40105
// Two-gun play claims a mouse per player in the order they are first used, so
// there has to be a way to say "those are the wrong way round, start again".
#define IDM_EMU_REPAIR_GUNS 40106

// Ten contiguous slots each, so a command maps to a slot by subtraction.
#define IDM_EMU_SAVE_STATE_FIRST 40120
#define IDM_EMU_SAVE_STATE_LAST 40129
#define IDM_EMU_LOAD_STATE_FIRST 40130
#define IDM_EMU_LOAD_STATE_LAST 40139
#define IDM_EMU_DELETE_STATE_FIRST 40140
#define IDM_EMU_DELETE_STATE_LAST 40149
#define IDM_EMU_DELETE_ALL_STATES 40150

#define IDM_VIEW_SCALE_1X 40201
#define IDM_VIEW_SCALE_2X 40202
#define IDM_VIEW_SCALE_3X 40203
#define IDM_VIEW_SCALE_4X 40204
#define IDM_VIEW_DARK 40210
#define IDM_VIEW_LIGHT 40211
#define IDM_VIEW_FULLSCREEN 40212
#define IDM_VIEW_ASPECT_4_3 40220
#define IDM_VIEW_ASPECT_16_9 40221
#define IDM_VIEW_ASPECT_STRETCH 40222
#define IDM_VIEW_ASPECT_CUSTOM 40223
#define IDM_VIEW_CRT 40230
// Frames actually presented per second, measured rather than assumed.
#define IDM_VIEW_FPS 40231
// Bilinear scaling instead of the nearest-neighbour default.
#define IDM_VIEW_SMOOTH 40232

#define IDM_MP_PLACEHOLDER 40301
#define IDM_MP_NETPLAY_START 40302
#define IDM_MP_NETPLAY_JOIN 40303
#define IDM_MP_NETPLAY_EXIT 40304
// Two Game Gears side by side on one screen, joined by an emulated link cable.
#define IDM_MP_GEAR_LINK 40305
#define IDM_MP_GEAR_LINK_CLOSE 40306
// WHICH HALF OF A LINKED PAIR IS ON SCREEN. Plenty of link games show both
// players the same picture - both Mortal Kombats do - and then a second
// window is just the same thing again at half the size. These give one
// console the whole window and hide the other, which keeps running.
#define IDM_MP_LINK_VIEW_BOTH 40307
#define IDM_MP_LINK_VIEW_ONE 40308
#define IDM_MP_LINK_VIEW_TWO 40309

#define IDM_TOOLS_DUMP_VIDEO 40401

// Per-source mutes, for identifying which part of the sound hardware a given
// noise is coming from. Mixing only - the chips keep running.
#define IDM_TOOLS_MUTE_FM 40402
#define IDM_TOOLS_MUTE_PSG 40403
#define IDM_TOOLS_MUTE_DAC 40404
#define IDM_TOOLS_MUTE_PWM 40412
#define IDM_TOOLS_MUTE_CD 40413
#define IDM_TOOLS_RECORD_WAV 40405
#define IDM_TOOLS_LOG_FM 40406
#define IDM_TOOLS_LOG_PSG 40407
#define IDM_TOOLS_LOG_DAC 40408
// One shot rather than a running log: everything the machine can say about
// itself right now, written as text.
#define IDM_TOOLS_DUMP_LOG 40409
// VRAM and work RAM as hex. Its own item because it is hundreds of kilobytes.
#define IDM_TOOLS_DUMP_MEMORY 40410
// The on-screen Miracle Piano keyboard, in a window of its own.
#define IDM_TOOLS_MIRACLE_PIANO 40411

// Sega CD, on the bar in its own right rather than buried in Tools: swapping
// discs is something a player does mid-game, several times, in the middle of
// Night Trap or Fahrenheit.
#define IDM_CD_SWAP 40501
// The BIOS with no disc in the drive, which is a real thing a Sega CD does:
// its own front end, the CD player and the memory manager that formats and
// erases backup RAM. Reachable with no game loaded at all.
#define IDM_CD_BOOT 40502

// Cheats. Its own menu for the same reason: a code is typed while playing.
#define IDM_CHEAT_GAME_GENIE 40601

// Game Genie codes: a list, an entry box and the usual add/remove. Its own
// 600 block - the 400s are full of BIOS-page control ids and reusing numbers
// across dialogs is legal but reads as a collision.
#define IDD_GAME_GENIE 600
#define IDC_GG_LIST 601
#define IDC_GG_CODE 602
#define IDC_GG_DESCRIPTION 603
#define IDC_GG_ADD 604
#define IDC_GG_REMOVE 605
#define IDC_GG_ENABLE 606
#define IDC_GG_NOTE 607
#define IDC_GG_STATUS 608
#define IDC_GG_CODE_LABEL 609
#define IDC_GG_DESCRIPTION_LABEL 610
// A code carries the list of games it is for. One button adds the game that
// is loaded to it, and the check box filters the list down to that game.
// There is deliberately no "not for this game": unticking the code says the
// same thing with a control that is already there.
#define IDC_GG_USE_HERE 611
#define IDC_GG_ONLY_THIS 613

// Configure is a tabbed dialog: a frame holding a tab control, with one child
// dialog per page. Child dialogs rather than one flat template so each page's
// controls are laid out against its own origin and can be shown and hidden as a
// unit.
#define IDD_CONFIGURE 300
#define IDC_CONFIG_TABS 301

#define IDD_PAGE_GENERAL 320
// One games folder per system, in the same order as the save folders below and
// as the SaveFolder enum, so a system indexes both lists with one number.
#define IDC_GEN_GAMES_GENESIS 302
#define IDC_GEN_GAMES_GENESIS_BROWSE 303
#define IDC_GEN_GAMES_GG 304
#define IDC_GEN_GAMES_GG_BROWSE 305
#define IDC_GEN_GAMES_32X 306
#define IDC_GEN_GAMES_32X_BROWSE 307
#define IDC_GEN_GAMES_CD 308
#define IDC_GEN_GAMES_CD_BROWSE 309
#define IDC_GEN_SAVE_GENESIS 323
#define IDC_GEN_SAVE_GENESIS_BROWSE 324
#define IDC_GEN_SAVE_GG 325
#define IDC_GEN_SAVE_GG_BROWSE 326
#define IDC_GEN_SAVE_32X 327
#define IDC_GEN_SAVE_32X_BROWSE 328
#define IDC_GEN_SAVE_CD 329
#define IDC_GEN_SAVE_CD_BROWSE 330
#define IDC_GEN_HIDE_CURSOR 331
#define IDC_GEN_GUN_NOTE 332
#define IDC_GEN_VOLUME 333
#define IDC_GEN_VOLUME_VALUE 334
#define IDC_GEN_MUTE 335
#define IDC_GEN_CONSOLE_FILTER 336
// Labels. They carry real IDs rather than -1 because the theming code walks the
// page by ID to colour them. The range must cover EVERY label on the page, so
// it lives above the other pages' controls where there is room to grow: two
// group boxes and eight folder rows already need twelve.
#define IDC_GEN_LABEL_FIRST 365
#define IDC_GEN_LABEL_LAST 379

#define IDD_PAGE_INPUT 350
#define IDC_IN_P1_LABEL 351
#define IDC_IN_P1_KIND 352
#define IDC_IN_P1_DEVICE 353
#define IDC_IN_P1_PIC 354
#define IDC_IN_P2_LABEL 355
#define IDC_IN_P2_KIND 356
#define IDC_IN_P2_DEVICE 357
#define IDC_IN_P2_PIC 358
#define IDC_IN_PROFILE_LABEL 359
#define IDC_IN_PROFILE 360
#define IDC_IN_PROFILE_SAVE 361
#define IDC_IN_PROFILE_DELETE 362
#define IDC_IN_NOTE 363
#define IDC_IN_DIVIDER 364

#define IDD_PAGE_HOTKEYS 380
#define IDC_HK_NOTE 381
#define IDC_HK_LIST 382
#define IDC_HK_ASSIGN 383
#define IDC_HK_CLEAR 384
#define IDC_HK_DEFAULTS 385

// BIOS page. Firmware is optional in GenX - every 32X cartridge boots on the
// synthesised startup - so this page says so rather than presenting an empty
// box that looks like something is missing.
#define IDD_PAGE_BIOS 420
#define IDC_BIOS_32X 421
#define IDC_BIOS_32X_BROWSE 422
// Live: says what the folder actually holds, checked as it is typed.
#define IDC_BIOS_32X_STATUS 423
#define IDC_BIOS_LABEL_FIRST 424
#define IDC_BIOS_LABEL_LAST 431
// Sega CD: one folder, holding one BIOS per region. The status line says which
// regions it covers, because a disc only boots on its own region's BIOS.
#define IDC_BIOS_SEGACD 432
#define IDC_BIOS_SEGACD_BROWSE 433
#define IDC_BIOS_SEGACD_STATUS 434
// Labels on the BIOS page carry literal IDs 424-431 and 435 in the .rc.

// Player 2's profile row. Player 1's keeps the original IDC_IN_PROFILE* names
// above, since a profile is now ONE player's setup rather than both players'.
#define IDC_IN_P2_PROFILE_LABEL 386
#define IDC_IN_P2_PROFILE 387
#define IDC_IN_P2_PROFILE_SAVE 388
#define IDC_IN_P2_PROFILE_DELETE 389

// Explains what player 2 does on a Game Gear, which is nothing at all except
// on the two games that share one console between two people.
#define IDC_IN_P2_NOTE 393

// A one-line text prompt, used to name an input profile.
#define IDD_NAME 390
#define IDC_NAME_PROMPT 391
#define IDC_NAME_EDIT 392

// One template for both hosting and joining - they differ by a single field,
// and two near-identical templates would drift apart.
#define IDD_NETPLAY 400
#define IDC_NET_HEADING 401
#define IDC_NET_NICKNAME 402
#define IDC_NET_NICKNAME_LABEL 403
#define IDC_NET_ADDRESS 404
#define IDC_NET_ADDRESS_LABEL 405
#define IDC_NET_PORT 406
#define IDC_NET_PORT_LABEL 407
#define IDC_NET_PASSPHRASE 408
#define IDC_NET_PASSPHRASE_LABEL 409
#define IDC_NET_DELAY 410
#define IDC_NET_DELAY_LABEL 411
#define IDC_NET_NOTE 412
#define IDC_NET_PLAY_AS_TWO 413

#define IDD_ASPECT 310
#define IDC_ASPECT_WIDTH 311
#define IDC_ASPECT_HEIGHT 312
#define IDC_ASPECT_NOTE 313

#endif  // MERCURY_RUNTIME_RESOURCES_HPP
