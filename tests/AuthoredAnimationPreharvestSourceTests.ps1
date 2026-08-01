$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$preharvestPath = Join-Path $repoRoot 'src/paper_toolkit/WeaponAnimationPreharvest.cpp'
$runtimePath = Join-Path $repoRoot 'src/paper_toolkit/PaperToolkitRuntime.cpp'
$modePath = Join-Path $repoRoot 'src/paper_toolkit/MotionPathMode.h'
$telemetryPath = Join-Path $repoRoot 'src/paper_toolkit/WeaponClipTelemetry.cpp'
$configPath = Join-Path $repoRoot 'src/PaperToolkitConfig.cpp'
$defaultIniPath = Join-Path $repoRoot 'data/config/PAPER_Toolkit.ini'
$libraryFormatPath = Join-Path $repoRoot 'src/paper_toolkit/MotionLibraryFormat.h'

$preharvest = Get-Content -LiteralPath $preharvestPath -Raw
$runtime = Get-Content -LiteralPath $runtimePath -Raw
$mode = Get-Content -LiteralPath $modePath -Raw
$telemetry = Get-Content -LiteralPath $telemetryPath -Raw
$config = Get-Content -LiteralPath $configPath -Raw
$defaultIni = Get-Content -LiteralPath $defaultIniPath -Raw
$libraryFormat = Get-Content -LiteralPath $libraryFormatPath -Raw
$productionSource = (Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -File |
    Where-Object { $_.Extension -in '.cpp', '.h' } |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"

if (Test-Path -LiteralPath (Join-Path $repoRoot 'src/paper_toolkit/WeaponClipMotionHarvest.cpp')) {
    throw 'removed WeaponClipMotionHarvest implementation returned'
}
if (Test-Path -LiteralPath (Join-Path $repoRoot 'src/paper_toolkit/WeaponPartMotionScrubPolicy.h')) {
    throw 'removed scrub-named path policy returned'
}

function Assert-Contains {
    param(
        [string] $Label,
        [string] $Text,
        [string] $Pattern
    )
    if ($Text -notmatch $Pattern) {
        throw "${Label}: required pattern '$Pattern' was not found"
    }
}

function Assert-Excludes {
    param(
        [string] $Label,
        [string] $Text,
        [string] $Pattern
    )
    if ($Text -match $Pattern) {
        throw "${Label}: forbidden pattern '$Pattern' was found"
    }
}

Assert-Contains 'exact AnimationFileData lookup' $preharvest 'kGetAnimationFilesForSubgraph\s*=\s*0x1769140'
Assert-Contains 'loaded exact subgraph path fallback' $preharvest 'copyLoadedExactSubgraphPaths'
Assert-Contains 'loaded subgraph fallback takes graph lock' $preharvest 'BSAutoLock<RE::BSSpinLock>'
Assert-Contains 'loaded subgraph fallback remains identifier-pinned' $preharvest 'candidateIdentifier\s*==\s*state\.job\.subgraphIdentifier'
Assert-Contains 'direct HKX resource load' $preharvest 'kLoadAnimationResource\s*=\s*0x1728BA0'
Assert-Contains 'native hka sampler slot' $preharvest 'kSampleTracksVtableSlot\s*=\s*5'
Assert-Contains 'first-person exact subgraph selection' $preharvest 'selectFirstPersonGraph'
Assert-Contains 'weapon hierarchy reconstruction' $preharvest 'buildBoneChainBelowAncestor'
Assert-Contains 'per-sample Weapon-relative conversion' $preharvest 'relativeTransform\(\s*weaponInParent,\s*partInWeaponParent\s*\)'
Assert-Contains 'incremental full-clip sampling' $preharvest 'kSamplesPerFrame\s*=\s*24'
Assert-Contains 'exact source authority stamp' $preharvest 'AuthoredClipSource::ExactWeaponPreharvest'
Assert-Contains 'weapon-root-local source stamp' $preharvest 'AuthoredTrackSpace::WeaponRootLocal'
Assert-Contains 'nested exact tracks own their scene branches' $runtime 'crossesDifferentExactLeader'
Assert-Contains 'heap-safe clip-work clear' $preharvest 'clearClipWork\(state\.clip\)'
Assert-Contains 'thread owner is established atomically' $preharvest 'ownerThreadId\.compare_exchange_strong'
Assert-Contains 'cross-thread reset is deferred' $preharvest 'resetRequested\.store\(true,\s*std::memory_order_release\)'
Assert-Contains 'owner consumes deferred reset' $preharvest 'resetRequested\.exchange\(\s*false,\s*std::memory_order_acq_rel\)'
Assert-Excludes 'no giant aggregate clip-work temporary' $preharvest 'state\.clip\s*=\s*\{\}'
Assert-Excludes 'no live clip generator dependency' $preharvest 'hkbClipGenerator'
Assert-Excludes 'no live activation hook dependency' $preharvest 'ensureClipActivationHookInstalled'
Assert-Excludes 'no user-controlled clip-time dependency' $preharvest 'userControlled'
Assert-Contains 'zero-motion clip detail stays below INFO' $preharvest 'PAPER_TOOLKIT_LOG_DEBUG\(Animation,[\s\S]*movingGroups=0'

Assert-Contains 'exact preharvest always collects independently of serving source' $runtime 'updateAuthoredAnimationPreharvest\(\s*weaponNode,\s*generationKey,\s*weaponFormId\s*\);'
Assert-Contains 'learned observation always collects independently of serving source' $runtime '_learner\.beginObservationFrame\(\);'
Assert-Contains 'clip telemetry is evidence-gated rather than source-gated' $runtime 'if\s*\(_richCaptureActive\)\s*\{\s*updateWeaponClipTelemetry'
Assert-Contains 'serving remains strict to selected source' $runtime '_learner\.findPath\(partKey,\s*g_paperToolkitConfig\.motionPathMode\)'
Assert-Excludes 'runtime contains no hybrid selector' $runtime 'MotionPathMode::Hybrid'
Assert-Excludes 'runtime contains no clip-time control mode' $runtime 'ClipScrub|setClipScrub|clipScrubSession|userControlled'
Assert-Excludes 'mode enum contains no hybrid value' $mode '\bHybrid\s*='
Assert-Excludes 'mode parser accepts no hybrid text' $mode 'equalsIgnoreCase\(text,\s*"hybrid"\)'
Assert-Excludes 'mode enum contains no clip-scrub value' $mode '\bClipScrub\s*='
Assert-Excludes 'mode parser accepts no scrub text' $mode 'equalsIgnoreCase\(text,\s*"scrub"\)'

Assert-Contains 'passive update calls native behavior first' $telemetry 's_originalClipUpdate\(clipGeneratorRaw,\s*context,\s*timestep\);[\s\S]*publishRichClipActivityTime\(clipGenerator\);'
Assert-Contains 'passive activation observes after native behavior' $telemetry 's_originalClipActivate\(clipGenerator,\s*context\);[\s\S]*captureFromClipGenerator\(clipGenerator,\s*context\);'
Assert-Excludes 'telemetry cannot switch clip-generator mode' $telemetry 'kClipGeneratorModeOffset|UserFraction|userControlled'
Assert-Excludes 'telemetry cannot publish gameplay stroke groups' $telemetry 'buildAuthoredGroups|drainGroups|storeAuthoredGroup'
Assert-Excludes 'telemetry contains no clip-time drive lifecycle' $telemetry 'ClipScrub|clipScrub|setClipScrub|endClipScrub'
Assert-Excludes 'production contains no removed clip-time control surface' $productionSource 'ClipScrub|clipScrub|kClipGeneratorModeOffset|UserFraction|setClipScrub|endClipScrub|bClipScrubSweep'

Assert-Contains 'config documents two serving sources' $config 'valid:\s*authored, learned'
Assert-Excludes 'config contains no removed mode' $config 'hybrid|scrub'
Assert-Excludes 'default INI contains no removed mode' $defaultIni 'hybrid|scrub'
Assert-Contains 'default INI selects exact authored serving' $defaultIni 'sMotionPathMode\s*=\s*authored'
Assert-Contains 'default INI retains passive clip diagnostics' $defaultIni 'sClipTelemetryFilter\s*=\s*Reload'
Assert-Contains 'compact serving library stays at V1' $libraryFormat 'kFormatVersion\s*=\s*1'
Assert-Contains 'compact authored stage field remains present' $libraryFormat 'StageData\s+authored'
Assert-Contains 'compact authored provenance field remains present' $libraryFormat 'bool\s+authoredFallback'

Write-Host 'Authored animation preharvest source invariants passed.'
