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

struct AvailableUpdate {
	qint64 versionCode = 0;
	QString version;
};

enum class Phase {
	Idle,
	Checking,
	Available,
	Downloading,
	Ready,
	Failed,
	Latest,
};

struct Status {
	Phase phase = Phase::Idle;
	AvailableUpdate available;
	Core::UpdateChecker::Progress progress = {};
	bool manualOnly = false;
};

[[nodiscard]] Status CurrentStatus();
[[nodiscard]] rpl::producer<Status> StatusValue();
[[nodiscard]] bool SupportsSelfUpdate();
void SetAutomaticDownload(bool enabled);
void DownloadUpdate();

void StartUpdateCheck();

void StopUpdateCheck();
[[nodiscard]] bool IsAvailable();
[[nodiscard]] AvailableUpdate CurrentAvailable();
void OpenReleasePage();

[[nodiscard]] bool IsReadyToInstall();

void InstallAndRestart();

[[nodiscard]] rpl::producer<> CheckingEvents();
[[nodiscard]] rpl::producer<> IsLatestEvents();
[[nodiscard]] rpl::producer<> FailedEvents();
[[nodiscard]] rpl::producer<AvailableUpdate> AvailableEvents();

[[nodiscard]] auto ProgressEvents()
-> rpl::producer<Core::UpdateChecker::Progress>;

[[nodiscard]] Core::UpdateChecker::State CurrentState();
[[nodiscard]] int AlreadyDownloaded();
[[nodiscard]] int TotalSize();
[[nodiscard]] bool PreferPercent();

}
