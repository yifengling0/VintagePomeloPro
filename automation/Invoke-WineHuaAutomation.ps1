#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('core', 'audio', 'opengl', 'd3d8', 'd3d9', 'host-vulkan', 'host-heaven', 'host-heaven-material-depth', 'host-heaven-inputs', 'venus', 'venus-sampled', 'venus-sampled-idle', 'venus-depth-cube', 'venus-depth-cube-array-2d-golden', 'venus-depth-cube-graphics', 'venus-heaven-material', 'venus-heaven-material-depth', 'venus-heaven-captured', 'venus-heaven-inputs', 'venus-heaven-captured-ab', 'venus-heaven-discard-ab', 'venus-heaven-material-layout', 'venus-heaven-draw0', 'venus-heaven-draw170', 'venus-heaven-f647', 'capabilities', 'wine-vulkan', 'wine-vulkan-present', 'dxvk', 'dxvk-long', 'dxvk-replay', 'dxvk-layout-general', 'dxvk-combined', 'dxvk-dynamic', 'all', 'long')]
    [string]$Suite = 'core',
    [ValidateSet('reuse', 'clean')]
    [string]$Prefix = 'reuse',
    [ValidateNotNullOrEmpty()]
    [ValidatePattern('^[a-z0-9-]+$')]
    [string]$GraphicsExperiment = 'observe-product-summary',
    [int]$Runs = 1,
    [ValidateRange(60, 3600)]
    [int]$LongSeconds = 3600,
    [switch]$Gate,
    [switch]$SkipBuild,
    [switch]$SkipInstall,
    [switch]$AllowDualAbi,
    [switch]$PreflightOnly,
    [string]$HapPath = '',
    [string]$ExpectedHapSha256 = '',
    [string]$RepoWsl = '',
    [string]$Container = 'vp-build',
    [string]$WslDistro = 'Ubuntu',
    [switch]$BatchMappedFlush,
    [string]$DeviceId = '',
    [string]$HdcPath = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe',
    [string]$ReplayFragmentSpv = '',
    [string]$ReplayVertexSpv = '',
    [string]$ArchiveRoot = '',
    [int]$TimeoutMinutes = 15
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AutomationPreflight.ps1')
. (Join-Path $PSScriptRoot 'NormalSmoke.ps1')
$batchMappedFlushOverrideRequested = $PSBoundParameters.ContainsKey('BatchMappedFlush')
Assert-AutomationOptions -SkipBuild $SkipBuild -SkipInstall $SkipInstall -HapPath $HapPath `
    -ExpectedHapSha256 $ExpectedHapSha256 -BatchOverrideRequested $batchMappedFlushOverrideRequested `
    -BatchMappedFlush $BatchMappedFlush
$SourceRepo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$product = Get-Content -Raw -LiteralPath (Join-Path $SourceRepo 'AppScope/app.json5') | ConvertFrom-Json
$ContainerRepo = '/data/src/winehua'
$Bundle = [string]$product.app.bundleName
if ($Bundle -notmatch '^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)+$') { throw 'Invalid product bundle name' }
if (-not $ArchiveRoot) { $ArchiveRoot = Join-Path $SourceRepo '.hvigor/outputs/automation' }
$Ability = 'EntryAbility'
$Hdc = $HdcPath
$HapWsl = "$RepoWsl/entry/build/default/outputs/default/entry-default-signed.hap"
$HapWindows = $HapPath
if (-not $SkipBuild) {
    if ($RepoWsl -notmatch '^/(home|opt|srv)/[A-Za-z0-9_./-]+$' -or $RepoWsl -match '/\.\.?(/|$)') {
        throw 'Building requires an explicit ext4 -RepoWsl path; no Windows mount or implicit clone'
    }
    $HapWindows = "\\wsl.localhost\$WslDistro$($HapWsl.Replace('/', '\'))"
}
$DeviceSandbox = "/data/app/el2/100/base/$Bundle"

function Invoke-NativeChecked {
    param([scriptblock]$Command, [string]$Description)
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE" }
}

function Invoke-Hdc {
    $output = @(& $Hdc -t $script:DeviceId @args)
    # HDC can print [Fail] while returning exit code zero after USB loss.
    # Never mistake an empty process list from that failure for successful stop.
    if ($LASTEXITCODE -ne 0 -or @($output | Where-Object { $_ -match '^\[Fail\]' }).Count) {
        throw 'HDC request failed; check the selected target connection'
    }
    return $output
}

function Stop-AutomationApp {
    Invoke-Hdc shell aa force-stop $Bundle | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not stop the previous application session' }
    for ($attempt = 0; $attempt -lt 25; ++$attempt) {
        $processes = @(Invoke-Hdc shell ps -A -o 'PID,PPID,NAME')
        if ($LASTEXITCODE -ne 0) { throw 'Could not verify application process-tree shutdown' }
        if (@($processes | Where-Object { $_ -match [regex]::Escape($Bundle) }).Count -eq 0) {
            Start-Sleep -Milliseconds 300
            return
        }
        Start-Sleep -Milliseconds 200
    }
    throw 'Previous application/native child session did not stop; refusing an overlapping run'
}

function Get-DeviceText {
    param([string]$RemotePath)
    $text = & $Hdc -t $script:DeviceId shell cat $RemotePath 2>$null
    if (@($text | Where-Object { $_ -match '^\[Fail\]|Permission denied' }).Count) {
        throw 'HDC result read failed: target disconnected or result path inaccessible'
    }
    if ($LASTEXITCODE -ne 0) { return '' }
    $joined = ($text -join "`n").Trim()
    $start = $joined.IndexOf('{')
    $end = $joined.LastIndexOf('}')
    if ($start -lt 0 -or $end -lt $start) { return '' }
    return $joined.Substring($start, $end - $start + 1)
}

function Save-DeviceFile {
    param([string]$RemotePath, [string]$LocalPath)
    $parent = Split-Path -Parent $LocalPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    & $Hdc -t $script:DeviceId file recv $RemotePath $LocalPath *> $null
}

