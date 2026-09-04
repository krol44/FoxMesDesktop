/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/event_stream.h>
#include <rpl/producer.h>

#include <QtCore/QString>

#include <memory>

namespace CustomBackend::Updates {

// GitHub Releases based update notification for FoxMes.
//
// The client never downloads anything by itself: it polls a constant
// `version.json` manifest asset of the latest stable GitHub release,
// and if the manifest version is newer than the running build it only
// exposes the permanent "latest release" page URL.
//
struct AvailableUpdate {
	qint64 versionCode = 0;
	QString version;
};

// Entry points used exclusively by thin hooks in Core::Updater /
// Core::UpdateChecker (see BRIDGE.md for the hook shape rules).
void StartUpdateCheck();
void StopUpdateCheck();
[[nodiscard]] bool IsAvailable();
[[nodiscard]] AvailableUpdate CurrentAvailable();
void OpenReleasePage();

[[nodiscard]] rpl::producer<> CheckingEvents();
[[nodiscard]] rpl::producer<> IsLatestEvents();
[[nodiscard]] rpl::producer<> FailedEvents();
[[nodiscard]] rpl::producer<AvailableUpdate> AvailableEvents();

} // namespace CustomBackend::Updates
