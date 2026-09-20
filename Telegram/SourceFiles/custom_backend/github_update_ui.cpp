/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/github_update_ui.h"

#include "boxes/about_box.h"
#include "custom_backend/github_update.h"
#include "lang/lang_keys.h"
#include "settings/sections/settings_advanced.h"
#include "settings/settings_builder.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_settings.h"

namespace CustomBackend::Updates {
namespace {

QString StatusText(const Status &status) {
	switch (status.phase) {
	case Phase::Checking:
		return tr::lng_settings_update_checking(tr::now);
	case Phase::Downloading: {
		const auto ready = status.progress.already;
		const auto total = status.progress.size;
		const auto progress = (total > 0)
			? Ui::FormatDownloadText(ready, total)
				+ u" (%1%)"_q.arg(std::clamp(ready * 100 / total, int64(0), int64(100)))
			: Ui::FormatSizeText(ready);
		return tr::lng_settings_downloading_update(
			tr::now,
			lt_progress,
			progress);
	}
	case Phase::Ready:
		return tr::lng_settings_update_ready(tr::now);
	case Phase::Failed:
		return tr::lng_settings_update_fail(tr::now);
	case Phase::Available:
		return tr::lng_settings_update_available(
			tr::now,
			lt_version,
			status.available.version);
	case Phase::Latest:
		return tr::lng_settings_latest_installed(tr::now);
	case Phase::Idle:
		return tr::lng_settings_current_version(
			tr::now,
			lt_version,
			currentVersionText());
	}
	Unexpected("Update phase");
}

void ShowUpdateSettings(not_null<Window::Controller*> window) {
	if (const auto session = window->sessionController()) {
		session->showSettings(Settings::AdvancedId());
	} else {
		window->showSettings();
	}
}

} // namespace

void ActivateUpdate(Window::Controller *window) {
	if (IsReadyToInstall()) {
		InstallAndRestart();
		if (window && CurrentStatus().phase == Phase::Failed) {
			ShowUpdateSettings(window);
		}
		return;
	}
	if (CurrentStatus().manualOnly) {
		OpenReleasePage();
		return;
	}
	DownloadUpdate();
	if (window) {
		ShowUpdateSettings(window);
	}
}

void SetupUpdateSettings(not_null<Ui::VerticalLayout*> container) {
	const auto supported = SupportsSelfUpdate();
	const auto toggle = container->add(object_ptr<Ui::SettingsButton>(
		container,
		supported
			? tr::lng_settings_update_automatically()
			: tr::lng_settings_version_info(),
		st::settingsUpdateToggle));
	if (supported) {
		toggle->toggleOn(rpl::single(cAutoUpdate()));
		toggle->toggledValue() | rpl::on_next([](bool enabled) {
			SetAutomaticDownload(enabled);
		}, toggle->lifetime());
	}

	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		toggle,
		StatusValue() | rpl::map(StatusText),
		st::settingsUpdateState);
	rpl::combine(
		toggle->widthValue(),
		label->widthValue()
	) | rpl::on_next([=] {
		label->moveToLeft(
			st::settingsUpdateStatePosition.x(),
			st::settingsUpdateStatePosition.y());
	}, label->lifetime());
	label->setAttribute(Qt::WA_TransparentForMouseEvents);

	const auto check = container->add(object_ptr<Ui::SettingsButton>(
		container,
		tr::lng_settings_check_now(),
		st::settingsButtonNoIcon));
	check->addClickHandler([] { StartUpdateCheck(); });
	const auto update = Ui::CreateChild<Ui::SettingsButton>(
		check,
		StatusValue() | rpl::map([](const Status &status) {
			return (status.phase == Phase::Failed)
				? tr::lng_bot_download_retry(tr::now)
				: tr::lng_update_telegram(tr::now);
		}),
		st::settingsUpdate);
	check->widthValue() | rpl::on_next([=](int width) {
		update->resizeToWidth(width);
		update->moveToLeft(0, 0);
	}, update->lifetime());
	update->setClickedCallback([] { ActivateUpdate(); });
	StatusValue() | rpl::on_next([=](const Status &status) {
		check->setAttribute(Qt::WA_TransparentForMouseEvents,
			status.phase == Phase::Checking);
		update->setVisible(status.phase == Phase::Ready
			|| status.phase == Phase::Available
			|| status.phase == Phase::Failed);
	}, check->lifetime());
}

void BuildUpdateSettings(
		Settings::Builder::SectionBuilder &builder,
		bool atTop) {
	if (!atTop) {
		builder.addDivider();
	}
	builder.addSkip();
	builder.addSubsectionTitle({
		.id = u"advanced/version"_q,
		.title = tr::lng_settings_version_info(),
		.keywords = { u"version"_q, u"update"_q, u"check"_q },
	});
	builder.addControl({
		.factory = [](not_null<Ui::VerticalLayout*> container) {
			auto content = object_ptr<Ui::VerticalLayout>(container);
			SetupUpdateSettings(content);
			return content;
		},
		.id = u"advanced/auto_update"_q,
		.title = tr::lng_settings_update_automatically(),
		.keywords = { u"update"_q, u"automatic"_q, u"version"_q },
	});
	builder.addSkip();
	if (atTop) {
		builder.addDivider();
	}
}

} // namespace CustomBackend::Updates
