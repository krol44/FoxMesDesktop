#pragma once

#include <QString>

#include <functional>

namespace Main {
class Session;
}

namespace CustomBackend {

void RequestWebPagePreview(
	Main::Session *session,
	const QString &link,
	std::function<void(const MTPDmessageMediaWebPage&)> done,
	std::function<void()> fail);

void CancelWebPagePreview(Main::Session *session, const QString &link);

}
