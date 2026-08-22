# Android VirtualCam Manager

**Pure Magisk module + single controller APK**  
**NO LSPosed / NO Xposed manager.**

> **v2.1.0:** Camera1 surface swap (original VCAM algorithm) + MediaPlayer on the app preview surface, with GL/OES as fallback. Still needs a check on a real rooted phone.

## Status (v2.1.0)

| Layer | Status |
|-------|--------|
| Magisk paths + flags | **Working** |
| APK one-tap enable / import | **Working** |
| Dual video path (Camera1 + `/data/adb/virtualcam`) | **Working** |
| Zygisk `.so` multi-ABI + **16 KB page size** | **CI builds** |
| Camera1 `setPreviewTexture` / `setPreviewDisplay` swap | **New in 2.1.0** — device test required |
| MediaPlayer loop on stolen surface | **New in 2.1.0** — device test required |
| ShadowHook staged into app `code_cache` | **New in 2.1.0** |
| Status files mirrored to `DCIM/Camera1/.vcam_*` | **New in 2.1.0** |
| ShadowHook `glBindTexture` + draw | Fallback |
| AMediaCodec + OES / 2D upload | Fallback |
| Camera2 session surface rewrite | **Not done** |
| Real apps show virtual feed on device | **Not verified** |

## Download

[Actions ← latest green run ← Artifacts](https://github.com/smithluke874/Android-VirtualCam-Manager/actions)

- `VirtualCam-Manager-debug` / `release`
- `VirtualCam-Manager-Magisk-v2.1.0`

## Install (3 steps)

1. **Magisk** → Settings → **Zygisk ON** → Modules → Install from storage → flash the Magisk zip → **reboot**
2. Install the **Manager APK** → open it → **allow root** when Magisk asks
3. On **Home**:
   - Tap **Pick video or image**
   - Flip **VirtualCam ON**
   - Open a **Camera1** app first (Open Camera, many stock cameras)
   - Watch status: `texture_swapped` → `surface_playing`

## What to expect

v2.1 prefers the original method:

1. App calls `Camera.setPreviewTexture(appTexture)`
2. Module gives the real camera a **dummy** SurfaceTexture
3. `MediaPlayer` loops `virtual.mp4` onto the **app** surface

Camera2 / CameraX apps that never touch `android.hardware.Camera` will still show the real sensor until the Camera2 session path lands.

## Hook / path status values

| Status | Meaning |
|--------|--------|
| `gate_off` / `no_video` | Control plane closed |
| `hooks_installing` / `hooks_ready` | Both JNI + GL attempted |
| `jni_hooks_ready` / `jni_camera_patched` | RegisterNatives intercept is in |
| `texture_swapped` / `display_swapped` | Dummy surface given to Camera1 |
| `surface_playing` | MediaPlayer is on the app surface |
| `surface_player_fail` | Swap happened, playback failed |
| `gl_bind_redir:tex#N` | GL bind redirect hit |
| `oes_ready` / `oes_fallback_2d` | GL upload path |

## Next

- Device smoke test on latest Magisk/Zygisk (Open Camera first)
- Camera2 output surface rewrite
- GitHub Release so artifacts are not Actions-only