function Test-FixedFrame {
    param([string]$ImagePath, [string]$JsonPath)
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::new($ImagePath)
    try {
        $classes = @{
            red    = @{ Count = 0L; X = 0L; Y = 0L }
            green  = @{ Count = 0L; X = 0L; Y = 0L }
            blue   = @{ Count = 0L; X = 0L; Y = 0L }
            yellow = @{ Count = 0L; X = 0L; Y = 0L }
        }
        $step = 4
        for ($y = 0; $y -lt $bitmap.Height; $y += $step) {
            for ($x = 0; $x -lt $bitmap.Width; $x += $step) {
                $pixel = $bitmap.GetPixel($x, $y)
                $name = $null
                if ($pixel.R -gt 170 -and $pixel.G -lt 100 -and $pixel.B -lt 120) { $name = 'red' }
                elseif ($pixel.G -gt 150 -and $pixel.R -lt 120 -and $pixel.B -lt 130) { $name = 'green' }
                # Desktop blue also occupies most of a windowed screenshot.
                # Match the saturated probe blue, not the surrounding desktop.
                elseif ($pixel.B -gt 200 -and $pixel.R -lt 90 -and $pixel.G -lt 100) { $name = 'blue' }
                elseif ($pixel.R -gt 170 -and $pixel.G -gt 140 -and $pixel.B -lt 120) { $name = 'yellow' }
                if ($name) {
                    $classes[$name].Count++
                    $classes[$name].X += $x
                    $classes[$name].Y += $y
                }
            }
        }
        $sampleCount = [math]::Ceiling($bitmap.Width / $step) * [math]::Ceiling($bitmap.Height / $step)
        $minimum = [math]::Max(80, [int]($sampleCount * 0.003))
        $centroids = @{}
        $enough = $true
        foreach ($name in @('red', 'green', 'blue', 'yellow')) {
            $entry = $classes[$name]
            if ($entry.Count -lt $minimum) { $enough = $false }
            $centroids[$name] = @{
                count = $entry.Count
                x = if ($entry.Count) { [double]$entry.X / $entry.Count } else { -1 }
                y = if ($entry.Count) { [double]$entry.Y / $entry.Count } else { -1 }
            }
        }
        # The OHOS presentation transform follows the display's native orientation.
        # A landscape snapshot can therefore contain a 90/180/270 degree rotation of
        # the canonical Vulkan framebuffer.  Require the exact four-colour topology,
        # but accept rotations; a reflection or duplicated/missing quadrant still
        # fails the visual gate.
        $centerX = ($centroids.red.x + $centroids.green.x + $centroids.blue.x + $centroids.yellow.x) / 4
        $centerY = ($centroids.red.y + $centroids.green.y + $centroids.blue.y + $centroids.yellow.y) / 4
        $quadrants = @{}
        foreach ($name in @('red', 'green', 'blue', 'yellow')) {
            $column = if ($centroids[$name].x -lt $centerX) { 'L' } else { 'R' }
            $row = if ($centroids[$name].y -lt $centerY) { 'T' } else { 'B' }
            $quadrants["$row$column"] = $name
        }
        $layouts = @(
            @{ name = 'identity';  TL = 'red';    TR = 'green';  BL = 'blue';   BR = 'yellow' },
            @{ name = 'rotate90';  TL = 'blue';   TR = 'red';    BL = 'yellow'; BR = 'green' },
            @{ name = 'rotate180'; TL = 'yellow'; TR = 'blue';   BL = 'green';  BR = 'red' },
            @{ name = 'rotate270'; TL = 'green';  TR = 'yellow'; BL = 'red';    BR = 'blue' }
        )
        $detectedTransform = $null
        foreach ($layout in $layouts) {
            if ($quadrants.Count -eq 4 -and
                $quadrants.TL -eq $layout.TL -and $quadrants.TR -eq $layout.TR -and
                $quadrants.BL -eq $layout.BL -and $quadrants.BR -eq $layout.BR) {
                $detectedTransform = $layout.name
                break
            }
        }
        $xValues = @($centroids.red.x, $centroids.green.x, $centroids.blue.x, $centroids.yellow.x)
        $yValues = @($centroids.red.y, $centroids.green.y, $centroids.blue.y, $centroids.yellow.y)
        $separatedColumns = (($xValues | Measure-Object -Maximum).Maximum - ($xValues | Measure-Object -Minimum).Minimum) -gt ($bitmap.Width * 0.08)
        $separatedRows = (($yValues | Measure-Object -Maximum).Maximum - ($yValues | Measure-Object -Minimum).Minimum) -gt ($bitmap.Height * 0.08)
        $pass = $enough -and $separatedColumns -and $separatedRows -and $null -ne $detectedTransform
        [ordered]@{
            schemaVersion = 1
            status = if ($pass) { 'PASS' } else { 'FAIL' }
            validator = 'rgba-quadrants-v1-rotations'
            image = $ImagePath
            width = $bitmap.Width
            height = $bitmap.Height
            minimumSamplesPerColor = $minimum
            detectedTransform = $detectedTransform
            quadrants = $quadrants
            centroids = $centroids
        } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $JsonPath -Encoding UTF8
        return $pass
    }
    finally {
        $bitmap.Dispose()
    }
}

function Test-D3D11CubeFrame {
    param([string]$ImagePath, [string]$JsonPath)
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::new($ImagePath)
    try {
        $step = 4
        $colored = 0L
        $dark = 0L
        $buckets = @{ red = 0L; green = 0L; blue = 0L }
        $minX = $bitmap.Width
        $minY = $bitmap.Height
        $maxX = -1
        $maxY = -1
        for ($y = 0; $y -lt $bitmap.Height; $y += $step) {
            for ($x = 0; $x -lt $bitmap.Width; $x += $step) {
                $pixel = $bitmap.GetPixel($x, $y)
                $maximum = [math]::Max($pixel.R, [math]::Max($pixel.G, $pixel.B))
                $minimum = [math]::Min($pixel.R, [math]::Min($pixel.G, $pixel.B))
                if ($maximum -lt 55) { $dark++ }
                if ($maximum -gt 100 -and ($maximum - $minimum) -gt 55) {
                    $colored++
                    $minX = [math]::Min($minX, $x)
                    $minY = [math]::Min($minY, $y)
                    $maxX = [math]::Max($maxX, $x)
                    $maxY = [math]::Max($maxY, $y)
                    if ($pixel.R -eq $maximum) { $buckets.red++ }
                    elseif ($pixel.G -eq $maximum) { $buckets.green++ }
                    else { $buckets.blue++ }
                }
            }
        }
        $sampleCount = [math]::Ceiling($bitmap.Width / $step) * [math]::Ceiling($bitmap.Height / $step)
        $minimumColored = [math]::Max(500, [int]($sampleCount * 0.005))
        $activeBuckets = @($buckets.Values | Where-Object { $_ -gt ($minimumColored * 0.08) }).Count
        $boxWidth = if ($maxX -ge $minX) { $maxX - $minX + 1 } else { 0 }
        $boxHeight = if ($maxY -ge $minY) { $maxY - $minY + 1 } else { 0 }
        $pass = $colored -ge $minimumColored -and $activeBuckets -ge 3 -and
            $boxWidth -gt ($bitmap.Width * 0.08) -and $boxHeight -gt ($bitmap.Height * 0.08) -and
            $dark -gt ($sampleCount * 0.03)
        [ordered]@{
            schemaVersion = 1
            status = if ($pass) { 'PASS' } else { 'FAIL' }
            validator = 'd3d11-cube-color-depth-v1'
            image = $ImagePath
            width = $bitmap.Width
            height = $bitmap.Height
            coloredSamples = $colored
            minimumColoredSamples = $minimumColored
            darkSamples = $dark
            activeColorBuckets = $activeBuckets
            colorBuckets = $buckets
            coloredBounds = @{ x = $minX; y = $minY; width = $boxWidth; height = $boxHeight }
        } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $JsonPath -Encoding UTF8
        return $pass
    }
    finally {
        $bitmap.Dispose()
    }
}

