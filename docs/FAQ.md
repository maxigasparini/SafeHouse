# FAQ and troubleshooting

[Back to the README](../README.md)

## Does SafeHouse include the games?

No. SafeHouse contains no game data. You need your own PlayStation 2 disc image (`.iso`) of each game, made from a disc
you own. The launcher installs the game from it.

## Which discs work?

| Game | Discs |
| --- | --- |
| Sly Cooper and the Thievius Raccoonus | USA (`SCUS-97198`), Europe, Japan and Korea |
| Sly 2: Band of Thieves | USA (`SCUS-97316`), Europe v1.00 and v2.01 (`SCES-52529`), Japan (`SCPS-15090`) |
| Sly 3: Honor Among Thieves | USA (`SCUS-97464`): installs, but the port is early in development and not fully playable |

Use an uncompressed `.iso` of the whole disc. When an image is refused, the launcher says why: another game, an
unsupported region, an incomplete image or not enough free space.

## Is Sly 3 playable?

Not yet. Sly 3 is early in development and not fully playable. It has been rebuilt on the finished Sly 2 port's engine, which carries most of the work over; for now it installs and plays its opening movies, and the rest is still being ported.
Follow progress on [Discord](https://discord.com/invite/SfvzvMg7Mk).

## Where are my files?

| | Installed games | Saves, settings and mods |
| --- | --- | --- |
| Linux | `~/.local/share/projectcane/installations` | `~/.config/projectcane` (Sly 2: `sly2/`) |
| Windows setup | `%LOCALAPPDATA%\ProjectCane\installations` | `%LOCALAPPDATA%\ProjectCane\user` (Sly 2: `sly2\`) |
| Windows portable | `data\installations` beside `bin` | `data\user` beside `bin` |

The folders keep the project's old name (ProjectCane) on purpose, so games, saves and settings from earlier versions
carry over. The game page's **Open save folder** and **Open game folder** buttons open them. Re-extracting or
uninstalling a game never touches your saves.

### Can I choose where games are installed?

Yes. Open **Settings > Games > Game installation folder** and choose the destination.
New installations go to that folder in subdirectories such as `sly1` and `sly2`, with
additional disc regions installed beside them. Existing installations remain in their original
folders and are still recognized by the launcher; changing the setting never moves or deletes
game files. The **Use default install folder** button restores the standard destination.
Saves, settings, and logs remain in the profile locations listed above.

### Can SafeHouse find disc images in my own games folder?

Yes. Open **Settings > Games > Disc images folder** and select the folder holding your
legally obtained `.iso` or `.chd` files. When you open a game's installation page,
SafeHouse automatically searches that directory and its subfolders (up to two levels deep),
along with the usual locations, and recognizes supported games and regions. The search
is bounded to 200 disc images and 20,000 entries. Finding a disc does **not** install
it automatically; you choose which one to install. **Clear disc images folder** stops
scanning the custom folder.


## How do I update?

The launcher updates itself. At startup it checks GitHub for the latest release; when there is a newer one it shows
what changed and how old your build is ("Your build is N days old") and offers **Update now**, **Later** or **Skip
this version**. **Update now** downloads the new package and replaces the launcher in place: the Windows setup and
portable installs (no need to run Setup again), the AppImage, the `.tar.gz` folder and the macOS app. Your saves and
installed games are never touched.

Turn the check off with **Settings > Launcher > Check for updates**; **Check for updates now** there checks at any
time. You can also download a package from the [Releases page](https://github.com/FridiNaTor1/SafeHouse/releases)
yourself. Releases are tagged `v0.1.<build number>`.

After an update that changed the installed files, a game's page shows **Update needed**: the game is extracted again
from your disc image, and your saves are kept.

## Steam Deck

Install in Desktop Mode (the AppImage is the simplest), open SafeHouse, install your games, then use **Add to Steam**
on each game's page. Exit Steam first, or restart it afterwards, so it picks up the new shortcuts. Back in Game Mode,
the games are in your library with their artwork. Run the Linux package natively; do not force Proton on it.

## The AppImage does not start

Make it executable (`chmod +x SafeHouse-*.AppImage`, or the file's Properties). Without FUSE, run it with
`--appimage-extract-and-run`, or use the `.tar.gz` package and open **SafeHouse**.

## Windows warns about the program

The packages are not code-signed, so Windows SmartScreen may warn the first time. Choose **More info > Run anyway** if
you downloaded it from this repository's Releases page.

## The game runs slowly

- Lower **Settings > Graphics > Internal resolution** and **MSAA**.
- On weak integrated GPUs, **Fullscreen** usually performs better than **Borderless** or **Windowed**.
- A frame-rate limit equal to your monitor's refresh rate (or half of it) with VSync on gives the smoothest result.
- The games need OpenGL 4.3 (Metal on macOS).

## The controller does not work, or shows the wrong buttons

SafeHouse uses SDL's controller support and SDL_GameControllerDB. Steam Input can rename a controller; if the prompts
are wrong, pick a style in **Settings > Launcher > Button prompts**, or turn on **Original PlayStation Icons** in the
controls settings. Bindings for keyboard, mouse and controller are in each game's **Controls** settings.

## Reporting a bug

1. Turn on **Save logs** in **Settings > Advanced**.
2. Reproduce the problem.
3. Open **Log folder** there and attach `launcher.log` to an [issue](https://github.com/FridiNaTor1/SafeHouse/issues),
   with the game, its region and what you were doing.

Stuck in a job in Sly 2? With **Debug mode** on, **F9** completes the current job (see
[DEBUG-MENU.md](DEBUG-MENU.md#debug-keys)); its log line names the job, which helps the report.
