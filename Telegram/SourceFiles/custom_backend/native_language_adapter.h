/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <vector>

namespace Lang {
struct Language;
} // namespace Lang

namespace CustomBackend::Language {

// Interface language. Upstream reads the list from langpack.getLanguages and
// applies a choice through langpack.getStrings / langpack.getDifference, none
// of which answer under the bridge - which is why the language box never even
// opened (LanguageBox::Show waits for a languageListChanged that never comes)
// and why clicking a language did nothing (switchToLanguage sits on
// langpack.getStrings forever, and even past it restartAfterSwitch would wait
// on a langpack difference request that also never completes).
//
// The list is what the client actually has: FoxMes ships one compiled pack.
// The backend already serves GET /langs for the day there is a second one, so
// adding a translation later is a row there and nothing here.

// The languages this client can switch to.
[[nodiscard]] std::vector<Lang::Language> List();

// Applies a language and restarts, without any MTProto round trip. Returns
// false when the id is not one we have, so the caller can keep upstream's own
// "not found" branch.
bool Switch(const Lang::Language &language);

} // namespace CustomBackend::Language
