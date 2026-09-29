param(
    [string]$SampleDirectory = 'C:\Users\admin\Downloads\ThumbTest',
    [int]$Size = 256,
    [int]$Iterations = 3,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$SaveImages
)

$ErrorActionPreference = 'Stop'
if (!$VcpkgRoot) {
    $vcpkg = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkg) { $VcpkgRoot = Split-Path -Parent $vcpkg.Source }
}
if (!$VcpkgRoot) { throw 'Pass -VcpkgRoot or set VCPKG_ROOT.' }
$dll = Join-Path $PSScriptRoot '..\src\x64\Release\HEICThumbnailHandler.dll'
$dependencies = Join-Path $VcpkgRoot 'installed\x64-windows\bin'
if (!(Test-Path -LiteralPath $dll)) { throw "Build the x64 Release DLL first: $dll" }

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

public static class ThumbnailBenchmark {
    public static bool Failed;
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetDllDirectory(string path);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibrary(string path);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)]
    static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("shlwapi.dll", CharSet=CharSet.Unicode, PreserveSig=false)]
    static extern void SHCreateStreamOnFileW(string path, uint mode, out IStream stream);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr bitmap);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int GetClassObject(ref Guid clsid, ref Guid iid, out IntPtr factory);

    [ComImport, Guid("00000001-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IClassFactory {
        [PreserveSig] int CreateInstance(IntPtr outer, ref Guid iid, out IntPtr result);
        [PreserveSig] int LockServer(bool locked);
    }

    [ComImport, Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IInitializeWithStream {
        [PreserveSig] int Initialize(IStream stream, uint mode);
    }

    [ComImport, Guid("e357fccd-a995-4576-b01f-234630154e96"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IThumbnailProvider {
        [PreserveSig] int GetThumbnail(uint size, out IntPtr bitmap, out uint alpha);
    }

    public static void Run(string dll, string dependencies, string path, int size, int count, bool save) {
        if (!SetDllDirectory(dependencies)) throw new System.ComponentModel.Win32Exception();
        var module = LoadLibrary(dll);
        if (module == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Cannot load thumbnail DLL");
        var export = GetProcAddress(module, "DllGetClassObject");
        if (export == IntPtr.Zero) throw new Exception("DllGetClassObject missing");
        var getClass = (GetClassObject)Marshal.GetDelegateForFunctionPointer(export, typeof(GetClassObject));
        var clsid = new Guid("2c93d534-2a1f-40d2-a375-babc92996987");
        var iid = new Guid("00000001-0000-0000-C000-000000000046");
        IntPtr factoryPointer;
        int hr = getClass(ref clsid, ref iid, out factoryPointer);
        if (hr < 0) Marshal.ThrowExceptionForHR(hr);
        var factory = (IClassFactory)Marshal.GetObjectForIUnknown(factoryPointer);
        Marshal.Release(factoryPointer);
        try {
            for (int iteration = 0; iteration < count; ++iteration) {
                var timer = Stopwatch.StartNew();
                IStream stream;
                SHCreateStreamOnFileW(path, 0, out stream);
                var initializeId = new Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f");
                IntPtr providerPointer;
                hr = factory.CreateInstance(IntPtr.Zero, ref initializeId, out providerPointer);
                if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                var provider = (IInitializeWithStream)Marshal.GetObjectForIUnknown(providerPointer);
                Marshal.Release(providerPointer);
                IntPtr bitmap = IntPtr.Zero;
                try {
                    hr = provider.Initialize(stream, 0);
                    if (hr < 0) Marshal.ThrowExceptionForHR(hr);
                    uint alpha;
                    hr = ((IThumbnailProvider)provider).GetThumbnail((uint)size, out bitmap, out alpha);
                    timer.Stop();
                    string dimensions = "none";
                    if (bitmap != IntPtr.Zero) {
                        using (var image = Image.FromHbitmap(bitmap)) {
                            dimensions = image.Width + "x" + image.Height;
                            if (image.Width > size || image.Height > size) Failed = true;
                            int minimum = 255, maximum = 0;
                            for (int y = 0; y < 4; ++y)
                                for (int x = 0; x < 4; ++x) {
                                    Color pixel = ((Bitmap)image).GetPixel(x * image.Width / 4, y * image.Height / 4);
                                    minimum = Math.Min(minimum, Math.Min(pixel.R, Math.Min(pixel.G, pixel.B)));
                                    maximum = Math.Max(maximum, Math.Max(pixel.R, Math.Max(pixel.G, pixel.B)));
                                }
                            if (maximum - minimum < 12) Failed = true;
                            if (save && iteration == 0)
                                image.Save(Path.Combine(Path.GetDirectoryName(dll), Path.GetFileName(path) + ".png"), ImageFormat.Png);
                        }
                    }
                    Console.WriteLine(Path.GetFileName(path) + " run=" + (iteration+1) +
                        " hr=0x" + hr.ToString("X8") + " ms=" + timer.ElapsedMilliseconds + " pixels=" + dimensions);
                    if (hr < 0) { Failed = true; break; }
                } finally {
                    if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
                    Marshal.ReleaseComObject(provider);
                    Marshal.ReleaseComObject(stream);
                }
            }
        } finally { Marshal.ReleaseComObject(factory); }
    }
}
'@ -ReferencedAssemblies 'System.Drawing'

Get-ChildItem -LiteralPath $SampleDirectory -File |
    Where-Object Extension -In '.heic', '.heif', '.dng' |
    ForEach-Object {
        [ThumbnailBenchmark]::Run((Resolve-Path -LiteralPath $dll).Path,
            $dependencies, $_.FullName, $Size, $Iterations, $SaveImages.IsPresent)
    }
if ([ThumbnailBenchmark]::Failed) { exit 1 }
