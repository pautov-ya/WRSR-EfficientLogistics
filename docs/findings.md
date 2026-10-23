# Findings: construction office deliveries (SOVIET64.exe 1.1.1.9)

Goal: a construction office truck carries for several construction sites in
one trip, instead of one trip per site with exactly what that site needs.

## Types and objects

- Building type ids (`[type+0x360]`, set by the type loader `FUN_14010e1f0`):
  `$TYPE_CONSTRUCTION_OFFICE` = `0xC`, `$TYPE_CONSTRUCTION_OFFICE_RAIL` = `0x1C`.
- Vehicle: `+0x4F8` its workplace (the office), `+0x1708` its type,
  `+0x680` schedule (std::vector<building*>, first = where it goes now),
  `+0x698` current stop, `+0x4F0` building it is in.
- Vehicle type: `+0x85E0 .. +0x85E8` std::vector of 16-byte records: the
  mechanism kinds it works as on a site (int kind, float rate); `+0x8600`
  cargo class (7 = passengers, 0x11 = mixer drum), `+0x8604` capacity.
- Vehicle cargo: `+0x3B8 .. +0x3C0` std::vector of 16-byte entries
  {resource*, float amount, float}; `+0x3A0` its wagons/trailers. "Discard
  cargo" (`FUN_1406beee0(v, 0)`, button text 1478) zeroes every amount.
- Resource: `+0xCC + class * 0x20` float unit; a truck holds
  `unit * type+0x8604` of it when full. `+0x44` int index (-1 = workers).
- Building: `+0xC70 .. +0xC78` std::vector<vehicle*> of vehicles inside it.
- Construction site: `+0x610 .. +0x618` its phases, 0x21B8 bytes each
  (`+0` float progress mark, `+4` int phase kind, `+8 .. +0x10` vector of
  16-byte resource entries); `+0x634` current phase index.

## Where things happen