function Get-D3D11Coverage {
    param([object]$Summary, [string]$RunSuite)
    $entries = @()
    foreach ($test in @($Summary.tests)) {
        $m = $test.metrics
        # Visual examples (for example dxvk-cube-x64) prove device creation
        # and presentation but intentionally do not duplicate the exhaustive
        # feature metrics emitted by winehua_d3d11_smoke.
        if ($null -eq $m -or $null -eq $m.rgba8SampleMatrix) { continue }
        $checks = [ordered]@{
            featureLevel11 = ($m.featureLevel -eq '11.0')
            shaderModel5 = [bool]$m.shaderModel5
            cubeGeometry = [bool]$m.cubeGeometry
            drawIndexedInstanced = [bool]$m.drawIndexedInstanced
            depthStencil = [bool]$m.depthStencil
            alphaBlend = [bool]$m.alphaBlend
            rasterizerState = [bool]$m.rasterizerState
            constantBuffer = [bool]$m.constantBuffer
            dynamicConstantBuffer = ($RunSuite -ne 'dxvk-dynamic' -or [bool]$m.dynamicConstantBuffer)
            dynamicConstantReadback = ($RunSuite -ne 'dxvk-dynamic' -or [bool]$m.dynamicConstantReadback)
            textureUpdate = [bool]$m.textureUpdate
            textureUploadReadback = [bool]$m.textureUploadReadback
            textureSamplingFunctional = [bool]$m.textureSampling
            rgba8LoadPs = [bool]$m.rgba8SampleMatrix.loadPs.pass
            rgba8LoadCs = [bool]$m.rgba8SampleMatrix.loadCs.pass
            rgba8PointPs = [bool]$m.rgba8SampleMatrix.pointPs.pass
            rgba8PointCs = [bool]$m.rgba8SampleMatrix.pointCs.pass
            rgba8LinearPs = [bool]$m.rgba8SampleMatrix.linearPs.pass
            rgba8LinearCs = [bool]$m.rgba8SampleMatrix.linearCs.pass
            rgba8UpdatedUpload = [bool]$m.rgba8SampleMatrix.updated.uploadPass
            rgba8UpdatedLoadPs = [bool]$m.rgba8SampleMatrix.updated.loadPs.pass
            rgba8UpdatedLoadCs = [bool]$m.rgba8SampleMatrix.updated.loadCs.pass
            rgba8UpdatedPointPs = [bool]$m.rgba8SampleMatrix.updated.pointPs.pass
            rgba8UpdatedPointCs = [bool]$m.rgba8SampleMatrix.updated.pointCs.pass
            descriptorIdentity = [bool]$m.descriptorMatrix.initial.pass
            descriptorRebindDirtyState = [bool]$m.descriptorMatrix.rebind.pass
            descriptorUnbound = [bool]$m.descriptorMatrix.unbound.pass
            descriptorLifetime = [bool]$m.descriptorMatrix.lifetime.pass
            subresourceArrayLayers = [bool]$m.subresourceMatrix.arrayLayers
            subresourceMipLevels = [bool]$m.subresourceMatrix.mipLevels
            subresourceExplicitLod = [bool]$m.subresourceMatrix.explicitLod
            subresourceBarrierUpdate = [bool]$m.subresourceMatrix.barrierUpdate
            subresourceMatrix = [bool]$m.subresourceMatrix.pass
            texture3dCreated = [bool]$m.texture3dMatrix.created
            texture3dUpload = [bool]$m.texture3dMatrix.upload
            texture3dSingleDispatch = [bool]$m.texture3dMatrix.singleDispatch
            texture3dUavToSrvBarrier = [bool]$m.texture3dMatrix.uavToSrvBarrier
            texture3dPingPong = [bool]$m.texture3dMatrix.pingPong
            heavenCubeMatrix = [bool]$m.heavenResourceMatrix.cube.pass
            heavenTexture3dR8 = [bool]$m.heavenResourceMatrix.texture3d.r8.pass
            heavenTexture3dRg8 = [bool]$m.heavenResourceMatrix.texture3d.rg8.pass
            heavenD32DepthComparison = [bool]$m.heavenResourceMatrix.depthComparisonSampler.pass
            heavenD24S8DepthComparison = [bool]$m.heavenResourceMatrix.d24s8DepthComparisonSampler.pass
            heavenD24S8ExtendedMatrix = [bool]$m.heavenResourceMatrix.d24s8ExtendedMatrix.pass
            heavenResourceMatrix = [bool]$m.heavenResourceMatrix.pass
            bcTextureCreated = ($m.bcTextureTest -eq 'created_sampled')
            bcSamplingSubmitted = [bool]$m.bcSamplingSubmitted
            bcSamplingFunctional = [bool]$m.bcSamplingFunctional
            offscreenRenderTarget = [bool]$m.offscreenRenderTarget
            msaa4xSupported = [bool]$m.msaa4xSupported
            msaaResolveFunctional = [bool]$m.msaaResolveFunctional
            stencilQueryEnabled = [bool]$m.stencilQueryEnabled
            stencilPixelFunctional = [bool]$m.stencilPixelFunctional
            stencilQueryFunctional = [bool]$m.stencilFunctional
            computeShaderDispatch = [bool]$m.computeShaderDispatch
            computeUavSubmitted = [bool]$m.computeUavSubmitted
            computeUavFunctional = [bool]$m.computeUavFunctional
            computeSampledImageFunctional = [bool]$m.computeSampledImageFunctional
            longWallClock = ($RunSuite -ne 'dxvk-long' -or
                [int64]$m.durationMs -ge ([int64]$LongSeconds * 1000 - 2000))
            present60Frames = ([int]$m.presentFrames -ge 60)
            presentResultSuccess = ([int]$m.presentResult -eq 0)
            cpuFullFrameReadbackZero = ([int]$m.cpuReadBytes -eq 0)
            cpuFullFrameUploadZero = ([int]$m.cpuUploadBytes -eq 0)
            perFrameDeviceWaitIdleZero = ([int]$m.perFrameDeviceWaitIdle -eq 0)
            noFallback = (-not [bool]$m.fallbackDetected)
        }
        $missing = @($checks.GetEnumerator() | Where-Object { -not [bool]$_.Value } | ForEach-Object { $_.Key })
        $submittedOnly = @()
        if ([bool]$m.bcSamplingSubmitted -and -not [bool]$m.bcSamplingFunctional) { $submittedOnly += 'bcSampling' }
        if ([bool]$m.computeUavSubmitted -and -not [bool]$m.computeUavFunctional) { $submittedOnly += 'computeUav' }
        $entries += [ordered]@{
            testId = $test.testId
            appStatus = $test.status
            requiredPass = ($missing.Count -eq 0)
            missingRequired = $missing
            submittedOnly = $submittedOnly
            metrics = [ordered]@{
                presentFrames = [int]$m.presentFrames
                queueSubmitCount = [int]$m.queueSubmitCount
                featureProbeReadBytes = [int]$m.featureProbeReadBytes
                featureProbeGpuCopies = [int]$m.featureProbeGpuCopies
                durationMs = [int64]$m.durationMs
                rgba8SampleMatrix = $m.rgba8SampleMatrix
                descriptorMatrix = $m.descriptorMatrix
                subresourceMatrix = $m.subresourceMatrix
                heavenResourceMatrix = $m.heavenResourceMatrix
                cpuReadBytes = [int]$m.cpuReadBytes
                cpuUploadBytes = [int]$m.cpuUploadBytes
            }
        }
    }
    $requiredPass = ($entries.Count -gt 0 -and
        @($entries | Where-Object { -not $_.requiredPass }).Count -eq 0)
    return [ordered]@{
        schemaVersion = 1
        suite = $RunSuite
        status = if ($requiredPass) { 'PASS' } else { 'FAIL' }
        tests = $entries
        policy = 'required API/object/RGBA8 Load-POINT-LINEAR PS-CS/descriptor/subresource array-mip-explicit-LOD-update/texture sampling/D24S8 2D-array-per-view-cube-cube-array-linear-border/present/readback coverage; ordinary R32_FLOAT comparison, optional MSAA resolve, and stencil query are reported separately'
    }
}

