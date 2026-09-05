param(
    [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")),
    [string]$ArtifactRoot = (Join-Path $SourceRoot "artifacts\windows"),
    # Seeding mode: build the dependencies, prune them for caching and stop
    # before the app. Lets a first run populate the cache without also paying
    # for a full app build in the same job.
    [switch]$OnlyCache
)

$ErrorActionPreference = "Stop"
$version = "1.4.4"
$telegramRoot = Join-Path $SourceRoot "Telegram"
$buildRoot = Join-Path $SourceRoot "out"
# prepare.py puts x64 dependencies in Libraries\win64, a sibling of the
# checkout (prepare.py:54, cmake/variables.cmake:105).
$librariesPath = Join-Path (Split-Path -Parent $SourceRoot) "Libraries\win64"
$releaseRoot = Join-Path $buildRoot "Release"
$executable = Join-Path $releaseRoot "FoxMes.exe"

# Patches that live inside submodules have to be re-applied before every
# build: git submodule update discards them. Mirrors Telegram/patches/apply.sh,
# which this runner has no bash to execute.
function Apply-Patch([string]$Name, [string]$TargetRelative) {
    $patch = Join-Path $telegramRoot "patches\$Name"
    $target = Join-Path $SourceRoot $TargetRelative

    # The --reverse probe below is expected to fail whenever the patch is not
    # applied yet, and git reports that on stderr. Redirecting a native
    # command's stderr feeds it into the PowerShell pipeline as ErrorRecords,
    # which $ErrorActionPreference = "Stop" turns into a terminating error - so
    # the normal "not applied yet" path killed the build before it compared a
    # single line. Drop to Continue here and drive the flow off $LASTEXITCODE.
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

# Reduces the dependency tree to what the build actually consumes, so that it
# fits in the 10 GB an entire repository gets for caches. Opt-in through
# FOXMES_PRUNE_LIBRARIES, because it is destructive and has no business running
# over a developer's tree.
#
# The keep list is upstream tdesktop's Windows one. It is longer than the macOS
# equivalent for a real reason: on Windows cmake/external/* links straight out
# of the build directories - ${libs_loc}/libjxl/lib/Release/jxl.lib,
# dav1d/builddir-release, openal-soft/build/RelWithDebInfo - so those trees have
# to survive, and only Qt's source tree is safely droppable.
#
# Two deliberate differences from upstream. Empty directories are left alone:
# prepare.py skips a stage only while Libraries\win64\<stage> exists
# (prepare.py:199), and deleting empty directories is how that silently stops
# being true. They cost nothing in a cache. And the duplicate static libraries
# staged inside Qt's build tree are dropped, because the app links the installed
# copy under Libraries\win64\Qt-<version> (cmake/external/qt/package.cmake:20).
function Remove-UnusedLibraryFiles() {
    if ($env:FOXMES_PRUNE_LIBRARIES -ne "1") { return }
    if (-not (Test-Path -LiteralPath $librariesPath)) { return }

    Write-Host "pruning $librariesPath for caching"
    # .pl and .bat are here for gas-preprocessor, which is a single perl script
    # plus a generated cpp.bat and would otherwise be emptied entirely.
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
            # Normalised so the fragments above match regardless of which
            # separator the path came back with, which also makes this function
            # testable off Windows.
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

    # Qt stages a second copy of every static library that cmake --install
    # already placed in Libraries\win64\Qt-<version>, which is the only copy
    # anything links. Careful with the glob: qt_* is the build tree, Qt-* is the
    # install prefix and must not be touched.
    Get-ChildItem -LiteralPath $librariesPath -Directory -Force `
            -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "qt_*" } |
        ForEach-Object {
            Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force `
                    -Include *.lib, *.a -ErrorAction SilentlyContinue |
                Remove-Item -Force -ErrorAction SilentlyContinue
        }

    # Because empty directories are kept above, a stage can never disappear here
    # the way it can on macOS - prepare.py will always see it and skip. The
    # Windows failure is the other one: the directory survives, the filter ate
    # everything inside it, prepare.py skips the stage anyway and the link fails
    # much later on a missing library. An emptied stage directory is that exact
    # signal, so that is what this checks.
    $keys = Join-Path $librariesPath "cache_keys"
    if (Test-Path -LiteralPath $keys) {
        # Qt is exempt, and deliberately so: emptying its build tree is the point
        # of this prune, not an accident. The app links the install prefix at
        # Libraries\win64\Qt-<version> (cmake/external/qt/package.cmake:20), and
        # prepare.py only needs the build directory to still exist, not to have
        # anything in it (prepare.py:199). Empty directories survive here, so the
        # stage still gets skipped and roughly 25 GB stays out of the cache.
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
Apply-Patch "cmake-bzip2-stub.patch" "cmake"
Apply-Patch "cmake-gcc-restrict-warning.patch" "cmake"
Apply-Patch "cmake-qt-win-dep-paths.patch" "cmake"

Push-Location $telegramRoot
try {
    # skip-debug, not skip-release: the app below is built --config Release, so
    # the dependencies have to be Release too. skip-release would build only
    # Debug ones and the link would fail on the missing Release libraries;
    # passing neither would build both and roughly double an hours-long job.
    & ".\build\prepare\win.bat" "skip-debug" "skip-dump-syms" "silent" "qt6"
    if ($LASTEXITCODE -ne 0) { throw "Windows dependency preparation failed." }

    Remove-UnusedLibraryFiles

    # The dependencies are complete and pruned at this point, which is exactly
    # what the caches hold. The workflow saves them on this marker rather than
    # on the job succeeding, so a failure in the app build below cannot discard
    # an hour and a half of work that was perfectly good.
    if ($env:RUNNER_TEMP) {
        New-Item -ItemType File -Force `
            -Path (Join-Path $env:RUNNER_TEMP "foxmes-deps-complete") | Out-Null
    }

    if ($OnlyCache) {
        Write-Host "OnlyCache: dependencies are ready, stopping before the app."
        return
    }

    # cmake/variables.cmake defaults this to Embedded for every non-Debug
    # configuration, which puts /Z7 debug info into every object file and makes
    # options_win.cmake link with /DEBUG. Nothing consumes those symbols here -
    # the packages are built with DESKTOP_APP_DISABLE_CRASH_REPORTS=ON - so an
    # empty value leaves the cache entry falsy and the link becomes /DEBUG:NONE.
    #
    # No CMAKE_COMPILE_WARNING_AS_ERROR: the runner now carries Visual Studio
    # 2026, whose toolset is far newer than the 14.44 upstream pins precisely
    # because later ones break tdesktop. Its new warnings would fail the build
    # hours in, the same way -Werror=restrict did on Linux.
    # Ninja Multi-Config rather than the Visual Studio generator, which is what
    # configure.bat picks by default. Two problems disappear with it.
    #
    # Memory: options_win.cmake passes /MP, which parallelises files inside one
    # cl.exe invocation. MSBuild then parallelises projects on top, and four
    # nodes times four compilers exhausted the heap on a 4-core, 16 GB runner
    # ("error C1060") on the generated Qt resource files. Ninja invokes cl.exe
    # once per file, so /MP has nothing to spread and concurrency is just the
    # job count.
    #
    # Toolset: the VS generator resolved the compiler through "-T v143"
    # (cmake/run_cmake.py:28) while prepare.py used whatever cl.exe was on PATH,
    # and the two disagreed - Qt compiled against one STL, the app linked
    # another. Ninja takes cl.exe from PATH as well, so both halves match by
    # construction.
    #
    # Upstream builds this same configuration: "Ninja Multi-Config" is in the
    # generator matrix of their win.yml. The x64 argument has to go, because
    # run_cmake.py:33 rejects it together with an explicit generator; the
    # architecture comes from the cl.exe VsDevCmd put on PATH instead, and
    # cmake/variables.cmake:82 derives build_win64 from its pointer size.
    & ".\configure.bat" "-G" "Ninja Multi-Config" "qt6" `
        "-D" "CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=" `
        "-D" "CMAKE_CONFIGURATION_TYPES=Release" `
        "-D" "TDESKTOP_API_TEST=ON" `
        "-D" "DESKTOP_APP_DISABLE_AUTOUPDATE=ON" `
        "-D" "DESKTOP_APP_DISABLE_CRASH_REPORTS=ON" `
        "-D" "FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF"
    if ($LASTEXITCODE -ne 0) { throw "Windows configuration failed." }

    # Everything above only *arranges* for both build systems to use one
    # toolset; this is where we find out whether they actually did. A mismatch
    # caught here costs two minutes instead of an unresolved __std_rotate at
    # link time ninety minutes later.
    #
    # The compiler path lives in CMakeFiles/<ver>/CMakeCXXCompiler.cmake, not in
    # CMakeCache.txt: the Visual Studio generator resolves the compiler through
    # MSBuild and does not record it as a cache entry the way Ninja does.
    #
    # A check that cannot find its evidence warns and moves on. Failing the
    # build because the verification could not locate a file is how this step
    # broke a run whose compilers were, in fact, correctly aligned.
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

# Full parallelism, which needs Ninja to be safe. Under MSBuild this line had
# to be --parallel 1: cmake/options_win.cmake:34 passes /MP, so every project
# already compiles as many files at once as there are cores, and MSBuild's own
# /m then built that many projects at once too - four nodes times four
# compilers is sixteen cl.exe on a four-core, 16 GB runner, and the generated
# Qt resource files (one is 82849 lines) exhausted the heap with "error C1060:
# compiler is out of heap space". Ninja runs one cl.exe per file, so that
# multiplication cannot happen and /MP is simply inert.
cmake --build $buildRoot --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "Windows build failed." }
if (-not (Test-Path $executable)) { throw "FoxMes.exe was not produced." }
# Trimmed, here and for the installer below: whoever writes a version resource
# decides how to pad it. The compiler does not pad at all, so this check on
# FoxMes.exe was fine; Inno pads every string to a fixed width - 60 characters
# for the company and product, 50 for the version - and the installer check
# below rejected a perfectly correct "Foxtail" over 53 trailing spaces on
# 2026-09-04, after a two-hour build.
$versionInfo = (Get-Item $executable).VersionInfo
if ("$($versionInfo.CompanyName)".Trim() -ne "Foxtail") { throw "Unexpected executable publisher." }
if ($versionInfo.ProductVersion -notlike "1.4.4*") { throw "Unexpected executable version." }

# Emptied for the same reason the Linux and macOS scripts empty theirs: a
# package left by an earlier version would otherwise be uploaded as part of
# this release on any runner that keeps its workspace.
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
# Still printed, because the padding above was only found by printing it, and
# the next surprise in this resource will be found the same way.
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
    throw "The version 1.4.4 installer must be unsigned."
}

$temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$testInstall = Join-Path $temporaryRoot "FoxMesInstallTest"
$telegramProtocolBefore = Test-Path "HKCU:\Software\Classes\tg"
$installer = Start-Process -FilePath $setup -Wait -PassThru -ArgumentList `
    "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/DIR=$testInstall"
if ($installer.ExitCode -ne 0) { throw "Silent installation failed." }
if (-not (Test-Path (Join-Path $testInstall "FoxMes.exe"))) {
    throw "Installed FoxMes.exe was not found."
}
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
