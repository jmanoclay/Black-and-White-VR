# Performance: frame time in the headset

The goal: the Index's native refresh with under 1% of frames reprojected. The player's Index runs at **120 Hz: 8.3 ms a frame**.

## Where it stood (2026-10-19)
In the player's session of 2026-10-19, **25% of frames were reprojected** (17,949 of 70,682), and up to 45% in busy stretches. The game delivered about 105 frames a second against 120. On 2026-10-06, with less in the frame, the same pipeline had 0–4 per 600.

## How it's measured
- **The 5-second stats** (`bwvr.log`) now break a VR frame down:
  `frame 8.27 ms: wait 0.03, setup 0.06, eyes 2.66/2.46, readback 1.43/1.40, after 0.01, outside 0.21; longest 12.0 ms (most: eyes 8.4, readback 3.7, outside 4.4; outside over 2 ms 2), over 8.6 ms 133/603 (readback: lock 1.55, copy 1.05)`
  - wait: in `WaitGetPoses` (slack: the frame was early);
  - eyes: each eye's 3D pass;
  - readback: each eye's picture brought back from the GPU and uploaded for SteamVR (lock: DirectDraw bringing it back; copy: the mod's copy into the upload texture);
  - outside: the game's work outside the 3D pass (turns, 2D, its window).
  - SteamVR's own count is on the same line: `openvr: 604 presents, 0 dropped, 3 reprojected`. "Over 8.6 ms" counts the gaps between the mod's frames. They jitter with the wait, so SteamVR's reprojected count is the one that matters.
- **SteamVR runs from the harness** with the Index on the desk (`vr openvr`). Without the base stations the headset is untracked, but the frame timing is real.
- **A sampling profiler:** `stereo profile <ms>` samples the game's thread every 0.25 ms, recording the instruction pointer, a few return addresses, and which part of the frame it's in, into `captures\profile.txt`. `python tools/perf/profile_report.py [--parts 2,3]` names the game's functions from the decomp's symbols and other code by its module.

## What the frame was made of (before)
SteamVR, at 120 Hz:

| | ms |
|---|---|
| Left eye's 3D pass | 2.9 |
| Right eye's 3D pass | 2.7 |
| Readback + upload | 3.7 (lock 1.6, copy into a buffer 0.9, upload 0.65, the rest timing) |
| Everything else | 0.4 |
| **Frame** | **9.7** (every frame over 8.3 ms) |

- The GPU is nearly idle (about 1 ms a frame). It's all CPU.
- **In the eye passes, about half the time is Direct3D 7 and the driver** (`d3dim700`, `DDRAW`, `nvd3dum`): call overhead for about 700 draws an eye. Most of the rest is the engine's own landscape and object code (`LH3DP3`, the SSE landscape renderer, at the top).
- The mod's own per-eye work is small: near-plane clipping about 0.2 ms an eye, the world-space sea about 0.25 ms; the others don't measure.

## Changed (2026-10-19)
- **Readback straight into the upload texture.** Each eye's picture used to be copied from the locked render target into a buffer, then uploaded with `UpdateSubresource`. Now the rows go straight into a mapped `D3D11_USAGE_DYNAMIC` texture, which is copied to the submitted texture on the GPU (`map_eye` / `submit_mapped_eye`). That's one 12 MB copy an eye less. The copy is shared with three helper threads (`RowCopier`). Only the crop is locked, not the whole render target.
- **Both eyes are read back after both are drawn**, rather than each right after it's drawn.
- Result: the readback went from 3.7 to about 2.8 ms, and the frame from 9.7 to about 8.3–8.6 ms. In the harness view, steady 5-second stretches now show **0–3 reprojected of 604**, with some stretches still around 60–130 (10–20%).
- Checked that SteamVR receives the right picture: `mirror_check.py` compares SteamVR's mirror with the mod's eye images. Untracked, SteamVR greys out what the lens shows; outside that, the two agree within about 2 levels (0.1–0.3% of pixels differ by more than 40).

