#include "custom_backend/native_web_page_adapter.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"

#include "main/main_session.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <map>
#include <optional>
#include <utility>

namespace CustomBackend {
namespace {

std::map<Main::Session*, QString> &PendingPreviewLinks() {
	static auto result = std::map<Main::Session*, QString>();
	return result;
}

} // namespace

void RequestWebPagePreview(
		Main::Session *session,
		const QString &link,
		std::function<void(const MTPDmessageMediaWebPage&)> done,
		std::function<void()> fail) {
	if (!session || link.isEmpty()) {
		if (fail) fail();
		return;
	}
	PendingPreviewLinks()[session] = link;
	ClientFor(session).linkPreview(link, [=](
			QJsonDocument doc,
			QString error,
			int status) {
		auto &pending = PendingPreviewLinks();
		const auto i = pending.find(session);
		if (i == pending.end() || i->second != link) {
			return;
		}
		pending.erase(i);
		const auto page = (error.isEmpty() && doc.isObject())
			? doc.object().value("web_page").toObject()
			: QJsonObject();
		const auto media = page.isEmpty()
			? std::nullopt
			: NativeBridge::WebPageMedia(session, page);
		if (!media) {
			if (fail) fail();
			return;
		}
		if (done) done(media->c_messageMediaWebPage());
	});
}

void CancelWebPagePreview(Main::Session *session, const QString &link) {
	if (!session) {
		return;
	}
	auto &pending = PendingPreviewLinks();
	const auto i = pending.find(session);
	if (i != pending.end() && i->second == link) {
		pending.erase(i);
	}
}

}
