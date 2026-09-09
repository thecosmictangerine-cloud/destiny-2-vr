# The body servo, F9, and the travel hang that turned out not to be ours (2026-09-09)

> **Read this box first.** The servo runaway described below is real and is fixed, and it is worth
> having fixed. It was **NOT** the cause of the hang when travelling between zones, and this file
> originally said it was. The correction, and the evidence, are in the last section: the hang
> reproduces with the OpenXR session torn down and VR switched off.


Both faults come from the first real Quest 3 session in which the weapon was declared right, so
neither had been seen before. Evidence:
`evidence/2026-09-09-quest3-link-6dof-ok.log` and `evidence/2026-09-09-quest3-link-hang.log`.

## 1. The gain learner ran away

### What it looked like

It was found while looking for the cause of the travel hang, and it looked like the cause: the
runaway peaked seconds before the freeze, every time. Section 3 shows it was not, so read this
section as "a real fault, found and fixed, that happened to be next to the crime scene".

### The timeline, from the log

```
t=504985  world changed to fleet_freeroam        counts/rad=524   mouse ~20k cumulative
t=515938  activity:physics_join                  counts/rad=507
t=518766  the learner starts to run away         counts/rad=1551  mouse=48308
t=521266  it keeps going                         counts/rad=3211  mouse=71385  (~28k counts/s)
t=521969  activity:in_world
t=528328  stall detected, idle=6000              (so the freeze began around t=522300)
```

### Why

Two mechanisms, and it is their combination that is fatal.

**The learner accepted samples from a character that was not responding.** The test was only that
the character had moved *more than 0.002 rad*:

```
} else if (g_lastServoCounts != 0 && std::fabs(achieved) > 0.002F && sign_agrees) {
    const float measured = -g_lastServoCounts / achieved;
```

`measured` blows up as `achieved` approaches zero. During `physics_join` the character is being
created and barely turns, so every frame handed the learner a huge `measured`, and with
`kServoCountsPerRadianMax` at **12000** a wide band of nonsense was admitted. Asking for a turn and
getting a crumb is not a measurement of the mouse conversion; it is a measurement of the character
being unavailable.

**The injection ceiling scales with the gain, so the error feeds itself.**

```
const float ceiling = kServoMaxRadiansPerFrame * g_countsPerRadian;
```

At the learned 579 counts/rad that is 35 counts a frame. At 3211 it is 193 — and at 90 Hz, 17k
counts a second. A gain that drifts up injects more, which produces worse samples, which drifts it
up further. The observed peak was about 28k counts a second. Flooding the game's raw-input path at
that rate looked like more than enough to stall it -- but section 3 shows the game stalls the same
way with none of this happening at all.

The same runaway appears in the other log for a different reason: with the game window **unfocused**
the injection is gated off before it reaches anything, so the character never turns at all. There
the gain pinned at 4872 and 330,000 counts went out while `body_yaw` moved 0.017 rad. Clicking the
game window before putting the headset on avoided it, which is what made the good session good.

### The fix — four parts, in `vr_camera.cpp`

1. **Learn only from a real response.** `|achieved| > kServoMinResponseFraction * |expected|`
   (0.25) replaces the 0.002 rad floor. This alone rejects every sample that caused the runaway.
2. **`kServoCountsPerRadianMax` 12000 → 3000.** The value learns to 579 on this machine, twice,
   from a 900 guess; 3000 is headroom for a different mouse sensitivity and far below flooding.
3. **An absolute cap of `kServoMaxCountsPerFrame` = 150**, independent of the gain. This is the
   backstop that makes a bad gain survivable rather than fatal — four times the headroom over
   normal work, while making a flood arithmetically impossible.
4. **Stop injecting after `kServoStuckFrames` = 20 frames of no response**, with a log line
   (`ev=vr.body servo=stuck`). Pushing harder at an absent character cannot help. It recovers by
   itself: one frame in twenty still probes, and any real response clears the counter.

Consequence worth knowing: the character will not follow the gaze while the game window is
unfocused, and now it will not try to either. That is the correct behaviour — the alternative is
what froze the game — but it means **desktop testing wants the window focused**, and a headset
session should have the game clicked once before the headset goes on.

