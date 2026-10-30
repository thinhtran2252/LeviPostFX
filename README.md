### LeviPostFX

**A VHS-inspired camera and post-processing mod for Minecraft Bedrock on Android.**

LeviPostFX adds a configurable camera layer to the game through LeviLauncher and the Android Preloader SDK. It is built around a lightweight full-screen OpenGL ES 3 post-processing pass, with controls exposed directly in the in-game Levi Mod Menu.

The look combines analog-camera character with optional image effects: lens distortion, RGB channel separation, scanlines, signal noise, a camera-centred flashlight overlay, and glow around bright pixels. Effects can be mixed or disabled individually instead of forcing one fixed preset.

> **Important:** LeviPostFX currently processes the final framebuffer at `eglSwapBuffers`. That framebuffer contains both the world and the game UI. The effects can therefore affect menus and HUD elements too. The flashlight is a screen-space visual overlay, not a real Minecraft light source, and it does not detect held light-emitting items.

## Features

| Feature | What it does |
| --- | --- |
| Screen Distortion | Adds adjustable lens-style warping. |
| Distortion Zoom | Zooms the sampled image in or out while distortion is enabled. |
| Chromatic Aberration | Separates red and blue channels toward the edge of the frame. |
| Scanlines | Adds subtle horizontal camera/display lines. |
| Desaturation | Reduces the image's colour intensity. |
| Shadow Darkness | Pushes very dark areas much closer to black while keeping lit areas readable. |
| Luma + Chroma Noise | Recreates the example shader's animated analog signal noise in YUV colour space, with separate brightness and colour controls. |
| Flashlight | Adds a warm, camera-centred beam with adjustable intensity and on-screen range. It is always active by default and can be switched off in the Mod Menu. |
| Bloom | Samples bright pixels in eight surrounding directions and adds a soft glow controlled by intensity and threshold. |
| 4:3 Ratio | Places a centred 4:3 image inside the display without stretching it. |
| Dev Tools Overlay | Displays recent mod log messages in-game for debugging. |

## Levi Mod Menu settings

Open the **Levi Mod Menu** in-game and select **VHS Shader**. Changes are saved to the mod configuration.

### Camera and image

| Setting | Default | Range / options | Description |
| --- | ---: | --- | --- |
| VHS Effects | On | On / Off | Master switch for the post-processing pass. |
| Screen Distortion | On | On / Off | Enables lens distortion. |
| Distortion Strength | 0.035 | 0.000–0.080 | Amount of lens warping. |
| Distortion Zoom | 0.000 | -1.000–1.000 | Negative zooms out; positive zooms in. |
| Chromatic Aberration | On | On / Off | Enables RGB channel separation. |
| Chromatic Aberration Intensity | 1.000 | 0.000–3.000 | Amount of channel separation. |
| Scanlines | On | On / Off | Enables scanlines. |
| Scanline Intensity | 0.035 | 0.000–0.080 | Scanline contrast. |
| Desaturation | Off | On / Off | Enables colour desaturation. |
| Desaturation Intensity | 0.150 | 0.000–1.000 | Amount of desaturation. |
| Shadow Darkness | 0.850 | 0.000–1.000 | Strength of the deep-shadow response. Set to 0 to disable it. |
| 4:3 Ratio | Off | On / Off | Adds a centred 4:3 presentation with black bars. |

### Noise

| Setting | Default | Range / options | Description |
| --- | ---: | --- | --- |
| Luma Noise | On | On / Off | Adds animated noise to image brightness. |
| Luma Noise Strength | 0.050 | 0.000–1.000 | Brightness-channel noise amount. |
| Chroma Noise | On | On / Off | Adds animated noise to image colour. |
| Chroma Noise Strength | 0.200 | 0.000–1.000 | Colour-channel noise amount. |

