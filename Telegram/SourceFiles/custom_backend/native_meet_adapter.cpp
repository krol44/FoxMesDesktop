/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_meet_adapter.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/debug_log.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "core/file_utilities.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/toast/toast.h"

#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace CustomBackend::Meet {
namespace {

base::flat_set<std::pair<const Main::Session*, qint64>> gPending;

void ShowError(const QString &reason) {
	if (!reason.isEmpty()) {
		LOG(("FoxMes: meet booking failed: %1").arg(reason));
	}
	Ui::Toast::Show(u"Could not create a meet."_q);
}

void SendLink(not_null<History*> history, const QString &url) {
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = { url };
	message.action.clearDraft = false;
	history->session().api().sendMessage(std::move(message));
}

} // namespace

bool Available(PeerData *peer) {
	if (!Enabled() || !peer) {
		return false;
	}
	const auto user = peer->asUser();

	return user && !user->isSelf() && !user->isBot();
}

void Start(not_null<History*> history) {
	const auto session = &history->session();
	const auto bridge = Enabled() ? BridgeFor(session) : nullptr;
	if (!bridge) {
		return;
	}
	const auto weak = base::make_weak(session);
	bridge->resolveChatId(history, [=](qint64 chatId) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		if (chatId <= 0) {
			ShowError(u"chat could not be resolved"_q);
			return;
		}
		const auto key = std::pair<const Main::Session*, qint64>(
			strong,
			chatId);
		if (!gPending.emplace(key).second) {
			return;
		}
		ClientFor(strong).createMeet(
			chatId,
			QUuid::createUuid().toString(QUuid::WithoutBraces),
			[=](QJsonDocument doc, QString error, int) {
				gPending.remove(key);
				const auto strong = weak.get();
				if (!strong) {
					return;
				}
				const auto url = doc.isObject()
					? doc.object().value("url").toString()
					: QString();
				if (!error.isEmpty() || url.isEmpty()) {
					ShowError(error);
					return;
				}
				const auto target = BridgeFor(strong)
					? BridgeFor(strong)->historyForChat(chatId)
					: nullptr;
				if (!target) {
					ShowError(u"chat disappeared while booking"_q);
					return;
				}
				SendLink(target, url);
				File::OpenUrl(url);
			});
	});
}

}
