param(
    [string[]]$SamplePath = @(),
    [string]$SampleDirectory,
    [int]$Size = 256
)

$ErrorActionPreference = 'Stop'
if (![Environment]::Is64BitProcess) { throw 'Run this diagnostic with 64-bit PowerShell.' }
if ($Size -lt 1) { throw 'Size must be positive.' }

$clsid = '{2c93d534-2a1f-40d2-a375-babc92996987}'
$thumbnailSlot = 'shellex\{e357fccd-a995-4576-b01f-234630154e96}'

function Get-ClassValue([Microsoft.Win32.RegistryHive]$Hive, [string]$Path, [string]$Name = '') {
    $root = [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $root.OpenSubKey($Path)
        if ($null -eq $key) { return '<missing key>' }
        try {
            $value = $key.GetValue($Name, $null)
            if ($null -eq $value) { return '<missing value>' }
            return [string]$value
        } finally { $key.Dispose() }
    } finally { $root.Dispose() }
}

function Write-ClassValue([string]$Label, [Microsoft.Win32.RegistryHive]$Hive, [string]$Path, [string]$Name = '') {
    $value = Get-ClassValue $Hive $Path $Name
    Write-Output ("{0}: {1}" -f $Label, $value)
}

$user = [Microsoft.Win32.RegistryHive]::CurrentUser
$machine = [Microsoft.Win32.RegistryHive]::LocalMachine
$classes = [Microsoft.Win32.RegistryHive]::ClassesRoot
Write-Output "User: $([Security.Principal.WindowsIdentity]::GetCurrent().Name)"
Write-Output "Process: $([Environment]::Is64BitProcess) (64-bit), Size: $Size"
foreach ($name in @('IconsOnly', 'DisableThumbnails')) {
    Write-ClassValue "Explorer $name" $user 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' $name
}
foreach ($hive in @($user, $machine)) {
    Write-ClassValue "$hive policy DisableThumbnails" $hive 'Software\Microsoft\Windows\CurrentVersion\Policies\Explorer' 'DisableThumbnails'
}

$inproc = "Software\Classes\CLSID\$clsid\InprocServer32"
foreach ($hive in @($user, $machine)) {
    Write-ClassValue "$hive CLSID InprocServer32" $hive $inproc
    Write-ClassValue "$hive CLSID ThreadingModel" $hive $inproc 'ThreadingModel'
}
$registeredDll = Get-ClassValue $classes "CLSID\$clsid\InprocServer32"
Write-Output "Effective CLSID InprocServer32: $registeredDll"
$failed = $false
if ($registeredDll -notlike '<*' -and (Test-Path -LiteralPath $registeredDll -PathType Leaf)) {
    Write-Output "Effective DLL SHA256: $((Get-FileHash -LiteralPath $registeredDll -Algorithm SHA256).Hash)"
} else {
    Write-Output 'Effective DLL is missing or unregistered.'
}

try {
    $instance = [Activator]::CreateInstance([type]::GetTypeFromCLSID([guid]$clsid, $true))
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($instance)
    Write-Output 'Registered COM activation: OK'
} catch {
    Write-Output ('Registered COM activation: FAILED {0} (0x{1:X8})' -f $_.Exception.Message, $_.Exception.HResult)
    $failed = $true
}

$samples = @($SamplePath)
if ($SampleDirectory) {
    $samples += @(Get-ChildItem -LiteralPath $SampleDirectory -File |
        Where-Object Extension -In '.heic', '.heif', '.dng' | Select-Object -ExpandProperty FullName)
}
if ($samples.Count -eq 0) { Write-Output 'No sample specified; pass -SamplePath or -SampleDirectory for a Shell thumbnail test.' }

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

