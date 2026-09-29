# Fast Windows HEIC and DNG Thumbnail Provider

Windows 10 does not support HEIC files by default, which are the native photo image format of recent iPhones.

HEIC files are similar to JPEG files, but with better quality in half the file size.

This per-user shell extension generates Windows Explorer thumbnails for **.heic**, **.heif**, and **.dng** files.

- HEIC/HEIF: reads the embedded EXIF JPEG preview first, then a standard HEIF `thmb` via a seekable stream, without loading the full file. If neither is present, the original full-image path is used; primary images above 32 megapixels without previews are skipped to protect Explorer from expensive decoding.
- DNG: reads an embedded JPEG preview if present. Otherwise, it samples only the needed rows from an uncompressed 16-bit, 2x2 Bayer RAW image and applies a basic tone curve and white balance. This is a quick thumbnail, not a full RAW render. Other RAW layouts without JPEG previews are currently unsupported.
- Preview reads are bounded to 8 MB; the DNG RAW path caps output at 512 pixels. Explorer's normal thumbnail cache handles repeated browsing.

![20220606-201945-explorer](https://user-images.githubusercontent.com/323682/172850354-902dbd7d-686f-4749-acc5-23990e65128e.png)

To open or edit HEIC files you'll still need another application such as [Paint.NET](https://www.getpaint.net/) or [Krita](https://krita.org/).

# Installing

- Requires 64-bit Windows 10/11
- Install the latest [Microsoft Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe), if required. You may already have this installed, but if you get an error when you run the `regsvr32` command, install this and then try again.

On the build PC, run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1` after building x64 Release. The ZIP under `artifacts\` contains the handler, all three native dependencies, licenses, and an installer. Copy the ZIP to a 64-bit Windows 10/11 PC, extract it, then run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1` inside the extracted folder. The target PC does not need vcpkg, Visual Studio, or admin rights; it needs the x64 Visual C++ runtime linked above.

For installing directly from a source checkout on the build PC, run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\install.ps1`. This mode finds vcpkg on PATH or via `VCPKG_ROOT` / `-VcpkgRoot`. Both modes copy the DLLs into `%LOCALAPPDATA%\FastImageThumbnails`, register them for the current user, and save the previous handlers for each extension.

Existing cached blank thumbnails may require a cache refresh before Explorer requests them again.

# Uninstalling

Run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall` from the extracted ZIP, or `scripts\install.ps1 -Uninstall` from the source checkout. This restores the saved providers; it leaves the DLL files in place until you remove them manually after the shell releases them.

Existing thumbnails may continue to display, but new thumbnails will not be created.

# Building

This project was built with Visual Studio 2022.

Requires [libheif](https://github.com/strukturag/libheif) which can be installed with [vcpkg](https://github.com/microsoft/vcpkg).

`vcpkg install libheif:x64-windows`

Build `src\HEICThumbnailHandler.vcxproj` as `Release|x64` with MSBuild, passing `/p:VcpkgRoot=C:\path\to\vcpkg\` unless you already enabled vcpkg's Visual Studio integration. The project uses C++17 and links `heif.lib` from `installed\x64-windows` when `VcpkgRoot` is set.

Use `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\benchmark.ps1 -SampleDirectory C:\path\to\samples` to benchmark the DLL without installing it. After installation, `tests\explorer_benchmark.ps1` exercises Windows' `IShellItemImageFactory` path. Both commands fail when a sample thumbnail cannot be generated.

The original vcpkg overlay can optionally remove the unused x265 encoder dependency if rebuilt against a compatible libheif version; the package script currently includes `libx265.dll` as required by the stock vcpkg build.

`vcpkg install libheif:x64-windows --overlay-ports=..\windows-heic-thumbnails\vcpkg-overlay`