## Tried, and dropped
- **The GPU copying each eye to a system-memory surface** (`Blt` to `DDSCAPS_SYSTEMMEMORY`, to overlap the transfer with the next eye): the blit is synchronous, about 2 ms an eye. Worse.
- **Reading the left eye on another thread while the right eye is drawn:** DirectDraw serialises the lock with the drawing. The right eye went from 2.5 to 3.8 ms, more than the readback saved.
- **Reading back only what the lens shows** (the hidden-area mesh: the lens shows 75% of each picture; locked in bands of rows): the lock costs about 0.7 ms an eye whatever its size (a synchronisation with the GPU), and more bands only add copying.
- **`D3DDP_DONOTCLIP`** on the eye-pass draws (skipping Direct3D's own clipping of pre-transformed triangles): no measurable change, and images identical.
- **Merging draws:** only 20% of eye-pass draws follow one with the same state (the engine changes texture or state on most), so batching would save about 0.25 ms an eye. Not done.

## The zero-copy path: dxwrapper (2026-10-19)
The game's Direct3D 7 now runs on Direct3D 9Ex through **dxwrapper** (github.com/elishacloud/dxwrapper, zlib licence; release v1.8.8600.25, built by `tools/build_dxwrapper.ps1` with `third_party/dxwrapper.patch`). Our proxy loads it as the "real" ddraw.dll: `[proxy] chain=bwvr\dxwrapper\dxwrapper.dll`, with its settings in `bwvr\dxwrapper\dxwrapper.ini` (`config/dxwrapper.ini`). If it can't be loaded, the proxy falls back to the system's ddraw.dll.
- **The eyes go to SteamVR on the GPU.** dxwrapper creates each of our eye render targets as a Direct3D 9 surface. A new export in our patch, `DxwGetD3D9Surface`, hands us that surface and dxwrapper's device (`src/render/d3d9_bridge.cpp`). Each frame each eye's crop is copied (`StretchRect`) into a D3D9Ex texture shared with D3D11, then copied into the texture submitted to SteamVR (`share_eye` in `openvr_backend.cpp`). An event query makes sure Direct3D 9 has finished first. No readback: 0.55–0.75 ms a frame for both eyes, against 2.8 ms (and 3.7 before today).
- **Checked:** SteamVR's mirror against our eye images agrees within 0.5 levels (the readback path: 2). The pictures match the native ones (the same frozen views, 0.0 difference between two dxwrapper boots; against native only the clouds differ, which are weather). The regression suite passes as natively, with the same known failures (`hand_hints`' flaky label, `catch_throw` after other tests, `pet_creature`: the creature isn't where the test expects in the current save, natively too).
- **What it cost, and what was fixed:**
  - dxwrapper's draws cost more on the CPU than Direct3D 7's: eyes 3.65/3.5 ms at first, against about 2.7/2.5.
  - **Its busy-wait mitigation** (from DDrawCompat): a `Sleep(0)` in `GetTickCount`/`timeGetTime` once a thread had run 3 million cycles. The game reads the time constantly while drawing; about 8% of each eye. Left out in our patch.
  - **`D3DCREATE_MULTITHREADED`:** every Direct3D 9 call took a lock. `DdrawNoMultiThreaded = 1` (dxwrapper's own present thread only runs in exclusive fullscreen).
  - **The window's mirror** (the eye blitted into the game's window): dxwrapper does a stretched DirectDraw Blt slowly, about 1.3 ms. Now a Direct3D 9 `StretchRect` (`EyeTarget::blit_to` through the bridge).
  - **Its emulated wait for the monitor's vertical blank** on the game's flips: `ForceVsyncMode = 1`, `EnableVSync = 0`.
  - **Our near-plane clipping:** in VR many big draws (landscape blocks, the world-space sea) have a vertex behind the head. Each vertex is now tested once, without a division; the indices are written in place; the vertices are copied only when a clipped vertex is added. Pictures identical. (A first version wrote past the index buffer when clipped triangles became quads and crashed the game in SteamVR; fixed: room for twice the triangles.)
- **Result** (SteamVR at 120 Hz, the headset untracked on the desk):

  | View | Eyes (ms) | Hand-off | Frame | Reprojected |
  |---|---|---|---|---|
  | Boot view, 50 s | 3.0 / 3.0 | 0.75 | 8.33 (0.8 ms to spare) | **0 of 6048** |
  | Over the village, looking down | 3.4 / 3.4 | 0.75 | 8.33 | 0 |
  | Across the village to the horizon (draw distance 250%) | 3.6 / 3.5 | 0.75 | 8.3–9.1 | 0–30% by stretch |
  | The same at the game's own draw distance | 3.4 / 3.3 | 0.75 | 8.33 | 0 |
  | Head swaying ±20° | 3.4 / 3.3 | 0.75 | 8.7–9.1 | 13–20% |

  In the heavy views a few slow frames (12–18 ms) tip SteamVR into reprojecting at half rate for a while, so a stretch shows either about 0 or about 200 of 600.
- **First headset session (2026-10-19): the whole world white and grey, without textures.** The harness hadn't shown it because no controller was active there. The mod's own draws (the hand's marker, the laser, the ring, panels: `draw_tris`; the cinema screen's quad) set their own texture stages inside a Direct3D 7 state block and applied it afterwards to put the game's states back. Through dxwrapper that didn't bring the texture stages back, and the game, which rarely sets them, went on drawing everything with the vertex colour only. Now `render::StateSaver` remembers each state those draws change and sets it back (`src/render/state_saver.h`); no state blocks. `tools/validate/textures_with_hand.py` checks that the land keeps its colour with the hand and the laser drawn: it fails on the old code (saturation 0.05) and passes natively and through dxwrapper (0.6). After a cutscene on the cinema screen the world is textured too.
- **Also from that session:** the main menu was still open when VR started (through dxwrapper it comes later than the harness's Esc at 25 s). `bwctl boot` (and `bwctl vr`) now presses Esc again while the mod's screen info shows a menu.
- **Not working through dxwrapper:** the depth captures (its z-buffer can't be locked): `stereo_checks.py` and `sea_lock.py` need them. `texture_shimmer.py` can't tell its two cases apart (the large LOD bias of its "without" case doesn't take effect); the shimmer it measures is lower than natively with mipmaps (1.7 against 3.35).
- **Tools:** `stereo flip_every <n>` (the game's window only every nth frame, an experiment that found the mirror's cost); `profile_report.py` names dxwrapper's functions (`dxw:`) and the mod's (`mod:`) from their PDBs, and puts time in Windows and the driver on its caller.

## Spikes: the game's sound stops (2026-10-28)
The player's session (120 Hz): the average frame had 2-4 ms to spare, but 45 of 96 five-second stretches had a frame of 20-170 ms, and each one tipped SteamVR into reprojecting at half rate for a while ("low frame rate").
- **Spike catcher:** a watcher thread checks every millisecond how long the frame's current part has run; past 15 ms it samples the game's thread until that part ends, into `captures/spikes.txt` (`stereo spike_ms <n>`, 0 off). Nothing happens in frames that aren't slow, so it stays on in play. `profile_report.py` reads the file; `--slow-ms` picks the slow frames out of an ordinary profile (each sample now carries its frame number).
- **The cause of most:** the game's audio library (`LHaudiodllR.dll`) stops a playing sound by fading it over 20 ms in QMixer and sleeping 20 ms on the game's thread for the fade (three places in `LHSampleStop`); a sleep lasts 20-30 ms or more, and a few stops together made 40-170 ms. In a harness tour of the island (the rig set down in ten places), 16 spikes over 15 ms, all the "outside the 3D pass" ones in that sleep.
- **Fixed** (`src/core/audio_stop.cpp`, `[audio] async_stop`): the library's imports are patched. Those three sleeps are skipped, and the stop's end (fade speed back, channel flushed) is done 25 ms later by a helper thread. Every other QMixer call the library makes goes through a thunk that first finishes a pending stop on that channel, so QMixer sees the calls in the same order as before. The same tour: 4 spikes, none outside the 3D pass.
- **Left:** spikes in the eye passes when new land comes into view: dxwrapper's texture copies (`D3DXLoadSurfaceFromSurface`, about a quarter) and the mod's mip levels for the new textures (about a fifth). To be judged from the spike catcher in real play before the mipmaps get a per-frame budget.

## The stutter as play starts (2026-10-30)
The player: stutter at the start and while the land is first travelled, smooth afterwards. Their session's spike log and 5-second stats say:
- **Most of it isn't spikes but heavy views.** From 61 to 116 s the game made 350-530 frames per 5 s instead of 600, with eye passes of 4-6.5 ms each (1.8-2 ms elsewhere), and 150-300 of 600 reprojected; no part of a frame passed 15 ms. They were 6-24 m above the village looking across it. From 120 s, at the workshop near the ground, 600 frames and 0 reprojected.
- **Reproduced through SteamVR** (`TOUR_OPENVR=1 python tools/perf/island_tour.py <tag> 5 2`, the headset on the desk): the same ten places, twice, gave eye passes up to 7.7 ms in both rounds, so it is what's in view, not loading it the first time. Over the village from 9 m: about 2,200 draws an eye (a light view: 300), 7.6 ms. The mock headset's fixed view looks elsewhere and stays at 1.6 ms.
  - `detail_distance_percent` (150): 100 gives 6.0 ms, 70 gives 5.1 ms there. Not enough alone at 120 Hz; at 90 Hz (11.1 ms) 100% would fit.
  - The profile there: the engine's own work per eye and Direct3D 9 calls through dxwrapper (`SetDrawStates` and `PrepDevice` about 16% of the eye pass); 24-32% of the draws could merge with the one before.
- **The first-time costs are a handful of spikes:** the first frame (the save loading, 0.5 s); textures as they're first drawn (dxwrapper's `D3DXLoadSurfaceFromSurface`, 25-33 ms, three in the first 4 s); an animation loaded the first time it's needed (`LHFile::ReadData`, `LHMatrix::SetYXZMatrixOnly`, 125 ms once, at the same place each fresh boot); the game opening a file with `fopen` (25-45 ms, twice; the name will now be logged).
- **dxwrapper's `CreateFileA` hook isn't the cost:** it only checks the security-attributes argument (`Utils::kernel_CreateFileA`) and calls the real one. The time under it is the file open in the kernel, which is slow when the file isn't in the system's cache (2-8 ms an open the first time after a reboot; 0 after). The profiler names the hook only because it's the last named frame.
- **The registry:** one 25 ms frame in the session, `LHNetGetCurrentProfileUlong` -> `LHLogR!RegistryRetrieveVal` (named with `tools/perf/name_exports.py`), not seen in the harness. Not cached: once in a session.
- **The mip levels** for new textures: 5-60 ms per 5 s (0.01-0.1 ms a frame). Not the cause; no budget needed.
- **Now logged** (`src/core/load_trace.cpp`): every `LHOSFile::Open` and `CreateFileA` from the game over 2 ms (the first 60), with the file's name, and the totals with the 5-second stats (`load: N file opens (ms)`); `stereo load_trace <n>` logs the next n.

## Heavy views, measured again (2026-10-30, evening)
The player's session: 102 frames a second drawn at 120 Hz, 31 a second reprojected, eyes 3.4 ms on average; over the village (and its island across the water) 6-7 ms an eye. In the harness through SteamVR (`tools/perf/heavy_view.py`, `tools/perf/dxw_ab.py <switch>`: the same views in one boot, a switch on and off):
- **dxwrapper's per-draw work** (82f496c and the vertex range after it): each draw bound its textures and unbound them after, set again every transform it had been given (that batch was never cleared), turned lighting off and on, set the viewport and z, and had Direct3D 9 copy all of the vertex array (a landscape draw uses a few dozen of a block's 289 vertices, once per texture). All fixed in our patch (`stereo dxw_fast 0|1`): 2-10% of an eye; the heaviest view 6.7 against 7.0 ms. Pictures as before.
- **Merging draws, tried and dropped:** the landscape is drawn per block and texture (4,238 draws a frame there, a texture bound before nearly each). Grouping the draws between state changes by texture, keeping the order wherever they overlap on the screen (most are blended), halved the draws (524k to 276k per 5 s), but the copying cost more than the calls saved: 7.5 against 6.7 ms. The call itself isn't the cost.
- **Where an eye's time goes there:** about two thirds the engine's own work (landscape, objects), repeated for the second eye; the rest dxwrapper, Direct3D 9 and the driver.
- **The draw distance** (`detail_distance_percent`, 150) in the heaviest view: 150 6.8, 120 6.2, 100 5.4, 80 4.9 ms an eye. Even 80% doesn't fit 120 Hz (8.3 ms a frame for both eyes); 100% fits 90 Hz only just.

## One engine pass for both eyes (2026-10-30, evening)
`[stereo] one_pass` (on): the engine's eye pass runs once, with the camera between the eyes (the same symmetric frustum and focal length both eyes render with), and its Direct3D 7 calls are recorded instead of drawn (state calls still reach the device). Each eye then replays them into its own target with every vertex moved to that eye: the vertices are pre-transformed, sx = cx + f X/Z, sy = cy - f Y/Z, rhw = near/Z (1 at the near plane: found by lining up the pictures; the near clipper's comment says 1/Z), so a lateral eye offset dx moves a vertex by f dx rhw / near.
- **Checked:** `tools/validate/one_pass_check.py` captures both eyes frozen with one_pass off and on: mean difference 0.13 levels, no shift near or far (with the wrong rhw convention: 2 px near). `hand_hints.py`: the label by the hand 40 px between the eyes (38 with two passes). The mod's own 2D/UI vertices now use the engine's rhw; text drawn at a made-up depth ("in front of everything") is moved by its real depth (`trace::set_record_depth`). The picking eye is the recording camera.
- **Timing** (`TOUR_OPENVR=1 python tools/perf/dxw_ab.py one_pass`, the same views in one boot): over the village 7.5 + 3.5 ms against 7.0 + 6.2 ms; reprojected 181-194 against 280-313 per 5 s. The left eye is the engine plus recording plus its replay; the right only the replay (Direct3D's share: ~3.4 ms there).
- **Not covered:** the cinema screen and state diffs (two passes, as before); things very near the outer edge of one eye that the middle camera doesn't see (a sliver); the gesture panel's icons (the harness can't show them).
- **The replay's draws grouped by texture** (546501e, `stereo replay_group 0|1`): planned once a frame, a draw joins its texture's last group unless a later group overlaps it on the screen (boxes widened by the most an eye moves it). Over the village 2,200 draws an eye become 1,400, but an eye's replay only goes from 2.9 to 2.6 ms: the cost is per vertex, not per call.
- **Where the heavy view's time is now** (eyes 7.5 + 2.6 ms): Direct3D 9 copying each draw's vertices in `DrawIndexedPrimitiveUP` (~13% of the eye passes) and the driver (~12%); the engine's own work; the recording and plan (~8%). 120 Hz (8.3 ms for the whole frame) is out of reach in that view; 90 Hz (11.1 ms) fits.
- **dxwrapper draws from its own dynamic buffers** (`stereo dxw_fast 3`; 1 the old way): each indexed draw's vertices and indices are appended to an 8 MB vertex and a 2 MB index buffer (no overwrite until they wrap, then discard) and drawn with `DrawIndexedPrimitive`, in place of `DrawIndexedPrimitiveUP`. (Done inside dxwrapper, not as vertex-buffer draws from the replay: the game's Direct3D 2/3 draws get dxwrapper's older texture-blend handling, which its Direct3D 7 vertex-buffer path skips.) The heaviest view: 7.8-8.1 + 2.15-2.3 ms against 8.4-8.6 + 2.6-2.8 ms, reprojected 158-171 against 187-198 per 5 s; pictures identical.

- **The replay hands dxwrapper an eye's vertices at once** (`stereo replay_prefill 0|1`; `DxwRingFill`, our patch): the replay writes all its groups' moved vertices and indices into two arrays, dxwrapper copies them into its buffers (now 32 and 8 MB) in one lock each, and draws inside them don't copy or lock. Heaviest view: frame 9.7-9.9 ms against 10.6-11.0 (eyes 7.0 + 1.4 against 7.6 + 2.0; the hand-off to SteamVR waits 0.3 ms longer for the GPU). SteamVR's reprojected count went up there (290 against 200 per 5 s): both are over 8.3 ms, and near the budget that count doesn't follow the frame time. At 90 Hz the frame now fits with room.

- **Adaptive detail distance** (`[stereo] adaptive_detail`, on): each frame's own work (from SteamVR's go to the frame's end, plus the game's work outside the 3D pass), smoothed, against the headset's frame time less 0.5 ms; over it the detail distance eases down 0.4% of the setting a frame (never below the game's own, 100%), with 1.2 ms to spare it eases back. Heaviest view: it settles at 100%, frame 8.3 ms, reprojected 2-85 per 5 s against 213-248; light views stay at 150%. The 5-second line says `detail N% (its work X ms a frame)`. The cost: in busy views, things go to plainer meshes and fade at the game's own distances.

## Options left
- **Fewer rows per eye.** At `eye_height` 1600 (the panel's own rows; now 1865), the readback is 0.2–0.3 ms less, the steady stretches have about 0.45 ms of slack, and the longest frame is about 9.5 ms. The picture is a little softer (1865 was the last 4% of sharpness the 2048 px limit allows).
- **90 Hz in SteamVR** (the Index's default): 11.1 ms a frame, which the current frame fits with room for the occasional slow one.
- **The zero-copy path is done** (above). Still possible on it: a larger render target than 2048 px (Direct3D 9 allows more), for sharper far detail.
- **More headroom in heavy views:** a filter in dxwrapper for states set to the value they already have (it sets z-enable, the viewport and transforms on every draw; it sets some states directly, so the filter must be invalidated there); fewer, merged draws; the draw distance (250% costs about 0.2 ms an eye in the heaviest view).
- **Drawing both eyes from one pass of the engine** (the engine's work done once, its pre-transformed vertices re-projected for the second eye): the largest saving, about 1.5–2 ms, and the largest change.
- **Engine-side savings in the eye passes** (the second eye repeats all the engine's CPU work: landscape tessellation, object setup) would need deep changes to the engine's drawing.
