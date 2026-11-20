# SPDX-License-Identifier: GPL-3.0-or-later
# Install only runtime code and hook definitions. Game code is rebuilt locally.
param(
    [ValidateSet('Install', 'Uninstall', 'Check', 'Locate')][string]$Mode = 'Install',
    [string]$GameRoot = '',
    [string]$Romfs = '',
    [string]$SourceExefs = ''
)
$ErrorActionPreference = 'Stop'
$officialSite = 'BD2676642A8D5ED48033359BAEE8383CE1B81443181C57D1ADC7F841F23A2DCA'
$officialRuntime = 'CC812C69D6AFC2C8663A6A8729FD283A5E4C7177471002CA077CFCEE21981CF6'
# @@MOD_SITE@@ / @@MOD_RUNTIME@@ are filled in by build_package.py from the built payload
$modSite = '@@MOD_SITE@@'
$modRuntime = '@@MOD_RUNTIME@@'
$previousModVersions = @(
    @{ site = '22AA8E76F7043155487BCBD243DD9201B0D31BFBA28C27DEC020BC217EC589DE'; runtime = '02B04F729D9AC140EB7762FA79D4CBAD6567425EF1C58D593650490821F27A97' },   # CoreMods 0.2.0 (official 0.2.0 base)
    @{ site = '22AA8E76F7043155487BCBD243DD9201B0D31BFBA28C27DEC020BC217EC589DE'; runtime = '986F6BB7F7F5DF5FA6118A071F2011E5740866A625B9C541CA540A80EB49C978' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = '63864ECE679D1CD1506B9500B30C253517286907CB477ED3940B06B44C409F79' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = 'C0B81FD0454EC06209D94C03F58AD94E476F148C08C979C7A0F202C07B53F73D' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = '3AD4FAA7AA0B0F69D25DCD88F91EFCCBF9C4AD859390669034552B7AAAD94EC4' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = '52DACCEC41A852C8915C420AA6137F9332EEFE6F6FCBFF4BB30754B22EB9CC1A' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = '56B34B5216A9524B18E7134682DDF9160BD9950841839D50666BBCACC8B3450C' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = 'B143D5BA077008F1037587B1D8CB249A2EAA395259DE122DA4880CBE1999D670' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = '83D976817C1F08A0F0CE0E6627A83B4FC1EB44C94477A8DAA1B69C504683E643' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = '2777162978DF257ACE743CFC686D5DAF438F9A2966E4B9B63AAFFFA4CF4A3625' },
    @{ site = '0AF3B010BD1BAA7CAB150EE0B9B43D5217782A0337C32A45E1AF39896F27F604'; runtime = '1A96AD4B1DBD25EF2D6F6F168444A98F00EF97C66A3EDE09B4686D7A53567FD5' },
    @{ site = 'EA47985575F5A10AF4E0933E0ACADE68E27C119B242AB83775CD7F3F77DF081E'; runtime = '47FA4514AFD6D213D209A76C6AC911EE2619BD8401D6C015A21891A7C2D4A468' },
    @{ site = 'FA39C673E09F7A563C2A06C1D3C20D446C83C990E352D8F26F823FE2BE3F4516'; runtime = '07CDA45CE204E40A7A1F291CE158F4C44AE108ED73CFF12FD3BCC022FBC02138' },
    @{ site = '83653CD1A7A207B98E9F73410E2BF18134D23EC4580BBF5FE6260C22C3F9CA78'; runtime = 'BC8393A826CBFC01FB90E4F3FCEF19A4B089BC3159D1F605B0A972F14749DC6D' },
    @{ site = '83653CD1A7A207B98E9F73410E2BF18134D23EC4580BBF5FE6260C22C3F9CA78'; runtime = 'DBA5CF735C48F630580A1A810F29F1604081AD60CE3A8AC1F3C2F39519249E1C' },
    @{ site = '847BF78C1E2222BF38326F0486B94B1B4D312E8CEFE0C290C1F8BD249D044426'; runtime = '473EF9867E8156024D5AE9DA92F6C6A7BB6FDA8F8C04600495FC6CD0BA57D26E' },
    @{ site = '847BF78C1E2222BF38326F0486B94B1B4D312E8CEFE0C290C1F8BD249D044426'; runtime = '40203C642C9978757136A51680669F5F6E8E24E1789D858D52E34D55B3E144C0' }
)

