/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_language_adapter.h"

#include "core/application.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "storage/localstorage.h"

namespace CustomBackend::Language {

std::vector<Lang::Language> List() {
	return { Lang::DefaultLanguage() };
}

bool Switch(const Lang::Language &language) {
	const auto id = Lang::LanguageIdOrDefault(language.id);
	auto found = Lang::Language();
	for (const auto &item : List()) {
		if (item.id == id) {
			found = item;
			break;
		}
	}
	if (found.id.isEmpty()) {
		return false;
	}
	if (Lang::LanguageIdOrDefault(Lang::GetInstance().id()) == found.id) {
		return true;
	}
	Local::pushRecentLanguage(found);
	Lang::GetInstance().switchToId(found);
	Core::Restart();
	return true;
}

}
