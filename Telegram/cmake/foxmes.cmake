# This file is part of FoxMes, an unofficial desktop application
# based on Telegram Desktop.

set(FOXMES_APP_NAME "FoxMes")
set(FOXMES_APP_DISPLAY_NAME "FoxMes Desktop")
set(FOXMES_PUBLISHER "Foxtail")
set(FOXMES_APP_ID "ing.foxtail.FoxMes")
set(FOXMES_HOMEPAGE "https://foxtail.ing")
set(FOXMES_REPOSITORY "https://github.com/krol44/FoxMesDesktop")
set(FOXMES_RELEASES_URL "${FOXMES_REPOSITORY}/releases")
set(FOXMES_ISSUES_URL "${FOXMES_REPOSITORY}/issues")

option(
    FOXMES_ALLOW_ENDPOINT_OVERRIDE
    "Allow FOXMES_URL and development TLS overrides."
    ON)

option(
    FOXMES_LOCAL_BUILD
    "Mark this as a local developer build: FoxMes-local, own identifier."
    OFF)

set(FOXMES_LOCAL_SIGN_IDENTITY "" CACHE STRING
    "Code signing identity (name or SHA-1) for the local build; empty = ad-hoc.")
set(FOXMES_LOCAL_SIGN_TEAM "" CACHE STRING
    "Development team of FOXMES_LOCAL_SIGN_IDENTITY.")

set(FOXMES_BUNDLE_NAME "${FOXMES_APP_NAME}")

if (FOXMES_LOCAL_BUILD)
    set(FOXMES_BUNDLE_NAME "${FOXMES_APP_NAME}-local")
    set(FOXMES_APP_DISPLAY_NAME "${FOXMES_APP_DISPLAY_NAME} (local)")
    set(FOXMES_APP_ID "${FOXMES_APP_ID}-local")
endif()