Luma Noise and Chroma Noise are the two retained analog-noise layers. They follow the supplied example's RGB-to-YUV signal-noise approach. Since this post-process stage does not have access to the example shader's `noisetex` sampler, the port generates independent animated procedural noise channels instead. The separate shadow-only noise effect has been removed.

### Flashlight and bloom

| Setting | Default | Range / options | Description |
| --- | ---: | --- | --- |
| Flashlight | On | On / Off | Enables the camera-centred screen-space beam. Turn this off to disable it. |
| Flashlight Intensity | 3.000 | 3.000–10.000 | Beam brightness. |
| Flashlight Range | 1.000 | 0.500–2.000 | On-screen beam width. Higher values widen the beam. |
| Bloom | On | On / Off | Enables glow around bright image areas. |
| Bloom Intensity | 0.500 | 0.000–1.000 | Glow strength. |
| Bloom Threshold | 0.350 | 0.000–1.000 | Minimum brightness used to select bloom highlights. Lower values include more of the image. |

The flashlight is intentionally implemented as an image-space effect because the current `eglSwapBuffers` hook only receives the completed frame. It does not have Minecraft's held-item light value, world position, depth, or surface normals. As a result, the beam follows the centre of the view and cannot cast physically correct shadows or illuminate world geometry like a real game light.

### Debugging

| Setting | Default | Description |
| --- | --- | --- |
| Dev Tools Overlay | Off | Shows recent VHS Shader log lines on screen. |

## Installation

1. Build the `.levipack` package using the instructions below.
2. Import the generated package into **LeviLauncher**.
3. Enable **VHS Shader** in the launcher/mod controls.
4. Start Minecraft and open the Levi Mod Menu to adjust effects.

## Build from source

### Requirements

- Android SDK
- Android NDK 28.2.13676358, or a compatible NDK version
- CMake 3.22 or newer
- Ninja
- PowerShell 7+ (Windows PowerShell may also work)

The CMake project fetches the pinned `preloader-android` SDK dependency automatically.

### Build for arm64-v8a

Set `ANDROID_HOME` to your Android SDK directory, then run from the repository root:

```powershell
$env:ANDROID_HOME = "C:/Users/<you>/AppData/Local/Android/Sdk"
./scripts/package.ps1 -Abi arm64-v8a
```

Build both supported Android ABIs:

```powershell
./scripts/package.ps1 -Abi all
```

The script generates the typed config files, compiles the native library, and creates a `.levipack` package in the selected build directory. Import that package into LeviLauncher.

## Configuration files

The C++ defaults and schema live in `src/mod/Config.h`. The in-game settings are registered in `src/mod/DevTools.h`, and runtime uniforms plus the GLES post-processing shader are implemented in `src/mod/MyMod.cpp`.

The config version is currently **6**. Existing version 1 through 5 settings are migrated when the mod loads. The shadow-only noise setting has been retired; Luma Noise and Chroma Noise remain independent controls. Shadow Darkness adds a configurable deep-shadow response, and Bloom intensity is adjustable from 0.0 to 1.0, while Flashlight Intensity ranges from 3.0 to 10.0.

## Known limitations

- The current render hook runs at `eglSwapBuffers`, after the world and UI have been composited. UI/HUD exclusion is not available at this hook.
- The flashlight is an image-space beam approximation. It is not tied to held items, does not use world geometry, and does not produce real shadows or occlusion.
- Bloom uses an eight-direction screen-space sampling kernel. Its cost depends on the device's GPU and render resolution.
- The new signal noise uses procedural noise rather than the example shader's external noise texture.

## Project layout

```text
.
├── CMakeLists.txt
├── manifest.json.in
├── README.md
├── scripts/
│   └── package.ps1
└── src/
    ├── config_generator.cpp
    ├── main.cpp
    └── mod/
        ├── Config.h
        ├── Config.cpp
        ├── DevTools.h
        ├── MyMod.cpp
        └── MyMod.h
```

## License

See the repository's license file, if present, for the terms that apply to this project and its dependencies.
