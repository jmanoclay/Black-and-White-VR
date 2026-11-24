# Render pass map (Phase 0)

How Black & White v1.20 + Fan Patch draws a frame, measured with the logging proxy on 2026-10-05.
Scene: the creature pen at the start of the user's save, 1920×1080 windowed, max detail.
Tools: `tools/run_test.ps1 -Trace N`, then `tools/analysis/trace_report.py <trace_*.txt>`.

## Headline findings

1. **Every draw is pre-transformed (`XYZRHW`), with hardware T&L on or off.** The engine projects
   all geometry itself on the CPU and calls `IDirect3DDevice7::DrawIndexedPrimitive` with
   screen-space vertices. `SetTransform` runs once per frame and isn't used for geometry.
   Consequences:
   - Stereo **must be engine-level**: run the engine's projection once per eye with that eye's
     camera. That's the plan's primary approach; this rules out the "D3D view-matrix swap" shortcut.
   - API-level fallback remains possible in principle: each vertex carries screen x/y, z and
     1/w, so with the engine's projection known, vertices could be un-projected and re-projected
     per eye. That's kept as Plan B only.
2. **Almost everything funnels through `LH3DRender::DrawTriangle` (`0x82F810`).** It's a single
   choke point where we can tag, count or redirect draws by caller.
3. **2D UI is separable by call site.** Menus come from `SetupThing::Draw*` → `DrawAndClip2D` /
   `DrawAndClip`; text comes from `GatheringText::DrawTextRaw` → `LH3DTech::Draw3DScreenTriangle`. These
   can be routed to a floating VR panel instead of being drawn per eye.
4. **The engine caches D3D method pointers.** Patching the device vtable misses the draw calls;
   the tracer detours the functions inside `d3dim700.dll` instead.
5. **The game logic runs at 10 turns/s** (`GGame::ProcessOneGameTurn`), and 0/s while the in-game
   menu is open. Rendering runs at about 490 fps uncapped. Phase 1 must check whether object
   motion is interpolated between turns; if not, moving objects would update at 10 Hz in VR.
6. **Render cost:** `GGame::ProcessGraphicsEngine` averages **1.9 ms** (max 4.2 ms) per frame at
   1080p in this scene. `GGame::Draw` itself is negligible. Two eyes at Index resolution should
   fit the 11.1 ms budget at 90 Hz. Busy scenes (villages, miracles) still need measuring.

## Draw calls in one gameplay frame (163 draws)

| Draws | Indices | Call site (innermost first) | Likely content |
|---:|---:|---|---|
| 122 | 3195 | `LH3DRender::DrawTriangle` < `fn_00856D40` < `fn_0080D910` | meshes (trees, creatures, objects) |
| 23 | 474 | `LH3DRender::DrawTriangle` < `LH3DTech::Draw3DWorldTriangle` < `fn_00847060` | world-space effects/sprites |
| 6 | 369 | `LH3DRender::DrawTriangle` < `fn_0084BA90` < `fn_00867400` | (to identify) |
| 5 | 600 | `fn_0082A830` < `fn_0082F920` < `fn_00852C40` | multi-texture (T2) pass, likely landscape detail |
| 4 | 2430 | `LH3DRender::DrawTriangle` < `fn_0084C3C0` < `fn_00813BA0` | large batches, likely landscape |
| 2 | 66 | `LH3DRender::DrawTriangle` < `fn_0084BA90` < `fn_00812170` | (to identify) |
| 1 | 177 | `LH3DRender::DrawTriangle` < `fn_0085AB10` < `fn_005167D0` | (to identify) |

Vertex formats: `XYZRHW,D,S,T1` ×158 and `XYZRHW,D,S,T2` ×5. The `fn_…` names are functions the
decomp hasn't named yet. Phase 1 identifies them by toggling each one and comparing screenshots.

With the menu open, these extra draws appear: `SetupThing::DrawLine/DrawBox/DrawTab/DrawBevBox/DrawBg`
(menu chrome) and `GatheringText::DrawTextRaw` (text).

## Engine camera and projection (measured in Phase 1)
- `LH3DTech::g_camera` (`0xEA1DB8`) holds `pos`, `foc`, `fov` (horizontal, radians; 70° by default) and `look_at`.
- `g_world_to_camera` (`0xEA1D28`) is a row-vector 4×3: camera = p·M3 + row 3. Camera space is left-handed: +X right, +Y up, +Z forward. World +Y is up. The game camera has no roll.
- Projection: `sx = w/2 + f·X/Z`, `sy = h/2 − f·Y/Z`, `f = (w/2)/tan(fov/2)`, square pixels.
- `g_world_to_clipping` (`0xEA9E40`) is the same matrix with the X column scaled by 1/tan(fov/2) and the Y column by aspect/tan(fov/2). The decomp's `InfoTransform` field names for these two scales are swapped.
- Vertex depth: `z = 1 − 1/Z`, `rhw = 1/Z`. The 16-bit z-buffer can't be read back (Lock returns the clear value).
- `UpdateViewPort(w, h)` (fastcall) sets the resolution, centre and clip bounds (`g_MaxScreen` = w−1, h−1) and resets the FOV to 70°. `ChangeFov(fov)` (stdcall) sets the projection scales and the culling cone.
- `LH3DRender::StartFrame` measures real time into `g_delta_time`. It runs once per `Process3dEngine` call.

## Frame structure
- Frame boundary: `LHScreen::Flip` (`0x7DE090`), called once per presented frame.
- Order per frame: `SetTransform(VIEW)` once → `BeginScene` → state/texture changes and draws → `EndScene` → Flip.
- No `Clear` during gameplay frames (the sky/landscape covers the screen).

## Input path (for the harness and later the controllers)
- Cursor position: `MouseInputThreadProc` polls `GetCursorPosClient` (`0x7DBEB0`) →
  `MouseMoveHandler` → `LHMouse::UpdateCurrentPos`. Hooked for scripted input.
- Buttons and keys: window messages → `GameWindowProc` → `LHMouse::SetButtons`.
- DirectInput supplies only mouse deltas and the wheel (`LHMouse::UpdateDeltaPos`), used by camera controllers.

## Startup quirk fixed in the mod
Without admin rights, DSETUP's `DirectXSetupGetVersion` can't read `HKLM\Software\Microsoft\DirectX`
(it asks for `KEY_ALL_ACCESS`), and its fallback probe reports DirectX 6. The game then refuses to
start ("You need DirectX 7 or greater"); this happens with or without the mod. The mod hooks the import thunk
(`0x8A04F0`) and reports the registry's 4.09.00.0904.

## Graphics backend decision (step 0.5)
Phase 1 (offline stereo with the MockBackend) needs only CPU readback of the render target, which
works on native D3D7 today: the screenshot path already does this. So the native ddraw backend is
used for Phase 1. The zero-copy GPU path to SteamVR (D3D7 → D3D9Ex via dxwrapper, or a custom
D3D7 → D3D11 layer) is only needed for Phase 2. The wrapper comparison is deferred to the start of
Phase 2, when it can be judged on real per-eye render targets instead of guesses.