function Sha([string]$path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }
function Fail([string]$code, [string]$problem, [string]$action) {
    throw ("[CoreMods {0}] {1}`nWhat to do: {2}`nNo game files were changed by this check." -f $code, $problem, $action)
}
function Need([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Fail 'MISSING_FILE' "Required file is missing: $path" 'Check that this is a complete official 0.2.8 installation and run Set up in its launcher. If this is a CoreMods package file, extract the whole ZIP again.'
    }
}
function Same([string]$path, [string]$hash) { return (Test-Path -LiteralPath $path -PathType Leaf) -and ((Sha $path) -eq $hash) }
function Verify-OriginalMain([string]$folder) {
    $main = Join-Path $folder 'main'
    Need $main
    $hashFile = Join-Path $GameRoot 'tools\build\dump_hashes.json'
    Need $hashFile
    $hashes = Get-Content -LiteralPath $hashFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $supported = @($hashes.sha256.main, $hashes.main_plain_sha256) | Where-Object { $_ }
    $actual = Sha $main
    if ($actual -notin $supported) {
        Fail 'SOURCE_MAIN_VERSION' "Original ExeFS main does not match the game update supported by this 0.2.8 setup.`nOriginal main: $main`nSHA-256: $actual`nExpected SHA-256: $($supported -join ', ')" 'Select the original exefs folder from your own update 131072 dump. Do not select the built exefs folder beside fefw.exe. The -SourceExefs option cannot make a different update compatible.'
    }
}
function Verify-OfficialRuntime {
    if ((Same $site $officialSite) -and (Same $runtime $officialRuntime)) { return }
    $siteHash = if (Test-Path -LiteralPath $site -PathType Leaf) { Sha $site } else { 'MISSING' }
    $runtimeHash = if (Test-Path -LiteralPath $runtime -PathType Leaf) { Sha $runtime } else { 'MISSING' }
    Fail 'RUNTIME_MISMATCH' "The runtime files are not the supported clean official 0.2.8 files.`nHook file: $site`nHook SHA-256: $siteHash`nRuntime file: $runtime`nRuntime SHA-256: $runtimeHash" 'Use the official 0.2.8 launcher and complete Set up in a separate clean folder, then install CoreMods there. MODHOST or other modified runtime files cannot be combined by changing RomFS/ExeFS paths. Keep your save folder backed up.'
}