## 2. Switching the module off froze the headset

`present_frame` returned immediately when disabled:

```
if (!g_enabled.load(...)) { gamepad::release_all(); return; }
```

So `xr::begin_frame`/`xr::end_frame` stopped being called while the OpenXR session stayed alive.
On the monitor that looks exactly right — the view returns to normal — but the runtime has no new
frame to show and holds the last one it was given. In the headset F9 therefore reads as "it will not
come out of VR", which is what the user reported.

Worse, it defeated the documented workaround for the destination-loading hang, which is to switch
the module off before travelling: doing so froze the headset, so there was no way to travel at all
while wearing it.

**Fix: switching off tears the OpenXR session down**, and F9 brings it up again through the lazy
initialize that always existed. That is the symmetry the switch implied all along, and only giving
up the session actually returns the player to the runtime's own environment.

Two weaker versions were tried first and are recorded because neither does what "off" has to mean
in a headset:

- *Return early*, the original: leaves a live session receiving no frames, so the runtime holds the
  last one it was handed. The monitor looks right and the headset is frozen.
- *Keep the frame loop running and end each frame with no layer*: the session stays healthy, and the
  Meta runtime **still** shows the last thing it was given. The user confirmed it looked identical.

Verified in the mock: `ev=vr.camera toggle=off xr=shutdown` on the way down, the pose lines stop, and
a second F9 logs `vr.xr init result=ok` and `palm_spaces=1` again with the poses flowing. So
re-initialising after a teardown works -- `shutdown()` clears `g_attempted` for exactly that.

One consequence to expect: shutdown asks for a recentre, so toggling out and back in **re-seats the
origin**. That is what a player toggling out and back in wants, but it is a visible change.

## 3. The travel hang is not ours, and the servo was not causing it

The servo fixes went in and the hang stayed. That is the useful result, so here is the evidence
rather than the earlier guess.

**The servo fixes worked.** In `evidence/2026-09-09-quest3-travel-hang-2.log` the gain stays inside
555-926 across the whole session where it previously reached 3211, the largest mouse figure in any
reporting period is **5874** against **71385** before, and `servo=stuck` fired twice and recovered.
And the game still froze, 200 ms after `activity:in_world`.

**Nothing in the stall is ours.** That log carries 357 stack dumps from the stall probe and **not
one frame is in this module** -- every frame resolves to `exe+...`. What does stop is Sunrise's own
network tick: `latch=1 callbacks=34243` becomes `latch=0 callbacks=34349` and never moves again.
That is the signature already recorded for the destination-loading hang of 2026-09-08. The game
also reports the transition itself as `took: [48468 ms], average: [35462.500000 ms]`.

**And it happens with VR switched off.** From `evidence/2026-09-09-quest3-f9-teardown.log`:

```
t=436766  F9 off -> ev=vr.camera toggle=off xr=shutdown     (the OpenXR session is gone)
t=471703  travel back to orbit                              (VR off for 35 s by now)
t=505485  travel to mission_ember                            (VR off for 69 s by now)
t=516750  activity:in_world
t=523188  stall detected, idle=6141                          (the freeze began ~250 ms after in_world)
```

No session, no frames, no camera write, no servo, no injection -- and the same failure with the same
timing. So the travel hang is not caused by anything this module does while running.

**What is still not ruled out**, stated plainly: the weapon's three detours stay installed after F9
off, so this is not a *pure* control. A session in which F9 is never pressed installs nothing at all
(`CLAUDE.md`: before F9 the module has not touched the engine), and that is the test that would
settle it completely, alongside building upstream `7e3875d` as the control the project notes already
prescribe.

**The shape of the failure points away from VR anyway.** The FIRST destination load of a session
works -- in the `6dof-ok` log the user played five minutes in `fleet_freeroam` after one -- and it is
the second that hangs, at `in_world`, every time. That reads as state left behind by the first load.

## What this does not change

The **black render during the ship-in-flight transition** (`CLAUDE.md`, gotchas) is a separate,
cosmetic thing and is untouched.
