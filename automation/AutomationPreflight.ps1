# Read-only artifact checks shared by the runner and its host tests. No HDC,
# Docker, installs, extraction, or launch environment belongs in this file.
function Assert-AutomationOptions {
    param([bool]$SkipBuild, [bool]$SkipInstall, [string]$HapPath,
        [string]$ExpectedHapSha256, [bool]$BatchOverrideRequested, [bool]$BatchMappedFlush)
    if ($BatchOverrideRequested -and -not $BatchMappedFlush) {
        throw 'batchMappedFlush must remain enabled; omit the override to use product policy'
    }
    if ($SkipInstall -and -not $SkipBuild) {
        throw '-SkipInstall requires -SkipBuild (do not build a candidate then test an older installed package)'
    }
    if ($SkipBuild) {
        if (-not $HapPath -or $ExpectedHapSha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw '-SkipBuild requires an explicit -HapPath and -ExpectedHapSha256'
        }
    } elseif ($HapPath -or $ExpectedHapSha256) {
        throw 'An explicit reference HAP requires -SkipBuild; a build uses its own fresh output'
    }
}

function Select-AutomationDevice {
    param([string[]]$Targets)
    $connected = @($Targets | ForEach-Object { "$($_)".Trim() } |
        Where-Object { $_ -and $_ -notmatch '^\[' } |
        ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -Unique)
    if ($connected.Count -ne 1) {
        throw "Expected one connected HDC target, found $($connected.Count); specify -DeviceId"
    }
    return $connected[0]
}

function Select-AutomationAppPid {
    param([string[]]$ProcessLines, [string]$Bundle)
    $rows = @($ProcessLines | ForEach-Object {
        if ($_ -match '^\s*(\d+)\s+(\d+)\s+(\S+)\s*$' -and $Matches[3] -ceq $Bundle) {
            [pscustomobject]@{ pid = [string]$Matches[1]; parent = [string]$Matches[2] }
        }
    })
    # Forked native/Wine children share the bundle process name. pidof alone
    # returns all of them, not necessarily the Ability process.
    $ids = @($rows | ForEach-Object { $_.pid })
    $roots = @($rows | Where-Object { $_.parent -notin $ids })
    if ($roots.Count -ne 1) { throw 'Cannot identify one application process-tree root' }
    return $roots[0].pid
}

function Assert-AutomationMounts {
    param([object[]]$Mounts, [string]$RepoWsl, [string]$ContainerRepo)
    $source = @($Mounts | Where-Object { $_.Type -eq 'bind' -and $_.Destination -eq $ContainerRepo -and
        $_.Source -eq $RepoWsl -and $_.RW })
    $sdk = @($Mounts | Where-Object { $_.Type -eq 'bind' -and $_.Destination -eq '/apps/harmony' -and -not $_.RW })
    if ($source.Count -ne 1 -or ($Mounts.Count -ne 1 -and -not ($sdk.Count -eq 1 -and $Mounts.Count -eq 2))) {
        throw 'Container mounts do not match the selected ext4 source and optional read-only SDK'
    }
}

function Get-InstalledBundleVersion {
    param([string]$BundleDump)
    $start = $BundleDump.IndexOf('{')
    if ($start -lt 0) { throw 'Installed product bundle was not found' }
    # Harmony reports both metaData and metadata. Keep case-sensitive JSON keys
    # without copying the bundle's unrelated private fields into the result.
    $installed = $BundleDump.Substring($start) | ConvertFrom-Json -AsHashtable
    if (-not $installed.versionName -or [long]$installed.versionCode -le 0) {
        throw 'Installed product version is missing'
    }
    return [pscustomobject]@{ versionName=$installed.versionName; versionCode=$installed.versionCode }
}

