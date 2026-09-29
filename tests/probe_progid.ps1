param(
    [Parameter(Mandatory = $true)][string]$SamplePath,
    [switch]$ForceProbe
)

$ErrorActionPreference = 'Stop'
if (![Environment]::Is64BitProcess) { throw 'Run with 64-bit PowerShell.' }
$path = (Resolve-Path -LiteralPath $SamplePath).Path
$extension = [IO.Path]::GetExtension($path).ToLowerInvariant()
if ($extension -notin @('.heic', '.heif', '.dng')) { throw 'Expected a HEIC, HEIF or DNG file.' }

$diagnostic = Join-Path $PSScriptRoot 'diagnose-explorer.ps1'
if (!(Test-Path -LiteralPath $diagnostic)) {
    $diagnostic = Join-Path $PSScriptRoot 'diagnose_explorer.ps1'
}
if (!(Test-Path -LiteralPath $diagnostic)) { throw "Missing diagnostic script: $diagnostic" }

$user = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
$classes = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::ClassesRoot, [Microsoft.Win32.RegistryView]::Registry64)

function Get-DefaultValue([Microsoft.Win32.RegistryKey]$Root, [string]$KeyPath) {
    $key = $Root.OpenSubKey($KeyPath)
    if ($null -eq $key) { return $null }
    try { return $key.GetValue('', $null) } finally { $key.Dispose() }
}

function Remove-EmptyKey([Microsoft.Win32.RegistryKey]$Root, [string]$KeyPath) {
    $key = $Root.OpenSubKey($KeyPath)
    if ($null -eq $key) { return }
    try { $empty = $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0 }
    finally { $key.Dispose() }
    if ($empty) { $Root.DeleteSubKey($KeyPath, $false) }
}

function Test-RegistryKeyExists([Microsoft.Win32.RegistryKey]$Root, [string]$KeyPath) {
    $key = $Root.OpenSubKey($KeyPath)
    if ($null -eq $key) { return $false }
    $key.Dispose()
    return $true
}

function Invoke-ShellProbe([string]$Label) {
    Write-Host "`n[$Label]"
    $lines = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $diagnostic -SamplePath $path 2>&1
    $status = $LASTEXITCODE
    foreach ($line in $lines) {
        if ([string]$line -match 'AssocQueryString\(|Direct provider:|Shell stream binding:|Shell bind in-process:|Shell thumbnail:|Module (before Shell|after stream|after thumbnail bind|after Shell image)|Sample missing:') {
            Write-Host $line
        }
    }
    Write-Host "ExitCode: $status"
    return $status
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ThumbnailAssociationNotify {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(uint eventId, uint flags, IntPtr first, IntPtr second);
    public static void Changed() { SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero); }
}
'@

try {
    $progId = Get-DefaultValue $user "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$extension\UserChoice"
    if (!$progId) { $progId = Get-DefaultValue $classes $extension }
    if ($progId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
        throw "Invalid or missing effective ProgID: $progId"
    }

    $slot = '{e357fccd-a995-4576-b01f-234630154e96}'
    $clsid = '{2c93d534-2a1f-40d2-a375-babc92996987}'
    $parent = "Software\Classes\$progId"
    $shellEx = "$parent\shellex"
    $entry = "$shellEx\$slot"
    $parentExisted = Test-RegistryKeyExists $user $parent
    $shellExExisted = Test-RegistryKeyExists $user $shellEx
    $existing = $user.OpenSubKey($entry)
    $entryExisted = $null -ne $existing
    $oldValue = $null
    $oldKind = $null
    $hadValue = $false
    if ($existing) {
        try {
            $hadValue = $existing.GetValueNames() -contains ''
            if ($hadValue) {
                $oldValue = $existing.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                $oldKind = $existing.GetValueKind('')
            }
        } finally { $existing.Dispose() }
    }

    Write-Host "Temporary test: HKCU\$entry -> $clsid"
    Write-Host "Previous default: $(if ($hadValue) { $oldValue } else { '<not set>' })"
    $baseline = Invoke-ShellProbe 'BEFORE'
    if ($baseline -eq 0 -and !$ForceProbe) {
        Write-Host 'Baseline already succeeds; no registry change was made.'
        return
    }

    $written = $false
    try {
        $key = $user.CreateSubKey($entry)
        try {
            $key.SetValue('', $clsid, [Microsoft.Win32.RegistryValueKind]::String)
            $written = $true
        }
        finally { $key.Dispose() }
        [ThumbnailAssociationNotify]::Changed()
        $during = Invoke-ShellProbe 'PROGID SET'
    } finally {
        $key = $user.OpenSubKey($entry, $true)
        if ($key) {
            try {
                if ($key.GetValue('', $null) -eq $clsid) {
                    if ($hadValue) { $key.SetValue('', $oldValue, $oldKind) }
                    else { $key.DeleteValue('', $false) }
                } elseif ($written) {
                    throw "The ProgID handler changed during the probe; refusing to overwrite it: HKCU\$entry"
                }
            } finally { $key.Dispose() }
        }
        if (!$entryExisted) { Remove-EmptyKey $user $entry }
        if (!$shellExExisted) { Remove-EmptyKey $user $shellEx }
        if (!$parentExisted) { Remove-EmptyKey $user $parent }
        [ThumbnailAssociationNotify]::Changed()
        Write-Host 'Original ProgID handler restored.'
    }
    $after = Invoke-ShellProbe 'AFTER RESTORE'
    Write-Host "Comparison: BEFORE=$baseline PROGID_SET=$during AFTER_RESTORE=$after (0 means both thumbnail paths succeeded)"
} finally {
    $classes.Dispose()
    $user.Dispose()
}
