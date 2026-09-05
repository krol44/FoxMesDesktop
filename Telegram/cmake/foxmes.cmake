# This file is part of FoxMes, an unofficial desktop application
# based on Telegram Desktop.

set(FOXMES_APP_NAME "FoxMes")
set(FOXMES_APP_DISPLAY_NAME "FoxMes Desktop")
set(FOXMES_PUBLISHER "Foxtail")
set(FOXMES_APP_ID "ru.fxl.foxMes")
set(FOXMES_HOMEPAGE "https://fxl.ru")
set(FOXMES_REPOSITORY "https://github.com/krol44/FoxMesDesktop")
set(FOXMES_RELEASES_URL "${FOXMES_REPOSITORY}/releases")
set(FOXMES_ISSUES_URL "${FOXMES_REPOSITORY}/issues")

option(
    FOXMES_ALLOW_ENDPOINT_OVERRIDE
    "Allow FOXMES_URL and development TLS overrides."
    ON)

# A build made by dev-client.sh / prod-client.sh, as opposed to a release from
# Telegram/build/foxmes/build-*. It is a separate application, not another copy
# of the same one: its own bundle name, its own bundle identifier, and its own
# working directory (CustomBackend::ProfileSuffix()).
#
# On macOS that separation is what keeps the two out of each other's way. TCC
# keys a microphone or camera grant to the bundle identifier plus the code
# signature, and both builds are only ad-hoc signed, so two bundles sharing
# ru.fxl.foxMes with different signatures make the system refuse the second one
# without ever showing a prompt - which is how a locally built client and an
# installed release stopped being able to record at the same time.
#
# The C++ half of the same split - the working directory and the QSettings
# store - rides on FOXMES_ALLOW_ENDPOINT_OVERRIDE instead, which the same two
# scripts already leave on; see CustomBackend::ProfileSuffix().
option(
    FOXMES_LOCAL_BUILD
    "Mark this as a local developer build: FoxMes-local, own identifier."
    OFF)

# The name the binary and the macOS bundle are built under. It is a separate
# variable from FOXMES_APP_NAME because the CMake project has to keep the plain
# name in every configuration: project() names the generated Xcode project, and
# renaming that throws away the build database next to it - every dependency
# recompiles from scratch, and again on the way back.
set(FOXMES_BUNDLE_NAME "${FOXMES_APP_NAME}")

if (FOXMES_LOCAL_BUILD)
    set(FOXMES_BUNDLE_NAME "${FOXMES_APP_NAME}-local")
    set(FOXMES_APP_DISPLAY_NAME "${FOXMES_APP_DISPLAY_NAME} (local)")
    set(FOXMES_APP_ID "${FOXMES_APP_ID}-local")
endif()
