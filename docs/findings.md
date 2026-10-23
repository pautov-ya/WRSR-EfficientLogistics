# Findings: construction office deliveries (SOVIET64.exe 1.1.1.9)

Goal: a construction office truck carries for several construction sites in
one trip, instead of one trip per site with exactly what that site needs.

## Types and objects

- Building type ids (`[type+0x360]`, set by the type loader `FUN_14010e1f0`):
  `$TYPE_CONSTRUCTION_OFFICE` = `0xC`, `$TYPE_CONSTRUCTION_OFFICE_RAIL` = `0x1C`.
- Vehicle: `+0x4F8` its workplace (the office), `+0x1708` its type,
  `+0x680` schedule (std::vector<building*>, first = where it goes now),
  `+0x698` current stop, `+0x4F0` building it is in.
- Vehicle type: `+0x85E0 .. +0x85E8` std::vector of 16-byte records, one per
  resource it can carry (first int = resource id); `+0x8600` transport class.
- Building: `+0xC70 .. +0xC78` std::vector<vehicle*> of vehicles inside it.
- Construction site: `+0x610 .. +0x618` its phases, 0x21B8 bytes each
  (`+0` float progress mark, `+4` int phase kind, `+8 .. +0x10` vector of
  16-byte resource entries); `+0x634` current phase index.

## Where things happen

- `FUN_1407838e0` - the construction window (texts 1427 "Resources needed to
  finish entire construction", 1432 "Resources on construction site").
- `FUN_140159070(world, site)` - the construction site's tick. For every
  vehicle inside the site whose workplace is a construction office (type
  0xC): walks the resources its type carries, asks
  `FUN_14010c2b0(phase kind, resource, 0)` how much the site needs - the
  unloading. Then the vehicle is sent on: `[[v+0x680]] = next building`,
  `v+0x610 = 0`, `v+0x528 = 0`, `v+0x530 = 0`, `v+0x548 = v+0x540`,
  `v+0x558 = 0` (route reset). The next building comes from
  `FUN_140161c90(world, office, building, 0, 0, ...)`.
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
- No amount is written here: the truck decides how much to take at the
  source, from what the site needs - which is why it carries for one site.

## Next

1. Read `FUN_1401561c0` through: which site it picks, the amount it loads
   (the site's need), where the amount already on its way is recorded.
2. Find where the load amount is set at the source storage (the truck loads
   exactly the site's need) - probably in the loading code of
   `FUN_14068b330` for a vehicle whose workplace is an office of type 0xC.
3. Then the plan: (a) let the truck take more and add the extra sites as
   stops between the first site and the office; (b) count what is on its way
   for every site on board so no other truck brings the same.
