/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <vector>

namespace Lang {
struct Language;
}

namespace CustomBackend::Language {


[[nodiscard]] std::vector<Lang::Language> List();

bool Switch(const Lang::Language &language);

}
