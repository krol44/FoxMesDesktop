param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")),
    [string]$ArtifactRoot = (Join-Path $SourceRoot "artifacts\windows"),
    [switch]$OnlyCache
)

$ErrorActionPreference = "Stop"
$version = "1.9.6"
$telegramRoot = Join-Path $SourceRoot "Telegram"
$buildRoot = Join-Path $SourceRoot "out"
$librariesPath = Join-Path (Split-Path -Parent $SourceRoot) "Libraries\win64"
$releaseRoot = Join-Path $buildRoot "Release"
$executable = Join-Path $releaseRoot "FoxMes.exe"

function Apply-Patch([string]$Name, [string]$TargetRelative) {
    $patch = Join-Path $telegramRoot "patches\$Name"
    $target = Join-Path $SourceRoot $TargetRelative

    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & git -C $target apply --reverse --check $patch 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "patches: $Name already applied"
            return
        }
        $details = (& git -C $target apply --check $patch 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            throw "patches: $Name does not apply to $TargetRelative. Upstream most likely changed the surrounding code: re-create the patch against the current submodule commit, or drop it if upstream fixed the same thing.`n$details"
        }
        $details = (& git -C $target apply $patch 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "patches: failed to apply $Name.`n$details" }
        Write-Host "patches: $Name applied to $TargetRelative"
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Remove-UnusedLibraryFiles() {
    if ($env:FOXMES_PRUNE_LIBRARIES -ne "1") { return }
    if (-not (Test-Path -LiteralPath $librariesPath)) { return }

    Write-Host "pruning $librariesPath for caching"
    $keepExtensions = @(
        ".lib", ".a", ".exe", ".h", ".hpp", ".inc", ".cmake", ".pc", ".pl", ".bat")
    $keepFragments = @(
        "\include\", "\objects-", "\cache_keys\", "\patches\",
        "\nv-codec-headers\")

    $removed = 0
    Get-ChildItem -LiteralPath $librariesPath -Recurse -File -Force `
            -ErrorAction SilentlyContinue | ForEach-Object {
        $keep = $keepExtensions -contains $_.Extension.ToLowerInvariant()
        if (-not $keep) {
            $path = $_.FullName.Replace([char]47, [char]92)
            foreach ($fragment in $keepFragments) {
                if ($path.Contains($fragment)) { $keep = $true; break }
            }
        }
        if (-not $keep) {
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
            $removed++
        }
    }

    Get-ChildItem -LiteralPath $librariesPath -Directory -Force `
            -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "qt_*" } |
        ForEach-Object {
            Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force `
                    -Include *.lib, *.a -ErrorAction SilentlyContinue |
                Remove-Item -Force -ErrorAction SilentlyContinue
        }

    $keys = Join-Path $librariesPath "cache_keys"
    if (Test-Path -LiteralPath $keys) {
        $emptied = Get-ChildItem -LiteralPath $keys -File -Force |
                Where-Object { $_.Name -notlike "qt_*" } | ForEach-Object {
            $stage = Join-Path $librariesPath $_.Name
            if (-not (Test-Path -LiteralPath $stage)) { return $_.Name }
            $kept = Get-ChildItem -LiteralPath $stage -Recurse -File -Force `
                -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $kept) { return $_.Name }
        }
        if ($emptied) {
            throw ("prune left nothing in these stages: " + ($emptied -join ", ") +
                ". prepare.py will still skip them, so the failure would surface " +
                "as a missing library at link time. Add a keep rule for them.")
        }
    }
    Write-Host "prune removed $removed files"
}

Apply-Patch "lib_ui-animated-icon-webm.patch" "Telegram\lib_ui"
Apply-Patch "lib_base-screen-capture-settings-mac.patch" "Telegram\lib_base"
Apply-Patch "lib_webrtc-screen-capture-permission-mac.patch" "Telegram\lib_webrtc"
Apply-Patch "tgcalls-screen-share-quality.patch" "Telegram\ThirdParty\tgcalls"
Apply-Patch "tgcalls-call-recovery.patch" "Telegram\ThirdParty\tgcalls"
Apply-Patch "cmake-bzip2-stub.patch" "cmake"
Apply-Patch "cmake-gcc-restrict-warning.patch" "cmake"
Apply-Patch "cmake-qt-win-dep-paths.patch" "cmake"

