[CmdletBinding()]
param(
    [switch]$Configure,
    # Build without the GPU profile-guided optimization profile.
    [switch]$NoPgo,
    # Check the checkout against the pinned upstream ReXGlue commit (the
    # original development tree); a fork checkout skips this.
    [switch]$VerifyProvenance,
    # Ready-made player package: without the NVIDIA DLSS and Streamline SDKs
    # (ready-made packages leave them out until NVIDIA has approved the credits
    # its terms require) and without paths of this machine in the DLLs, in its own
    # trees (out/build/win-amd64-player, out/win-amd64-player/Release).
    [switch]$Player
)

. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
Initialize-DeveloperToolchain

$source = Join-Path $root 'external/ReXGlue'
$tree = if ($Player) { 'win-amd64-player' } else { 'win-amd64' }
$binary = Join-Path $source "out/build/$tree"
$cache = Join-Path $binary 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "ReXGlue source not found: $source (git submodule update --init --recursive)"
}

# The project's configuration: ThinLTO, the GPU PGO profile when present,
# player builds without diagnostics, Direct3D 12 only, no ReXGlue tests, the
# FidelityFX API from build/deps (scripts/fetch-sdks.ps1) instead of a fetch.
$profile = Join-Path $root 'config/pgo/rexgpu-v519.profdata'
$pgo = 'OFF'
if (-not $NoPgo -and (Test-Path -LiteralPath $profile -PathType Leaf)) { $pgo = 'USE' }
$options = @(
    '-D', 'REXGLUE_GPU_THINLTO=ON',
    '-D', "REXGLUE_GPU_PGO=$pgo",
    '-D', 'REXGLUE_GPU_DIAGNOSTICS=OFF',
    '-D', 'REXGLUE_USE_VULKAN=OFF',
    '-D', 'REXGLUE_BUILD_TESTS=OFF',
    '-D', 'REXGLUE_ENABLE_FIDELITYFX=OFF',
    '-D', 'REXGLUE_ENABLE_SANITIZERS=OFF',
    '-D', ('REXGLUE_OUTPUT_DIR=' + ((Join-Path $source "out/$tree") -replace '\\', '/'))
)
if ($pgo -eq 'USE') { $options += @('-D', ('REXGLUE_GPU_PGO_PROFILE=' + ($profile -replace '\\', '/'))) }
if ($Player) {
    # The preset's flags plus repository-relative __FILE__ and debug paths.
    $prefix = "/clang:-ffile-prefix-map=$root\= /clang:-ffile-prefix-map=$($root -replace '\\', '/')/="
    $options += @(
        '-B', $binary,
        '-D', ('REXGLUE_DLSS_SDK_DIR=' + ((Join-Path $root 'build/deps/no-dlss-in-player-packages') -replace '\\', '/')),
        '-D', ('REXGLUE_STREAMLINE_SDK_DIR=' + ((Join-Path $root 'build/deps/no-streamline-in-player-packages') -replace '\\', '/')),
        '-D', "CMAKE_C_FLAGS=/clang:-march=x86-64-v3 $prefix",
        '-D', "CMAKE_CXX_FLAGS=/clang:-march=x86-64-v3 $prefix")
}

# A player tree configured before an SDK option existed would take that
# option's default (the SDK) on CMake's automatic re-run; configure it again.
$configureNeeded = $Configure -or -not (Test-Path -LiteralPath $cache -PathType Leaf)
if ($Player -and -not $configureNeeded -and
    -not (Select-String -LiteralPath $cache -Pattern '^REXGLUE_STREAMLINE_SDK_DIR:' -Quiet)) {
    $configureNeeded = $true
}

Push-Location $source
try {
    if ($configureNeeded) {
        Write-CommandLine ('cmake --preset win-amd64 ' + ($options -join ' '))
        & cmake --preset win-amd64 @options
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    if ($Player) {
        Write-CommandLine "cmake --build $binary --config Release"
        & cmake --build $binary --config Release
    } else {
        Write-CommandLine 'cmake --build --preset win-amd64-release'
        & cmake --build --preset win-amd64-release
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

& (Join-Path $root 'scripts/verify-rexglue-release-policy.ps1') -BuildDirectory "external/ReXGlue/out/build/$tree" |
    Out-Host
if ($VerifyProvenance) {
    & (Join-Path $root 'scripts/verify-rexglue-provenance.ps1') |
        Out-Host
}
