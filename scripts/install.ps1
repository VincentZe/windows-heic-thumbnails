param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'FastImageThumbnails'),
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
if (![Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell to register the x64 thumbnail provider.' }
$portableSource = Join-Path $PSScriptRoot 'HEICThumbnailHandler.dll'
$isPortable = Test-Path -LiteralPath $portableSource
if (!$VcpkgRoot -and !$Uninstall -and !$isPortable) {
    $vcpkg = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkg) { $VcpkgRoot = Split-Path -Parent $vcpkg.Source }
}
if (!$VcpkgRoot -and !$Uninstall -and !$isPortable) { throw 'Pass -VcpkgRoot or set VCPKG_ROOT.' }
$handler = Join-Path $Destination 'HEICThumbnailHandler.dll'
function Invoke-Registration([switch]$Remove) {
    $arguments = if ($Remove) { @('/u', '/s', ('"{0}"' -f $handler)) } else { @('/s', ('"{0}"' -f $handler)) }
    $process = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
        -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "regsvr32 returned $($process.ExitCode)" }
}
if ($Uninstall) {
    if (!(Test-Path -LiteralPath $handler)) { throw "Installed handler not found: $handler" }
    Invoke-Registration -Remove
    Write-Host "Thumbnail associations restored. DLL files remain in $Destination until manually removed."
    return
}

$source = if ($isPortable) { $portableSource } else { Join-Path $PSScriptRoot '..\src\x64\Release\HEICThumbnailHandler.dll' }
$bin = if ($isPortable) { $PSScriptRoot } else { Join-Path $VcpkgRoot 'installed\x64-windows\bin' }
$files = @(
    @{ Source = $source; Name = 'HEICThumbnailHandler.dll' },
    @{ Source = (Join-Path $bin 'heif.dll'); Name = 'heif.dll' },
    @{ Source = (Join-Path $bin 'libde265.dll'); Name = 'libde265.dll' },
    @{ Source = (Join-Path $bin 'libx265.dll'); Name = 'libx265.dll' }
)
foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file.Source)) { throw "Missing dependency: $($file.Source)" }
}

# The server is installed outside the build directory so rebuilds do not replace a loaded DLL.
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
if ((Test-Path -LiteralPath $handler) -and
    (Get-FileHash -LiteralPath $handler).Hash -ne (Get-FileHash -LiteralPath $source).Hash) {
    Invoke-Registration -Remove
}
foreach ($file in $files) {
    $target = Join-Path $Destination $file.Name
    if (!(Test-Path -LiteralPath $target) -or
        (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $file.Source).Hash) {
        Copy-Item -LiteralPath $file.Source -Destination $target -Force
    }
}
Invoke-Registration
Write-Host "Registered HEIC, HEIF and DNG thumbnail provider from $Destination"