function Capture-D3D11Frame {
    param([string]$RemoteImage, [string]$LocalImage, [string]$JsonPath)
    $lastJson = $null
    for ($attempt = 0; $attempt -lt 4; $attempt++) {
        if ($attempt -gt 0) { Start-Sleep -Milliseconds 750 }
        Invoke-Hdc shell snapshot_display -f $RemoteImage | Out-Null
        Save-DeviceFile $RemoteImage $LocalImage
        Invoke-Hdc shell rm $RemoteImage | Out-Null
        $attemptJson = "$JsonPath.attempt$attempt"
        $lastJson = $attemptJson
        if (Test-D3D11CubeFrame -ImagePath $LocalImage -JsonPath $attemptJson) {
            Copy-Item -LiteralPath $attemptJson -Destination $JsonPath -Force
            return $true
        }
    }
    if ($lastJson -and (Test-Path -LiteralPath $lastJson)) {
        Copy-Item -LiteralPath $lastJson -Destination $JsonPath -Force
    }
    return $false
}

function Assert-BuildEnvironment {
    $mountsText = wsl -d $WslDistro -- docker inspect $Container --format '{{json .Mounts}}'
    if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect build container' }
    $mounts = $mountsText | ConvertFrom-Json
    # The existing vp-build image can contain its SDK; a read-only SDK bind is
    # also supported. Never recreate a container just to match an old layout.
    Assert-AutomationMounts -Mounts @($mounts) -RepoWsl $RepoWsl -ContainerRepo $ContainerRepo
}

function Invoke-Build {
    param([string]$LogPath)
    Assert-BuildEnvironment
    wsl -d $WslDistro -- docker start $Container | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Unable to start the existing build container' }
    wsl -d $WslDistro -- docker exec $Container test -x /apps/harmony/sdk/default/openharmony/native/llvm/bin/clang
    if ($LASTEXITCODE -ne 0) { throw 'Existing container has no usable Harmony SDK' }
    $script:BuildStartedUtc = [DateTime]::UtcNow
    $output = & wsl -d $WslDistro -- docker exec -w $ContainerRepo $Container make hap NATIVE_ARCH=arm64-v8a 2>&1
    $exitCode = $LASTEXITCODE
    $redacted = $output | ForEach-Object {
        $_ -replace '(-keyPwd\s+)\S+', '$1<redacted>' -replace '(-keystorePwd\s+)\S+', '$1<redacted>'
    }
    $redacted | Set-Content -LiteralPath $LogPath -Encoding UTF8
    if ($exitCode -ne 0) { throw "Docker build failed; see $LogPath" }
    if ((Get-Item -LiteralPath $HapWindows).LastWriteTimeUtc -lt $script:BuildStartedUtc) {
        throw 'Build did not produce a fresh signed HAP; refusing the older artifact'
    }
}

function Get-ArtifactMetadata {
    param([string]$OutputDirectory)
    $stat = wsl -d $WslDistro -- stat -c '%y %s' $HapWsl
    if ($LASTEXITCODE -ne 0) { throw 'Signed HAP does not exist' }
    $hapHash = ((wsl -d $WslDistro -- sha256sum $HapWsl) -split '\s+')[0]
    if ($LASTEXITCODE -ne 0 -or $hapHash -notmatch '^[0-9a-f]{64}$') { throw 'Could not hash the built HAP' }
    $identity = Get-ReferenceHapMetadata -HapPath $HapWindows -ExpectedHapSha256 $hapHash -Bundle $Bundle -AllowDualAbi:$AllowDualAbi
    $rawHash = ((wsl -d $WslDistro -- sha256sum "$RepoWsl/entry/src/main/resources/rawfile/wine-data.zip") -split '\s+')[0]
    if ($LASTEXITCODE -ne 0 -or $rawHash -notmatch '^[0-9a-f]{64}$') { throw 'Could not hash the assembled runtime' }
    $embeddedHash = $identity.rawfileSha256
    if ($rawHash -ne $embeddedHash) { throw 'HAP embedded wine-data.zip hash does not match assembled payload' }

    $smokeList = wsl -d $WslDistro -- unzip -l "$RepoWsl/entry/src/main/resources/rawfile/wine-data.zip"
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the assembled runtime archive' }
    foreach ($required in @('smoke/manifest.json', 'smoke/x64/winehua_audio_smoke.exe',
        'smoke/x86/winehua_audio_smoke.exe', 'smoke/x64/winehua_graphics_smoke.exe',
        'smoke/x86/winehua_graphics_smoke.exe', 'smoke/x64/winehua_vulkan_smoke.exe',
        'smoke/x86/winehua_vulkan_smoke.exe',
        'smoke/x64/winehua_d3d8_smoke.exe', 'smoke/x86/winehua_d3d8_smoke.exe',
        'smoke/x64/winehua_d3d_switch_cube.exe', 'smoke/x86/winehua_d3d_switch_cube.exe',
        'smoke/x64/winehua_d3d11_smoke.exe', 'smoke/x86/winehua_d3d11_smoke.exe',
        'dxvk/manifest.json',
        'dxvk/legacy/x64/d3d11.dll', 'dxvk/legacy/x64/dxgi.dll',
        'dxvk/legacy/x86/d3d11.dll', 'dxvk/legacy/x86/dxgi.dll',
        'bin/guest_vulkan/lib/libvulkan.so.1',
        'bin/guest_vulkan/lib/libvulkan_virtio.so',
        'bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json')) {
        if (-not ($smokeList -match [regex]::Escape($required))) { throw "Payload missing $required" }
    }

    $guestArch = wsl -d $WslDistro -- bash -lc "unzip -p '$RepoWsl/entry/src/main/resources/rawfile/wine-data.zip' bin/guest_gfx/lib/libEGL.so.1 | file -"
    $hostArch = $identity.hostArchitecture
    if ($guestArch -notmatch 'x86-64') { throw "Guest EGL architecture invalid: $guestArch" }

    $mainCommit = wsl -d $WslDistro -- git -C $RepoWsl rev-parse HEAD
    $submodules = wsl -d $WslDistro -- git -C $RepoWsl submodule status --recursive
    $dirty = wsl -d $WslDistro -- git -C $RepoWsl status --short
    $metadata = [ordered]@{
        schemaVersion = 1
        hap = $HapWsl
        hapTimestampAndSize = $stat
        hapSha256 = $hapHash
        rawfileSha256 = $rawHash
        buildMirrorCommit = $mainCommit
        bundle = $identity.bundle
        versionName = $identity.versionName
        versionCode = $identity.versionCode
        minAPIVersion = $identity.minAPIVersion
        targetAPIVersion = $identity.targetAPIVersion
        submodules = @($submodules)
        dirtySummary = @($dirty)
        guestArchitecture = $guestArch
        hostArchitecture = $hostArch
    }
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'artifact.json') -Encoding UTF8
    return $metadata
}