function Get-ReferenceHapMetadata {
    param([string]$HapPath, [string]$ExpectedHapSha256, [string]$Bundle, [switch]$AllowDualAbi)
    $file = Get-Item -LiteralPath $HapPath -ErrorAction Stop
    if ($file.PSIsContainer -or $file.Length -eq 0) { throw 'Reference HAP is empty or not a file' }
    $actualHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ExpectedHapSha256.ToLowerInvariant()) { throw 'Reference HAP SHA-256 mismatch' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($file.FullName)
    try {
        $manifest = $zip.GetEntry('module.json')
        if (-not $manifest) { throw 'HAP module.json is missing' }
        $reader = [IO.StreamReader]::new($manifest.Open())
        try { $module = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($module.app.bundleName -ne $Bundle -or $module.module.name -ne 'entry') {
            throw 'HAP bundle/module does not match the current product'
        }
        if ([int]$module.app.minAPIVersion % 1000 -lt 23 -or
            [int]$module.app.targetAPIVersion % 1000 -lt 23) { throw 'HAP requires API 23 or newer' }
        $extraAbis = @($zip.Entries | Where-Object { $_.FullName -match '^libs/(?!arm64-v8a/)[^/]+/' })
        if ($extraAbis.Count -and -not $AllowDualAbi) {
            throw 'This physical-device runner requires an ARM64-only HAP'
        }
        if (@($extraAbis | Where-Object { $_.FullName -notmatch '^libs/x86_64/' }).Count) {
            throw 'Unsupported native ABI in HAP'
        }
        $abis = @('arm64-v8a')
        if ($extraAbis.Count) { $abis += 'x86_64' }
        foreach ($abi in $abis) {
          $libraries = @('libentry.so', 'libwine_child.so', 'libwinehua_vtest_server.so',
            'libvirglrenderer.so.1', 'libvirgl_child.so')
          if ($abi -eq 'arm64-v8a') { $libraries += 'box64.so' }
          $machine = if ($abi -eq 'arm64-v8a') { 183 } else { 62 }
          foreach ($library in $libraries) {
            $entry = $zip.GetEntry("libs/$abi/$library")
            if (-not $entry -or $entry.Length -lt 64) { throw "HAP missing native payload: $library" }
            $stream = $entry.Open()
            try {
                $header = New-Object byte[] 64
                $offset = 0
                while ($offset -lt $header.Length) {
                    $read = $stream.Read($header, $offset, $header.Length - $offset)
                    if ($read -eq 0) { throw "Truncated ELF header: $library" }
                    $offset += $read
                }
                if ($header[0] -ne 0x7f -or $header[1] -ne 69 -or $header[2] -ne 76 -or
                    $header[3] -ne 70 -or $header[4] -ne 2 -or $header[5] -ne 1 -or
                    $header[18] -ne $machine -or $header[19] -ne 0) { throw "Wrong ELF architecture for ${abi}: $library" }
            } finally { $stream.Dispose() }
          }
        }
        $runtime = $zip.GetEntry('resources/rawfile/wine-data.zip')
        if (-not $runtime -or $runtime.Length -eq 0) { throw 'HAP guest runtime is missing' }
        $runtimeStream = $runtime.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $runtimeHash = ([BitConverter]::ToString($sha.ComputeHash($runtimeStream))).Replace('-', '').ToLowerInvariant()
        } finally { $sha.Dispose(); $runtimeStream.Dispose() }
        return [ordered]@{
            schemaVersion = 2
            hap = $file.FullName
            hapSha256 = $actualHash
            hapSize = $file.Length
            hapTimestampUtc = $file.LastWriteTimeUtc.ToString('o')
            bundle = $module.app.bundleName
            versionName = $module.app.versionName
            versionCode = $module.app.versionCode
            minAPIVersion = $module.app.minAPIVersion
            targetAPIVersion = $module.app.targetAPIVersion
            hostArchitecture = 'AArch64'
            packagedAbis = $abis
            rawfileSha256 = $runtimeHash
            provenance = 'explicit-hash-pinned-reference'
            # A reference is previously qualified, not a new build of this checkout.
            signatureVerification = 'requires-prior-verification'
            nestedRuntimeValidation = 'requires-prior-verification'
        }
    } finally { $zip.Dispose() }
}
