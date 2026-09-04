#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
METADATA = (
    ROOT / "Telegram/Telegram.plist",
    ROOT / "Telegram/Resources/winrc/Telegram.rc",
    ROOT / "Telegram/Resources/uwp/AppX/AppxManifest.xml",
    ROOT / "lib/xdg/org.telegram.desktop.desktop",
    ROOT / "lib/xdg/org.telegram.desktop.metainfo.xml",
    ROOT / "lib/xdg/org.telegram.desktop.service",
    ROOT / "Telegram/build/foxmes/foxmes.iss",
)
FORBIDDEN = (
    "org.telegram.desktop",
    "org.foxmes.desktop",
    "Telegram.exe",
    "Telegram.app",
    "x-scheme-handler/tg",
    "x-scheme-handler/tonsite",
    "OWNER/REPO",
    "krol44/FoxMes/releases",
)

# Runner images allowed for a release build. Anything else has to be a
# self-hosted label set; see the runs-on check below.
PINNED_RUNNERS = ("windows-2025", "macos-15", "ubuntu-24.04")
# Every platform is built on GitHub's infrastructure, so all three images are
# checked by name and dropping one cannot pass unnoticed. Compared against the
# parsed runs-on values rather than the file text: "macos-15" is a substring of
# "macos-15-intel", so a text search would have accepted a half-finished move
# between the two.
HOSTED_RUNNERS = ("windows-2025", "macos-15", "ubuntu-24.04")

OLD_RELEASE_WORKFLOWS = (
    "linux.yml",
    "mac.yml",
    "mac_packaged.yml",
    "master_updater.yml",
    "snap.yml",
    "win.yml",
    "winget.yml",
)

EXPECTED_CONSTANTS = {
    'FOXMES_APP_NAME "FoxMes"': ROOT / "Telegram/cmake/foxmes.cmake",
    'FOXMES_APP_DISPLAY_NAME "FoxMes Desktop"': ROOT / "Telegram/cmake/foxmes.cmake",
    'FOXMES_PUBLISHER "Foxtail"': ROOT / "Telegram/cmake/foxmes.cmake",
    'FOXMES_APP_ID "ru.fxl.foxMes"': ROOT / "Telegram/cmake/foxmes.cmake",
    'FOXMES_REPOSITORY "https://github.com/krol44/FoxMesDesktop"': (
        ROOT / "Telegram/cmake/foxmes.cmake"
    ),
    '{F65B4EBE-8E1B-58C8-AED1-B3E8E207EA5C}': (
        ROOT / "Telegram/SourceFiles/core/version.h"
    ),
    '{B05EE107-9901-530D-B744-6B3849FC20F2}': (
        ROOT / "Telegram/SourceFiles/config.h"
    ),
    '{FC0D9DA5-24CF-5319-8323-38E581B6D401}': (
        ROOT / "Telegram/SourceFiles/platform/win/windows_toast_activator.h"
    ),
    'L"ru.fxl.foxMes"': (
        ROOT / "Telegram/SourceFiles/platform/win/windows_app_user_model_id.cpp"
    ),
    'L"FoxMesStartupTask"': (
        ROOT / "Telegram/SourceFiles/platform/win/windows_autostart_task.cpp"
    ),
}