function Get-CanonicalCapabilities {
    param([object]$Capabilities)
    return [ordered]@{
        deviceApiVersion = [string]$Capabilities.deviceApiVersion
        pushConstantBytes = [int]$Capabilities.pushConstantBytes
        geometryShader = [bool]$Capabilities.geometryShader
        tessellationShader = [bool]$Capabilities.tessellationShader
        multiDrawIndirect = [bool]$Capabilities.multiDrawIndirect
        descriptorIndexing = [bool]$Capabilities.descriptorIndexing
        scalarBlockLayout = [bool]$Capabilities.scalarBlockLayout
        robustness2 = [bool]$Capabilities.robustness2
        transformFeedback = [bool]$Capabilities.transformFeedback
        shaderInt8 = [bool]$Capabilities.shaderInt8
        shaderInt16 = [bool]$Capabilities.shaderInt16
        shaderInt64 = [bool]$Capabilities.shaderInt64
        timelineSemaphore = [bool]$Capabilities.timelineSemaphore
        synchronization2 = [bool]$Capabilities.synchronization2
        dynamicRendering = [bool]$Capabilities.dynamicRendering
        maintenance4 = [bool]$Capabilities.maintenance4
        maintenance5 = [bool]$Capabilities.maintenance5
        maintenance6 = [bool]$Capabilities.maintenance6
        presentWait = [bool]$Capabilities.presentWait
        swapchainMaintenance = [bool]$Capabilities.swapchainMaintenance
        customBorderColorExtension = [bool]$Capabilities.customBorderColorExtension
        customBorderColors = [bool]$Capabilities.customBorderColors
        customBorderColorWithoutFormat = [bool]$Capabilities.customBorderColorWithoutFormat
        bc1 = [bool]$Capabilities.bc1
        bc2 = [bool]$Capabilities.bc2
        bc3 = [bool]$Capabilities.bc3
        bc4 = [bool]$Capabilities.bc4
        bc5 = [bool]$Capabilities.bc5
        bc6 = [bool]$Capabilities.bc6
        bc7 = [bool]$Capabilities.bc7
        etc2 = [bool]$Capabilities.etc2
        astc4x4 = [bool]$Capabilities.astc4x4
        astc8x8 = [bool]$Capabilities.astc8x8
    }
}

