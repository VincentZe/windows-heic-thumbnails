param(
    [string]$SampleDirectory = 'C:\Users\admin\Downloads\ThumbTest',
    [int]$Size = 256
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)] public struct ThumbSize {
    public int Width, Height;
    public ThumbSize(int width, int height) { Width = width; Height = height; }
}
[ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IShellItemImageFactory {
    [PreserveSig] int GetImage(ThumbSize size, int flags, out IntPtr bitmap);
}
public static class ExplorerThumbnailTest {
    public static bool Failed;
    [DllImport("shell32.dll", CharSet=CharSet.Unicode, PreserveSig=false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr bind, ref Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out IShellItemImageFactory factory);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr bitmap);
    public static void Check(string path, int size) {
        var iid = typeof(IShellItemImageFactory).GUID;
        var timer = Stopwatch.StartNew();
        IShellItemImageFactory factory;
        SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out factory);
        IntPtr bitmap = IntPtr.Zero;
        try {
            // SIIGBF_THUMBNAILONLY: use the same provider path Explorer requests.
            int hr = factory.GetImage(new ThumbSize(size,size), 0x8, out bitmap);
            timer.Stop();
            Console.WriteLine(System.IO.Path.GetFileName(path) + " hr=0x" + hr.ToString("X8") +
                " ms=" + timer.ElapsedMilliseconds + " bitmap=" + (bitmap != IntPtr.Zero));
            if (hr < 0 || bitmap == IntPtr.Zero) Failed = true;
        } finally {
            if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
            Marshal.ReleaseComObject(factory);
        }
    }
}
'@
Get-ChildItem -LiteralPath $SampleDirectory -File |
    Where-Object Extension -In '.heic', '.heif', '.dng' |
    ForEach-Object { [ExplorerThumbnailTest]::Check($_.FullName, $Size) }
if ([ExplorerThumbnailTest]::Failed) { exit 1 }
