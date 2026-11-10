# ReResorep

A native rewrite of **Resorep**, the DirectX 11 texture replacer, aimed at Rainbow Six Siege
modding. It has no Java requirement: a single `ReResorep.exe` texture manager plus a `d3d11.dll`
proxy. Neither needs a runtime or a VC++ redistributable.

Existing Resorep texture packs keep working unchanged. That includes `<hash>.dds`,
`<hash>~name.dds`, encrypted `resorep1` packs, and the original's ±2 hash tolerance.

> **DirectX 12:** current Siege (Y10S1 and later, including Siege X) runs on DirectX 12 only,
> so the `d3d11.dll` proxy only works with older builds that still had DX11. D3D12 support is
> not implemented yet; see [docs/DX12.md](docs/DX12.md) for how it could work.

> **Anti-cheat:** online Siege runs BattlEye, and a replacement `d3d11.dll` in the game folder
> is the kind of thing it can flag. Use this for offline/modded setups.

## Using it

1. Run `ReResorep.exe`.
2. **Games** tab → *Add game…* → pick the game's exe (for Siege, `RainbowSix.exe`) → *Install*.
   This copies `proxy\d3d11.dll` next to the exe and writes `dllsettings.ini`. If the game folder
   already has a `d3d11.dll` (ReShade, for example), it is kept as `ori_d3d11.dll` and still loads.
3. **Mods** tab → *Import .dds…*, or drag files onto the window. Files named after a hash are
   imported directly; for any others you're asked which hash they replace.
