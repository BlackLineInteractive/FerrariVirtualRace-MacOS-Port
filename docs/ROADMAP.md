# Roadmap

## Done

- Static recompilation of `FerrariVR_Hi.exe` to native `x86_64` with a Win32 / DirectX runtime on SDL3 and OpenGL 4.1
- Races on Ultra settings: shader model 2/3 translation, hardware shadow maps, instancing, HDR pipeline
- Anti-aliasing menu: FXAA, MSAA 2x / 4x / 8x (with alpha-to-coverage), FSR 1
- Temporal AA and temporal upscaling (1.5x, 2x) in the style of FSR 2, with per-object motion vectors rebuilt from the previous frame's shader constants
- Screen-space reflections on the track surface and ground-truth ambient occlusion (GTAO), both optional in `options.txt`
- Options to turn off the game's own cube-map reflections (half of the draw calls) and its baked track lighting
- Ray-tracing scene: a tinybvh CWBVH per mesh, the remembered track merged into one CWBVH in the background, a TLAS per frame, traced by Metal compute next to OpenGL (IOSurface), checked against the game's frame
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

### Ray-traced reflections and glass (Whitted ray tracing)

Mirror-like reflections on the car paint and real refraction and reflection through the windows need rays traced against the actual scene, not the screen. OpenGL 4.1 on macOS has neither compute shaders nor ray-tracing support, so this comes after the Metal backend:

Status:
- **Scene.** Captured from the scene pass with exact world matrices (`WorldMtx`, or the track's `WorldOffset`); a CWBVH per mesh, built once and cached. The track is remembered after it leaves the view, so reflections can show what is behind the camera, and merged in a background thread into one world-space CWBVH (about 500,000 triangles, 0.2-0.3 s per rebuild); the TLAS holds that, the cars and anything else drawn this frame (about 150-300 instances, 0.4 ms on the CPU).
- **GPU.** Metal compute on the Radeon Pro 5500M next to OpenGL, sharing an RGBA16F image through an IOSurface, with a CWBVH / TLAS traversal ported from an existing Metal path tracer. Primary rays at 960x540 take a median of 2-4 ms (`FVR_RT_VIEW`).
- **Not captured yet:** instanced trees, camera-facing tree billboards (their centre is `POSITION1`), grass, the sky, debris, and the alpha masks of fences.
- **Materials.** `pbr=1` converts the car material to GGX / Schlick with clear coat and Fresnel glass (runtime/pbr.cpp).
- **Rays only where they pay off.** Whitted rays suit mirror-like surfaces only: the PBR shader marks pixels with roughness below about 0.2 and a reflection (clear coat, glass, chrome, later the wet track), and Metal traces just those, a fraction of the frame; rough surfaces keep the cube map, SSR or ambient light.
- **Next:** reflection rays from the game's depth and normals for the car paint (mask from the car shaders) and the track, with materials (diffuse texture at the hit point), composited before the temporal pass; then glass.

1. **Scene capture.** The port already sees every draw: vertex and index buffers, the vertex declaration and, per draw, the world matrix (`WorldMtx` c0) and the previous frame's match used for motion vectors. Each distinct mesh becomes a bottom-level acceleration structure (built once, refitted for the cars' moving parts) and each frame's draws an instance list for the top level.
2. **Materials.** Rays need albedo, normals and textures at the hit point: the diffuse texture bound to sampler 0 and the material constants of each draw, recorded together with the instance.
3. **Tracing.** Metal's ray-tracing API (`MTLAccelerationStructure`, intersection functions) runs on the Radeon Pro 5500M in compute, without dedicated ray-tracing hardware. A budget of one reflection ray per pixel at half resolution, plus one refraction and one reflection ray for glass, denoised by the temporal pass, is the realistic target; car paint first (replacing the game's 256x256 cube map), then glass, then the wet track.
4. **Fallback.** Screen-space reflections stay for GPUs and settings where tracing is too slow.

Real per-material data would also make the screen-space reflections exact (today they guess the material from the surface orientation and colour).

## FVR 2.0

Everything below goes beyond porting: it changes what the game contains or how it looks. Each item needs the format tools from the Content section first, or the Metal backend, or both.

- **Rendering**
  - New lighting: physically based sun and sky (time of day), image-based ambient light from a captured or rendered sky, proper exposure.
  - PBR renderer: metal / roughness materials for the cars (paint with clear coat and metallic flakes, carbon, rubber, chrome, glass) and the track (asphalt, kerbs, grass, concrete), with the original textures converted as a starting point.
  - Weather: rain with a wet track (puddles, spray from the tyres, rain on the windscreen, darker and more reflective asphalt), fog and volumetric fog with light shafts.
  - Ray-traced reflections and glass (above), shadows with soft penumbrae, higher-resolution track textures and denser vegetation.
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
