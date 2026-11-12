# Roadmap

## Done

- Static recompilation of `FerrariVR_Hi.exe` to native `x86_64` with a Win32 / DirectX runtime on SDL3 and OpenGL 4.1
- Races on Ultra settings: shader model 2/3 translation, hardware shadow maps, instancing, HDR pipeline
- Anti-aliasing menu: FXAA, MSAA 2x / 4x / 8x (with alpha-to-coverage), FSR 1
- Temporal AA and temporal upscaling (1.5x, 2x) in the style of FSR 2, with per-object motion vectors rebuilt from the previous frame's shader constants
- Screen-space reflections on the track surface and ground-truth ambient occlusion (GTAO), both optional in `options.txt`
- Options to turn off the game's own cube-map reflections (half of the draw calls) and its baked track lighting
- `options.txt` written with every setting and its explanation on the first launch
- Ray tracing, PBR materials, ambient occlusion and screen reflections as entries of the game's own *Video settings* menu (added through the menu's own functions), taking effect at once; RT / AO / SSR also without the temporal modes
- Physically based car materials from the game's own material data (`pbr`)
- Ray-traced reflections (`rt`): Metal compute next to OpenGL, rays only on mirror-like pixels (clear coat, glass, chrome), hits shaded with textures, the sun with shadow rays and the game's ambient light; glass refraction for models that have glass thickness (`rt_refract`)
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

### Ray tracing

Ray-traced reflections are in (see the README). How it is built, and what is left:

**Done**
- **Scene.** Captured from the scene pass with exact world matrices (`WorldMtx`, or the track's `WorldOffset`); a tinybvh CWBVH per mesh, built once and cached, large deformed meshes (a crashed car body) rebuilt in the background. The track is remembered after it leaves the view and merged in a background thread into one world-space CWBVH (about 540,000 triangles, 0.2-0.3 s per rebuild); the per-frame TLAS holds that, the cars and anything else drawn this frame (about 150-300 instances, 0.5 ms on the CPU).
- **GPU.** Metal compute on the Radeon Pro 5500M next to OpenGL, images shared through IOSurfaces, the CWBVH / TLAS walk ported from an existing Metal path tracer; kernels compiled in the background.
- **Materials.** The car material converted to GGX / Schlick with clear coat and Fresnel glass (`pbr`).
- **Rays only where they pay off.** Whitted rays suit mirror-like surfaces only: the PBR shader marks pixels with GGX alpha up to 0.15 (clear coat, glass, chrome) and Metal traces just those, 20,000-40,000 rays per frame at 960x540.
- **Hit shading.** The frame's own colour where the hit is visible, otherwise the texture at the hit (215 textures, 8 MB as copies of at most 128x128), the sun with a shadow ray, the track's baked light or the cars' hemisphere light; 1-2.5 ms in Metal.
- **Glass.** Reflections with Fresnel; refraction traced too, but the shipped cars' glass, lamp covers included, is single sheets without thickness (measured: about 6,400 glass pixels per frame, none with a second surface), so it bends nothing. It is ready for car models with real glass.

**Next**
- **Asynchronous hand-over between OpenGL and Metal.** Today the frame waits for OpenGL to finish the scene and then for Metal, so the CPU and GPU stop overlapping: a race drops from about 100 to 55-60 fps although the rays take 1-2.5 ms. Double-buffered masks and results with one frame of latency (fences instead of waits) would recover most of it; the Metal renderer removes the hand-over altogether.
- **Wet track**, together with the rain: a material table for the track's textures (asphalt, kerbs, paint, grass), then mirror-like puddles and wet asphalt through the same mask.
- **More of the scene in the BVH:** instanced trees, the camera-facing tree billboards (their centre is `POSITION1`), grass, debris, the alpha masks of fences, and fog on distant hits.
- **Track windows** (the game's fixed 64x64 environment map today) through the same path.

## FVR 2.0

Everything below goes beyond porting: it changes what the game contains or how it looks. Each item needs the format tools from the Content section first, or the Metal backend, or both.

- **Rendering**
  - New lighting: physically based sun and sky (time of day), image-based ambient light from a captured or rendered sky, proper exposure.
  - PBR renderer: metal / roughness materials for the cars (paint with clear coat and metallic flakes, carbon, rubber, chrome, glass) and the track (asphalt, kerbs, grass, concrete), with the original textures converted as a starting point.
  - Weather: rain with a wet track (puddles, spray from the tyres, rain on the windscreen, darker and more reflective asphalt), fog and volumetric fog with light shafts.
  - More ray tracing: the wet track, soft ray-traced shadows, refraction on new cars with real glass; higher-resolution track textures and denser vegetation.
- **Content**
  - A larger Ferrari line-up across eras (each with its model, interior, sound and physics data), starting with the tools to import a car into the Synetic formats.
  - New tracks, which need the full track format: geometry, AI racing line, cameras, collision, pit lane.
- **Game**
  - Multiplayer: the race state is driven by the recompiled game code, so the first step is finding the simulation tick and car state to synchronise; local split-screen is a simpler first target than online play.
  - Wheel and gamepad support with force feedback and per-device mappings.

Original assets only: anything derived from the game's files stays on the player's machine, and new cars and tracks must be original or properly licensed.

## Content

The game ships three cars (`Autos/F430scuderia`, `Autos/F599_GTB`, `Autos/F612`) and one track (`Scenarios/Mugello`), in Synetic engine formats (`.MOX` meshes, `.MTL` materials, `.CPO` collision).

1. **Format tools.** Document and write converters for `.MOX` / `.MTL` / `.CPO` and the track files (to and from glTF / Blender). This is the base for everything below.
2. **More Ferraris.** Find where the game lists its cars (car files, front-end tables, physics data) so additional models can be added, starting with existing assets converted into the Synetic format.
3. **Higher track detail.** Higher-resolution textures and denser vegetation for Mugello through the same file overlay the port already uses for saves, so the original files stay untouched.
4. **New tracks.** Needs the full track format (geometry, AI racing line, cameras, collision) and is the largest item.

Only original or properly licensed assets can go into this repository. Game files and anything derived from them stay on the player's machine.