- `FUN_1407838e0` - the construction window (texts 1427 "Resources needed to
  finish entire construction", 1432 "Resources on construction site").
- `FUN_140159070(world, site)` - the construction site's tick: the
  mechanisms working on it. For every vehicle inside the site whose workplace
  is a construction office it walks the vehicle type's `+0x85E0` entries
  (mechanism kinds, not cargo) and `FUN_14010c2b0(phase kind, mechanism, 0)`
  gives that mechanism's work factor for the phase - not the site's need,
  not unloading (corrected). Then the vehicle is sent on: `[[v+0x680]] = next
  building`, `v+0x610 = 0`, `v+0x528 = 0`, `v+0x530 = 0`, `v+0x548 = v+0x540`,
  `v+0x558 = 0` (route reset).
- `FUN_14068b330` - a vehicle standing at a building (loading / unloading
  timer `v+0x598`); a construction office vehicle (workplace type 0xC) may
  leave once at least half full (0.5).

## The office and its jobs

- `FUN_140161c90(world, office, site, ...)` assigns a site to an office (not a
  truck's job): the site goes into `office+0xF90 .. +0xF98` (the office's
  sites), the office into `site+0x880 .. +0x888` (the site's offices),
  `site+0x879` = 1 (assigned).
- `FUN_1401561c0(world, vehicle, office, int, char) -> bool` - most likely
  the office's "find a job for this truck" (60 KB decompiled,
  `re/out/cd_1561c0.c`): looks up the construction resources by name
  (`FUN_1402aa830(world, "gravel")`, "bricks", "steel", "boards",
  "prefabpanels", "concrete", "cement", "asphalt", "mcomponents",
  "ecomponents", "bitumen"); walks the office's sources
  `office+0xF78 .. +0xF80` (16-byte entries: building, int kind; kind 2 and
  0 seen); sums amounts per resource over `world+0x144A8 .. +0x144B0`
  (0x68-byte records, each a vector of {resource, float amount} - likely
  what is stored or reserved). `vehicle+0x50C` is cleared on entry,
  `vehicle+0x514` set = nothing to do.

### Handing the job to the truck (end of `FUN_1401561c0`)

- `vehicle+0x50C` = the job code (likely which resource; negative `-9 - n`
  marks special cases); checked across the office's vehicles
  (`office+0xC70`) so two trucks do not take the same job.
- The schedule is rebuilt: `FUN_14067cb30(v, 1)` clears it,
  `FUN_14067d5d0(v, building, -1, 0)` appends stops - the source storage,
  then the site - and the office itself is pushed onto `v+0x680` last; a
  fresh 0x198-byte stop record goes into `v+0x638`; `FUN_1406b8eb0(v, 0, 0,
  0)` plans the route.
- The record of stop 0 (the source) gets the filter `{resource, 0, 1.0}` in
  its `+0x08` vector and the load `+0x04 = min(1, need / capacity + 0.01)`,
  need = what the chosen site still needs. That fraction is the whole reason
  a truck carries for one site.
- Job codes (`v+0x50C`) = the resource: 1 gravel, 2 bricks, 3 steel,
  4 boards, 5 asphalt, 6 concrete, 7 workers, 8 mechanisms at the site,
  9 prefab panels, 10 mechanical components, 11 electrical components.

## A site's need and cargo in transit

`FUN_14014cdc0(world, site, resource, job, float* need, float* have,
float* progress, float* loading, building* source) -> bool`, called 9 times,
all from `FUN_1401561c0` (one per resource):

- in transit = sum over the site's offices (`site+0x880`), their vehicles
  (`office+0xC70`) with `v+0x50C == job`, at least 2 stops, `stop[1] == site`,
  `v+0x698 < 2`, a cargo entry for the resource:
  `capacity * record0.load`;
- the current phase = the first whose progress mark is above the site's
  progress (`site+0x604`); its resource entry: `+0x08` delivered, `+0x0C`
  required;
- `*need = required`, `*have = delivered + in transit`, true while
  `need - have > 0`. The office picks the site with the lowest
  `have / need - progress - 2 * office+0xE04`.

## Unloading at a site

`FUN_140420350(world, building, mode, resource, float amount, float* rest,
..., office)` moves cargo between a vehicle and a building. For a
construction site served by an office (type 0xC or 0x6A) it adds to the
phase entry's delivered amount and caps it at `required + 1`; what does not
fit comes back in `*rest`. So a truck can never unload more than a site
needs, and the rest stays on board - with the game's own +0.01 there is
never a real rest in the vanilla game.

## Schedule helpers

- `FUN_14067d5d0(v, building, after, raw)` inserts a stop after position
  `after` (-1 = at the end) together with a fresh 0x198-byte record, and
  re-finds `v+0x698` so it still points at the same building. `raw = 0`
  may merge with or drop a finished building at the end; `raw = 1` does not.
- New records alternate by position: even `+0x00 = 0, +0xC8 = 1`, odd
  `+0x00 = 1, +0xC8 = 0`; the office's return stop gets both 0.
- `FUN_14014e6a0` recomputes the source's load for worker buses (job 7) of a
  3-stop schedule only.

## The real dispatch path (found in game, 0.1.3 trace)

`FUN_1401561c0` is not what hands out deliveries: in game it was only called
by the office window with no truck (a preview), and by the office tick for
class 0x12 machines. Deliveries go this way (office tick `FUN_140146b10`):

- the resources the office's sites need (`FUN_140155050`), then per resource
  and site `FUN_14014eb10(world, out, site, resource, 0)` builds a 0x40-byte
  job: `+0x00` site, `+0x08` source, `+0x10` truck, `+0x18` second vehicle,
  `+0x20` resource, `+0x28` int (0 = return to the office), `+0x2C` score,
  `+0x30` load fraction, `+0x34` need;
- need = `FUN_1401544a0(world, site, resource, 1, &transit)` = required -
  delivered of the current phase - `FUN_140154090(world, resource, site, 0)`
  (in transit), at least 0.005;
- in transit per truck = `FUN_1401541a0(world, resource, truck, site, 0)`, its
  only call is in `FUN_140154090`: the site's first stop from the current one
  on; ahead -> capacity x stop 0 load (if stop 0 loads and its filter has
  the resource), at it -> what is on board;
- jobs are sorted; `FUN_140155b60` picks the truck whose capacity fits the
  need best, `FUN_14014e9f0` sets truck, source (`FUN_140155960`) and
  `load = need / capacity` (capacity = `FUN_1406bddb0(truck, res, 1)`,
  trailers included);
- `FUN_140150960(world, job, office)` (3 calls) builds the schedule:
  source (`rec+0 = 1`, `rec+4 = load`, filter `rec+8` = resource), site
  (`rec+0 = 0`, `rec+0xC8 = 1`, `rec+0xCC = 1.0`, filter `rec+0xD0`), office
  (both 0); then the job code via `FUN_1406b8d70`. Afterwards the tick takes
  the truck's capacity off the job and puts it back in the list if the site
  still needs more.

## 0.2.0 - works (tested 2026-10-23)

- `FUN_140150960`'s 3 calls go through `HookDispatch`: after a dispatch with
  source -> site -> office, the office's other sites that need the resource
  (`FUN_1401544a0`, in transit included) are inserted nearest first, up to
  `max_extra_sites`, and stop 0's load becomes their sum / capacity (max 1).
- The one call of `FUN_1401541a0` goes through `HookTransit`: for a truck
  with a plan, a site ahead counts its planned share, the site it is at no
  more than that share.
- In game: a 25 t dump truck loaded 25 t for two road sites (17.8 + 7.2,
  17.5 + 7.5, 14.2 + 2.8); the second site got exactly its 17.5 t over three
  trips, nothing left on board.
- Plans live in memory only; after a reload multi-site trucks still running
  fall back to the game's count.

- 0.2.1: the load is the planned amount + `load_margin_t` (0.2 t): without
  it a few hundredths of a tonne went missing and the office sent a truck
  again for 0.03 / 0.06 t.
- 0.2.2: several trucks in one tick - the tick builds all its jobs first, so
  a job may predate a trip extended earlier in the same tick (a second
  truck served one of its sites, the first kept 6.2 t on board). The
  dispatch hook takes the job's need again (`FUN_1401544a0`, in transit
  included): nothing left -> no trip (return 0, the truck waits for the next
  tick), less -> job `+0x34` / `+0x30` lowered. Tested 2026-10-23: 5 houses,
  2 trucks, all built, nothing left on board, no tiny trips.
- A job without a source (`+0x08 = 0`) exists: the truck goes straight to
  the site (seen with need 21.65, load 1.0) - probably cargo already on
  board. Left alone by the plugin.

## Open
- A rest after the last site is thrown away with the next job. Next: start
  the next trip from it.
- Save/load in the middle of a multi-site trip.
