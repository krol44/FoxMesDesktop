#pragma once

#include "custom_backend/dev_profile.h"

#include <QSettings>
#include <QString>
#include <QtGlobal>

namespace CustomBackend {

struct StoredTokens {
	QString access;
};

namespace details {

inline QString TokenStorePrefix(qint64 userId) {
	return u"accounts/"_q + QString::number(userId);
}

inline QSettings Settings() {
	return QSettings(u"FoxMes"_q, u"FoxMesDesktop"_q + ProfileSuffix());
}

}

inline StoredTokens LoadStoredTokens(qint64 userId) {
	const auto settings = details::Settings();
	const auto prefix = details::TokenStorePrefix(userId);
	auto result = StoredTokens();
	result.access = settings.value(prefix + u"/access"_q).toString();
	return result;
}

inline void SaveStoredTokens(qint64 userId, const QString &access) {
	auto settings = details::Settings();
	const auto prefix = details::TokenStorePrefix(userId);
	settings.setValue(prefix + u"/access"_q, access);
	settings.remove(prefix + u"/refresh"_q);
	settings.sync();
}

inline void ClearStoredTokens(qint64 userId) {
	auto settings = details::Settings();
	const auto prefix = details::TokenStorePrefix(userId);
	settings.remove(prefix + u"/access"_q);
	settings.remove(prefix + u"/refresh"_q);
	settings.sync();
}

}
