# Finds the draw that paints the ADS centre dot.
#
# The dot is not the HUD (hiding the whole UI composite leaves it). Bisection over draw ordinals does
# not work here: a skipped draw leaves its render target as the last frame left it, and with a still
# camera that IS the dot, so hiding half the frame proves nothing. What does work is a difference:
# capture a frame at the hip and one in ADS, seconds apart, list the pixel shaders (by checksum,
# stable across launches) drawn only in ADS, and hide each in turn with `hide pchk`, in ADS, looking
# for the dot (centre_dot.py). The one that removes it goes into vr_gpu.cpp's kAutoHidePixel.
param(
    [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\shots\dot'),
    [int]$Present = 8
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\GameIO.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib\VrCommands.ps1')
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }
$py = Join-Path $PSScriptRoot 'centre_dot.py'
$script:shot = 0
# Each test leaves ADS and enters it again with the rule in place: a draw into a target that is not
# cleared every frame leaves its last output behind when skipped, and a dot that survives its own
# draw being hidden would read as "not this one".
function Dot-Count([string]$tag) {
    $script:shot++
    $p = Join-Path $Out ("{0:D2}_{1}.png" -f $script:shot, $tag)
    [void](Send-Ads @('force off'))
    Start-Sleep -Milliseconds 700
    [void](Send-Ads @('force on'))
    Start-Sleep -Milliseconds 900
    Save-Shot $p | Out-Null
    return [int](& python $py $p)
}
function Pixel-Shaders([string]$path) {
    $counts = @{}
    foreach ($m in (Select-String -Path $path -Pattern ' psh=([0-9A-F]{8})')) {
        $c = $m.Matches[0].Groups[1].Value
        if ($c -ne '00000000') { $counts[$c] = 1 + [int]$counts[$c] }
    }
    return $counts
}

[void](Focus-Game)
[void](Send-Gpu @('hide none'))
[void](Send-Ads @('force off'))
Start-Sleep -Seconds 2
[void](Invoke-GpuCapture)
Copy-Item C:\Games\Sunrise\SVR_GpuCapture.txt (Join-Path $Out 'hip.txt') -Force
[void](Send-Ads @('force on'))
Start-Sleep -Seconds 2
$base = Dot-Count 'ads'
Write-Host "dot pixels in ADS, nothing hidden: $base"
if ($base -lt $Present) { [void](Send-Ads @('force off')); throw 'no dot to find (is the game in ADS?)' }
[void](Invoke-GpuCapture)
Copy-Item C:\Games\Sunrise\SVR_GpuCapture.txt (Join-Path $Out 'ads.txt') -Force
$hip = Pixel-Shaders (Join-Path $Out 'hip.txt')
$ads = Pixel-Shaders (Join-Path $Out 'ads.txt')
$only = @($ads.Keys | Where-Object { -not $hip.ContainsKey($_) })
$more = @($ads.Keys | Where-Object { $hip.ContainsKey($_) -and $ads[$_] -gt $hip[$_] })
Write-Host ("pixel shaders only in ADS: {0}; drawn more often in ADS: {1}" -f ($only -join ','), ($more -join ','))
$found = @()
# Compute passes first: the dot is the hand cannon's iron-sight dot, which the flat game draws exactly
# on the front post at the centre of the screen -- a sight reticle, projected along the camera axis,
# and no draw (by vertex or pixel shader) turned out to paint it.
function Compute-Shaders([string]$path) {
    $counts = @{}
    foreach ($m in (Select-String -Path $path -Pattern '^x .* csh=([0-9A-F]{8})')) {
        $c = $m.Matches[0].Groups[1].Value
        if ($c -ne '00000000') { $counts[$c] = 1 + [int]$counts[$c] }
    }
    return $counts
}
$hipCs = Compute-Shaders (Join-Path $Out 'hip.txt')
$adsCs = Compute-Shaders (Join-Path $Out 'ads.txt')
$csOnly = @($adsCs.Keys | Where-Object { -not $hipCs.ContainsKey($_) -or $adsCs[$_] -gt $hipCs[$_] })
Write-Host ("compute shaders only (or more often) in ADS: {0}" -f ($csOnly -join ','))
foreach ($c in $csOnly) {
    [void](Send-Gpu @('hide none', "hide cchk $c"))
    $n = Dot-Count "cchk_$c"
    $hit = $n -lt $Present
    if ($hit) { $found += "cchk:$c" }
    Write-Host ("hide cchk {0} -> dot pixels {1}{2}" -f $c, $n, $(if ($hit) { '   <== this one' } else { '' }))
}
foreach ($c in @($only + $more)) {
    [void](Send-Gpu @('hide none', "hide pchk $c"))
    $n = Dot-Count "hide_$c"
    $hit = $n -lt $Present
    if ($hit) { $found += $c }
    Write-Host ("hide pchk {0} -> dot pixels {1}{2}" -f $c, $n, $(if ($hit) { '   <== this one' } else { '' }))
}
[void](Send-Gpu @('hide none'))
if ($found.Count -eq 0) {
    # Not an ADS-only shader: the dot's shader also runs at the hip (an effect whose opacity follows
    # ADS). Every pixel shader of the ADS frame, then, eight at a time (the rule table's width), the
    # frame's last draws first -- sight effects are drawn late -- and each group that removes the dot
    # split into its members.
    $order = New-Object System.Collections.ArrayList
    $lines = @(Select-String -Path (Join-Path $Out 'ads.txt') -Pattern '^d .* psh=([0-9A-F]{8})')
    [array]::Reverse($lines)
    foreach ($m in $lines) {
        $c = $m.Matches[0].Groups[1].Value
        if ($c -ne '00000000' -and -not $order.Contains($c)) { [void]$order.Add($c) }
    }
    Write-Host ("{0} distinct pixel shaders in the ADS frame" -f $order.Count)
    for ($i = 0; $i -lt $order.Count -and $found.Count -eq 0; $i += 8) {
        $group = @($order[$i..([Math]::Min($i + 7, $order.Count - 1))])
        [void](Send-Gpu (@('hide none') + @($group | ForEach-Object { "hide pchk $_" })))
        $n = Dot-Count ("g{0:D3}" -f $i)
        if ($n -ge $Present) { continue }
        Write-Host ("group at {0} removes the dot ({1}); splitting" -f $i, ($group -join ','))
        foreach ($c in $group) {
            [void](Send-Gpu @('hide none', "hide pchk $c"))
            $k = Dot-Count "hide_$c"
            Write-Host ("  hide pchk {0} -> dot pixels {1}" -f $c, $k)
            if ($k -lt $Present) { $found += $c }
        }
    }
    [void](Send-Gpu @('hide none'))
}
if ($found.Count -gt 0) {
    $lines = Select-String -Path (Join-Path $Out 'ads.txt') -Pattern (' psh=(' + ($found -join '|') + ')')
    $lines | ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) } | Write-Host
}
[void](Send-Ads @('force off'))
Write-Host ("FOUND: {0}" -f $(if ($found.Count) { $found -join ',' } else { 'none' }))
