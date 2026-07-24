$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$main = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'src/PAPER_ToolkitMain.cpp')
$drive = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'src/paper_toolkit/WeaponPartDriveSandbox.cpp')

function Assert-Contains([string]$Label, [string]$Text, [string]$Pattern) {
    if ($Text -notmatch $Pattern) {
        throw "${Label}: required pattern '$Pattern' was not found"
    }
}

Assert-Contains 'descriptor-backed owner callback extent' $main 'ROCK_PROVIDER_API_V1_OWNER_FRAME_CALLBACKS_TABLE_BYTES'
Assert-Contains 'required owner API pointers' $main 'registerConsumerV1\s*\|\|[\s\S]{0,220}unregisterConsumerV1\s*\|\|[\s\S]{0,220}registerFrameCallbackForOwnerV1\s*\|\|[\s\S]{0,220}unregisterFrameCallbackForOwnerV1'
Assert-Contains 'frame snapshot request' $main 'requestedCapabilities\s*=\s*static_cast<std::uint32_t>\([\s\S]{0,120}FrameSnapshots'
Assert-Contains 'exact frame grant validation' $main 'hasConsumerCapabilityV1\([\s\S]{0,160}grantedCapabilities[\s\S]{0,160}FrameSnapshots'
Assert-Contains 'partial frame grant rollback' $main 'if \(!granted\)[\s\S]{0,220}unregisterConsumerV1'
Assert-Contains 'owner callback registration' $main 'registerFrameCallbackForOwnerV1\([\s\S]{0,160}&onRockFrame'
Assert-Contains 'callback failure rolls back owner' $main 'callbackResult[\s\S]{0,500}unregisterConsumerV1\([\s\S]{0,180}s_ownerToken\s*=\s*0'
Assert-Contains 'weapon drive exact capability request' $drive 'requestedCapabilities\s*=[\s\S]{0,160}WeaponPartInteraction'
Assert-Contains 'weapon drive exact grant validation' $drive 'hasConsumerCapabilityV1\([\s\S]{0,180}grantedCapabilities[\s\S]{0,160}WeaponPartInteraction'
Assert-Contains 'weapon drive partial-grant rollback' $drive 'if \(!granted\)[\s\S]{0,220}unregisterConsumerV1'

Write-Host 'PAPER Toolkit provider API source invariants passed.'
