.PHONY: version version-check

VERSION ?=

version: ## Bump the FoxMes version everywhere: make version VERSION=1.1.0
	@if [ -z "$(VERSION)" ]; then \
		echo "Usage: make version VERSION=X.Y.Z"; \
		exit 1; \
	fi
	@Telegram/build/foxmes/set-version.sh "$(VERSION)"

version-check: ## Verify all hardcoded version literals match Telegram/build/version
	@Telegram/build/foxmes/set-version.sh --check