4. To find hashes, open **Settings** and turn on *Mod creator mode* and *Dump textures*, then play.
   The **Dumps** tab fills with the game's textures. Select one and choose *Replace with .dds…*.
   A texture already in the folder, or already dumped since the game started, is not dumped
   again, even if you delete its file. **Redump** makes a running game dump (and overwrite)
   every texture again as it loads it.
   Need an editor? We recommend [GIMP](https://www.gimp.org/). It's free, and it opens dumped
   `.dds` textures and exports your edits back to `.dds`.
5. Just playing? Turn on **Settings → Low impact mode**. The proxy then only replaces textures:
   no dumping or Redump, and it reads your mods when the game starts instead of watching the
   modded folder. Mods you change while playing apply on the next start, or when a setting
   changes. In any mode, the proxy doesn't look at a texture's data when there's nothing to
   replace or dump, and it rescans folders at background priority.

### Finding your way around big dump folders

- **Sort** by hash, name, date modified, file size, resolution, format or mod/category, in
  either direction.
- **Rotate / Mirror** previews. The default is 180°, which matches how dumped textures look
  in-game. This only changes the display; files are never altered.
- **Labels:** give any hash your own name in the details panel. Labels are kept in
  `labels.txt` next to `ReResorep.exe`, shared by the dump and every mod for that hash, and
  included when searching. *Settings → Labels* imports and exports them. The format is
  `hash=label` per line; CSV and tab-separated also work.
- **Texture types:** each texture is classified in the background on its own GPU device, and
  the result is cached in `texture_analysis.cache`. The types are *colour*, *normal map* and
  *depth/grayscale*. The last covers masks, roughness, height and monochrome icons. *Hide
  normal maps* and *Hide depth / grayscale* filter the grids.
- **Find its normal map / Find similar textures** (right-click a texture). Candidates are
  ranked by how well their detail lines up with the texture, whether they're the same size,
  and, for dumps, how close together they were saved: games load a material's textures
  together. Check the top results by eye.

### Mods and categories

Group textures into folders inside the modded folder. A top-level folder is a **mod**, and any
folder inside it is a **category**:

```
modded/
  12345.dds                  loose texture
  Ash Elite/                 mod
    Uniform/  2233445566.dds category
    Weapons/  3344556677.dds category
```

In the **Mods** tab, the sidebar lists every mod and category with an on/off checkbox and an
"active/total" count. Clicking one shows only its textures, and imports go into it. Right-click
for *New category*, *Import .dds here*, *Open folder* and *Move to Recycle Bin*. Right-click a
texture and use *Move to* to put it in a different category. Any texture, category or whole mod can
be turned off. On/off choices are saved in `modded/reresorep_disabled.ini`, and files are never
moved, so turning something back on restores it exactly.

#### Tinting

Right-click a mod texture, or one of several selected, and choose **Tint...** to recolor them,
for example to turn Black Ice red. Pick a colour, and the dialog shows a before/after preview:

- **Recolor** takes the colour's hue and keeps each pixel's own saturation and brightness, so
  whites, blacks and shading stay. This is the one for Black Ice and other one-colour skins.
- **Colorize** puts the colour's hue and saturation on every pixel, grey and white parts included.
- **Multiply** multiplies the texture by the colour, like tinted glass.

Strength, Saturation and Brightness fine-tune the result. Normal maps and packed material maps
are left out unless you tick the box for them. Each texture keeps its format and mips (BC1, BC2,
BC3, BC7 and 8-bit RGBA can be tinted; BC7 is re-encoded with a simpler encoder, which can soften
fine detail a little). Originals go to the Recycle Bin, so a tint can be undone from there.
Encrypted (resorep1) textures can't be tinted.

#### Sharing mods

Right-click a mod, or a category, and choose **Export…** to save it as a `.zip`. The zip
contains:

- the mod folder with its categories (empty ones too),
- which textures and categories are turned off (they stay off for whoever imports it),
- your labels for the textures in it.

Both of the last two are optional. Any zip tool can open the package.

To install a package, use **Import mod…** in the sidebar, or drop the `.zip` onto the window.
You can rename the mod first. If a mod with that name already exists, you're asked before
merging into it. Zips from other tools work too: their `.dds` files are imported with their
folders kept. Only `.dds` files are ever unpacked, and only inside the modded folder: entries
like `../file` or `C:\file` are refused, and every file's checksum is verified. Labels from a
package are added only for hashes you haven't labelled yourself.

If two active textures have the same hash, the game uses the first one by relative path, ignoring case. A folder named `00-Mod`, for example, sorts before a loose
texture named `123.dds`. The other copy is labelled *unused (duplicate)*.

The original Resorep only reads loose textures; it ignores sub-folders.

Changes reach a running game within about a second. The proxy watches the modded folder and its
settings file. Textures the game has already loaded keep their old data until it reloads them
(for example on the next map load).

### Several Siege builds

Every Siege build is a `RainbowSix.exe`, so ReResorep tells them apart by the SHA-256 of the exe.
The **Games** tab shows the first 8 hex digits of each game's hash. It works the hash out in the
background when a game is added, and again whenever the exe changes.

- **Default profile:** unless you change it, every game uses the shared folders from the
  **Settings** tab, as before.
- **Own folders:** tick *Own folders* on a game's row to give it its own mods and dumps in
  `games\<first 16 hex digits of its hash>\modded` and `\original`, next to `ReResorep.exe`.
  The proxy in that game picks up the change within a second.
- **Removing a game:** *Remove…* offers two choices. *Remove from list* leaves the proxy installed
  and remembers the game's own folders and hash, so adding the same exe back picks up where it
  left off. *Remove completely* uninstalls the proxy and forgets both. It can also move the
  game's own folders to the Recycle Bin, unless another game in the list uses them.
- **Game picker:** the picker at the top of the **Mods** and **Dumps** tabs chooses which
  folders those tabs show: *Default (shared folders)*, or a game that has its own folders.
  *Redump* only goes to games that use the folders being shown.
- **Which build a mod is for:** each mod can carry a `!reresorep_game.ini` in its folder with
  `game_exe_sha256=<hash>`. The `!` keeps it at the top of the folder in Explorer. The app
  doesn't list it among the mod's textures. A game skips mods marked for a different build,
  which matters in the shared Default folders. Mods without the file apply to every game, so
  existing mods keep working.
  - Mods created or imported while a game's own folders are shown are marked for that game.
  - In the Default profile, right-click a mod and choose **Made for** to pick a game, or
    *Any game*.
  - An exported package records the game too, and importing it into the Default profile keeps it,
    unless you merge it into a mod that is already marked.
- **Duplicates per build:** when several active mods replace the same hash, the app shows which one
  each build's game uses, by the same rule as the proxy. A mod marked for build A doesn't make an
  unmarked one "unused" for the other builds.
- **A patched or replaced exe** gets a new hash. Its game keeps its own folder, which is named
  after the hash it had when *Own folders* was ticked. The Games tab flags *exe changed*, and its
  **Fix…** button either moves the marks of every mod made for the old exe to the new one (same
  build, patched) or leaves them alone (a different build).

### Updating

**About → Check for updates** asks GitHub for the latest release. ReResorep goes online only then:
it never checks by itself, and it sends nothing but the requests. When there's a newer version, the
About tab shows its release notes and **Download update**, which:

1. downloads the release zip and its `.sha256.txt`, and checks the zip's SHA-256;
2. checks the `SHA256SUMS.txt` inside it, the seals of the new `ReResorep.exe` and
   `proxy\d3d11.dll` (see below), and that the exe's version is the release's;
3. unpacks them beside the current ones as `ReResorep.new.exe` and `proxy\d3d11.new.dll`.

**Restart and update** then renames the running exe to `ReResorep.old.exe` (Windows lets a running
exe be renamed, not overwritten), moves the new one in, closes ReResorep and starts the new version,
which deletes the `.old` files. If a step fails, the files are put back. Settings, mods, labels and
dumps are not touched.

The bundled `proxy\d3d11.dll` is replaced only when the release's copy is different, and the About
tab says so first, with the new proxy's SHA-256 for mods that whitelist it. Proxies already in game
folders are never changed by an update: the Games tab shows them as outdated, and **Update** there
puts the new one in.

### Damaged or modified copies

Each build seals `ReResorep.exe` and `proxy\d3d11.dll`. A small tool (`tools/seal.cpp`) writes
the SHA-256 of the file into a reserved slot inside the file, and each one checks itself when it
starts:

- `ReResorep.exe` that doesn't match shows an error and doesn't start.
- A `d3d11.dll` that doesn't match still loads, so the game starts, but it only passes calls
  through: no mods and no dumps. Its `dll.log` says why. The Games tab shows *Proxy modified*,
  and **Repair** installs a fresh copy.
- The manager won't install a bundled `proxy\d3d11.dll` that doesn't match.

The digest leaves out the slot, the PE checksum and an Authenticode certificate at the end of the
file, so the files can still be code-signed after sealing. Proxies from before sealing count as
older, not modified. This catches damaged downloads and copies changed by anything that doesn't
reseal them. It won't stop someone who patches the check itself out; only code signing does that.

## Building

Needs Visual Studio 2022 with the C++ workload (it includes CMake).

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The output goes to `build/bin/`: `ReResorep.exe` and `proxy/d3d11.dll`. Ship that folder as-is.
Both are linked into `build/unsealed/` first and sealed into `build/bin/` (see *Damaged or modified
copies*). The copies in `build/unsealed/` won't start; Visual Studio runs the sealed exe.

## What changed compared to the original

| | Original Resorep 1.7 | ReResorep |
|---|---|---|
| UI | Java 8 + Spring Swing app (~10 MB jar) | Native Dear ImGui exe with texture previews |
| Finding a mod | Directory listing on **every** texture creation, up to 5× | Hash map built once, refreshed by a folder watcher |
| Hash | Scalar loop | SSE2, same values |
| Settings in dump mode | `dllsettings.ini` re-read on every texture | Re-read only when the file changes |
| Dumping | Synchronous file write inside `CreateTexture2D` | Copied, then written on a background thread (with a memory cap) |
| Hooking | Device wrapper object | Vtable patch, so `ID3D11Device1..5` are covered and refcounts stay untouched |
| Loading the real d3d11 | Copy of System32's `d3d11.dll` placed in the game folder | Loads System32's directly (no stale copy after Windows updates); lazily, never under the loader lock |
| Mod format vs game format | Uses the DDS format | Keeps the game's format when it's the same family (such as BC3 vs BC3 sRGB), so the game's views still work |
| Global hook (AppInit_DLLs) | Windows 7 only | Dropped |
| Analytics / update check | Google Analytics, jimdo.com version check | No analytics; updates only when you press **Check for updates** |

## Layout

- `core/` is shared by the DLL and the GUI: the texture hash, Blowfish and the `resorep1`
  container, DDS reading and writing, the settings file, the hash index, and the on/off state of mods and categories.
- `proxy/` is the `d3d11.dll` proxy. Pass-through exports are generated from `proxy/exports.txt`.
- `manager/` is the ImGui texture manager.
- `tests/` has `core_tests` (Blowfish test vectors, hash, DDS round-trip) and `proxy_test`
  (loads the built proxy and checks replacement, encrypted packs, dumping and hot reload on a WARP device), and
  `analyzer_test` (texture-type detection and normal-map matching on generated textures).
  `tests/zip_fixture.inc` is a zip made by Python (`tools/gen_zip_fixture.py`), so the zip reader is tested against
  another tool, including path-traversal entries.
- `third_party/imgui` is Dear ImGui v1.91.8 (MIT).
- `docs/DX12.md` has design notes for a possible D3D12 proxy.

## Compatibility notes, from reverse-engineering `dx11proxy64.dll`

- Hash = 1 + Σ(int8 bytes) over `SysMemSlicePitch` bytes of the first subresource, as uint32.
- Only textures with `Usage` ≤ `IMMUTABLE`, `ArraySize == 1` and initial data are considered.
- `resorep1` packs: the magic, an IV-length byte, the IV, then Blowfish-CBC with a 16-byte key.
  The AES in the earlier reconstructed source was wrong.
- Lookup order: hash, +1, −1, +2, −2. Several files for one hash: the first by name wins.
- x64 only. Siege is 64-bit; the old 32-bit proxy was not ported.

## Everyday copy: build/testing

Run the app from `build/testing\ReResorep.exe`, not from `build/bin`. Every build copies the new
`ReResorep.exe` and `proxy/d3d11.dll` there; your settings, mods, dumps and labels in that folder
are never touched. If the testing copy is open during a build, the build still succeeds: a
warning says the copy wasn't updated, and the next build after closing it catches up.

`build/testing` is the same folder CTest uses for its logs (Windows ignores case), so test runs
add a `Temporary` subfolder there. It's harmless.

## Shipping a release

```bash
cmake --build build --config Release --target ship
```

This builds Release and creates `build/ship/ReResorep-<version>/` and
`build/ship/ReResorep-<version>-win64.zip`, ready to upload. The package contains:

- `ReResorep.exe` and `proxy/d3d11.dll`,
- a user-facing `README.txt` (from `packaging/README.txt`),
- `CREDITS.md`,
- `THIRD_PARTY_NOTICES.txt`, with the Dear ImGui licence,
- `SHA256SUMS.txt`.

The zip's own checksum is written to `…-win64.zip.sha256.txt` for the release page. Only those
files are copied, so your own settings, caches, dumps and mods in `build/bin` never end up in a
release. Packaging a Debug build is refused.

## Version and credits

The version is set once, in `CMakeLists.txt` (`project(... VERSION 2.5.5)`). From there it goes
into the window title, the **About** tab and the version info of `ReResorep.exe`.

The proxy has its own version, `RERESOREP_PROXY_VERSION` in `CMakeLists.txt`. It goes into the
proxy's version info (Explorer → Properties → Details) and its `dll.log`. Raise it only when the
proxy changes (`proxy/`, or the `core/` code it links): some mods whitelist `d3d11.dll` by its hash,
so every new proxy file means they have to update.

Releases that only change the manager therefore ship the same proxy file. Builds are reproducible
(`/Brepro`), and `packaging/proxy.sha256` holds the version and SHA-256 of the released proxy.
While the proxy version matches it, the `ship` target packages exactly that file, taking it from an
earlier package in `build/ship` if this build's differs. When the proxy version is raised, `ship`
prints the new hash; once that release is out, put it in `packaging/proxy.sha256`.

The **Games** tab shows which proxy version each game has installed.

Credits are in [CREDITS.md](CREDITS.md) and in the app's **About** tab: the original Resorep by
RayWing, Dear ImGui (MIT) and Blowfish.

## License

ReResorep is free software under the [GNU General Public License v3.0](LICENSE). You can use,
change and share it; if you share a changed version, it has to stay under the GPL with its source
available. Dear ImGui in `third_party/imgui` keeps its own MIT license. The app's **About** tab
shows the license, with its full text and a link to the source.