# Capture the installed program and the builder's live outputs before changing them. The builder's
# build/state.json and intermediate files stay together as its own cache, independently of build/done.json.
function Save-InstallSnapshot([string]$folder) {
    New-Item -ItemType Directory -Path $folder | Out-Null
    foreach ($relative in $installFiles) {
        $path = Join-Path $GameRoot $relative
        if (-not (Test-Path -LiteralPath $path)) { continue }
        Need $path
        $copy = Join-Path $folder $relative
        [IO.Directory]::CreateDirectory((Split-Path -Parent $copy)) | Out-Null
        Copy-Item -LiteralPath $path -Destination $copy
    }
}
function Restore-InstallSnapshot([string]$folder) {
    foreach ($relative in $installFiles) {
        $path = Join-Path $GameRoot $relative
        $copy = Join-Path $folder $relative
        if (Test-Path -LiteralPath $copy -PathType Leaf) {
            [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
            Copy-Item -LiteralPath $copy -Destination $path -Force
        } elseif (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}
function Invoke-InstallTransaction([string]$folder, [scriptblock]$operation, [scriptblock]$afterRollback, [switch]$KeepSnapshot) {
    $keep = $false
    try {
        Save-InstallSnapshot $folder
        # Keep the completed snapshot if execution is interrupted before either commit or rollback finishes.
        $keep = $true
        try {
            & $operation
            $keep = $KeepSnapshot.IsPresent
        } catch {
            $failure = $_
            try {
                Restore-InstallSnapshot $folder
                if ($afterRollback) { & $afterRollback }
                $keep = $false
            } catch {
                throw "Operation failed: $failure`nRollback failed: $_`nRecovery snapshot retained at $folder"
            }
            throw $failure
        }
    } finally {
        if (-not $keep) { Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

function Previous-ModVersion {
    foreach ($version in $previousModVersions) {
        if ((Same $site $version.site) -and (Same $runtime $version.runtime)) { return $version }
    }
    # Any other CoreMods build of this same official base (two 0.2.8 builds need not know each other's hashes):
    # the installation record says what was installed and that its official backup is this package's official pair.
    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        try { $record = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json } catch { return $null }
        if ($record.official_site -eq $officialSite -and $record.official_runtime -eq $officialRuntime -and
            (Same $site $record.mod_site) -and (Same $runtime $record.mod_runtime)) {
            return @{ site = [string]$record.mod_site; runtime = [string]$record.mod_runtime }
        }
    }
    return $null
}
function Full-InputPath([string]$value, [string]$label) {
    $value = [Environment]::ExpandEnvironmentVariables($value.Trim().Trim('"', "'"))
    if (-not $value -or $value.Contains([char]0xFFFD)) {
        Fail 'INVALID_PATH' "The $label path is empty or contains a damaged character: $value" 'Check the path in fefw.ini. Save fefw.ini as UTF-8, or pass the original folders with -Romfs and -SourceExefs.'
    }
    try {
        if ($label -ne 'GameRoot' -and -not [IO.Path]::IsPathRooted($value)) { $value = Join-Path $GameRoot $value }
        return [IO.Path]::GetFullPath($value)
    } catch [System.ArgumentException] {
        Fail 'INVALID_PATH' "Windows rejected the $label path: $value`nReason: $($_.Exception.Message)" 'Copy the folder path from File Explorer. Check for extra quotes or control characters. You can also pass -Romfs and -SourceExefs explicitly.'
    }
}
function Fs-PathFromIni([string]$key) {
    $ini = Join-Path $GameRoot 'fefw.ini'
    if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) { return '' }
    $inFs = $false
    # Windows PowerShell 5.1 otherwise reads UTF-8 without BOM as the local ANSI code page.
    # Non-ASCII folder names can then turn into invalid path characters.
    foreach ($line in (Get-Content -LiteralPath $ini -Encoding UTF8)) {
        if ($line -match '^\s*\[([^\]]+)\]') { $inFs = $Matches[1] -ieq 'fs'; continue }
        if (-not $inFs -or $line -notmatch '^\s*([^=;#]+?)\s*=\s*(.*?)\s*$') { continue }
        if ($Matches[1].Trim() -ine $key) { continue }
        $value = $Matches[2]
        if (-not $value) { return '' }
        try {
            return Full-InputPath $value "fefw.ini [$key]"
        } catch {
            Write-Warning "Ignoring invalid fefw.ini [fs] $key path: $value. Reason: $($_.Exception.Message) Use -Romfs and -SourceExefs to specify the original game files."
            return ''
        }
    }
    return ''
}
function Find-GameFiles {
    if ($Romfs -and $SourceExefs) { return }
    # Prefer the path recorded by Set up, then the usual dump layouts. Search
    # only immediate neighbours of this installation, never whole drives.
    $romfsPaths = @(
        (Fs-PathFromIni 'romfs'),
        (Join-Path $GameRoot 'nup_dump\updates\131072\romfs'),
        (Join-Path $GameRoot 'game\nup_dump\updates\131072\romfs'),
        (Join-Path $GameRoot 'romfs'),
        (Join-Path $GameRoot 'game\romfs')
    )
    $parent = Split-Path -Parent $GameRoot
    foreach ($neighbour in (Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue)) {
        if ($neighbour.FullName -eq $GameRoot) { continue }
        $romfsPaths += (Join-Path $neighbour.FullName 'romfs')
        $romfsPaths += (Join-Path $neighbour.FullName 'updates\131072\romfs')
        $romfsPaths += (Join-Path $neighbour.FullName 'nup_dump\updates\131072\romfs')
    }
    if (-not $Romfs) {
        foreach ($candidate in $romfsPaths) {
            if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'GameData\Item.bin') -PathType Leaf)) {
                $script:Romfs = [IO.Path]::GetFullPath($candidate)
                break
            }
        }
    }
    if (-not $SourceExefs) {
        $exefsPaths = @()
        if ($Romfs) { $exefsPaths += (Join-Path (Split-Path -Parent $Romfs) 'exefs') }
        $exefsPaths += @(
            (Join-Path $GameRoot 'nup_dump\updates\131072\exefs'),
            (Join-Path $GameRoot 'game\nup_dump\updates\131072\exefs'),
            (Join-Path $GameRoot 'game\updates\131072\exefs'),
            (Join-Path $GameRoot 'dump\updates\131072\exefs'),
            (Join-Path $previousBackup 'build-exefs'),
            (Join-Path $backup 'build-exefs')
        )
        foreach ($candidate in $romfsPaths) {
            if ($candidate) { $exefsPaths += (Join-Path (Split-Path -Parent $candidate) 'exefs') }
        }
        $hashes = Get-Content -LiteralPath (Join-Path $GameRoot 'tools\build\dump_hashes.json') -Raw | ConvertFrom-Json
        $supportedMain = @($hashes.sha256.main, $hashes.main_plain_sha256)
        foreach ($candidate in ($exefsPaths | Select-Object -Unique)) {
            $main = Join-Path $candidate 'main'
            if (-not (Test-Path -LiteralPath $main -PathType Leaf)) { continue }
            if ([IO.Path]::GetFullPath($candidate).TrimEnd('\') -ieq $exefs.TrimEnd('\')) { continue }
            if ((Sha $main) -in $supportedMain) {
                $script:SourceExefs = [IO.Path]::GetFullPath($candidate)
                break
            }
        }
    }
}
function Archive-Backup([string]$reason) {
    $destination = Join-Path $GameRoot ('CoreMods-public-backup-' + $reason + '-' + [Guid]::NewGuid().ToString('N'))
    if (-not $backup.StartsWith($GameRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        -not $destination.StartsWith($GameRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        (Test-Path -LiteralPath $destination)) { throw 'Backup path check failed.' }
    Move-Item -LiteralPath $backup -Destination $destination
    return $destination
}

if (-not $GameRoot) {
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'fefw-launcher.exe')) {
        $GameRoot = $PSScriptRoot
    } else {
        $GameRoot = Split-Path -Parent $PSScriptRoot
    }
}
$GameRoot = Full-InputPath $GameRoot 'GameRoot'
if ($Romfs) { $Romfs = Full-InputPath $Romfs 'RomFS' }
if ($SourceExefs) { $SourceExefs = Full-InputPath $SourceExefs 'SourceExefs' }
$sitePath = 'tools\build\recomp\games\fefw\main.sites'
$catalogPath = 'mods\item-add\items.json'
$backupName = 'CoreMods-public-backup-0.2.8'
$manifestPath = Join-Path $backupName 'manifest.json'
$site = Join-Path $GameRoot $sitePath
$runtime = Join-Path $GameRoot 'fefw_rt.dll'
$catalog = Join-Path $GameRoot $catalogPath
$backup = Join-Path $GameRoot $backupName
$previousBackup = Join-Path $GameRoot 'CoreMods-public-backup'
$manifest = Join-Path $GameRoot $manifestPath
$payloadSite = Join-Path $PSScriptRoot 'payload\main.sites'
$payloadRuntime = Join-Path $PSScriptRoot 'payload\fefw_rt.dll'
$python = Join-Path $GameRoot 'tools\python\python.exe'
$builder = Join-Path $GameRoot 'tools\build\fefw_build.py'
$exefs = Join-Path $GameRoot 'exefs'
$gameExe = Join-Path $GameRoot 'fefw.exe'
# The live ExeFS filenames are the 0.2.8 builder's EXEFS_FILES. Other files in exefs are left alone.
$installFiles = @($sitePath, 'fefw_rt.dll', $catalogPath, 'fefw.exe', 'fefw.sites', 'build\done.json', $manifestPath,
    'exefs\main', 'exefs\sdk', 'exefs\subsdk0', 'exefs\subsdk1', 'exefs\subsdk2', 'exefs\rtld', 'exefs\main.npdm')

Need (Join-Path $GameRoot 'fefw-launcher.exe')
Need $site
Need $runtime
Need $builder
Need $python
Need (Join-Path $GameRoot 'README.txt')
$releaseLine = Get-Content -LiteralPath (Join-Path $GameRoot 'README.txt') -TotalCount 1
if ($releaseLine -notmatch 'fefw-recomp 0\.2\.8') {
    Fail 'LAUNCHER_VERSION' "This launcher is not identified as fefw-recomp 0.2.8.`nREADME version line: $releaseLine`nGame folder: $GameRoot" 'Install the official fefw-recomp 0.2.8 release in a separate folder and run Set up before installing CoreMods.'
}
if ($Mode -notin @('Check', 'Locate')) {
    $runningHere = Get-CimInstance Win32_Process -Filter "name = 'fefw.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($gameExe, [StringComparison]::OrdinalIgnoreCase) }
    if ($runningHere) { throw 'Close this installation of the game before changing CoreMods.' }
}

if ($Mode -eq 'Check') {
    if ((Same $site $modSite) -and (Same $runtime $modRuntime)) { Write-Output 'CoreMods files installed. Run Set up if the launcher requests it.'; exit 0 }
    if (Previous-ModVersion) { Write-Output 'Previous CoreMods 0.2.8 installed. Run install.cmd to update.'; exit 0 }
    if ((Same $site $officialSite) -and (Same $runtime $officialRuntime)) { Write-Output 'Official files detected. CoreMods is not installed.'; exit 0 }
    Verify-OfficialRuntime
}

if ($Mode -eq 'Install' -and (Same $site $modSite) -and (Same $runtime $modRuntime)) {
    Need $manifest
    Need $payloadSite
    Need $payloadRuntime
    if (-not (Same $payloadSite $modSite) -or -not (Same $payloadRuntime $modRuntime)) {
        throw 'The CoreMods package is incomplete or has changed. No changes made.'
    }
    $saved = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    if ($saved.mod_site -ne $modSite -or $saved.mod_runtime -ne $modRuntime -or
        $saved.official_site -ne $officialSite -or $saved.official_runtime -ne $officialRuntime -or
        -not (Same (Join-Path $backup 'main.sites') $officialSite) -or
        -not (Same (Join-Path $backup 'fefw_rt.dll') $officialRuntime) -or
        -not (Test-Path -LiteralPath $catalog -PathType Leaf) -or
        (Sha $catalog) -ne $saved.catalog_hash) {
        throw 'CoreMods files are present, but the installation record or backup does not match. No changes made.'
    }
    Need (Join-Path $PSScriptRoot 'generate_catalog.py')
    Find-GameFiles
    if ($Romfs) {
        $tempCatalog = Join-Path $env:TEMP ('fefw-catalog-' + [Guid]::NewGuid().ToString('N') + '.json')
        try {
            & $python -I (Join-Path $PSScriptRoot 'generate_catalog.py') --romfs $Romfs --output $tempCatalog
            if ($LASTEXITCODE -ne 0) { throw 'Item catalog refresh failed; installed files were not changed.' }
            $rows = Get-Content -LiteralPath $tempCatalog -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($rows.Count -lt 1300) { throw 'Item catalog refresh is incomplete; installed files were not changed.' }
            $newHash = Sha $tempCatalog
            if ($newHash -ne $saved.catalog_hash) {
                $oldCatalog = Join-Path $backup ('items-before-refresh-' + [Guid]::NewGuid().ToString('N') + '.json')
                Copy-Item -LiteralPath $catalog -Destination $oldCatalog
                try {
                    Copy-Item -LiteralPath $tempCatalog -Destination $catalog -Force
                    $saved.catalog_hash = $newHash
                    $saved | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8
                } catch {
                    Copy-Item -LiteralPath $oldCatalog -Destination $catalog -Force
                    throw
                }
                Write-Output "CoreMods item names refreshed. Previous catalog kept at $oldCatalog"
            }
        } finally {
            if (Test-Path -LiteralPath $tempCatalog) { Remove-Item -LiteralPath $tempCatalog -Force }
        }
    }
    Write-Output 'CoreMods already installed. No rebuild needed.'
    exit 0
}

if (-not (Test-Path -LiteralPath (Join-Path $exefs 'main') -PathType Leaf)) {
    Fail 'BUILT_MAIN_MISSING' "The running installation has no exefs\main: $exefs" 'Complete Set up in the official 0.2.8 launcher. The original dump supplied with -SourceExefs is a different folder.'
}
Find-GameFiles
if ($Mode -eq 'Locate') {
    if ($Romfs) { Write-Output "RomFS: $Romfs" } else { Write-Output 'RomFS: not found. Search covers fefw.ini and folders near this game installation. Use -Romfs for a distant dump.' }
    if ($SourceExefs) { Write-Output "Original ExeFS: $SourceExefs" } else { Write-Output 'Original ExeFS: not found. Search covers matching update files near this game installation. Use -SourceExefs for a distant dump.' }
    if (-not $Romfs -or -not $SourceExefs) { exit 1 }
    exit 0
}
if ($SourceExefs) {
    $SourceExefs = [IO.Path]::GetFullPath($SourceExefs)
    Need (Join-Path $SourceExefs 'main')
    if ($SourceExefs.TrimEnd('\') -eq $exefs.TrimEnd('\')) {
        Fail 'BUILT_EXEFS_SELECTED' "The selected original ExeFS is actually the built ExeFS used to run this game: $SourceExefs" 'Choose the exefs folder in your original update 131072 game dump, not the exefs folder beside fefw.exe.'
    }
    Verify-OriginalMain $SourceExefs
    Write-Output "Original ExeFS: $SourceExefs"
}
if ($Mode -eq 'Install' -and (Previous-ModVersion)) {
    $previousVersion = Previous-ModVersion
    Need $manifest
    Need $catalog
    Need $payloadSite
    Need $payloadRuntime
    Need (Join-Path $PSScriptRoot 'generate_catalog.py')
    if (-not $SourceExefs) { Fail 'SOURCE_EXEFS_NOT_FOUND' "Original ExeFS was not found near $GameRoot" 'Find exefs\main in your original update 131072 dump, then run Install.ps1 with -SourceExefs <that exefs folder>.' }
    if (-not $Romfs) { Fail 'ROMFS_NOT_FOUND' "RomFS containing GameData\Item.bin was not found near $GameRoot" 'Find GameData\Item.bin in your original dump and pass its parent RomFS folder with -Romfs <folder>.' }
    $Romfs = [IO.Path]::GetFullPath($Romfs)
    Need (Join-Path $Romfs 'GameData\Item.bin')
    if (-not (Same $payloadSite $modSite) -or -not (Same $payloadRuntime $modRuntime)) {
        throw 'The CoreMods package is incomplete or has changed. No changes made.'
    }
    $saved = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    if ($saved.mod_site -ne $previousVersion.site -or $saved.mod_runtime -ne $previousVersion.runtime -or
        -not (Same $runtime $saved.mod_runtime) -or
        $saved.official_site -ne $officialSite -or $saved.official_runtime -ne $officialRuntime -or
        -not (Same (Join-Path $backup 'main.sites') $officialSite) -or
        -not (Same (Join-Path $backup 'fefw_rt.dll') $officialRuntime) -or
        (Sha $catalog) -ne $saved.catalog_hash) {
        throw 'Previous CoreMods files or backup do not match. No changes made.'
    }
    $tempCatalog = Join-Path $env:TEMP ('fefw-catalog-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        & $python -I (Join-Path $PSScriptRoot 'generate_catalog.py') --romfs $Romfs --output $tempCatalog
        if ($LASTEXITCODE -ne 0) { throw 'Item catalog generation failed; no game files were changed.' }
        $rows = Get-Content -LiteralPath $tempCatalog -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($rows.Count -lt 1300) { throw 'Item catalog is incomplete; no game files were changed.' }
        $upgradeBackup = Join-Path $GameRoot ($backupName + '-before-update-' + [Guid]::NewGuid().ToString('N'))
        Invoke-InstallTransaction -folder $upgradeBackup -KeepSnapshot -operation {
            Copy-Item -LiteralPath $payloadSite -Destination $site -Force
            Copy-Item -LiteralPath $payloadRuntime -Destination $runtime -Force
            Copy-Item -LiteralPath $tempCatalog -Destination $catalog -Force
            Write-Output 'Updating CoreMods. Rebuilding the game locally...'
            & $python -I $builder --exefs $SourceExefs --game $GameRoot
            if ($LASTEXITCODE -ne 0) { throw 'Local rebuild failed during the CoreMods update.' }
            $saved.mod_site = $modSite
            $saved.mod_runtime = $modRuntime
            $saved.catalog_hash = Sha $catalog
            $saved | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8
        }
        Write-Output "CoreMods 0.2.8 updated. Previous program kept at $upgradeBackup"
    } finally {
        Remove-Item -LiteralPath $tempCatalog -Force -ErrorAction SilentlyContinue
    }
    exit 0
}
if ($Mode -eq 'Install') {
    Need $payloadSite
    Need $payloadRuntime
    Need (Join-Path $PSScriptRoot 'generate_catalog.py')
    if (-not (Same $payloadSite $modSite) -or -not (Same $payloadRuntime $modRuntime)) {
        throw 'The CoreMods package is incomplete or has changed. No changes made.'
    }
    Verify-OfficialRuntime
    if (Test-Path -LiteralPath $backup) { throw "A prior backup exists: $backup. No changes made." }
    if (-not $Romfs) { Fail 'ROMFS_NOT_FOUND' "RomFS containing GameData\Item.bin was not found near $GameRoot" 'Find GameData\Item.bin in your original dump and pass its parent RomFS folder with -Romfs <folder>. Auto-search is limited to fefw.ini and nearby folders.' }
    $Romfs = [IO.Path]::GetFullPath($Romfs)
    Need (Join-Path $Romfs 'GameData\Item.bin')
    Write-Output "RomFS: $Romfs"
    $tempCatalog = Join-Path $env:TEMP ('fefw-catalog-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        & $python -I (Join-Path $PSScriptRoot 'generate_catalog.py') --romfs $Romfs --output $tempCatalog
        if ($LASTEXITCODE -ne 0) { throw 'Item catalog generation failed; no game files were changed.' }
        $rows = Get-Content -LiteralPath $tempCatalog -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($rows.Count -lt 1300) { throw 'Item catalog is incomplete; no game files were changed.' }
        $rollback = Join-Path $env:TEMP ('fefw-coremods-install-' + [Guid]::NewGuid().ToString('N'))
        Invoke-InstallTransaction -folder $rollback -operation {
            New-Item -ItemType Directory -Path $backup | Out-Null
            Copy-Item -LiteralPath $site -Destination (Join-Path $backup 'main.sites')
            Copy-Item -LiteralPath $runtime -Destination (Join-Path $backup 'fefw_rt.dll')
            $hadExecutable = Test-Path -LiteralPath $gameExe -PathType Leaf
            if ($hadExecutable) { Copy-Item -LiteralPath $gameExe -Destination (Join-Path $backup 'fefw.exe') }
            $rebuildSource = $SourceExefs
            if (-not $rebuildSource) {
                $rebuildSource = Join-Path $backup 'build-exefs'
                Copy-Item -LiteralPath $exefs -Destination $rebuildSource -Recurse
            }
            $hadCatalog = Test-Path -LiteralPath $catalog -PathType Leaf
            if ($hadCatalog) { Copy-Item -LiteralPath $catalog -Destination (Join-Path $backup 'items.json') }
            @{
                version = 2; official_site = $officialSite; official_runtime = $officialRuntime
                mod_site = $modSite; mod_runtime = $modRuntime; had_catalog = $hadCatalog
                had_executable = $hadExecutable; catalog_hash = (Sha $tempCatalog)
            } | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8
            Copy-Item -LiteralPath $payloadSite -Destination $site -Force
            Copy-Item -LiteralPath $payloadRuntime -Destination $runtime -Force
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $catalog) | Out-Null
            Copy-Item -LiteralPath $tempCatalog -Destination $catalog -Force
            Write-Output 'CoreMods files installed. Rebuilding the game locally from your own files...'
            & $python -I $builder --exefs $rebuildSource --game $GameRoot
            if ($LASTEXITCODE -ne 0) { throw 'Local rebuild failed during CoreMods installation.' }
        } -afterRollback {
            if (Test-Path -LiteralPath $backup -PathType Container) {
                $failedBackup = Archive-Backup 'failed'
                Write-Warning "Original files restored. Diagnostic backup kept at $failedBackup"
            }
        }
        if (Test-Path -LiteralPath (Join-Path $previousBackup 'manifest.json')) {
            Write-Output 'CoreMods 0.2.8 installed over an updated older installation. The previous backup was kept.'
        } else {
            Write-Output 'CoreMods 0.2.8 installed. Your saves and game dump were not copied into the package.'
        }
    } finally {
        Remove-Item -LiteralPath $tempCatalog -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

Need $manifest
if (-not $SourceExefs) { Fail 'SOURCE_EXEFS_NOT_FOUND' "Original ExeFS was not found near $GameRoot" 'Find exefs\main in your original update 131072 dump and pass that exefs folder with -SourceExefs <that exefs folder>.' }
$saved = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if (-not (Same $site $modSite) -or -not (Same $runtime $modRuntime)) {
    throw 'CoreMods files changed since installation. Uninstall stopped to preserve your edits.'
}
if (Test-Path -LiteralPath $catalog -PathType Leaf) {
    if ((Sha $catalog) -ne $saved.catalog_hash) {
        throw 'The item catalog changed since installation. Uninstall stopped to preserve your edits.'
    }
}
if (-not (Same (Join-Path $backup 'main.sites') $officialSite) -or
    -not (Same (Join-Path $backup 'fefw_rt.dll') $officialRuntime)) {
    throw 'Original backups failed verification. Uninstall stopped.'
}
$originalCatalog = Join-Path $backup 'items.json'
if ($saved.had_catalog) { Need $originalCatalog }
$rollback = Join-Path $env:TEMP ('fefw-coremods-uninstall-' + [Guid]::NewGuid().ToString('N'))
Invoke-InstallTransaction -folder $rollback -operation {
    Copy-Item -LiteralPath (Join-Path $backup 'main.sites') -Destination $site -Force
    Copy-Item -LiteralPath (Join-Path $backup 'fefw_rt.dll') -Destination $runtime -Force
    if ($saved.had_catalog) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $catalog) | Out-Null
        Copy-Item -LiteralPath $originalCatalog -Destination $catalog -Force
    } elseif (Test-Path -LiteralPath $catalog -PathType Leaf) {
        Remove-Item -LiteralPath $catalog -Force
    }
    Write-Output 'Official runtime files restored. Rebuilding the original game locally...'
    & $python -I $builder --exefs $SourceExefs --game $GameRoot
    if ($LASTEXITCODE -ne 0) { throw 'Local rebuild failed during CoreMods uninstall.' }
    $oldBackup = Archive-Backup 'uninstalled'
    Write-Output "CoreMods removed. Backup retained at $oldBackup"
} -afterRollback {
    Write-Warning 'The installed CoreMods files were restored. Fix the error and run uninstall.cmd again.'
}
