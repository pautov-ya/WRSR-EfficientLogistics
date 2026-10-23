[English](README.md) | [Русский](README_RU.md)

# Efficient Logistics

**[Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3825558097)**

A plugin for **Workers & Resources: Soviet Republic** that makes trucks of construction offices and
supply bases load up and serve several places in one trip.

### Construction offices

In the base game, a construction office truck loads only what one site needs: with a 20 t truck and
sites that need 5 t, 2 t and 10 t of gravel, it makes three trips. With Efficient Logistics it loads
17 t and drops at all three sites on the way.

### Supply bases

The same for a road supply base (distribution office): a truck taking one resource from a storage
to a smaller one also serves the other storages, shops and buildings that want that resource from
the same source.

The mod does not write anything into the save and can be removed at any time.

## Requirements

- Workers & Resources: Soviet Republic **1.1.1.9**, 64-bit.
- [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749) (RML).
- No DLC.

## Installation

1. Subscribe to [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749)
   and [Efficient Logistics](https://steamcommunity.com/sharedfiles/filedetails/?id=3825558097) on the Steam Workshop.
2. Start RML and enable **efficientlogistics** under **Plugins**.
3. Start the game from RML.

To uninstall, unsubscribe from the mod.

## Settings

`efficientlogistics.ini`, in `<SteamLibrary>\steamapps\workshop\content\784150\3825558097\plugins\`.
A Steam update of the mod overwrites it, so redo your changes after an update.

| Option | Default | |
|---|---|---|
| `max_extra_sites` | 3 | how many more sites / storages one trip may take; 0 = the game as it is |
| `max_distance_m` | 0 | only places within this distance (m, straight line) of the first one; 0 = any |
| `min_extra_t` | 0.1 | a place is added when it wants at least this much (t) |
| `load_margin_t` | 0.2 | loaded on top of what construction sites need, so none is left a few tenths short |
| `clear_rest` | 1 | throw away what is left on board after the last construction site |
| `logistics` | 1 | supply bases too; 0 = construction offices only |
| `log` | 0 | debugging: every trip in the RML log |

## How it works

The game still chooses the job and the truck; the plugin then adds the other stops to the truck's
schedule and raises the load. The office's count of cargo on its way to each site or storage is
corrected so that each stop counts only its share. Game code is found by byte signatures; if an
update breaks one, the plugin does nothing and the RML log says so. Details are in [docs/](docs/).

## Known limitations

- Trip plans are kept in memory only. After loading a save, trucks already on a several-stop trip
  are counted the game's way, so the office may send one extra truck.
- A storage that is emptied while a truck is on its way takes more than planned, and the next stop
  gets less; the office makes up for it with the next trip.
- Tested with road supply bases; rail and other distribution offices are left as they are.

## Building

```sh
./build.sh            # build and deploy to the game
./build.sh nodeploy   # build only
```

Requires [llvm-mingw](https://github.com/mstorsjo/llvm-mingw); paths are set in `build.sh`.

## Credits

- [TesmioLoader](https://github.com/MaxLegend/TesmioLoader) by MaxLegend: plugin API headers.
- [Republic Mod Loader](https://github.com/Ultimate-Universe/WRSR-RepublicModLoader) by Ultimate-Universe.

## License

[GPL-3.0](LICENSE).
