param (
    [Parameter(Mandatory)][string]$PayloadDir,
    [Parameter(Mandatory)][string]$MakeNsis,
    [string]$VCToolsRedist = $env:VCToolsRedistDir
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$payload = (Resolve-Path -LiteralPath $PayloadDir).Path.TrimEnd('\')
$compiler = (Resolve-Path -LiteralPath $MakeNsis).Path
$crtDir = Join-Path $VCToolsRedist 'x64\Microsoft.VC143.CRT'
foreach ($requiredFile in @(
    "$payload\VolumeMixer.exe", "$payload\Microsoft.UI.Xaml.dll",
    "$payload\LiquidGlassWinUI.LICENSE.txt", "$payload\TablerIcons.LICENSE.txt",
    "$crtDir\msvcp140.dll", "$crtDir\vcruntime140_1.dll"
)) {
    if (!(Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Missing required input $requiredFile"
    }
}

$files = [ordered]@{}
Get-ChildItem -LiteralPath $payload -Recurse -File |
    Where-Object Extension -NotIn '.pdb', '.exp', '.lib' |
    Sort-Object FullName |
    ForEach-Object { $files.Add($_.FullName.Substring($payload.Length + 1), $_) }
Get-ChildItem -LiteralPath $crtDir -File -Filter '*.dll' |
    ForEach-Object { $files[$_.Name] = $_ }
$files['THIRD_PARTY_NOTICES.txt'] = Get-Item -LiteralPath "$root\THIRD_PARTY_NOTICES.txt"
$files['LICENSE.txt'] = Get-Item -LiteralPath "$root\LICENSE"
$files['Licenses\NSIS-COPYING.txt'] = Get-Item -LiteralPath (Join-Path (Split-Path $compiler) 'COPYING')

$packages = [xml](Get-Content -LiteralPath "$root\packages.config" -Raw)
foreach ($package in $packages.packages.package) {
    if ($package.id -notmatch '^Microsoft\.(WindowsAppSDK\.(Base|DWrite|Foundation|InteractiveExperiences|Runtime|WinUI)|Web\.WebView2|Windows\.(ImplementationLibrary|CppWinRT))$') {
        continue
    }
    $packageDir = "$root\packages\$($package.id).$($package.version)"
    foreach ($file in (Get-ChildItem -LiteralPath $packageDir -File | Where-Object Name -Match '^(license|notice)(\.txt)?$')) {
        $files["Licenses\$($package.id).$($file.BaseName).txt"] = $file
    }
}

$manifest = [xml](Get-Content -LiteralPath "$root\Package.appxmanifest" -Raw)
$version = $manifest.Package.Identity.Version
$output = New-Item -Path "$root\dist" -ItemType Directory -Force
$setup = Join-Path $output.FullName 'VolumeMixer-Setup-x64.exe'
$work = New-Item -Path "$root\x64\InstallerBuild" -ItemType Directory -Force
$installFiles = Join-Path $work.FullName 'InstallFiles.nsh'
$uninstallFiles = Join-Path $work.FullName 'UninstallFiles.nsh'

function ConvertTo-NsisPath([string]$path) {
    $path.Replace('$', '$$')
}

$directories = @{}
$install = foreach ($relativePath in $files.Keys) {
    $dir = Split-Path $relativePath -Parent
    $targetDir = '$INSTDIR'
    if ($dir) {
        $targetDir += '\' + (ConvertTo-NsisPath $dir)
        while ($dir) {
            $directories[$dir] = $true
            $dir = Split-Path $dir -Parent
        }
    }
    $source = ConvertTo-NsisPath $files[$relativePath].FullName
    $name = ConvertTo-NsisPath (Split-Path $relativePath -Leaf)
    'SetOutPath "{0}"' -f $targetDir
    'File "/oname={0}" "{1}"' -f $name, $source
}
$uninstall = foreach ($relativePath in $files.Keys) {
    'Delete "$INSTDIR\{0}"' -f (ConvertTo-NsisPath $relativePath)
}
$uninstall += foreach ($dir in ($directories.Keys | Sort-Object -Property Length -Descending)) {
    'RMDir "$INSTDIR\{0}"' -f (ConvertTo-NsisPath $dir)
}
$install | Set-Content -LiteralPath $installFiles -Encoding UTF8
$uninstall | Set-Content -LiteralPath $uninstallFiles -Encoding UTF8

$sizeKiB = [math]::Ceiling(($files.Values | Measure-Object -Property Length -Sum).Sum / 1KB)
& $compiler /V2 /WX "/DVersion=$version" "/DSetupFile=$setup" "/DEstimatedSize=$sizeKiB" `
    "/DInstallFiles=$installFiles" "/DUninstallFiles=$uninstallFiles" "$PSScriptRoot\VolumeMixer.nsi"
if ($LASTEXITCODE -ne 0) {
    throw "NSIS failed with exit code $LASTEXITCODE"
}
Get-Item -LiteralPath $setup
