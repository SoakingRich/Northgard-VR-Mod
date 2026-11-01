# openxr.hdll

Phase 0b (current): exports `xr_smoke`, called once per frame, which logs to `NorthgardVR_log.txt` (active OpenXR runtime + FPS).

Phase 1 onward:

A C++ HashLink native library, loaded automatically when `vrboot.dat` references
`openxr@...` natives.

Planned natives (HL signatures are in progress):

| Native | Purpose |
|---|---|
| `xr_init(dx_resource) : bool` | get the `ID3D11Device` via `GetDevice()`, create instance/session/swapchains |
| `xr_frame_begin() : bool` | `xrWaitFrame` + `xrBeginFrame` + `xrLocateViews`; returns shouldRender |
| `xr_eye_count() : i32` | 2 |
| `xr_eye_begin(i32 eye) : void` | marks the active eye for `xr_camera_override` |
| `xr_camera_override(mcam, mproj, m) : void` | writes the eye view/proj into Heaps matrices (no-op when no eye is active) |
| `xr_eye_submit(i32 eye, dx_resource tex) : void` | copy the Heaps eye target → swapchain image |
| `xr_frame_end(dx_resource uiTex) : void` | projection layer + UI quad layer, `xrEndFrame` |
| `xr_poll_input(...)` | controller poses/buttons for the diorama and pointer |

Build: CMake + MSVC, linking the Khronos OpenXR loader (vcpkg `openxr-loader` or the
OpenXR-SDK release). Built against the HashLink headers (`hl.h`) for the HL version
matching the game's `libhl.dll`.