function Get-TextSha256 {
    param([string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function Write-CapabilityMatrix {
    param([string]$RootDirectory, [object[]]$RunRecords)
    $hostRun = $RunRecords | Where-Object { $_.suite -eq 'host-vulkan' -and $_.passed } | Select-Object -Last 1
    $venusRun = $RunRecords | Where-Object { $_.suite -eq 'venus' -and $_.passed } | Select-Object -Last 1
    if (-not $hostRun -or -not $venusRun) { throw 'Capability matrix requires passing Host Vulkan and Venus runs' }

    $hostPath = Join-Path $RootDirectory "$($hostRun.runId)\device-results\host-vulkan.json"
    $venusPath = Join-Path $RootDirectory "$($venusRun.runId)\device-results\venus-offscreen-x64.json"
    $hostResult = Get-Content -Raw -LiteralPath $hostPath | ConvertFrom-Json
    $venusResult = Get-Content -Raw -LiteralPath $venusPath | ConvertFrom-Json
    $hostCanonical = Get-CanonicalCapabilities $hostResult.capabilities
    $venusCanonical = Get-CanonicalCapabilities $venusResult.capabilities
    $hostJson = $hostCanonical | ConvertTo-Json -Compress
    $venusJson = $venusCanonical | ConvertTo-Json -Compress
    $hostHash = Get-TextSha256 $hostJson
    $venusHash = Get-TextSha256 $venusJson
    Write-Utf8NoBom (Join-Path $RootDirectory 'host-capabilities.canonical.json') $hostJson
    Write-Utf8NoBom (Join-Path $RootDirectory 'venus-capabilities.canonical.json') $venusJson

    $differences = @()
    foreach ($property in $hostCanonical.Keys) {
        $hostValue = $hostCanonical[$property]
        $venusValue = $venusCanonical[$property]
        if ("$hostValue" -ne "$venusValue") {
            $differences += [ordered]@{ capability = $property; host = $hostValue; venus = $venusValue }
        }
    }
    $matrix = [ordered]@{
        schemaVersion = 1
        status = 'PASS'
        host = [ordered]@{
            deviceName = $hostResult.capabilities.deviceName
            driverVersion = $hostResult.capabilities.driverVersion
            capabilityHash = $hostHash
            canonical = $hostCanonical
        }
        venus = [ordered]@{
            deviceName = $venusResult.capabilities.deviceName
            driverVersion = $venusResult.capabilities.driverVersion
            capabilityHash = $venusHash
            canonical = $venusCanonical
        }
        differences = $differences
    }
    $matrix | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $RootDirectory 'capability-matrix.json') -Encoding UTF8
    return $matrix
}

function Invoke-OneRun {
    param([string]$RunSuite, [string]$RunPrefix, [string]$RunId, [string]$RootDirectory)
    $runDirectory = Join-Path $RootDirectory $RunId
    New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
    $remoteStable = "$DeviceSandbox/files/automation/results/$RunId/suite-summary.json"
    $remoteHostResults = "$DeviceSandbox/files/automation/results/$RunId"
    $remotePrefix = if ($RunPrefix -eq 'clean') { '.wine-smoke' } else { '.wine' }
    $remoteResults = "$DeviceSandbox/files/$remotePrefix/drive_c/smoke/results/$RunId"

    Stop-AutomationApp
    # HDC shell cannot remove application-owned sandbox files. EntryAbility
    # performs and verifies the clean-prefix reset under the App UID before
    # starting Wayland, wineserver or Wine.
    Invoke-Hdc shell 'power-shell wakeup' | Out-Null
    Invoke-Hdc shell 'power-shell timeout -o 2147483647' | Out-Null
    Invoke-Hdc shell 'hilog -x' | Out-Null
    $batchMappedFlushArgument = if ($batchMappedFlushOverrideRequested) {
        $batchMappedFlushValue = if ($BatchMappedFlush) { '1' } else { '0' }
        " --ps winehua.batch_mapped_flush $batchMappedFlushValue"
    } else {
        ''
    }
    $startCommand = "aa start -a $Ability -b $Bundle --ps winehua.mode smoke --ps winehua.run_id $RunId --ps winehua.suite $RunSuite --ps winehua.prefix $RunPrefix --ps winehua.graphics_experiment $GraphicsExperiment --ps winehua.long_seconds $LongSeconds$batchMappedFlushArgument"
    $startOutput = Invoke-Hdc shell $startCommand
    if (($startOutput -join "`n") -match '10106102') {
        # Devices without a credential can be dismissed with one deterministic
        # swipe.  A credential-protected lock remains an infrastructure error.
        Invoke-Hdc shell 'uitest uiInput swipe 1280 1350 1280 300 1200' | Out-Null
        $startOutput = Invoke-Hdc shell $startCommand
    }
    $startOutput | Set-Content -LiteralPath (Join-Path $runDirectory 'start.log') -Encoding UTF8
    if (($startOutput -join "`n") -notmatch 'start ability successfully') {
        throw "Want start failed: $($startOutput -join ' ')"
    }

    $runTimeoutMinutes = if ($RunSuite -eq 'dxvk-long') {
        [Math]::Max($TimeoutMinutes, [Math]::Ceiling($LongSeconds / 60.0) + 5)
    } else { $TimeoutMinutes }
    $deadline = (Get-Date).AddMinutes($runTimeoutMinutes)
    $captured = @{}
    $summaryText = ''
    while ((Get-Date) -lt $deadline) {
        if ($RunSuite -in @('core', 'opengl', 'all', 'long')) {
            foreach ($testId in @('opengl-x64', 'opengl-x86')) {
                if ($captured.ContainsKey($testId)) { continue }
                $remoteResult = "$remoteResults/$testId.json"
                $resultText = Get-DeviceText $remoteResult
                if ($resultText -match '"message"\s*:\s*"fixed-frame"') {
                    $remoteImage = "/data/local/tmp/winehua-$RunId-$testId.jpeg"
                    $localImage = Join-Path $runDirectory "$testId.jpeg"
                    Invoke-Hdc shell snapshot_display -f $remoteImage | Out-Null
                    Save-DeviceFile $remoteImage $localImage
                    Invoke-Hdc shell rm $remoteImage | Out-Null
                    $visualJson = Join-Path $runDirectory "$testId-visual.json"
                    $captured[$testId] = Test-FixedFrame -ImagePath $localImage -JsonPath $visualJson
                }
            }
        }
        if ($RunSuite -in @('d3d9', 'dxvk', 'dxvk-long', 'dxvk-dynamic', 'all')) {
            $dxvkTests = if ($RunSuite -eq 'd3d9') {
                @('d3d9-cube-x86', 'd3d9-cube-x64')
            } elseif ($RunSuite -eq 'dxvk-dynamic') {
                @('dxvk-dynamic-cb-x86', 'dxvk-dynamic-cb-x64')
            } elseif ($RunSuite -eq 'dxvk-long') {
                @('dxvk-long-x64')
            } elseif ($RunSuite -eq 'all') {
                @('d3d9-cube-x86', 'd3d9-cube-x64', 'dxvk-legacy-x64', 'dxvk-legacy-x86')
            } else {
                @('dxvk-legacy-x64', 'dxvk-legacy-x86')
            }
            foreach ($testId in $dxvkTests) {
                if ($captured.ContainsKey($testId)) { continue }
                $remoteResult = "$remoteResults/$testId.json"
                $resultText = Get-DeviceText $remoteResult
                if ($resultText -match '"message"\s*:\s*"fixed-frame"') {
                    $remoteImage = "/data/local/tmp/winehua-$RunId-$testId.jpeg"
                    $localImage = Join-Path $runDirectory "$testId.jpeg"
                    Invoke-Hdc shell snapshot_display -f $remoteImage | Out-Null
                    Save-DeviceFile $remoteImage $localImage
                    Invoke-Hdc shell rm $remoteImage | Out-Null
                    $visualJson = Join-Path $runDirectory "$testId-visual.json"
                    $captured[$testId] = Capture-D3D11Frame -RemoteImage $remoteImage -LocalImage $localImage -JsonPath $visualJson
                }
            }
        }
        if ($RunSuite -eq 'host-vulkan' -and -not $captured.ContainsKey('host-vulkan')) {
            $hostResultText = Get-DeviceText "$remoteHostResults/host-vulkan.json"
            if ($hostResultText -match '"message"\s*:\s*"fixed-frame"') {
                $remoteImage = "/data/local/tmp/winehua-$RunId-host-vulkan.jpeg"
                $localImage = Join-Path $runDirectory 'host-vulkan.jpeg'
                Invoke-Hdc shell snapshot_display -f $remoteImage | Out-Null
                Save-DeviceFile $remoteImage $localImage
                Invoke-Hdc shell rm $remoteImage | Out-Null
                $visualJson = Join-Path $runDirectory 'host-vulkan-visual.json'
                $captured['host-vulkan'] = Test-FixedFrame -ImagePath $localImage -JsonPath $visualJson
            }
        }
        $summaryText = Get-DeviceText $remoteStable
        if ($summaryText) { break }
        Start-Sleep -Milliseconds 500
    }

    if (-not $summaryText) { throw "Suite $RunId timed out without suite-summary.json" }
    $summaryPath = Join-Path $runDirectory 'suite-summary.json'
    $summaryText | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $summary = $summaryText | ConvertFrom-Json

    if ($RunSuite -in @('core', 'opengl', 'all', 'long')) {
        foreach ($testId in @('opengl-x64', 'opengl-x86')) {
            if (-not $captured.ContainsKey($testId)) { $captured[$testId] = $false }
        }
    }
    if ($RunSuite -in @('d3d9', 'dxvk', 'dxvk-long', 'dxvk-dynamic', 'all')) {
        $dxvkTests = if ($RunSuite -eq 'd3d9') {
            @('d3d9-cube-x86', 'd3d9-cube-x64')
        } elseif ($RunSuite -eq 'dxvk-dynamic') {
            @('dxvk-dynamic-cb-x86', 'dxvk-dynamic-cb-x64')
        } elseif ($RunSuite -eq 'dxvk-long') {
            @('dxvk-long-x64')
        } elseif ($RunSuite -eq 'all') {
            @('d3d9-cube-x86', 'd3d9-cube-x64', 'dxvk-legacy-x64', 'dxvk-legacy-x86')
        } else {
            @('dxvk-legacy-x64', 'dxvk-legacy-x86')
        }
        foreach ($testId in $dxvkTests) {
            if (-not $captured.ContainsKey($testId)) { $captured[$testId] = $false }
        }
    }
    if ($RunSuite -eq 'host-vulkan' -and -not $captured.ContainsKey('host-vulkan')) {
        $captured['host-vulkan'] = $false
    }

    (& $Hdc -t $script:DeviceId shell 'hilog -z 10000 -t app') |
        Where-Object { $_ -match [regex]::Escape($Bundle) -and $_ -notmatch '__env|entryParams=' } |
        Set-Content -LiteralPath (Join-Path $runDirectory 'hilog.txt') -Encoding UTF8
    Save-DeviceFile "$DeviceSandbox/temp/wine_stderr_$(Get-Date -Format yyyyMMdd).log" (Join-Path $runDirectory 'wine-stderr.log')
    Save-DeviceFile "$DeviceSandbox/cache/winehua_virgl_host.log" (Join-Path $runDirectory 'virgl-host.log')
    Save-DeviceFile "$DeviceSandbox/temp/winehua_vtest_frontbuffer.log" (Join-Path $runDirectory 'vtest-frontbuffer.log')
    if ($RunSuite -eq 'host-vulkan') {
        Save-DeviceFile $remoteHostResults (Join-Path $runDirectory 'device-results')
    } else {
        Save-DeviceFile $remoteResults (Join-Path $runDirectory 'device-results')
    }

    $customBorderSelections = @()
    $wineStderrPath = Join-Path $runDirectory 'wine-stderr.log'
    if (Test-Path -LiteralPath $wineStderrPath) {
        foreach ($match in Select-String -LiteralPath $wineStderrPath -Pattern 'custom-border path=([a-z-]+) reason=([a-zA-Z0-9_-]+)') {
            $path = $match.Matches[0].Groups[1].Value
            $reason = $match.Matches[0].Groups[2].Value
            $key = "$path|$reason"
            if (-not ($customBorderSelections | Where-Object { $_.key -eq $key })) {
                $customBorderSelections += [ordered]@{ key = $key; path = $path; reason = $reason }
            }
        }
    }

    $visualPass = -not ($captured.Values -contains $false)
    $coverage = if ($RunSuite -in @('dxvk', 'dxvk-long', 'dxvk-dynamic', 'all')) {
        Get-D3D11Coverage -Summary $summary -RunSuite $RunSuite
    } else { $null }
    $coveragePass = $null -eq $coverage -or $coverage.status -eq 'PASS'
    $hostSummary = [ordered]@{
        schemaVersion = 1
        runId = $RunId
        suite = $RunSuite
        prefix = $RunPrefix
        graphicsExperiment = $GraphicsExperiment
        batchMappedFlush = if ($batchMappedFlushOverrideRequested) { [bool]$BatchMappedFlush } else { $null }
        batchMappedFlushPolicy = if ($batchMappedFlushOverrideRequested) { 'explicit-on' } else { 'product-default-on' }
        appStatus = $summary.status
        visualStatus = if ($visualPass) { 'PASS' } else { 'FAIL' }
        coverageStatus = if ($null -eq $coverage) { 'NOT_APPLICABLE' } else { $coverage.status }
        visuals = $captured
        coverage = $coverage
        customBorderSelections = @($customBorderSelections | ForEach-Object {
            [ordered]@{ path = $_.path; reason = $_.reason }
        })
        status = if ($summary.status -eq 'PASS' -and $visualPass -and $coveragePass) { 'PASS' } else { 'FAIL' }
    }
    $hostSummary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDirectory 'host-summary.json') -Encoding UTF8
    return $hostSummary.status -eq 'PASS'
}

if ($Runs -lt 1) { throw '-Runs must be at least 1' }
$referenceArtifact = if ($SkipBuild) {
    Get-ReferenceHapMetadata -HapPath $HapWindows -ExpectedHapSha256 $ExpectedHapSha256 -Bundle $Bundle -AllowDualAbi:$AllowDualAbi
} else { $null }
if ($PreflightOnly) {
    if (-not $SkipBuild) { Assert-BuildEnvironment }
    [ordered]@{ bundle = $Bundle; build = -not $SkipBuild; install = -not $SkipInstall
        artifact = $referenceArtifact; archiveRoot = $ArchiveRoot } | ConvertTo-Json -Depth 6
    return
}
# The former App-side SmokeRunner was removed. Reject unmigrated suites before
# build/install/launch instead of starting a library page and waiting for files.
Assert-NormalSmokeSuite $Suite $Prefix ([bool]$Gate)
if (-not (Test-Path -LiteralPath $Hdc)) { throw "Windows HDC not found: $Hdc" }

if (-not $DeviceId) {
    $DeviceId = Select-AutomationDevice -Targets @(& $Hdc list targets)
}
if (-not $DeviceId) { throw 'No HDC device is connected' }
$script:DeviceId = $DeviceId
$deviceAbi = ((Invoke-Hdc shell param get const.product.cpu.abilist) -join '').Trim()
if ($deviceAbi -notmatch 'arm64-v8a') { throw 'This runner requires a physical ARM64 target' }
$deviceApi = ((Invoke-Hdc shell param get const.ohos.apiversion) -join '').Trim()
if ($deviceApi -notmatch '^\d+$' -or [int]$deviceApi -lt 23) { throw 'Device must expose API 23 or newer' }
if ($SkipInstall) {
    $bundleDump = (Invoke-Hdc shell bm dump -n $Bundle) -join "`n"
    $installed = Get-InstalledBundleVersion $bundleDump
    if ($installed.versionCode -ne $referenceArtifact.versionCode -or
        $installed.versionName -ne $referenceArtifact.versionName) {
        throw 'Installed product version does not match the reference HAP'
    }
}

$sessionId = "phase2-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$sessionDirectory = Join-Path $ArchiveRoot $sessionId
New-Item -ItemType Directory -Force -Path $sessionDirectory | Out-Null

if (-not $SkipBuild) { Invoke-Build -LogPath (Join-Path $sessionDirectory 'build.log') }
$artifact = if ($SkipBuild) { $referenceArtifact } else { Get-ArtifactMetadata -OutputDirectory $sessionDirectory }
$artifact.installation = if ($SkipInstall) { 'reused-installed-package-unverified-reference' } else { 'overwrite-install-pending' }
$artifact | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $sessionDirectory 'artifact.json') -Encoding UTF8
if (-not $SkipInstall) {
    if ((Get-FileHash -LiteralPath $HapWindows -Algorithm SHA256).Hash.ToLowerInvariant() -ne $artifact.hapSha256) {
        throw 'HAP changed after validation; refusing installation'
    }
    $installOutput = & $Hdc -t $DeviceId install -r $HapWindows 2>&1
    $installOutput | Set-Content -LiteralPath (Join-Path $sessionDirectory 'install.log') -Encoding UTF8
    if ($LASTEXITCODE -ne 0 -or ($installOutput -join "`n") -notmatch 'install bundle successfully') {
        throw 'HAP overwrite install did not report install bundle successfully'
    }
    $artifact.installation = 'overwrite-installed-this-run'
    $artifact | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $sessionDirectory 'artifact.json') -Encoding UTF8
} else {
    Write-Warning 'Testing the installed package without replacement. Reference HAP hash is not proof of installed bytes or extracted runtime state.'
}

