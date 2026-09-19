/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/update_checker.h"

#include <rpl/event_stream.h>
#include <rpl/producer.h>

#include <QtCore/QString>

#include <memory>

namespace CustomBackend::Updates {

// GitHub Releases based updater for FoxMes, gated by fxl-api.
//
// The rollout is decided by an administrator, not by the fact that a release
// exists: the client first asks `GET /desktop/version`, and only when that
// version is newer than the running build does it read the constant
// `version.json` manifest asset of the latest GitHub release, download the
// package for this platform, verify its SHA-256 against the manifest and
// offer to install it. A package whose digest does not match is deleted and
// never executed.
//
struct AvailableUpdate {
	qint64 versionCode = 0;
	QString version;
};

// Entry points used exclusively by thin hooks in Core::Updater /
// Core::UpdateChecker (see BRIDGE.md for the hook shape rules).
void StartUpdateCheck();

// Stops the hourly poll for good. Hook it to an explicit user request only,
// never to a destructor: Core::UpdateChecker values are short-lived stack
// objects, so the Updater they own dies right after the check is started.
void StopUpdateCheck();
[[nodiscard]] bool IsAvailable();
[[nodiscard]] AvailableUpdate CurrentAvailable();
void OpenReleasePage();

// A verified package is on disk and can be installed right now.
[[nodiscard]] bool IsReadyToInstall();

// Hands the package to the platform installer and quits. No-op unless
// IsReadyToInstall().
void InstallAndRestart();

[[nodiscard]] rpl::producer<> CheckingEvents();
[[nodiscard]] rpl::producer<> IsLatestEvents();
[[nodiscard]] rpl::producer<> FailedEvents();
[[nodiscard]] rpl::producer<AvailableUpdate> AvailableEvents();

// Download progress for the upstream settings UI, which already renders it.
[[nodiscard]] auto ProgressEvents()
-> rpl::producer<Core::UpdateChecker::Progress>;

// Never returns State::Ready. Upstream compares against Ready in branches
// that call Core::checkReadyUpdate()/Core::Restart() - the disabled upstream
// updater's path, which would restart without installing anything. A package
// of ours that is ready is announced through AvailableEvents() instead, and
// installed from the update button.
[[nodiscard]] Core::UpdateChecker::State CurrentState();
[[nodiscard]] int AlreadyDownloaded();
[[nodiscard]] int TotalSize();
[[nodiscard]] bool PreferPercent();

} // namespace CustomBackend::Updates
