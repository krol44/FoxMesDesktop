# FoxMes Desktop release scripts

These scripts are the only supported path for FoxMes release artifacts. They build `Release`, require Qt WebSockets, disable the upstream auto-updater and crash upload, and compile with `FOXMES_ALLOW_ENDPOINT_OVERRIDE=OFF`.

The Windows, macOS and Linux scripts produce the platform artifacts in `artifacts/`. `make-release-manifest.py` combines them with `version.json` and `SHA256SUMS`. `verify-source.py` audits user-facing package metadata before a build.

## Windows desktop shortcuts during updates

Silent installation (`/SILENT`, `/VERYSILENT`), including automatic updates started by an existing client, skips desktop shortcut creation. Existing shortcuts are preserved, and shortcuts deleted by the user are not recreated. The normal installation wizard still offers the desktop shortcut checkbox.

Windows CI tests silent installation with the `desktopicon` task explicitly selected, an update with an existing shortcut, and an update after that shortcut is deleted. These checks run after the installer is built and do not compile the app again.

## Distributed release builds

Each platform uses dependency preparation, shared-object preparation, four parallel compilation jobs, a link job, and a packaging job. Every job has its own six-hour limit. The pipeline can start with empty caches and pass intermediate results through artifacts created during the same run.

Windows, macOS and Linux CI use Ninja Multi-Config for the distributed stages. The build plan contains the Release compiler commands and identifies objects needed by more than one compilation group. The preparation job compiles those shared objects once. Each compilation group imports them, compiles its assigned objects, and uploads a separate archive. This includes the generated sources and build tools needed by subsequent stages. Swift modules are included in shared preparation and have their own import guard. Local full-build commands retain their existing generators.

Object archives record the source revision, compiler executable hashes, compiler/SDK environment, dependency artifact digest, build plan, source checksums and object checksums. Each group and the link job verify those inputs. The link job imports the prepared objects and all four groups. Its compiler launcher can only validate and reuse imported objects: missing objects, changed arguments, different sources or damaged files fail the job instead of starting another full compilation. Native linkers, archive tools and resource generators still run when required. Every distributed stage sets `SOURCE_DATE_EPOCH` to the source commit time. Qt resource generation therefore uses the same timestamps in every group, even when fresh checkouts have different file modification times. The timestamp is part of the verified build fingerprint.

The macOS supervisor prints CPU, memory, swap and disk diagnostics every minute. It stops compilation before the outer step timeout. Compiler cache uploads use a separate snapshot. Linux build containers carry a run-specific label so cleanup stops only that job's build container.

## Full cold verification

Run the workflow on `master` with `cold_build` enabled and `only_cache` disabled:

```bash
gh workflow run foxmes-release.yml --repo krol44/FoxMesDesktop --ref master -f cold_build=true -f only_cache=false
```

This mode skips dependency and compiler cache restores and saves. The compiler launcher bypasses ccache even when it is installed. Dependency builds also set `CCACHE_DISABLE`, including the dependency compilation layers of the Linux image. Linux builds a fresh dependency image with Docker `--no-cache`, publishes it under a run-specific image reference and passes its exact digest to this run's jobs. The image and object archives are intermediate outputs of the current run.

A cold verification produces the Windows installer/portable ZIP, macOS DMG and Linux packages as workflow artifacts. It does not publish a GitHub Release, start a follow-up cache warm-up or delete existing caches. A complete successful run without retries is the acceptance check for the cold pipeline.

The distributed transfer regression tests build a small real CMake project with generated code, a code generator, precompiled headers, static/object libraries and response files. CI also regenerates Qt resources with different file modification times and checks that the archived sources are identical. When Swift is installed, the tests also compile and import a real Swift module. They verify linking without native source compilation and reject missing archives, damaged objects, changed compiler flags and changed sources. Run them with:

```bash
python3 -m unittest discover -s Telegram/build/foxmes/tests -v
```

## Caches and when to warm them

Caches are optional acceleration. Dependency caches are keyed on the preparation scripts and toolchain. Compiler caches are stored separately for shared preparation and each of the four groups. Successful default-branch runs retain the newest cache for each group. The `only_cache` mode runs preparation, compilation and verified linking without producing release packages.

After a tag release, `refresh-caches` starts an `only_cache` run on `master`. The workflow has no periodic schedule. GitHub can expire unused caches according to its retention policy; a cold run must not depend on them remaining available. A toolchain or dependency change can invalidate compiler cache entries even when the cache still exists.

Windows compilation with ccache uses forced includes in place of MSVC precompiled headers. macOS adds `-Xclang -fno-pch-timestamp` to C-family compilation, including the shared precompiled headers. Linux removes the image's `CCACHE_DISABLE` for the app; cold mode still bypasses ccache in the distributed compiler launcher.

The Linux job pins appimagetool 1.9.1 (commit `8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81`) with SHA256 `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0`. Its type-2 runtime is pinned to release `20251108` (commit `dd6cebedcbddde9c82f89b011e8e1d40b6e43868`) with SHA256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`. Both URLs use immutable release tags; the release job never downloads a mutable `latest` or `continuous` asset.

Version 1.13.5 is unsigned. The macOS app is the exception: it carries FoxMes's self-signed certificate (`codesign.pem`, key in the `macos-signing` environment) so that macOS keeps its privacy permissions across updates, but it is not notarized. Windows SmartScreen and macOS Gatekeeper may warn on first launch. Users should verify `SHA256SUMS` and the GitHub build provenance before running an artifact. No Microsoft Store, Mac App Store, Snap, Flatpak or WinGet package is produced.
