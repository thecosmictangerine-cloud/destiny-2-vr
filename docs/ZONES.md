# Zones, loading, and how to land in one on purpose

Persistent reference (unlike `HANDOFF.md`). Everything here was measured on the pinned build with
Sunrise master `7e3875d`, 2026-09-08. Raw logs are in `evidence/`.

## Why patrol zones loaded black, and the fix

Patrol destinations are **public** bubbles. Sunrise's own `hooks/bootflow/region_private.cpp`
says it: *"a public region holds its slice-set switch until a public activity host connects; the
answer is public unless `client.region_private` is on, or a destination is forced."* Whether the
embedded host connects in time is a race. The Tower is a social space, never public, hence "the
Tower always loads".

Losing side of the race, as seen from the user and the log: orbit, landing animation, then black.
No HUD, no radar, no audio. **Esc opens the menu and the Director works**, so the game is not
hung. `sunrise.log` shows the client reaching `activity:in_world`, `slice_set_transition_manager`
going `fully_enabled`, loopback networking perfect, no `stall`, no `hitch`, no error. It is not a
loading hang; the world never starts.

Fix, in `C:\Games\Sunrise\bin\x64\Sunrise\settings.json`:

```json
"client": { "region_private": true }
```

Set on 2026-09-08 21:47. Since then every patrol loaded first try (Io, Mercury, Mars, EDZ, and
the user's later ones). The log line flips from `ev=bootflow stage=region result=public` to
`result=forced`. An unmodified upstream build showed the same black screens, so the VR module was
never the cause (`evidence/2026-09-08-upstream-clean-build-black-loads.log`).

A **different**, rarer failure exists: the main loop stalling in `network_send` with
`ev=probe stage=stall result=detected` and `hitch detected ... network_send` every 3 s, seen on
2026-09-07 evening. Not reproduced since; keep the two apart.

## Landing in a chosen zone without the Director

`hooks/vr/vr_destination.cpp` reads `C:\Games\Sunrise\SVR_Destination.txt` on the first camera
frame (that is orbit) and calls `state::activity::forced::publish`, the same call Sunrise's
Activity override panel makes but which the panel never saves. Any Director launch afterwards
lands in the forced destination; the client's own pick is replaced server-side. A forced
destination also makes the region private (see above). The file is re-read when its contents
change and never deleted. Rename it to `.off` to give the Director back to a human.

Format, one line:

```
<package_name> <bubble> <slice_set> [<activity_index> [<spawn_set_hash>]]
```

**Give the activity index.** Forcing replaces the package but, by Sunrise's design, drops the
activity definition, so the launch keeps the Director node's one. A launch from a raid node with
`eden_freeroam 20 160` changed the world to Io with a raid-type activity (`nadir_endgame`, 255)
and sat in the ship forever, never reaching `physics_join` (2026-09-08 22:00). The index is the
`activity=N` from `ev=bap svc=42` for that package (table below). The spawn hash is optional;
without it the client picks a point in the bubble. The log confirms with
`ev=vr.dest forced result=ok name=... bubble=... slice_set=... activity=... active=1`.

Verified end to end on 2026-09-08 22:10 with `Run-Patrol.ps1` and `eden_freeroam 20 160 7`: the
scripted Director click still resolves to a raid node (`svc=42 ... name=nadir_endgame` is the
client's own request and keeps saying so), the server lands the ship in Io regardless, the client
reaches `physics_join` and `in_world` about 12 s after the world swap, and the user confirmed
Giant's Scar on screen. The `svc=42` line is therefore not the place to read the real destination;
`changed world to:` and `region result=forced slice_set=` are.

## Zone table

`slice_set` is the value the client reports on landing; `bubble = slice_set / 8`. `activity` is
the investment activity index Sunrise prints for the package (`ev=bap svc=42 ... activity=N`).

| Zone | package | activity | slice_set | bubble | Notes |
| --- | --- | --- | --- | --- | --- |
| Io, Giant's Scar | `eden_freeroam` | 7 | 160 | 20 | User's preferred first-person bed |
| EDZ, The Gulch | `edz_freeroam` | 8 | 280 | 35 | User's second suggestion; second region 96 seen after walking |
| Nessus (user's landing) | `planet_x_freeroam` | 9 | 24 | 3 | Which node is unknown |
| Titan (user's landing) | `fleet_freeroam` | 10 | 16 | 2 | Which node is unknown |
| Mercury | `mercury_freeroam` | 29 | 120 | 15 | |
| Mars | `polaris_freeroam` | 30 | 8 | 1 | |
| Tower | `city_tower_social_d2` | 20 | 48 | 6 | Third person; Sunrise's own default |
| Moon (Sorrow's Harbor) | `luna_freeroam` | ? | ? | ? | Loaded on 2026-09-08 afternoon, values not captured |

Ready-made lines:

```
eden_freeroam 20 160 7
edz_freeroam 35 280 8
```

## Capturing a new zone

1. `core.logging.levels.client` = `info` in `settings.json` (already set; `server` too).
2. Let the user land there through the Director.
3. Read `ev=bap svc=42 stage=configuration ... name=<package>` for the package and
   `ev=bootflow stage=region result=... slice_set=N` for the slice set. Bubble names are not in
   any catalogue (only hashes), so the Director is the only way to learn a node's values.

Internal names, for grepping the catalogues (`bin\x64\Sunrise\cache\build_data.bin`):
Io = `eden`, Nessus = `planet_x`, Titan = `fleet`, Mars = `polaris`, Moon = `luna`,
Dreaming City = `dreaming_city`, Tangled Shore = `tangled_shore`, Leviathan raid area = `leviathan`.

## Log signatures worth knowing (client at `info`)

| Line | Meaning |
| --- | --- |
| `world_controller: successfully changed world to: <package>` | World package swapped in |
| `Entering state 'activity:initial_slice_set_loading'` → `physics_join` → `in_world` | Normal load, about 25–35 s |
| `ev=bootflow stage=region result=forced slice_set=N` | Private region, will render |
| `ev=bootflow stage=region result=public slice_set=N` | Public region, may stay black |
| `application_state: Suspend state changed [active]→[partial]` | Window lost focus |
| `ev=probe stage=stall result=detected idle=N` | Sunrise's own main-loop stall detector |
| `ev=vr.dest forced result=ok ...` | Our forced destination was published |
