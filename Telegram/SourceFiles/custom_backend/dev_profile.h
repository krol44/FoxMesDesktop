#pragma once

#include <QString>

#include <cstdlib>

namespace CustomBackend {

[[nodiscard]] inline QString ProfileSuffix() {
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
	const auto url = std::getenv("FOXMES_URL");
	if (url && !QString::fromUtf8(url).trimmed().isEmpty()) {
		return u"-dev"_q;
	}
	return u"-local"_q;
#else
	return QString();
#endif
}

}
