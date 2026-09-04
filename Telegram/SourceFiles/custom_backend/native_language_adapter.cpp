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
	// Telegram/Resources/langs holds exactly one translation, lang.strings;
	// the *.lproj folders next to it are macOS bundle strings, not UI packs.
	// Everything else upstream offers comes from the cloud langpack.
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
		// Already current. Upstream answers this with lng_language_already
		// from requestLanguageAndSwitch; the box calls us only after its own
		// check, so this is the belt for a direct caller.
		return true;
	}
	Local::pushRecentLanguage(found);
	Lang::GetInstance().switchToId(found);
	// Restart immediately rather than through CloudManager::restartAfterSwitch:
	// that one defers while a langpack request is in flight, and under the
	// bridge such a request never completes, so the restart would never happen.
	// Nothing is in flight here - the pack is compiled in.
	Core::Restart();
	return true;
}

} // namespace CustomBackend::Language
