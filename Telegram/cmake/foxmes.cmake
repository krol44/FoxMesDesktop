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
