#pragma once

#include "custom_backend/dev_profile.h"
#include "main/main_account.h"
#include "storage/storage_account.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>

#include <array>
#include <string>

namespace CustomBackend {

struct StoredAuth {
	qint64 sid = 0;
	QByteArray secret;
	QJsonObject user;
};

namespace details {

inline constexpr auto kSidPref = std::string_view("foxmes_sid");
inline constexpr auto kSecretPref = std::string_view("foxmes_secret");
inline constexpr auto kUserPref = std::string_view("foxmes_user");
inline constexpr auto kChatsCachePref = std::string_view("foxmes_chats_cache");
inline constexpr auto kReadJournalPref = std::string_view(
	"foxmes_read_journal");
inline constexpr auto kNotifyScopes = std::array{
	std::string_view("user"),
	std::string_view("group"),
	std::string_view("channel"),
};

[[nodiscard]] inline std::string NotifyDefaultsPref(std::string_view scope) {
	return "foxmes_notify_defaults_" + std::string(scope);
}

inline QSettings Settings() {
	return QSettings(u"FoxMes"_q, u"FoxMesDesktop"_q + ProfileSuffix());
}

inline void ForgetPlaintextData() {
	auto settings = Settings();
	settings.remove(u"auth"_q);
	settings.remove(u"read_journal"_q);
	settings.beginGroup(u"accounts"_q);
	for (const auto &group : settings.childGroups()) {
		settings.remove(group + u"/access"_q);
		settings.remove(group + u"/refresh"_q);
		settings.remove(group + u"/user"_q);
		settings.remove(group + u"/chats_cache"_q);
		settings.beginGroup(group);
		for (const auto &key : settings.childKeys()) {
			if (key.startsWith(u"notify_defaults"_q)) {
				settings.remove(key);
			}
		}
		settings.endGroup();
	}
	settings.endGroup();
	settings.sync();
}

}

inline StoredAuth LoadStoredAuth(not_null<Main::Account*> account) {
	auto &local = account->local();
	const auto user = QJsonDocument::fromJson(
		local.readPref<QString>(details::kUserPref).toUtf8());
	return {
		.sid = local.readPref<QString>(details::kSidPref).toLongLong(),
		.secret = QByteArray::fromBase64(
			local.readPref<QString>(details::kSecretPref).toLatin1()),
		.user = user.isObject() ? user.object() : QJsonObject(),
	};
}

inline void SaveStoredAuth(
		not_null<Main::Account*> account,
		qint64 sid,
		const QByteArray &secret,
		const QJsonObject &user) {
	auto &local = account->local();
	if (sid > 0 && !secret.isEmpty()) {
		local.writePref<QString>(details::kSidPref, QString::number(sid));
		local.writePref<QString>(
			details::kSecretPref,
			QString::fromLatin1(secret.toBase64()));
	} else {
		local.clearPref(details::kSidPref);
		local.clearPref(details::kSecretPref);
	}
	local.writePref<QString>(
		details::kUserPref,
		QString::fromUtf8(
			QJsonDocument(user).toJson(QJsonDocument::Compact)));
}

inline void ClearStoredAuth(not_null<Main::Account*> account) {
	auto &local = account->local();
	local.clearPref(details::kSidPref);
	local.clearPref(details::kSecretPref);
	local.clearPref(details::kUserPref);
}

[[nodiscard]] inline QByteArray ReadStoredData(
		not_null<Main::Account*> account,
		std::string_view key) {
	return account->local().readPref<QString>(key).toUtf8();
}

inline void WriteStoredData(
		not_null<Main::Account*> account,
		std::string_view key,
		const QByteArray &value) {
	auto &local = account->local();
	if (value.isEmpty()) {
		local.clearPref(key);
	} else {
		local.writePref<QString>(key, QString::fromUtf8(value));
	}
}

inline void ClearStoredCaches(not_null<Main::Account*> account) {
	auto &local = account->local();
	local.clearPref(details::kChatsCachePref);
	local.clearPref(details::kReadJournalPref);
	for (const auto scope : details::kNotifyScopes) {
		local.clearPref(details::NotifyDefaultsPref(scope));
	}
}

}
