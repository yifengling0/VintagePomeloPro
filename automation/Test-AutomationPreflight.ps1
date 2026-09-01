#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AutomationPreflight.ps1')

function Assert-Rejected {
    param([scriptblock]$Action, [string]$Reason)
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw "Accepted invalid input: $Reason" }
}

$pin = 'a' * 64
Assert-AutomationOptions $true $true 'reference.hap' $pin $false $false
Assert-AutomationOptions $false $false '' '' $true $true
Assert-Rejected { Assert-AutomationOptions $true $false '' '' $false $false } 'implicit stale HAP'
Assert-Rejected { Assert-AutomationOptions $false $true '' '' $false $false } 'build without install'
Assert-Rejected { Assert-AutomationOptions $true $true 'reference.hap' $pin $true $false } 'batching off'
Assert-Rejected { Assert-AutomationOptions $false $false 'reference.hap' $pin $false $false } 'build/reference ambiguity'
Assert-Rejected { Assert-AutomationOptions $true $false 'reference.hap' 'not-a-hash' $false $false } 'malformed pin'
if ((Select-AutomationDevice @('test-only-target')) -ne 'test-only-target') { throw 'Single-device selection failed' }
Assert-Rejected { Select-AutomationDevice @('[Empty]') } 'no device'
Assert-Rejected { Select-AutomationDevice @('test-a', 'test-b') } 'ambiguous physical targets'
Assert-Rejected { Select-AutomationDevice @('test-a', '127.0.0.1:1234') } 'implicit emulator selection'
$sameNameProcesses = @('PID PPID NAME', '101 10 test.bundle', '102 101 test.bundle',
    '103 102 test.bundle', '99 10 other.bundle')
if ((Select-AutomationAppPid $sameNameProcesses 'test.bundle') -ne '101') { throw 'Fork children confused App PID selection' }
Assert-Rejected { Select-AutomationAppPid @('101 10 test.bundle', '105 10 test.bundle') 'test.bundle' } 'multiple app roots'
Assert-Rejected { Select-AutomationAppPid @('[Fail] disconnected') 'test.bundle' } 'missing app process'
$source = [pscustomobject]@{ Type='bind'; Source='/home/test/repo'; Destination='/data/src/winehua'; RW=$true }
$sdk = [pscustomobject]@{ Type='bind'; Source='/home/test/sdk'; Destination='/apps/harmony'; RW=$false }
Assert-AutomationMounts @($source) '/home/test/repo' '/data/src/winehua'
Assert-AutomationMounts @($source, $sdk) '/home/test/repo' '/data/src/winehua'
Assert-Rejected { Assert-AutomationMounts @($source) '/home/test/other' '/data/src/winehua' } 'wrong source'
$sdk.RW = $true
Assert-Rejected { Assert-AutomationMounts @($source, $sdk) '/home/test/repo' '/data/src/winehua' } 'writable SDK'
$source.Type = 'volume'
Assert-Rejected { Assert-AutomationMounts @($source) '/home/test/repo' '/data/src/winehua' } 'named source volume'
$installed = Get-InstalledBundleVersion 'bundle dump: {"versionName":"1.3.2","versionCode":1003002,"metaData":[],"metadata":{}}'
if ($installed.versionCode -ne 1003002 -or $installed.versionName -ne '1.3.2') { throw 'Case-sensitive bundle JSON failed' }
Assert-Rejected { Get-InstalledBundleVersion 'bundle not found' } 'missing installed bundle'
Assert-Rejected { Get-InstalledBundleVersion '{"metadata":{}}' } 'missing installed version'
Assert-Rejected { Get-InstalledBundleVersion '{"versionName":' } 'truncated installed JSON'

# Exercise the real HDC wrapper, but never invoke a device command in this test.
$runnerAst = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'Invoke-WineHuaAutomation.ps1'), [ref]$null, [ref]$null)
$hdcFunction = $runnerAst.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-Hdc'
}, $false)
. ([scriptblock]::Create($hdcFunction.Extent.Text))
$Hdc = 'Invoke-PreflightMockHdc'
$script:DeviceId = 'synthetic-target'
$script:MockHdcExit = 0
$script:MockHdcText = '[Fail] selected target is disconnected'
function Invoke-PreflightMockHdc {
    $global:LASTEXITCODE = $script:MockHdcExit
    return $script:MockHdcText
}
Assert-Rejected { Invoke-Hdc shell ps } 'HDC zero-exit disconnect message'
$script:MockHdcText = 'PID PPID NAME'
$script:MockHdcExit = 1
Assert-Rejected { Invoke-Hdc shell ps } 'HDC nonzero exit'
$script:MockHdcExit = 0
if ((Invoke-Hdc shell ps) -ne 'PID PPID NAME') { throw 'HDC success output was lost' }