if ($Suite -in @('venus-heaven-material', 'venus-heaven-material-layout')) {
    if (-not $ReplayFragmentSpv -or -not (Test-Path -LiteralPath $ReplayFragmentSpv)) {
        throw 'venus-heaven-material requires -ReplayFragmentSpv pointing to the captured final SPIR-V'
    }
    if (-not $ReplayVertexSpv -or -not (Test-Path -LiteralPath $ReplayVertexSpv)) {
        throw 'venus-heaven-material requires -ReplayVertexSpv pointing to the captured/remapped VS SPIR-V'
    }
    # /data/local/tmp is visible to the HDC shell but not to the App's
    # sandboxed native child. Stage the captured shaders in the app-owned temp
    # directory and let SmokeRunner use its logical storage alias.
    $remoteReplaySpv = "$DeviceSandbox/temp/winehua_heaven_final_fs.spv"
    $remoteReplayVertexSpv = "$DeviceSandbox/temp/winehua_heaven_final_vs.spv"
    $sendOutput = & $Hdc -t $DeviceId file send $ReplayFragmentSpv $remoteReplaySpv 2>&1
    $sendOutput | Set-Content -LiteralPath (Join-Path $sessionDirectory 'replay-shader-send.log') -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { throw 'Failed to stage the external replay SPIR-V on the device' }
    $localReplayHash = (Get-FileHash -LiteralPath $ReplayFragmentSpv -Algorithm SHA256).Hash.ToLowerInvariant()
    $deviceReplayHashLine = (& $Hdc -t $DeviceId shell sha256sum $remoteReplaySpv 2>$null | Select-Object -First 1)
    $deviceReplayHash = if ($deviceReplayHashLine) { ("$deviceReplayHashLine" -split '\s+')[0].ToLowerInvariant() } else { '' }
    if ($deviceReplayHash -and $deviceReplayHash -match '^[0-9a-f]{64}$' -and
        $deviceReplayHash -ne $localReplayHash) {
        throw "External replay SPIR-V hash mismatch: local=$localReplayHash device=$deviceReplayHash"
    }
    $sendVertexOutput = & $Hdc -t $DeviceId file send $ReplayVertexSpv $remoteReplayVertexSpv 2>&1
    $sendVertexOutput | Set-Content -LiteralPath (Join-Path $sessionDirectory 'replay-vertex-send.log') -Encoding UTF8
    if ($LASTEXITCODE -ne 0) { throw 'Failed to stage the external replay vertex SPIR-V on the device' }
    $localVertexHash = (Get-FileHash -LiteralPath $ReplayVertexSpv -Algorithm SHA256).Hash.ToLowerInvariant()
    $deviceVertexHashLine = (& $Hdc -t $DeviceId shell sha256sum $remoteReplayVertexSpv 2>$null | Select-Object -First 1)
    $deviceVertexHash = if ($deviceVertexHashLine) { ("$deviceVertexHashLine" -split '\s+')[0].ToLowerInvariant() } else { '' }
    if ($deviceVertexHash -and $deviceVertexHash -match '^[0-9a-f]{64}$' -and
        $deviceVertexHash -ne $localVertexHash) {
        throw "External replay vertex SPIR-V hash mismatch: local=$localVertexHash device=$deviceVertexHash"
    }
    Copy-Item -LiteralPath $ReplayFragmentSpv -Destination (Join-Path $sessionDirectory 'heaven-final-fragment.spv')
    Copy-Item -LiteralPath $ReplayVertexSpv -Destination (Join-Path $sessionDirectory 'heaven-final-vertex.spv')
}

