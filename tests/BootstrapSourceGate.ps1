$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bootstrapPath = Join-Path $root 'src/PAPER_ToolkitMain.cpp'
$telemetryPath = Join-Path $root 'src/paper_toolkit/WeaponClipTelemetry.cpp'
$authoringPath = Join-Path $root 'src/paper_toolkit/AuthoringRuntime.cpp'
$bootstrap = Get-Content -Raw -LiteralPath $bootstrapPath
$telemetry = Get-Content -Raw -LiteralPath $telemetryPath
$authoring = Get-Content -Raw -LiteralPath $authoringPath

if ($bootstrap -notmatch 'REL::Module::IsVR\(\)') {
    throw 'Bootstrap must reject non-VR modules with REL::Module::IsVR().'
}
if ($bootstrap -notmatch 'const\s+auto\s+executableVersion\s*=\s*REL::Module::get\(\)\.version\(\)') {
    throw 'Bootstrap must read the executable version from REL::Module.'
}

foreach ($token in @(
    'getReloadAnimationCatalogStateV1',
    'copyReloadAnimationTracksV1',
    'copyReloadAnimationSamplesV1',
    'catalogRevision',
    'RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal',
    'setWeaponPartDriveTargetsV1'
)) {
    if (-not $authoring.Contains($token)) {
        throw "Authoring snapshot/preview contract is missing token: $token"
    }
}
if ($authoring -match 'REL::safe_write|hkbClipGenerator|s_originalClip(?:Activate|Update|Deactivate)|ClipGeneratorMode|UserFraction') {
    throw 'Authoring workstation must not patch or control the native clip lifecycle.'
}
if ($bootstrap -notmatch 'executableVersion\s*!=\s*F4SE::RUNTIME_VR_1_2_72') {
    throw 'Bootstrap must require the exact Fallout4VR.exe 1.2.72 version.'
}
if ($bootstrap -match 'RuntimeVersion\(\)\s*(?:==|!=|<|>|<=|>=)\s*F4SE::RUNTIME_(?:LATEST_VR|VR_1_2_72)') {
    throw 'F4SE query compatibility RuntimeVersion must not be compared with VR executable constants.'
}

$requiredTelemetryTokens = @(
    'kClipGeneratorVtableModuleOffset = 0x2E0FB38',
    'kClipGeneratorActivateModuleOffset = 0x192CA40',
    'kClipGeneratorUpdateModuleOffset = 0x192D0D0',
    'kClipGeneratorDeactivateModuleOffset = 0x192D510',
    'liveActivate != moduleBase + kClipGeneratorActivateModuleOffset',
    'liveUpdate != moduleBase + kClipGeneratorUpdateModuleOffset',
    'liveDeactivate != moduleBase + kClipGeneratorDeactivateModuleOffset'
)
foreach ($token in $requiredTelemetryTokens) {
    if (-not $telemetry.Contains($token)) {
        throw "Native telemetry identity gate is missing token: $token"
    }
}

Write-Host 'PAPER_Toolkit bootstrap/native identity source gate passed.'
