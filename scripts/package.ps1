param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$MsbuildPath,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts')
)

$ErrorActionPreference = 'Stop'
if (!$VcpkgRoot) {
    $vcpkg = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkg) { $VcpkgRoot = Split-Path -Parent $vcpkg.Source }
}
if (!$VcpkgRoot) { throw 'Pass -VcpkgRoot or set VCPKG_ROOT.' }
$VcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path
if (!$MsbuildPath) {
    $msbuild = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($msbuild) { $MsbuildPath = $msbuild.Source }
}
if (!$MsbuildPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installation = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installation) { $MsbuildPath = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe' }
    }
}
if (!(Test-Path -LiteralPath $MsbuildPath -PathType Leaf)) { throw 'MSBuild with x64 C++ tools is required. Pass -MsbuildPath.' }

$build = Join-Path $PSScriptRoot '..\src\x64\Release\HEICThumbnailHandler.dll'
$bin = Join-Path $VcpkgRoot 'installed\x64-windows\bin'
$licenseRoot = Join-Path $VcpkgRoot 'installed\x64-windows\share'
$project = Join-Path $PSScriptRoot '..\src\HEICThumbnailHandler.vcxproj'
$vcpkgProperty = '/p:VcpkgRoot=' + $VcpkgRoot.TrimEnd('\') + '\'
& $MsbuildPath $project '/t:Rebuild' '/p:Configuration=Release' '/p:Platform=x64' $vcpkgProperty '/m' '/v:minimal'
if ($LASTEXITCODE -ne 0) { throw "Clean x64 Release rebuild failed: $LASTEXITCODE" }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot '..\tests\com_interfaces.ps1') `
    -DllPath $build -DependencyDirectory $bin
if ($LASTEXITCODE -ne 0) { throw 'Newly rebuilt DLL failed COM interface verification.' }

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
    @{ Source = (Join-Path $PSScriptRoot '..\tests\com_interfaces.ps1'); Name = 'verify-com.ps1' },
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
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$commit = (& git -C $repository rev-parse HEAD).Trim()
$treeState = if (& git -C $repository status --porcelain) { 'modified' } else { 'clean' }
$dllHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $folder 'HEICThumbnailHandler.dll')).Hash
@(
    "SourceCommit: $commit"
    "SourceTree: $treeState"
    "BuildTimeUTC: $((Get-Date).ToUniversalTime().ToString('o'))"
    "HEICThumbnailHandler.dll SHA256: $dllHash"
) | Set-Content -LiteralPath (Join-Path $folder 'BUILDINFO.txt') -Encoding ASCII
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $folder 'verify-com.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Packaged DLL failed COM interface verification.' }
Compress-Archive -LiteralPath $folder -DestinationPath $zip -CompressionLevel Optimal
Get-FileHash -Algorithm SHA256 -LiteralPath $zip | Select-Object Path,Hash