$matrix = @()
if ($Gate) {
    1..3 | ForEach-Object { $matrix += ,@('core', 'reuse') }
    $matrix += ,@('core', 'clean')
} elseif ($Suite -eq 'capabilities') {
    $matrix += ,@('host-vulkan', 'reuse')
    $matrix += ,@('venus', 'reuse')
} else {
    1..$Runs | ForEach-Object { $matrix += ,@($Suite, $Prefix) }
}

$allPassed = $true
$runRecords = @()
$index = 0
foreach ($entry in $matrix) {
    $index++
    $runSuite = $entry[0]
    $runPrefix = $entry[1]
    $runId = "$sessionId-$('{0:D2}' -f $index)-$runSuite-$runPrefix"
    try {
        $passed = Invoke-NormalSmokeRun -RunSuite $runSuite -RunId $runId -RootDirectory $sessionDirectory
    } catch {
        $passed = $false
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $sessionDirectory "$runId-infrastructure-error.txt") -Encoding UTF8
    } finally {
        # Stop our normal game-launcher test session after evidence collection.
        try { Stop-AutomationApp } catch {
            # A disconnected device must not mask the original run failure or
            # prevent the session summary from recording incomplete cleanup.
            $passed = $false
            $_ | Out-String | Set-Content -LiteralPath (Join-Path $sessionDirectory "$runId-cleanup-error.txt") -Encoding UTF8
        }
    }
    $runRecords += [ordered]@{ runId = $runId; suite = $runSuite; prefix = $runPrefix; passed = $passed }
    if (-not $passed) { $allPassed = $false }
}

$capabilityMatrix = $null
if ($Suite -eq 'capabilities') {
    try {
        $capabilityMatrix = Write-CapabilityMatrix -RootDirectory $sessionDirectory -RunRecords $runRecords
    } catch {
        $allPassed = $false
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $sessionDirectory 'capability-matrix-error.txt') -Encoding UTF8
    }
}

[ordered]@{
    schemaVersion = 2
    sessionId = $sessionId
    referenceHapSha256 = $artifact.hapSha256
    installation = $artifact.installation
    gate = [bool]$Gate
    graphicsExperiment = $GraphicsExperiment
    batchMappedFlush = if ($batchMappedFlushOverrideRequested) { [bool]$BatchMappedFlush } else { $null }
    status = if ($allPassed) { 'PASS' } else { 'FAIL' }
    runs = $runRecords
    capabilityHashes = if ($capabilityMatrix) {
        [ordered]@{ host = $capabilityMatrix.host.capabilityHash; venus = $capabilityMatrix.venus.capabilityHash }
    } else { $null }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $sessionDirectory 'automation-summary.json') -Encoding UTF8

$finalLabel = if ($allPassed) { 'PASS' } else { 'FAIL' }
Write-Host "Automation ${finalLabel}: $sessionDirectory"
if (-not $allPassed) { exit 1 }
