param(
    [string]$DllPath = $(if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'HEICThumbnailHandler.dll')) {
        Join-Path $PSScriptRoot 'HEICThumbnailHandler.dll'
    } else {
        Join-Path $PSScriptRoot '..\src\x64\Release\HEICThumbnailHandler.dll'
    }),
    [string]$DependencyDirectory
)

$ErrorActionPreference = 'Stop'
$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
if (!$DependencyDirectory) {
    $DependencyDirectory = Split-Path -Parent $DllPath
    if (!(Test-Path -LiteralPath (Join-Path $DependencyDirectory 'heif.dll'))) {
        $vcpkg = Get-Command vcpkg -ErrorAction SilentlyContinue
        if (!$vcpkg) { throw 'Pass -DependencyDirectory containing heif.dll.' }
        $DependencyDirectory = Join-Path (Split-Path -Parent $vcpkg.Source) 'installed\x64-windows\bin'
    }
}
$DependencyDirectory = (Resolve-Path -LiteralPath $DependencyDirectory).Path

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class ComInterfaceCheck {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool SetDllDirectory(string directory);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibrary(string path);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)]
    static extern IntPtr GetProcAddress(IntPtr module, string name);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int GetClassObject(ref Guid clsid, ref Guid iid, out IntPtr result);

    [ComImport, Guid("00000001-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IClassFactory {
        [PreserveSig] int CreateInstance(IntPtr outer, ref Guid iid, out IntPtr result);
        [PreserveSig] int LockServer(bool locked);
    }

    static void Require(int hr, string operation) {
        Console.WriteLine(operation + " hr=0x" + hr.ToString("X8"));
        if (hr != 0) throw new COMException(operation, hr);
    }

    public static void Check(string path, string dependencies) {
        if (!SetDllDirectory(dependencies)) throw new System.ComponentModel.Win32Exception();
        IntPtr module = LoadLibrary(path);
        if (module == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(
            Marshal.GetLastWin32Error(), "Cannot load thumbnail DLL");
        IntPtr export = GetProcAddress(module, "DllGetClassObject");
        if (export == IntPtr.Zero) throw new Exception("DllGetClassObject missing");
        var getClass = (GetClassObject)Marshal.GetDelegateForFunctionPointer(export, typeof(GetClassObject));
        var clsid = new Guid("2c93d534-2a1f-40d2-a375-babc92996987");
        var factoryId = new Guid("00000001-0000-0000-C000-000000000046");
        var unknownId = new Guid("00000000-0000-0000-C000-000000000046");
        var initializeId = new Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f");
        var thumbnailId = new Guid("e357fccd-a995-4576-b01f-234630154e96");
        IntPtr factoryPointer = IntPtr.Zero;
        Require(getClass(ref clsid, ref factoryId, out factoryPointer), "DllGetClassObject(IClassFactory)");
        var factory = (IClassFactory)Marshal.GetObjectForIUnknown(factoryPointer);
        Marshal.Release(factoryPointer);
        try {
            IntPtr instance = IntPtr.Zero;
            Require(factory.CreateInstance(IntPtr.Zero, ref unknownId, out instance), "CreateInstance(IUnknown)");
            try {
                foreach (var entry in new[] {
                    new { Name = "IInitializeWithStream", Id = initializeId },
                    new { Name = "IThumbnailProvider", Id = thumbnailId }
                }) {
                    var id = entry.Id;
                    IntPtr queried = IntPtr.Zero;
                    int hr = Marshal.QueryInterface(instance, ref id, out queried);
                    try { Require(hr, "QueryInterface(" + entry.Name + ")"); }
                    finally { if (queried != IntPtr.Zero) Marshal.Release(queried); }
                }
            } finally { Marshal.Release(instance); }
        } finally { Marshal.ReleaseComObject(factory); }
    }
}
'@

[ComInterfaceCheck]::Check($DllPath, $DependencyDirectory)
