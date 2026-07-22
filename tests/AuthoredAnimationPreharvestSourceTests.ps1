$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$preharvestPath = Join-Path $repoRoot 'src/redux/WeaponAnimationPreharvest.cpp'
$runtimePath = Join-Path $repoRoot 'src/redux/ReduxRuntime.cpp'
$modePath = Join-Path $repoRoot 'src/redux/MotionPathMode.h'

$preharvest = Get-Content -LiteralPath $preharvestPath -Raw
$runtime = Get-Content -LiteralPath $runtimePath -Raw
$mode = Get-Content -LiteralPath $modePath -Raw

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
Assert-Contains 'direct HKX resource load' $preharvest 'kLoadAnimationResource\s*=\s*0x1728BA0'
Assert-Contains 'native hka sampler slot' $preharvest 'kSampleTracksVtableSlot\s*=\s*5'
Assert-Contains 'first-person exact subgraph selection' $preharvest 'selectFirstPersonGraph'
Assert-Contains 'weapon hierarchy reconstruction' $preharvest 'buildBoneChainBelowAncestor'
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

Assert-Contains 'authored runtime calls only preharvest lane' $runtime 'if\s*\(authoredMode\)[\s\S]*updateAuthoredAnimationPreharvest'
Assert-Contains 'legacy harvesting remains outside authored branch' $runtime 'else\s*\{\s*drainWeaponClipHarvest[\s\S]*updateWeaponClipHarvestWalk'
Assert-Contains 'authored suppresses learned observations' $runtime 'collectLearnedMotion\s*=\s*g_reduxConfig\.motionPathMode\s*!=\s*MotionPathMode::AuthoredOnly'
Assert-Excludes 'runtime contains no hybrid selector' $runtime 'MotionPathMode::Hybrid'
Assert-Excludes 'mode enum contains no hybrid value' $mode '\bHybrid\s*='
Assert-Excludes 'mode parser accepts no hybrid text' $mode 'equalsIgnoreCase\(text,\s*"hybrid"\)'

Write-Host 'Authored animation preharvest source invariants passed.'
