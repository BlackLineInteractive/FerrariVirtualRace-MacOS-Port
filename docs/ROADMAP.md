# Roadmap

## Done

- Static recompilation of `FerrariVR_Hi.exe` to native `x86_64` with a Win32 / DirectX runtime on SDL3 and OpenGL 4.1
- Races on Ultra settings: shader model 2/3 translation, hardware shadow maps, instancing, HDR pipeline
- Anti-aliasing menu: FXAA, MSAA 2x / 4x / 8x (with alpha-to-coverage), FSR 1
- Temporal AA and temporal upscaling (1.5x, 2x) in the style of FSR 2, with per-object motion vectors rebuilt from the previous frame's shader constants
- Screen-space reflections on the track surface (optional, `options.txt`)
- Resolutions up to 4K and the native Retina size
- Device re-creation after changing video settings
- `FerrariVR.app` bundle with SDL3 inside, game-folder picker, log file

## Next: engine and rendering

- **Fewer draw calls.** A race frame issues about 12,000 draws: six faces of the car's reflection cube map (roughly 1,000 draws each), four shadow cascades and the scene. The OpenGL driver's per-draw cost, not the GPU, sets the frame rate. The game's own *Update reflections* option already lowers how often the cube is redrawn; spreading the six faces over several frames in the port (one or two per frame) would remove about half of the draws at the highest setting.
- **Metal backend.** The OpenGL driver on macOS is deprecated and single-queue. A Metal renderer behind the same D3D9 layer is the long-term fix for stable frame times.
- **Faster temporal pass.** It costs about 8-10 ms at 4K output on a Radeon Pro 5500M, 4 ms of which is writing two 4K half-float targets; a compute-free GL 4.1 pipeline limits what can be fused.
- **Better foliage under MSAA.** Grass is drawn alpha-blended, so neither MSAA nor alpha-to-coverage smooths it; the temporal modes do.
- **Input.** Check force feedback wheels and gamepad mappings on SDL3.
- **Windows exception handling (SEH)** stubs for rare error paths.

### FSR 3 frame generation, FSR 4

- FSR 3's upscaler works like FSR 2 and is covered by the `Temporal` modes. Its frame generation (an interpolated frame between every two real ones, from the same motion vectors) is possible on top of them but needs the HUD separated from the 3D image and paced presentation; at the game's 30-60 fps it would add visible latency and artefacts.
- FSR 4 is a machine-learning upscaler for RDNA 4 GPUs on DirectX 12 / Vulkan. It cannot run on a Radeon Pro 5500M (RDNA 1) or under OpenGL / Metal on macOS.

### Reflections

The screen-space reflections guess the material from the surface orientation and colour. Real per-material data (a reflectivity value written by the game's shaders, which would need the material constants identified per shader) would allow wet-track and car-paint reflections without touching grass.

## Content

The game ships three cars (`Autos/F430scuderia`, `Autos/F599_GTB`, `Autos/F612`) and one track (`Scenarios/Mugello`), in Synetic engine formats (`.MOX` meshes, `.MTL` materials, `.CPO` collision).

1. **Format tools.** Document and write converters for `.MOX` / `.MTL` / `.CPO` and the track files (to and from glTF / Blender). This is the base for everything below.
2. **More Ferraris.** Find where the game lists its cars (car files, front-end tables, physics data) so additional models can be added, starting with existing assets converted into the Synetic format.
3. **Higher track detail.** Higher-resolution textures and denser vegetation for Mugello through the same file overlay the port already uses for saves, so the original files stay untouched.
4. **New tracks.** Needs the full track format (geometry, AI racing line, cameras, collision) and is the largest item.

Only original or properly licensed assets can go into this repository. Game files and anything derived from them stay on the player's machine.