def main() -> int:
    failures = []
    combined = ""
    for path in METADATA:
        text = path.read_text(encoding="utf-8")
        combined += text
        for token in FORBIDDEN:
            if token in text:
                failures.append(f"{path.relative_to(ROOT)} contains {token!r}")
    for required in ("ru.fxl.foxMes", "Foxtail", "krol44/FoxMesDesktop"):
        if required not in combined:
            failures.append(f"metadata does not contain {required!r}")
    publisher_files = (
        ROOT / "Telegram/Resources/winrc/Telegram.rc",
        ROOT / "Telegram/Resources/uwp/AppX/AppxManifest.xml",
        ROOT / "lib/xdg/org.telegram.desktop.metainfo.xml",
        ROOT / "Telegram/build/foxmes/foxmes.iss",
    )
    for path in publisher_files:
        if "Foxtail" not in path.read_text(encoding="utf-8"):
            failures.append(f"{path.relative_to(ROOT)} has no Foxtail publisher")
    # The installer carries the publisher in two independent places - the
    # uninstall entry and the Setup.exe version resource - and a substring test
    # for "Foxtail" anywhere in the file passes with either of them missing.
    # This is the exact contract build-windows.ps1 used to assert at runtime,
    # on a binary, three hours into a build; asserted here it costs nothing.
    installer = (ROOT / "Telegram/build/foxmes/foxmes.iss").read_text(
        encoding="utf-8"
    )
    for directive in ('#define MyAppPublisher "Foxtail"',
                      "AppPublisher={#MyAppPublisher}",
                      "VersionInfoCompany={#MyAppPublisher}"):
        if directive not in installer:
            failures.append(f"foxmes.iss is missing {directive}")
    scheme_files = (
        ROOT / "Telegram/Telegram.plist",
        ROOT / "Telegram/Resources/uwp/AppX/AppxManifest.xml",
        ROOT / "lib/xdg/org.telegram.desktop.desktop",
        ROOT / "Telegram/build/foxmes/foxmes.iss",
    )
    for path in scheme_files:
        if "foxmes" not in path.read_text(encoding="utf-8").lower():
            failures.append(f"{path.relative_to(ROOT)} has no foxmes URL scheme")
    release_scripts = (
        ROOT / "Telegram/build/foxmes/build-windows.ps1",
        ROOT / "Telegram/build/foxmes/build-macos.sh",
        ROOT / "Telegram/build/foxmes/build-linux.sh",
    )
    release_options = (
        "DESKTOP_APP_DISABLE_AUTOUPDATE=ON",
        "DESKTOP_APP_DISABLE_CRASH_REPORTS=ON",
        "FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF",
    )
    for path in release_scripts:
        text = path.read_text(encoding="utf-8")
        for option in release_options:
            if option not in text:
                failures.append(f"{path.relative_to(ROOT)} does not set {option}")
        if path.name == "build-linux.sh" and "/continuous/" in text:
            failures.append("build-linux.sh downloads a mutable continuous asset")
    runtime = (ROOT / "Telegram/SourceFiles/custom_backend/native_runtime.cpp").read_text(
        encoding="utf-8"
    )
    if "#if FOXMES_ALLOW_ENDPOINT_OVERRIDE" not in runtime:
        failures.append("native_runtime.cpp does not compile-time guard FOXMES_URL")
    if 'u"https://api-fox-mes.fxl.ru"_q' not in runtime:
        failures.append("native_runtime.cpp has no baked-in production endpoint")

    for expected, path in EXPECTED_CONSTANTS.items():
        if expected not in path.read_text(encoding="utf-8"):
            failures.append(f"{path.relative_to(ROOT)} does not contain {expected!r}")

    version_values = {}
    for line in (ROOT / "Telegram/build/version").read_text(encoding="utf-8").splitlines():
        key, value = line.split(maxsplit=1)
        version_values[key] = value
    if version_values.get("AppVersion") != "1004001":
        failures.append("Telegram/build/version has an unexpected version code")
    if version_values.get("AppVersionStr") != "1.4.1":
        failures.append("Telegram/build/version has an unexpected version string")

    startup_task = (
        ROOT / "Telegram/SourceFiles/_other/startup_task_win.cpp"
    ).read_text(encoding="utf-8")
    if 'L"\\\\FoxMes.exe"' not in startup_task or "Telegram.exe" in startup_task:
        failures.append("startup_task_win.cpp does not launch only FoxMes.exe")

    updater_linux = (
        ROOT / "Telegram/SourceFiles/_other/updater_linux.cpp"
    ).read_text(encoding="utf-8")
    if "/.TelegramDesktop/" in updater_linux:
        failures.append("updater_linux.cpp still searches the Telegram profile")

    deep_link = (
        ROOT / "Telegram/SourceFiles/custom_backend/native_deep_link.cpp"
    ).read_text(encoding="utf-8")
    if 'u"foxmes"_q' not in deep_link or "tg://" in deep_link or "tonsite://" in deep_link:
        failures.append("native_deep_link.cpp does not isolate the foxmes scheme")
    if "IsOpenRoute" not in deep_link or 'url.host() == u"open"_q' not in deep_link:
        failures.append("native_deep_link.cpp does not restrict activation to foxmes://open")

    update_source = (
        ROOT / "Telegram/SourceFiles/custom_backend/github_update.cpp"
    ).read_text(encoding="utf-8")
    for url in (
        "https://github.com/krol44/FoxMesDesktop/releases/latest/download/version.json",
        "https://github.com/krol44/FoxMesDesktop/releases/latest",
    ):
        if url not in update_source.replace('"_q\n\t+ u"', ""):
            failures.append(f"github_update.cpp does not bake in {url!r}")
    if "std::shared_ptr<GitHubUpdateChecker> InstanceValue" not in update_source:
        failures.append("github_update.cpp does not retain the checker lifetime")

    qsettings = (
        ROOT / "Telegram/SourceFiles/custom_backend/token_store.h"
    ).read_text(encoding="utf-8")
    if 'u"FoxMes"_q' not in qsettings or 'u"FoxMesDesktop"_q' not in qsettings:
        failures.append("token_store.h changed the existing FoxMes QSettings namespace")

    dev_profile = (
        ROOT / "Telegram/SourceFiles/custom_backend/dev_profile.h"
    ).read_text(encoding="utf-8")
    if "#if FOXMES_ALLOW_ENDPOINT_OVERRIDE" not in dev_profile:
        failures.append("dev_profile.h does not compile-time guard the dev suffix")

    # Cross-namespace friend declarations are a standing Windows trap. In
    #     namespace CustomBackend::Reactions { void ApplyDefault(...); }
    #     class Data::Reactions { friend void CustomBackend::Reactions::ApplyDefault(
    #         not_null<Reactions*>); };
    # MSVC looks the parameter types up inside CustomBackend::Reactions, where
    # "Reactions" is the namespace, and rejects it with C2882. GCC and clang
    # pick the injected class name instead and compile it happily, so this only
    # ever surfaces three hours into a Windows build. Requiring the namespace's
    # own name never to appear unqualified in the parameter list catches it in
    # a second here.
    for header in sorted((ROOT / "Telegram/SourceFiles").rglob("*.h")):
        text = header.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(
                r"friend\s+\w+\s+CustomBackend::(\w+)::\w+\s*\(([^;]*?)\)\s*;",
                text, re.S):
            namespace, params = match.group(1), match.group(2)
            if re.search(r"(?<![:\w])" + re.escape(namespace) + r"\b", params):
                failures.append(
                    f"{header.relative_to(ROOT)}: friend declaration uses "
                    f"'{namespace}' unqualified, which MSVC resolves to "
                    f"namespace CustomBackend::{namespace}")

    workflow_dir = ROOT / ".github/workflows"
    for name in OLD_RELEASE_WORKFLOWS:
        if (workflow_dir / name).exists():
            failures.append(f"legacy release workflow is still enabled: {name}")
    workflow = (workflow_dir / "foxmes-release.yml").read_text(encoding="utf-8")
    for option in release_options:
        if option not in "\n".join(
            path.read_text(encoding="utf-8") for path in release_scripts
        ):
            failures.append(f"release scripts do not enforce {option}")
    # The point of this check is that no job may drift onto a moving runner
    # image: a "-latest" label silently changes toolchain under a release. It
    # used to be spelled as "these three strings must appear", which broke the
    # moment the Linux jobs moved to a self-hosted runner. Same guarantee,
    # stated as a rule instead of a list: every runner is either one of the
    # pinned hosted images or an explicitly self-hosted label set. The two
    # platforms still built on GitHub are checked by name as well, so dropping
    # one of them cannot pass unnoticed.
    # Applied to every workflow, not just the release one. Checking a single
    # file left the guarantee hollow: a second workflow could use a floating
    # runner and unpinned actions and this audit would still pass.
    release_runners = set()
    for path in sorted(workflow_dir.glob("*.yml")):
        text = path.read_text(encoding="utf-8")
        label = path.name
        for declaration in re.findall(r"^\s*runs-on:[ \t]*(.+?)\s*$", text, re.M):
            if label == "foxmes-release.yml":
                release_runners.add(declaration)
            if "-latest" in declaration:
                failures.append(
                    f"{label} uses a floating runner label: {declaration}")
            elif declaration.startswith("["):
                if "self-hosted" not in declaration:
                    failures.append(
                        f"{label} uses an unpinned runner: {declaration}")
            elif declaration not in PINNED_RUNNERS:
                failures.append(
                    f"{label} uses an unpinned runner: {declaration}")
        refs = re.findall(r"uses:\s+[^@\s]+@([^\s#]+)", text)
        if any(not re.fullmatch(r"[0-9a-f]{40}", ref) for ref in refs):
            failures.append(f"{label} actions are not pinned to full commit SHAs")
    if not re.findall(r"uses:\s+[^@\s]+@([^\s#]+)", workflow):
        failures.append("release workflow pins no actions at all")
    for runner in HOSTED_RUNNERS:
        if runner not in release_runners:
            failures.append(f"release workflow does not use {runner}")

    # macOS ships a single-architecture build, and the flag and the artifact
    # name have to agree. They live in two different files, so a half-finished
    # rename would otherwise surface as "Missing release artifacts" three hours
    # into a release rather than here.
    macos_script = (ROOT / "Telegram/build/foxmes/build-macos.sh").read_text(
        encoding="utf-8"
    )
    if "CMAKE_OSX_ARCHITECTURES=arm64" not in macos_script:
        failures.append("build-macos.sh does not build arm64-only")
    if "x86_64;arm64" in macos_script:
        failures.append("build-macos.sh still builds a universal binary")
    # verify_arch passes on a universal binary too; only the equality check
    # asserts that nothing but arm64 is present.
    if "lipo -archs" not in macos_script:
        failures.append("build-macos.sh does not assert the exact architecture")
    manifest = (ROOT / "Telegram/build/foxmes/make-release-manifest.py").read_text(
        encoding="utf-8"
    )
    # Both sides of the contract, not just one: the script that creates the dmg
    # and the manifest that demands it by name. Checking only the manifest let a
    # rename in the script through, which fails in the provenance job long after
    # the build.
    for name, text in (("build-macos.sh", macos_script),
                       ("make-release-manifest.py", manifest)):
        if "macos-arm64.dmg" not in text:
            failures.append(f"{name} does not use the macos-arm64.dmg name")

    # The dependency prune is what makes the cache fit, and it is destructive.
    # Assert its two load-bearing filter terms are still there: cache_keys are
    # the stage markers prepare.py skips on, objects- holds real link inputs.
    # Matched as exact filter terms, not as bare words: "cache_keys" also
    # appears in the safety check below the prune, so a substring test stayed
    # green with the term missing from the filter itself.
    for term in ("-path '*/cache_keys/*'", "-path '*/objects-*'",
                 "-path '*/patches/*'", "! '(' -name '*.a'"):
        if term not in macos_script:
            failures.append(
                f"build-macos.sh prune no longer keeps {term}")
    # Qt stages a second copy of every static library it installs, and dropping
    # the duplicates is about 8 GB of a 10 GB repository-wide cache budget.
    # Losing this line does not corrupt anything, it just stops the cache
    # fitting - which shows up as an unexplained miss on every run.
    if 'qt_* -name \'*.a\' -delete' not in macos_script:
        failures.append("build-macos.sh no longer drops the duplicate Qt libraries")

    # The Windows script prunes too, with a longer keep list, because
    # cmake/external/* there links straight out of the build trees. Its own
    # guard is that no stage may be left empty - on Windows empty directories
    # survive, so prepare.py would skip a stage whose libraries are gone.
    windows_script = (ROOT / "Telegram/build/foxmes/build-windows.ps1").read_text(
        encoding="utf-8"
    )
    for term in ('"\\cache_keys\\"', '"\\patches\\"', '"\\objects-"',
                 '".lib"', "FOXMES_PRUNE_LIBRARIES", "OnlyCache"):
        if term not in windows_script:
            failures.append(f"build-windows.ps1 prune no longer keeps {term}")
    if "prune left nothing in these stages" not in windows_script:
        failures.append("build-windows.ps1 lost the emptied-stage check")

    # Every patch has to be registered in both appliers. apply.sh runs on macOS
    # and Linux, build-windows.ps1 re-implements it because the Windows runner
    # has no bash, and a patch added to one and not the other produces a build
    # that silently lacks the fix on the other platform.
    patches_dir = ROOT / "Telegram/patches"
    on_disk = {path.name for path in patches_dir.glob("*.patch")}
    apply_sh = (patches_dir / "apply.sh").read_text(encoding="utf-8")
    registered_sh = set(re.findall(r"^apply_one (\S+\.patch) ", apply_sh, re.M))
    registered_ps1 = set(re.findall(r'Apply-Patch "(\S+\.patch)"', windows_script))
    for label, registered in (("apply.sh", registered_sh),
                              ("build-windows.ps1", registered_ps1)):
        for name in sorted(on_disk - registered):
            failures.append(f"{name} is not applied by {label}")
        for name in sorted(registered - on_disk):
            failures.append(f"{label} applies {name}, which does not exist")

    # prepare.py hands Qt the exact paths of the Windows dependencies it built,
    # and cmake_helpers' external/qt/package.cmake re-states them for the app.
    # The two drifted apart once already: zlib now produces libzs.lib, while
    # package.cmake still named zlibstatic.lib, so CMake reported ZLIB as found
    # and the link input simply did not exist. Nothing fails until the very end
    # of a Windows build, which is why it is asserted here.
    prepare = (ROOT / "Telegram/build/prepare/prepare.py").read_text(
        encoding="utf-8"
    )
    dep_patch = (patches_dir / "cmake-qt-win-dep-paths.patch").read_text(
        encoding="utf-8"
    )
    added = "\n".join(
        line[1:] for line in dep_patch.splitlines() if line.startswith("+"))
    for variable in ("ZLIB_LIBRARY_RELEASE", "ZLIB_LIBRARY_DEBUG"):
        match = re.search(
            r'-D %s="[^"]*\\\\([^"\\]+)"' % variable, prepare)
        if match is None:
            failures.append(f"prepare.py no longer sets {variable} for Windows")
        elif match.group(1) not in added:
            failures.append(
                f"the Windows dependency patch does not use {match.group(1)}, "
                f"the file prepare.py builds for {variable}")
    # libwebp is staged under an architecture directory - x86, x64, ARM64 - and
    # upstream hardcodes the 32-bit one. A literal arch here would link a file
    # that a 64-bit build never produces.
    if re.search(r"release-static/(x86|x64|ARM64)/", added):
        failures.append(
            "the Windows dependency patch hardcodes a libwebp architecture")

    # only_cache is a seeding mode: it deliberately produces no artifacts. Left
    # switched on by accident it would ship a release with nothing in it, which
    # make-release-manifest.py catches - three hours later. Require the guard to
    # be an expression rather than a constant, on every upload.
    uploads = re.findall(
        r"- name: Upload [^\n]*\n(?:\s+if:[^\n]*\n)?\s+uses: actions/upload-artifact",
        workflow,
    )
    guarded = [u for u in uploads if "only_cache != 'true'" in u]
    if len(uploads) != len(guarded):
        failures.append(
            f"{len(uploads) - len(guarded)} upload step(s) are not guarded by only_cache")

    # The Linux job pulls a prebuilt centos_env image by an exact
    # content-addressed tag, and builds and publishes it itself when that tag
    # does not exist yet. Three things keep that honest.
    if "centos_env:latest" in workflow:
        failures.append("release workflow refers to a floating centos_env tag")
    # A floating tag could be overwritten under a running build; a hash of the
    # files that define the image cannot.
    digests = re.findall(r"^\s*DIGEST:\s*(.+?)\s*$", workflow, re.M)
    tag_inputs = ("centos_env/Dockerfile", "centos_env/gen_dockerfile.py",
                  "centos_env/pyproject.toml", "centos_env/poetry.lock")
    if not any(all(name in digest for name in tag_inputs) for digest in digests):
        failures.append(
            "the centos_env image tag is not hashed from the files that define it")
    # The image is rendered from a Jinja template, and DEBUG and LTO change what
    # it contains while staying invisible to that hash. build-linux.sh is the
    # only place that renders it, so that is where they have to be cleared.
    linux_script = (ROOT / "Telegram/build/foxmes/build-linux.sh").read_text(
        encoding="utf-8"
    )
    if "export DEBUG=" not in linux_script or "export LTO=" not in linux_script:
        failures.append("build-linux.sh no longer clears DEBUG and LTO")

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("FoxMes release metadata audit passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
