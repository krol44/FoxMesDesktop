/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/native_security_settings.h"

#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/sections/settings_local_passcode.h"
#include "settings/settings_builder.h"
#include "storage/storage_domain.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace CustomBackend::Security {

void BuildSection(Settings::Builder::SectionBuilder &builder) {
	const auto showOther = builder.showOther();
	const auto session = builder.session();

	builder.addSkip(st::settingsPrivacySkip);
	builder.addSubsectionTitle({
		.id = u"security/section"_q,
		.title = tr::lng_settings_security(),
		.keywords = { u"security"_q, u"passcode"_q },
	});

	auto passcodeHas = rpl::single(rpl::empty) | rpl::then(
		session->domain().local().localPasscodeChanged()
	) | rpl::map([=] {
		return session->domain().local().hasPasscode();
	});
	auto passcodeLabel = rpl::combine(
		tr::lng_settings_cloud_password_on(),
		tr::lng_settings_cloud_password_off(),
		rpl::duplicate(passcodeHas)
	) | rpl::map([](const QString &on, const QString &off, bool has) {
		return has ? on : off;
	});

	builder.addButton({
		.id = u"security/passcode"_q,
		.title = tr::lng_settings_passcode_title(),
		.icon = { &st::menuIconLock },
		.label = std::move(passcodeLabel),
		.onClick = [=, passcodeHas = std::move(passcodeHas)]() mutable {
			if (rpl::variable<bool>(std::move(passcodeHas)).current()) {
				showOther(Settings::LocalPasscodeCheckId());
			} else {
				showOther(Settings::LocalPasscodeCreateId());
			}
		},
		.keywords = { u"passcode"_q, u"lock"_q, u"pin"_q },
	});

	builder.addSkip(st::settingsPrivacySecurityPadding);
}

} // namespace CustomBackend::Security
