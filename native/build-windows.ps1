param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,
    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$nativeRoot = Join-Path $ProjectRoot 'native'
$fsrRoot = Join-Path $nativeRoot 'third_party\fsr2\api'
$vulkanInclude = Join-Path $nativeRoot 'third_party\vulkan\include'
$outputDirectory = Join-Path $OutputRoot 'windows-x86_64'
$objectDirectory = Join-Path $ProjectRoot 'build\native\obj\windows-x86_64'
New-Item -ItemType Directory -Force -Path $outputDirectory, $objectDirectory | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools were not found (vswhere.exe is missing).'
}
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw 'Install the Visual Studio C++ x64 build tools before building MacFSRMC.'
}
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat was not found under $visualStudio"
}

$jdkCandidates = @()
if ($env:JAVA_HOME) {
    $jdkCandidates += $env:JAVA_HOME
}
$jdkCandidates += Get-ChildItem 'C:\Program Files\Java' -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object FullName
$jdkHome = $jdkCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'include\jni.h') } | Select-Object -First 1
if (-not $jdkHome) {
    throw 'A JDK with JNI headers was not found. Set JAVA_HOME to JDK 25 or newer.'
}

$sources = @(
    (Join-Path $nativeRoot 'src\fsr2_bridge.cpp'),
    (Join-Path $nativeRoot 'src\macfsr_vulkan_shim.cpp'),
    (Join-Path $fsrRoot 'ffx_fsr2.cpp'),
    (Join-Path $fsrRoot 'ffx_assert.cpp'),
    (Join-Path $fsrRoot 'vk\ffx_fsr2_vk.cpp'),
    (Join-Path $fsrRoot 'vk\shaders\ffx_fsr2_shaders_vk.cpp')
)
$includes = @(
    (Join-Path $nativeRoot 'src'),
    (Join-Path $nativeRoot 'generated'),
    $fsrRoot,
    (Join-Path $fsrRoot 'vk'),
    (Join-Path $nativeRoot 'third_party\fsr2\generated\vk'),
    $vulkanInclude,
    (Join-Path $jdkHome 'include'),
    (Join-Path $jdkHome 'include\win32')
)

$dll = Join-Path $outputDirectory 'macfsrmc_fsr2.dll'
$arguments = @('/nologo', '/std:c++17', '/O2', '/EHsc', '/MT', '/LD', '/utf-8', '/permissive-', '/Zc:__cplusplus', '/W3')
$arguments += "/FI$(Join-Path $nativeRoot 'src\macfsr_portability.h')"
$arguments += $includes | ForEach-Object { "/I$_" }
$arguments += $sources
$arguments += "/Fe:$dll"
$arguments += "/Fo:$objectDirectory\"

$quotedArguments = $arguments | ForEach-Object {
    if ($_ -match '[\s&()]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
}
$command = 'call "' + $vcvars + '" >nul && cl ' + ($quotedArguments -join ' ')
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Native compilation failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Native compiler did not create $dll"
}
Write-Host "Built $dll"