Push-Location $telegramRoot
try {
    & ".\build\prepare\win.bat" "skip-debug" "skip-dump-syms" "silent" "qt6"
    if ($LASTEXITCODE -ne 0) { throw "Windows dependency preparation failed." }

    Remove-UnusedLibraryFiles

    if ($env:RUNNER_TEMP) {
        New-Item -ItemType File -Force `
            -Path (Join-Path $env:RUNNER_TEMP "foxmes-deps-complete") | Out-Null
    }

    if ($OnlyCache) {
        Write-Host "OnlyCache: dependencies are ready, stopping before the app."
        return
    }

    & ".\configure.bat" "-G" "Ninja Multi-Config" "qt6" `
        "-D" "CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=" `
        "-D" "CMAKE_CONFIGURATION_TYPES=Release" `
        "-D" "TDESKTOP_API_TEST=ON" `
        "-D" "DESKTOP_APP_DISABLE_AUTOUPDATE=ON" `
        "-D" "DESKTOP_APP_DISABLE_CRASH_REPORTS=ON" `
        "-D" "FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF"
    if ($LASTEXITCODE -ne 0) { throw "Windows configuration failed." }

    if ($env:FOXMES_EXPECTED_TOOLSET) {
        $compiler = $null
        $record = Get-ChildItem -LiteralPath $buildRoot -Recurse -File `
            -Filter "CMakeCXXCompiler.cmake" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($record) {
            $entry = Select-String -LiteralPath $record.FullName `
                -Pattern 'set\(CMAKE_CXX_COMPILER\s+"([^"]+)"' | Select-Object -First 1
            if ($entry) { $compiler = $entry.Matches[0].Groups[1].Value }
        }
        if (-not $compiler) {
            Write-Warning ("Could not determine the app compiler under $buildRoot; " +
                "skipping the toolset check.")
        } elseif ($compiler -notmatch [regex]::Escape($env:FOXMES_EXPECTED_TOOLSET)) {
            throw ("Toolset mismatch: dependencies were built with " +
                "$($env:FOXMES_EXPECTED_TOOLSET), but CMake resolved the app " +
                "compiler to $compiler. Qt and the app would link against " +
                "different STLs.")
        } else {
            Write-Host "app compiler: $compiler"
        }
    }
} finally {
    Pop-Location
}

cmake --build $buildRoot --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Windows build failed." }
if (-not (Test-Path $executable)) { throw "FoxMes.exe was not produced." }
$versionInfo = (Get-Item $executable).VersionInfo
if ("$($versionInfo.CompanyName)".Trim() -ne "Foxtail") { throw "Unexpected executable publisher." }
if ($versionInfo.ProductVersion -notlike "1.9.6*") { throw "Unexpected executable version." }

Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ArtifactRoot
New-Item -ItemType Directory -Force -Path $ArtifactRoot | Out-Null
$portableRoot = Join-Path $ArtifactRoot "FoxMes-$version-windows-x64-portable"
$portableMarker = Join-Path $portableRoot "FoxMesForcePortable"
New-Item -ItemType Directory -Force -Path $portableMarker | Out-Null
Copy-Item $executable $portableRoot
Set-Content -Path (Join-Path $portableMarker "README.txt") `
    -Value "This directory enables the isolated FoxMes portable profile." `
    -Encoding Ascii

$portableZip = Join-Path $ArtifactRoot "FoxMes-$version-windows-x64-portable.zip"
Compress-Archive -Path (Join-Path $portableRoot "*") -DestinationPath $portableZip -Force
$portableTest = Join-Path $buildRoot "FoxMesPortableTest"
Expand-Archive -Path $portableZip -DestinationPath $portableTest -Force
if (-not (Test-Path (Join-Path $portableTest "FoxMesForcePortable"))) {
    throw "The portable package does not contain FoxMesForcePortable."
}
Remove-Item -Recurse -Force $portableTest

$iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { throw "Inno Setup 6 was not found." }
& $iscc "/DReleasePath=$releaseRoot" "/DOutputPath=$ArtifactRoot" `
    (Join-Path $PSScriptRoot "foxmes.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed." }

$setup = Join-Path $ArtifactRoot "FoxMes-$version-windows-x64-setup.exe"
if (-not (Test-Path $setup)) { throw "Windows installer was not produced." }
$setupInfo = (Get-Item $setup).VersionInfo
Write-Host ("installer version info: company='{0}' product='{1}' version='{2}'" `
    -f $setupInfo.CompanyName, $setupInfo.ProductName, $setupInfo.ProductVersion)
$observedCompany = "$($setupInfo.CompanyName)".Trim()
$observedProduct = "$($setupInfo.ProductName)".Trim()
$observedVersion = "$($setupInfo.ProductVersion)".Trim()
if ($observedCompany -ne "Foxtail") {
    throw "Unexpected installer publisher: '$($observedCompany)'."
}
if ($observedProduct -ne "FoxMes Desktop") {
    throw "Unexpected installer product name: '$($observedProduct)'."
}
if ($observedVersion -ne $version) {
    throw "Unexpected installer version: '$($observedVersion)'."
}
if ((Get-AuthenticodeSignature $setup).Status -ne "NotSigned") {
    throw "The version 1.9.6 installer must be unsigned."
}

$temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$testInstall = Join-Path $temporaryRoot "FoxMesInstallTest"
$installedExecutable = Join-Path $testInstall "FoxMes.exe"
$telegramProtocolBefore = Test-Path "HKCU:\Software\Classes\tg"
$installer = Start-Process -FilePath $setup -PassThru -ArgumentList `
    "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/DIR=$testInstall"
if (-not $installer.WaitForExit(600000)) {
    Stop-Process -InputObject $installer -Force -ErrorAction SilentlyContinue
    throw "The silent installation did not finish within ten minutes."
}
if ($installer.ExitCode -ne 0) { throw "Silent installation failed." }
if (-not (Test-Path $installedExecutable)) {
    throw "Installed FoxMes.exe was not found."
}

$launched = $null
foreach ($attempt in 1..60) {
    $launched = Get-Process -Name "FoxMes" -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $installedExecutable }
    if ($launched) { break }
    Start-Sleep -Seconds 1
}
if (-not $launched) {
    throw ("The silent installation did not start $installedExecutable. " +
        "Self-update replaces the files and relies on this [Run] entry to " +
        "bring the client back, so it would update and stay closed.")
}
Stop-Process -InputObject $launched -Force -ErrorAction SilentlyContinue
Wait-Process -InputObject $launched -Timeout 60 -ErrorAction SilentlyContinue

$foxmesCommand = (Get-Item `
    "HKCU:\Software\Classes\foxmes\shell\open\command").GetValue("")
if ($foxmesCommand -notlike "*FoxMes.exe*--*%1*") {
    throw "foxmes URL registration is invalid."
}
if ((Test-Path "HKCU:\Software\Classes\tg") -ne $telegramProtocolBefore) {
    throw "The installer changed the Telegram URL registration."
}

$uninstaller = Start-Process `
    -FilePath (Join-Path $testInstall "unins000.exe") `
    -Wait `
    -PassThru `
    -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"
if ($uninstaller.ExitCode -ne 0) { throw "Silent uninstallation failed." }
if (Test-Path "HKCU:\Software\Classes\foxmes") {
    throw "Uninstallation left the FoxMes URL registration behind."
}

Remove-Item -Recurse -Force $portableRoot