# Synthetic archives exercise metadata validation, not signing/runtime qualification.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('winehua-preflight-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function New-TestHap {
    param([string]$Name, [string]$Bundle='com.vintage.pomelopro', [int]$Api=60100023,
        [int]$Machine=183, [string]$Omit='', [switch]$ExtraAbi, [switch]$DualAbi,
        [int]$ExtraMachine=62)
    $path = Join-Path $testRoot "$Name.hap"
    $zip = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $manifest = @{ app=@{bundleName=$Bundle; versionName='1.3.2'; versionCode=1003002;
            minAPIVersion=$Api; targetAPIVersion=$Api}; module=@{name='entry'} } | ConvertTo-Json -Depth 4 -Compress
        $entries = @{'module.json'=[Text.Encoding]::UTF8.GetBytes($manifest);
            'resources/rawfile/wine-data.zip'=[byte[]]@(80,75,5,6)}
        $header = New-Object byte[] 64
        $header[0]=127; $header[1]=69; $header[2]=76; $header[3]=70
        $header[4]=2; $header[5]=1; $header[18]=$Machine
        foreach ($lib in @('libentry.so', 'libwine_child.so', 'libwinehua_vtest_server.so',
            'libvirglrenderer.so.1', 'libvirgl_child.so', 'box64.so')) {
            $entries["libs/arm64-v8a/$lib"] = $header
        }
        if ($ExtraAbi) { $entries['libs/x86_64/libentry.so'] = $header }
        if ($DualAbi) {
            $extraHeader = $header.Clone()
            $extraHeader[18] = $ExtraMachine
            foreach ($lib in @('libentry.so', 'libwine_child.so', 'libwinehua_vtest_server.so',
                'libvirglrenderer.so.1', 'libvirgl_child.so')) {
                $entries["libs/x86_64/$lib"] = $extraHeader
            }
        }
        foreach ($name in $entries.Keys) {
            if ($name -eq $Omit) { continue }
            $stream = $zip.CreateEntry($name).Open()
            try { $stream.Write($entries[$name], 0, $entries[$name].Length) } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose() }
    return $path
}
function Read-TestHap {
    param([string]$Path, [switch]$AllowDualAbi)
    Get-ReferenceHapMetadata $Path (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash 'com.vintage.pomelopro' -AllowDualAbi:$AllowDualAbi
}
try {
    $valid = New-TestHap 'valid'
    $meta = Read-TestHap $valid
    if ($meta.versionCode -ne 1003002 -or $meta.provenance -ne 'explicit-hash-pinned-reference') {
        throw 'Valid reference metadata failed'
    }
    Assert-Rejected { Get-ReferenceHapMetadata $valid $pin 'com.vintage.pomelopro' } 'wrong HAP hash'
    Assert-Rejected { Read-TestHap (New-TestHap 'bundle' -Bundle 'test.other') } 'wrong bundle'
    Assert-Rejected { Read-TestHap (New-TestHap 'api' -Api 60100022) } 'API below 23'
    Assert-Rejected { Read-TestHap (New-TestHap 'arch' -Machine 62) } 'wrong ELF architecture'
    Assert-Rejected { Read-TestHap (New-TestHap 'missing' -Omit 'libs/arm64-v8a/box64.so') } 'missing native payload'
    Assert-Rejected { Read-TestHap (New-TestHap 'runtime' -Omit 'resources/rawfile/wine-data.zip') } 'missing runtime'
    Assert-Rejected { Read-TestHap (New-TestHap 'mixed' -ExtraAbi) } 'mixed ABI package'
    $dual = New-TestHap 'dual' -DualAbi
    Assert-Rejected { Read-TestHap $dual } 'dual ABI requires explicit opt-in'
    $dualMeta = Read-TestHap $dual -AllowDualAbi
    if (($dualMeta.packagedAbis -join ',') -ne 'arm64-v8a,x86_64') { throw 'Dual ABI inventory mismatch' }
    Assert-Rejected { Read-TestHap (New-TestHap 'dual-wrong' -DualAbi -ExtraMachine 183) -AllowDualAbi } 'wrong x86_64 ELF'
    Assert-Rejected { Read-TestHap (New-TestHap 'dual-missing' -DualAbi -Omit 'libs/x86_64/libvirgl_child.so') -AllowDualAbi } 'incomplete x86_64 payload'
    # Execute the real entrypoint's read-only path. It must not need HDC/Docker or
    # create an archive directory, even when an install would follow normally.
    $archive = Join-Path $testRoot 'must-not-exist'
    $plan = & (Join-Path $PSScriptRoot 'Invoke-WineHuaAutomation.ps1') -SkipBuild -SkipInstall `
        -PreflightOnly -HapPath $valid -ExpectedHapSha256 $meta.hapSha256 -ArchiveRoot $archive |
        ConvertFrom-Json
    if ($plan.build -or $plan.install -or (Test-Path -LiteralPath $archive) -or
        $plan.bundle -ne 'com.vintage.pomelopro') { throw 'Preflight caused side effects or selected wrong mode' }
    Write-Host 'Automation preflight PASS (options, target isolation, mounts, pinned HAP identity/ABI, read-only entrypoint)'
} finally {
    # Only remove the unique test directory allocated above, never a repo/cache.
    $resolved = (Resolve-Path -LiteralPath $testRoot).Path
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^winehua-preflight-[0-9a-f]{32}$') { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
