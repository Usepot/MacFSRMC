# MacFSRMC

MacFSRMC integrates AMD FidelityFX Super Resolution 2.2.1 into the experimental Vulkan renderer shipped with Minecraft Java Edition 26.2. It is a Fabric client mod for Java 25+.

The implementation uses AMD's real FSR 2 host API and Vulkan backend. The world is rendered into a reduced-resolution target, a depth-reprojection compute pass produces camera motion vectors and a conservative reactive mask, and FSR 2 reconstructs the result into a native-resolution Vulkan image. The hand, screen effects, post-upscale passes, and GUI are then rendered at native resolution.

## Requirements

- Minecraft Java Edition 26.2
- Fabric Loader 0.19.3 or newer
- Fabric API 0.154.2+26.2
- Java 25 or newer
- Minecraft's **Prefer Vulkan (Experimental)** graphics setting
- A Vulkan 1.2 driver with the capabilities required by Minecraft and FSR 2

Minecraft 26.2 uses MoltenVK on macOS, so the same Vulkan integration is used on Apple hardware. The native bridge must be built on each target OS; Windows x86-64 is built automatically in this checkout, while macOS and Linux use the included CMake build.

## Build

Windows (Visual Studio C++ Build Tools required):

```powershell
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-26'
.\gradlew.bat build
```

macOS/Linux (CMake, Clang/GCC, and a JDK with JNI headers required):

```bash
export JAVA_HOME="$(/usr/libexec/java_home -v 25)" # macOS
./gradlew build
```

The built mod is written to `build/libs/macfsrmc-1.0.0.jar`. `processResources` builds and packages the native library for the current host. Use `-PskipNative` only for Java-only development checks.

The GitHub Actions workflow builds and tests native jars for Windows x86-64, Linux x86-64/ARM64, and macOS Intel/Apple Silicon. It also publishes a `macfsrmc-universal` workflow artifact containing one `macfsrmc-<version>-universal.jar` with all five supported native libraries and a SHA-256 checksum. These CI binaries are not code-signed or notarized.

## Run in development

Select **Prefer Vulkan (Experimental)** in Minecraft's Video Settings, or set the equivalent option in the dev profile, then run:

```powershell
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-26'
.\gradlew.bat runClient
```

On first start the mod creates `config/macfsrmc.json`:

```json
{
  "enabled": true,
  "qualityMode": "QUALITY",
  "sharpness": 0.2,
  "reactiveMask": true,
  "reactiveScale": 0.65,
  "debugLogging": false
}
```

Supported quality modes follow AMD's prescribed ratios:

| Mode | Per-axis upscale | Render scale |
| --- | ---: | ---: |
| Quality | 1.5x | 66.7% |
| Balanced | 1.7x | 58.8% |
| Performance | 2.0x | 50% |
| Ultra Performance | 3.0x | 33.3% |

Restart the client after editing the file.

## Render integration

1. Keep Minecraft's native-resolution `MainTarget` as the presentation/UI target.
2. Before `GameRenderer.renderLevel`, switch only world rendering to a reduced `TextureTarget`, update `LevelRenderer` and `GlobalSettingsUniform`, and apply the official FSR 2 Halton jitter sequence.
3. Immediately before the hand pass clears depth, capture the world color/depth Vulkan images.
4. Record a camera-depth motion-vector and reactive-mask compute pass into a Minecraft-owned transient command buffer.
5. Dispatch the complete FSR 2.2.1 Vulkan pass chain into a storage-capable native output image.
6. Blit the reconstructed result into Minecraft's native target, restore all external images to `VK_IMAGE_LAYOUT_GENERAL`, and return the command buffer to `VulkanCommandEncoder.execute()` so Minecraft retains submission and semaphore ownership.
7. Restore the native target and render the hand, overlays, post-upscale work, and GUI at display resolution.

The bridge never submits independently to Minecraft's Vulkan queue. Context destruction and resolution changes wait for the device only at lifecycle boundaries, as required by the FSR 2 API.

## Current motion coverage

Minecraft 26.2 does not expose a material velocity attachment. This mod reconstructs camera motion from the reversed-Z `D32_FLOAT` depth buffer and current/previous jittered camera matrices. Camera and static-world motion are therefore covered correctly. Per-vertex animation and independently moving entities do not yet have exact object velocity; the reactive mask reduces history reliance around high-contrast/alpha edges to limit ghosting in those areas.

## Third-party code

- AMD FidelityFX Super Resolution 2.2.1, MIT license
- Khronos Vulkan Headers, Apache-2.0 license

Their license texts are included in the source tree and packaged under `META-INF/licenses`.
