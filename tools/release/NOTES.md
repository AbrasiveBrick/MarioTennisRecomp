A native PC port of Mario Tennis (N64), now on **Linux** as well as Windows, with **online play** for two players.

**You need your own ROM.** These downloads contain no game code or data. Use a dump of your own North American (USA) Mario Tennis cartridge; the game is translated from it when it starts.

## Downloads
- **Linux x86_64**: `MarioTennisRecompiled-<version>-linux-x86_64.tar.gz`. Extract it and run `./MarioTennisRecompiled` from the extracted folder. Needs Vulkan drivers plus SDL2, GTK 3, Freetype and libcurl (already installed on most desktops).
- **Windows x64**: `MarioTennisRecompiled-<version>-windows-x64.zip`. Unzip it and run `MarioTennisRecompiled.exe`.

Pick your ROM in the launcher and press **Start game**.

## Online play (experimental)
On the launcher, choose **Online play**.
- **Host**: keep port **7777** or pick another, set the input delay, then **Start hosting**. To play over the internet, forward that port as **UDP** on your router to your PC (and allow the game through your firewall), then give the other player your public IP address.
- **Join**: enter the host's address and port, then **Connect**.

The match starts on both PCs as soon as they connect; the host is player 1. Both players need the same ROM and the same release. The input delay is in game frames (2 is about 67 ms); raise it if the game stutters on a slow connection. If the two games drift apart the log shows `[net] DESYNC`; please report it with both players' logs.

## What's new
- Linux x86_64 build.
- Online play for two players over the internet or a LAN, with an Online play panel in the launcher.
- Redesigned launcher and settings menus.
- The launcher shows the official artwork, loaded from Nintendo's website at launch and never saved to disk (Linux build; offline it shows the built-in court).

Mario Tennis is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.
