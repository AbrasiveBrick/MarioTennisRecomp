Mario Tennis: Recompiled (Windows x64)
=====================================

This download contains no game code or data. You need your own dump of the
North American (USA) Mario Tennis cartridge (.z64, .n64 or .v64). The game is
translated from your ROM when it starts.

Run
---
  MarioTennisRecompiled.exe

Pick your ROM in the launcher and press Start game. Keep the files in this
folder together: the game loads assets/ and the controller database from here.

Needs 64-bit Windows 10 or 11 with an up-to-date graphics driver (Direct3D 12 or
Vulkan). Allow it through Windows Firewall when you host online play.

Online play (2 players)
-----------------------
Open Online play from the launcher.
 - Host: pick Host, keep port 7777 (UDP) or choose another, set the input delay,
   then Start hosting. For play over the internet, forward that UDP port on your
   router to this PC and give player 2 your public IP address.
 - Join: pick Join, enter the host's address and port, then Connect.
The match starts on both PCs as soon as they connect. The host is player 1.
Both players need the same ROM and the same version of this build. If the game
stutters on a slow connection, raise the input delay (2 frames is about 67 ms).

Saves and settings go to %LOCALAPPDATA%\MarioTennisRecompiled.

Source and licence: https://github.com/AbrasiveBrick/MarioTennisRecomp (GPL-3.0).
Mario Tennis is a trademark of Nintendo. This project is not affiliated with or
endorsed by Nintendo.
