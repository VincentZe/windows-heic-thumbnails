param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts')
)

$ErrorActionPreference = 'Stop'
if (!$VcpkgRoot) {
    $vcpkg = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkg) { $VcpkgRoot = Split-Path -Parent $vcpkg.Source }
}
if (!$VcpkgRoot) { throw 'Pass -VcpkgRoot or set VCPKG_ROOT.' }

$build = Join-Path $PSScriptRoot '..\src\x64\Release\HEICThumbnailHandler.dll'
$bin = Join-Path $VcpkgRoot 'installed\x64-windows\bin'
$licenseRoot = Join-Path $VcpkgRoot 'installed\x64-windows\share'
$name = 'FastImageThumbnails-win-x64-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$folder = Join-Path $OutputDirectory $name
$zip = Join-Path $OutputDirectory ($name + '.zip')
$files = @(
    @{ Source = $build; Name = 'HEICThumbnailHandler.dll' },
    @{ Source = (Join-Path $bin 'heif.dll'); Name = 'heif.dll' },
    @{ Source = (Join-Path $bin 'libde265.dll'); Name = 'libde265.dll' },
    @{ Source = (Join-Path $bin 'libx265.dll'); Name = 'libx265.dll' },
    @{ Source = (Join-Path $PSScriptRoot 'install.ps1'); Name = 'install.ps1' },
    @{ Source = (Join-Path $PSScriptRoot 'DEPLOY.txt'); Name = 'DEPLOY.txt' },
    @{ Source = (Join-Path $PSScriptRoot '..\LICENSE'); Name = 'LICENSE.txt' }
)
foreach ($file in $files) {
    if (!(Test-Path -LiteralPath $file.Source -PathType Leaf)) { throw "Missing packaging input: $($file.Source)" }
}
foreach ($library in @('libheif', 'libde265', 'x265')) {
    $license = Join-Path $licenseRoot "$library\copyright"
    if (!(Test-Path -LiteralPath $license -PathType Leaf)) { throw "Missing dependency license: $license" }
}
if ((Test-Path -LiteralPath $folder) -or (Test-Path -LiteralPath $zip)) {
    throw "Package already exists: $name. Retry after one second."
}

New-Item -ItemType Directory -Path (Join-Path $folder 'licenses') -Force | Out-Null
foreach ($file in $files) {
    Copy-Item -LiteralPath $file.Source -Destination (Join-Path $folder $file.Name)
}
foreach ($library in @('libheif', 'libde265', 'x265')) {
    Copy-Item -LiteralPath (Join-Path $licenseRoot "$library\copyright") `
        -Destination (Join-Path $folder "licenses\$library.txt")
}
Compress-Archive -LiteralPath $folder -DestinationPath $zip -CompressionLevel Optimal
Get-FileHash -Algorithm SHA256 -LiteralPath $zip | Select-Object Path,Hash
