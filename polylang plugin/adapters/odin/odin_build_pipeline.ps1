# =============================================================
# odin_build_pipeline.ps1  —  PolyLang v6.7 Odin Build Pipeline (Windows)
# =============================================================
# Usage: ./odin_build_pipeline.ps1 <src_path> <out_so>
# =============================================================

param (
    [Parameter(Mandatory=$true)]
    [string]$SrcPath,

    [Parameter(Mandatory=$true)]
    [string]$OutSo
)

# Convert to absolute paths
$SrcPath = Resolve-Path $SrcPath
$OutSoDir = Split-Path $OutSo
if (-not (Test-Path $OutSoDir)) {
    New-Item -ItemType Directory -Path $OutSoDir -Force | Out-Null
}

$OdinExe = "odin"
# Check if odin is in PATH
if (-not (Get-Command $OdinExe -ErrorAction SilentlyContinue)) {
    Write-Error "Odin compiler not found in PATH."
    exit 1
}

# Odin build command
# Note: we use -build-mode:shared to produce a DLL
Write-Host "Compiling Odin script: $SrcPath -> $OutSo"
& $OdinExe build $SrcPath -build-mode:shared -out:$OutSo

if ($LASTEXITCODE -ne 0) {
    Write-Error "Odin build failed (exit code $LASTEXITCODE)"
    exit $LASTEXITCODE
}

Write-Host "Odin build successful: $OutSo"
exit 0
