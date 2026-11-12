# Roadmap

## Done

- Static recompilation of `FerrariVR_Hi.exe` to native `x86_64` with a Win32 / DirectX runtime on SDL3 and OpenGL 4.1
- Races on Ultra settings: shader model 2/3 translation, hardware shadow maps, instancing, HDR pipeline
- Anti-aliasing menu: FXAA, MSAA 2x / 4x / 8x (with alpha-to-coverage), FSR 1
- Resolutions up to 4K and the native Retina size
- Device re-creation after changing video settings
- `FerrariVR.app` bundle with SDL3 inside, game-folder picker, log file

## Next: engine and rendering

- **Metal backend.** The remaining 25-35 ms frames come from the OpenGL driver on macOS (deprecated, single queue). A Metal renderer behind the same D3D9 layer is the real fix for stable frame times.
- **Better foliage under MSAA.** Grass is drawn alpha-blended, so neither MSAA nor alpha-to-coverage smooths it; a per-material switch to alpha-to-coverage would.
- **Input.** Check force feedback wheels and gamepad mappings on SDL3.
- **Windows exception handling (SEH)** stubs for rare error paths.

### FSR 2 / FSR 3 / FSR 4

These are temporal upscalers. Each frame they need a per-pixel motion-vector buffer and a sub-pixel camera jitter applied to the projection, both produced by the game's renderer. Ferrari Virtual Race (2009) produces neither.

- Motion vectors could be rebuilt for the static world from the depth buffer and the previous and current camera matrices, which would have to be found among the vertex shader constants. Moving cars, wheels and the rear-view mirror would still smear or ghost. This is a research project, not a menu entry.
- FSR 4 is a machine-learning upscaler that needs RDNA 4 hardware and DirectX 12 / Vulkan. It cannot run on a Radeon Pro 5500M (RDNA 1) or under OpenGL / Metal on macOS.
- Upscaling also only pays off when the game renders below the screen resolution. On a Retina Mac the better option is to render natively (2K / 4K are now in the menu) with MSAA, or to use FSR 1, which is already available.

### Screen-space reflections

SSR needs the scene's depth **and surface normals** (plus a roughness or reflectivity mask) before tone mapping. The game renders forward into an HDR colour target with depth only. Normals could be estimated from depth gradients and the pass inserted between the scene and the game's own tone mapping (the place where the MSAA resolve happens). Without a material mask it would also reflect on grass and kerbs, so it needs per-material information first. Possible later, together with the Metal backend.

## Content

The game ships three cars (`Autos/F430scuderia`, `Autos/F599_GTB`, `Autos/F612`) and one track (`Scenarios/Mugello`), in Synetic engine formats (`.MOX` meshes, `.MTL` materials, `.CPO` collision).

1. **Format tools.** Document and write converters for `.MOX` / `.MTL` / `.CPO` and the track files (to and from glTF / Blender). This is the base for everything below.
2. **More Ferraris.** Find where the game lists its cars (car files, front-end tables, physics data) so additional models can be added, starting with existing assets converted into the Synetic format.
3. **Higher track detail.** Higher-resolution textures and denser vegetation for Mugello through the same file overlay the port already uses for saves, so the original files stay untouched.
4. **New tracks.** Needs the full track format (geometry, AI racing line, cameras, collision) and is the largest item.

Only original or properly licensed assets can go into this repository. Game files and anything derived from them stay on the player's machine.
