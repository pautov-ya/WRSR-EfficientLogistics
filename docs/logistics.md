# Findings: road distribution office trips (SOVIET64.exe 1.1.1.9)

Goal: a distribution office truck carrying one resource from one storage to
several smaller ones serves them in one trip, like the construction trips.

Test 2026-10-24: a reefer truck, meat from one storage to three smaller
ones - three separate trips of 8 t each.

## Trace (constructlog `trace_others = 1`)

Each trip, the same three stops from one function:

| Caller | Stop | Building type | Settings |
|---|---|---|---|
| `+1E55E5` | 0 | 5 (storage, the source) | load |
| `+1E583A` | 1 | 5 (storage, the destination) | unload |
| `+1E59D2` | 2 | 0x2B (the distribution office) | neither |

## The dispatch: `FUN_1401e5480(world, truck, office) -> bool`

- The office's jobs: `office+0xD58 .. +0xD60`, 0x80-byte entries, sorted
  first (`FUN_14020af10`). The first one `FUN_1401de2b0(world, job, truck,
  office)` accepts is given to the truck; the function returns after it.
- Job (0x80): `+0x00` resource, `+0x10` source building, `+0x18` int link
  index (source side), `+0x28` double, `+0x30` double, `+0x40` u8,
  `+0x48` destination building, `+0x50` int link index (destination side),
  `+0x60` double, `+0x68` / `+0x70` doubles - the amount is `+0x70 - +0x68`,
  `+0x78` u8.
- Links / settings: `office+0xD38` vector, each `+0x10` -> settings, copied
  into the stop records (`FUN_1401e0fc0(rec, settings)`, `rec+0xC8` from
  `settings+200`, `rec+0x190` from `settings+400`); `settings+0x04` and
  `settings+0xCC` floats are checked by `FUN_1401de2b0` (fill thresholds).
- Schedule: cleared by hand (`+0x688 = +0x680`, records destroyed), then
  `FUN_14067d5d0(truck, source, -1, 0)`: load, filter {resource},
  `rec+4 = (amount -/+ 0.05) / capacity` (capacity `FUN_1406bddb0(truck,
  res, 0, 0)`, clamped 0..1; 1.0 when the destination is type 0x14);
  `FUN_14067d5d0(truck, destination, -1, 0)`: unload, filter {resource};
  `FUN_14067d5d0(truck, office, -1, 0)`: neither. `truck+0x698 = 0`, route
  `FUN_1406b8eb0`.
- Then `FUN_1401e4b10(world, office, job)`: every job of the office with the
  same source or the same destination is updated by `FUN_1401e3bd0(world,
  job)` - most likely where the amount in transit is taken off.

## In transit

- `FUN_1401e3bd0` (job update) reads what is coming / going from the
  buildings themselves: `building+0x9D0` (and `+0x9B8`), vectors of 16-byte
  {resource*, float}: negative on a source (promised away), positive on a
  destination (on its way).
- `FUN_140429a80(world, truck, list)` adds one truck from its current stop
  on: a load stop -= capacity x load; an unload stop (filter `rec+0xD0` has
  the resource) += what is on board, or the WHOLE capacity while the truck
  is empty. `list` 1 -> `+0x9B8`, 0 -> `+0x9D0`.
- Rebuilt from scratch every tick: `FUN_14030c790` (world update) ->
  `FUN_140139a70`: per office list (`world+0x11B80`, `+0x11B98`) the links'
  buildings are zeroed (`FUN_140429990` / `FUN_1404299f0`), then every
  office vehicle goes through the wrappers `0x42a380` (list 1) /
  `0x42a3b0` (list 0), one call each. The dispatch also registers the new
  trip at once (list 0).

## 0.3.0 (built 2026-10-24, untested)

- `FUN_1401e5480`'s 2 calls -> `HookLogDispatch`: after a 3-stop trip with
  one loaded resource, the office's jobs with the same resource and source
  and another destination (accepted by `FUN_1401de2b0`, free = `+0x70 -
  +0x68 - 0.05`) are inserted nearest first; the load is raised; the source
  and destination reservations are corrected at once; `FUN_1401e4b10` per
  added job.
- The two register wrappers -> corrected per plan: each destination ahead
  counts its share (at it: no more than what is on board).

## 1.0.1: the top-up at the source

- A supply base truck arriving at its source is topped up by
  `FUN_1406b0240` (called from the vehicle update, `+69DD5B`): the load
  becomes what the next destination can still take, never less than it was.
  It takes the truck itself off the destination's `+0x9D0` first.
- For a storage that amount comes from `FUN_1401e7940(_, destination,
  resource, truck)`: free room minus 0.05, without the cargo on its way.
  The office never sends a second truck while one is due, but a planned trip
  brings a share the office does not know of, so the second truck loaded it
  again (bug report: 21% -> 60%, 10 t brought back). Found with a hardware
  write breakpoint on the record's load (branch `debug/watch-load`).
- The single call of `FUN_1401e7940` is redirected: when another planned trip
  brings a share to that destination, what `+0x9D0` still holds is taken off.
