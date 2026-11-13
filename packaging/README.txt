ReResorep @VERSION@
==============================================================================

Texture replacer and texture manager for Direct3D 11 games, built for
Rainbow Six Siege modding. A rewrite of Resorep by RayWing: existing Resorep
texture packs keep working. No Java or extra runtimes needed.

Requirements: Windows 10 or 11, 64-bit. The game must be 64-bit and use DirectX 11.


!! Anti-cheat warning !!
------------------------------------------------------------------------------
ReResorep works by placing a d3d11.dll next to the game. Online Rainbow Six
Siege runs BattlEye, which can detect this and ban your account. Only use
ReResorep for offline / modded setups.


Getting started
------------------------------------------------------------------------------
1. Right-click the zip -> "Extract All...", put the folder anywhere you like
   (not inside the game folder or Program Files) and run ReResorep.exe from
   there. Don't run it from inside the zip: Windows then starts it from a
   temporary folder without "proxy", and your mods and settings get lost.
   Keep the "proxy" folder next to ReResorep.exe.
2. Games tab -> "Add game..." -> pick the game's exe (for Siege: RainbowSix.exe)
   -> "Install". If the game folder is in Program Files and this fails, run
   ReResorep as administrator once.
3. Mods tab -> "Import mod..." for a mod package (.zip), or "Import .dds..."
   for single textures. You can also drag files onto the window. Don't unzip
   mods yourself: ReResorep puts them in the right place.
4. Press "Start" in the Games tab, or start the game as usual.

Mods must end up in the "modded" folder in this folder (Mods tab -> "Open
folder" opens it). Mods put in the game folder, in "original", in "proxy" or
anywhere else are not loaded. For a .rar or .7z mod, extract it somewhere
temporary and copy its folder into the one "Open folder" shows.

Changes you make while the game runs are picked up within about a second, for
textures the game loads after that (for example on the next map).


Making your own mods
------------------------------------------------------------------------------
- Settings tab -> turn on "Mod creator mode" and "Dump textures", then play.
  The game's textures appear in the Dumps tab.
- Right-click a dump -> "Replace with .dds..." to make a mod for it, or
  "Find its normal map" to find the matching normal map.
- Label hashes in the details panel so you remember what they are.
- Group textures into mods and categories in the sidebar, then right-click a
  mod -> "Export mod..." to share it as a .zip.
- Several Siege builds: tick "Own folders" on the Games tab to give a build its
  own mods and dumps, and pick it at the top of the Mods/Dumps tabs. In the
  shared folders, right-click a mod -> "Made for" so other builds skip it
  (saved as !reresorep_game.ini in the mod's folder).


Updating
------------------------------------------------------------------------------
About tab -> "Check for updates". ReResorep only goes online when you press
it, and then only to GitHub. If there's a newer version, "Download update"
fetches and checks it, and "Restart and update" puts it in place and starts
it. Your settings, mods and labels stay. Games keep the d3d11.dll they have;
if an update brings a new proxy, the Games tab shows them as outdated and
"Update" there installs it.


Damaged or modified files
------------------------------------------------------------------------------
ReResorep.exe and proxy\d3d11.dll check themselves when they start. If
ReResorep.exe was damaged or changed, it shows an error and doesn't start:
download ReResorep again. If the d3d11.dll in a game folder was, the game
still starts but without mods; the Games tab shows "Proxy modified" and
"Repair" puts a fresh copy there.


Removing ReResorep
------------------------------------------------------------------------------
Games tab -> "Uninstall" for each game (this restores any d3d11.dll the game
had before), then delete this folder.


Where things are kept (all inside this folder)
------------------------------------------------------------------------------
  modded\                  your mods (each folder is a mod, sub-folders are categories)
  original\                dumped game textures
  games\<exe hash>\        mods and dumps of a game that has "Own folders" ticked
                           (Games tab); the folders above are the shared Default ones
  reresorep.ini            settings and your game list
  labels.txt               your texture labels
  texture_analysis.cache   detected texture types (safe to delete)
  texture_info.cache       texture sizes and formats, for a faster start (safe to delete)


Files in this download
------------------------------------------------------------------------------
  ReResorep.exe            the texture manager
  proxy\d3d11.dll          the part that goes into the game folder
  CREDITS.md               credits
  LICENSE.txt              ReResorep's licence (GNU GPL v3.0)
  THIRD_PARTY_NOTICES.txt  licences of included third-party code
  SHA256SUMS.txt           checksums to verify the download

ReResorep is free software under the GNU General Public License v3.0; the
source is at https://github.com/SosigDOGGO/ReResorep.

ReResorep is a fan-made modding tool and is not affiliated with or endorsed by
Ubisoft or any game developer.