[StructLayout(LayoutKind.Sequential)] public struct DiagnosticThumbSize {
    public int Width, Height;
    public DiagnosticThumbSize(int size) { Width = size; Height = size; }
}
[ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IDiagnosticShellItemImageFactory {
    [PreserveSig] int GetImage(DiagnosticThumbSize size, int flags, out IntPtr bitmap);
}
[ComImport, Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IDiagnosticInitializeWithStream {
    [PreserveSig] int Initialize(IStream stream, uint mode);
}
[ComImport, Guid("e357fccd-a995-4576-b01f-234630154e96"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IDiagnosticThumbnailProvider {
    [PreserveSig] int GetThumbnail(uint size, out IntPtr bitmap, out uint alpha);
}
public static class DiagnosticShellThumbnail {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode, PreserveSig=false)]
    static extern void SHCreateItemFromParsingName(string path, IntPtr bind, ref Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out IDiagnosticShellItemImageFactory factory);
    [DllImport("shlwapi.dll", CharSet=CharSet.Unicode, PreserveSig=false)]
    static extern void SHCreateStreamOnFileW(string path, uint mode, out IStream stream);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr bitmap);

    public static bool CheckProvider(string path, int size) {
        var timer = Stopwatch.StartNew();
        object instance = null;
        IStream stream = null;
        IntPtr bitmap = IntPtr.Zero;
        try {
            var clsid = new Guid("2c93d534-2a1f-40d2-a375-babc92996987");
            instance = Activator.CreateInstance(Type.GetTypeFromCLSID(clsid, true));
            SHCreateStreamOnFileW(path, 0, out stream);
            int hr = ((IDiagnosticInitializeWithStream)instance).Initialize(stream, 0);
            if (hr >= 0) {
                uint alpha;
                hr = ((IDiagnosticThumbnailProvider)instance).GetThumbnail((uint)size, out bitmap, out alpha);
            }
            timer.Stop();
            Console.WriteLine("Direct provider: " + path + " hr=0x" + hr.ToString("X8") +
                " ms=" + timer.ElapsedMilliseconds + " bitmap=" + (bitmap != IntPtr.Zero));
            return hr >= 0 && bitmap != IntPtr.Zero;
        } catch (Exception ex) {
            Console.WriteLine("Direct provider: " + path + " FAILED " + ex.Message +
                " (0x" + ex.HResult.ToString("X8") + ")");
            return false;
        } finally {
            if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
            if (stream != null) Marshal.ReleaseComObject(stream);
            if (instance != null) Marshal.ReleaseComObject(instance);
        }
    }

    public static bool Check(string path, int size) {
        var timer = Stopwatch.StartNew();
        var iid = typeof(IDiagnosticShellItemImageFactory).GUID;
        IDiagnosticShellItemImageFactory factory = null;
        IntPtr bitmap = IntPtr.Zero;
        try {
            SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out factory);
            // THUMBNAILONLY prevents success from an icon fallback.
            int hr = factory.GetImage(new DiagnosticThumbSize(size), 0x8, out bitmap);
            timer.Stop();
            Console.WriteLine("Shell thumbnail: " + path + " hr=0x" + hr.ToString("X8") +
                " ms=" + timer.ElapsedMilliseconds + " bitmap=" + (bitmap != IntPtr.Zero));
            return hr >= 0 && bitmap != IntPtr.Zero;
        } catch (Exception ex) {
            Console.WriteLine("Shell thumbnail: " + path + " FAILED " + ex.Message +
                " (0x" + ex.HResult.ToString("X8") + ")");
            return false;
        } finally {
            if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
            if (factory != null) Marshal.ReleaseComObject(factory);
        }
    }
}
'@

foreach ($sample in $samples) {
    if (!(Test-Path -LiteralPath $sample -PathType Leaf)) {
        Write-Output "Sample missing: $sample"
        $failed = $true
        continue
    }
    $path = (Resolve-Path -LiteralPath $sample).Path
    $extension = [IO.Path]::GetExtension($path).ToLowerInvariant()
    Write-Output "`nFile: $path"
    $userChoice = Get-ClassValue $user "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$extension\UserChoice" 'ProgId'
    $extensionProgId = Get-ClassValue $classes $extension
    Write-Output "UserChoice ProgID: $userChoice"
    Write-Output "Extension ProgID: $extensionProgId"
    foreach ($hive in @($user, $machine, $classes)) {
        $prefix = if ($hive -eq $classes) { '' } else { 'Software\Classes\' }
        Write-ClassValue "$hive $extension thumbnail handler" $hive "$prefix$extension\$thumbnailSlot"
    }
    foreach ($progId in @($userChoice, $extensionProgId) | Where-Object { $_ -and $_ -notlike '<*' } | Select-Object -Unique) {
        foreach ($hive in @($user, $machine, $classes)) {
            $prefix = if ($hive -eq $classes) { '' } else { 'Software\Classes\' }
            Write-ClassValue "$hive $progId thumbnail handler" $hive "$prefix$progId\$thumbnailSlot"
        }
    }
    if (![DiagnosticShellThumbnail]::CheckProvider($path, $Size)) { $failed = $true }
    if (![DiagnosticShellThumbnail]::Check($path, $Size)) { $failed = $true }
}
if ($failed) { exit 1 }
