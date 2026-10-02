param()
$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
& cmake -S $nativeRoot -B (Join-Path $nativeRoot 'build') -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build (Join-Path $nativeRoot 'build') --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed.' }
$release = Join-Path $nativeRoot 'release'
Copy-Item -LiteralPath (Join-Path $nativeRoot 'README.md') -Destination $release
Copy-Item -LiteralPath (Join-Path $nativeRoot 'VERSIONING.md') -Destination $release
Copy-Item -LiteralPath (Join-Path $nativeRoot 'runtime\vendor\minhook\LICENSE.txt') -Destination (Join-Path $release 'MinHook-LICENSE.txt')
Compress-Archive -LiteralPath (Join-Path $release 'ChemDrawPatchManager.exe'), (Join-Path $release 'README.md'), (Join-Path $release 'VERSIONING.md'), (Join-Path $release 'MinHook-LICENSE.txt') -DestinationPath (Join-Path $nativeRoot 'ChemDraw-Native-Patcher-r95.zip') -Force
Write-Output (Join-Path $release 'ChemDrawPatchManager.exe')
