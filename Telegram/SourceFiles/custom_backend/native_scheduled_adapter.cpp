/*
FoxMes bridge: scheduled messages.
*/
#include "custom_backend/native_scheduled_adapter.h"

#include "api/api_common.h"
#include "base/debug_log.h"
#include "base/flat_set.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/components/scheduled_messages.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>

namespace CustomBackend::Scheduled {
namespace {

[[nodiscard]] QString NewOperationId() {
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Turns a reminder page into the native messages the scheduled list is built
// from. A reminder that cannot be rendered is dropped rather than shown half
// built: the list is what the user will send, and a wrong preview of it is
// worse than a missing row.
[[nodiscard]] QVector<MTPMessage> BuildMessages(
		not_null<NativeBridge*> bridge,
		not_null<History*> history,
		const QJsonArray &items) {
	auto result = QVector<MTPMessage>();
	result.reserve(items.size());
	for (const auto &value : items) {
		if (!value.isObject()) {
			continue;
		}
		if (auto message = bridge->prepareReminder(history, value.toObject())) {
			result.push_back(std::move(*message));
		}
	}
	return result;
}

// The chat a reminder event belongs to. Looked up by peer id directly and not
// through peerLoaded(): an event must not depend on the load status of a peer.
[[nodiscard]] History *HistoryForEvent(
		not_null<Main::Session*> session,
		const QJsonObject &data) {
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return nullptr;
	}
	return bridge->historyForChat(
		data.value("chat_id").toVariant().toLongLong());
}

void ApplyItems(
		not_null<History*> history,
		const QJsonArray &items,
		bool full) {
	const auto session = &history->session();
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	Apply(
		&session->scheduledMessages(),
		history,
		BuildMessages(bridge, history, items),
		full);
}

} // namespace

TimeId DeliveryDate(const QJsonObject &reminder) {
	if (reminder.value("deliver_when_online").toBool()) {
		// Upstream already has a marker for a scheduled message with no time:
		// it is what HasScheduledDate() and the bottom info read as "when
		// online", so the reminder carries exactly that and nothing native
		// has to learn a new concept.
		return Api::kScheduledUntilOnlineTimestamp;
	}
	const auto at = reminder.value("deliver_at").toVariant().toLongLong();
	return (at > 0) ? TimeId(at) : TimeId(0);
}

void Request(not_null<History*> history) {
	const auto session = &history->session();
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	const auto raw = history.get();
	bridge->resolveChatId(raw, [=](qint64 chatId) {
		if (chatId <= 0) {
			return;
		}
		ClientFor(session).reminders(
			chatId,
			[=](QJsonDocument doc, QString error, int status) {
				if (!error.isEmpty() || !doc.isObject()) {
					LOG(("FoxMes: reminder list failed (%1, %2)"
						).arg(status).arg(error));
					return;
				}
				// A page is authoritative: it replaces the queue, so a
				// reminder cancelled elsewhere disappears here too.
				ApplyItems(raw, doc.object().value("items").toArray(), true);
			});
	});
}

void SendNow(not_null<PeerData*> peer, const QVector<MTPint> &ids) {
	const auto session = &peer->session();
	for (const auto &id : ids) {
		ClientFor(session).sendReminderNow(
			id.v,
			NewOperationId(),
			[](QJsonDocument, QString error, int status) {
				if (!error.isEmpty()) {
					LOG(("FoxMes: reminder send now failed (%1, %2)"
						).arg(status).arg(error));
				}
			});
	}
}

void Delete(not_null<PeerData*> peer, const QVector<MTPint> &ids) {
	const auto session = &peer->session();
	for (const auto &id : ids) {
		// One reminder per selected item: an album is several rows and the
		// user selected the ones they meant, so the whole group is never
		// taken down on their behalf.
		ClientFor(session).deleteReminder(
			id.v,
			false,
			NewOperationId(),
			[](QJsonDocument, QString error, int status) {
				if (!error.isEmpty()) {
					LOG(("FoxMes: reminder delete failed (%1, %2)"
						).arg(status).arg(error));
				}
			});
	}
}

bool ApplyEvent(
		not_null<Main::Session*> session,
		const QString &type,
		const QJsonObject &data) {
	if (type == u"reminder.created"_q || type == u"reminder.updated"_q) {
		if (const auto history = HistoryForEvent(session, data)) {
			// An event only adds and updates: it says nothing about the rest
			// of the queue, and treating it as a page would wipe everything
			// it happens not to mention.
			ApplyItems(history, data.value("items").toArray(), false);
		}
		return true;
	} else if (type == u"reminder.deleted"_q) {
		const auto history = HistoryForEvent(session, data);
		if (!history) {
			return true;
		}
		auto ids = QVector<MTPint>();
		for (const auto &value : data.value("reminder_ids").toArray()) {
			const auto id = value.toVariant().toLongLong();
			if (id > 0 && id <= INT32_MAX) {
				ids.push_back(MTP_int(int32(id)));
			}
		}
		if (ids.isEmpty()) {
			return true;
		}
		// sent_message_ids is what upstream calls sent_messages: it tells the
		// scheduled list which item became which real message, which is how
		// the chat scrolls to it instead of just losing a row.
		auto sent = QVector<MTPint>();
		for (const auto &value : data.value("sent_message_ids").toArray()) {
			const auto id = value.toVariant().toLongLong();
			sent.push_back(MTP_int((id > 0 && id <= INT32_MAX) ? int32(id) : 0));
		}
		using Flag = MTPDupdateDeleteScheduledMessages::Flag;
		const auto update = MTP_updateDeleteScheduledMessages(
			MTP_flags(sent.isEmpty() ? Flag() : Flag::f_sent_messages),
			peerToMTP(history->peer->id),
			MTP_vector<MTPint>(ids),
			MTP_vector<MTPint>(sent));
		session->scheduledMessages().apply(
			update.c_updateDeleteScheduledMessages());
		return true;
	}
	return false;
}

// Fills the scheduled list from a reminder page. Mirrors what the upstream
// MTProto parse does, minus the users and chats a REST payload does not carry:
// the entry is created even for an empty page, so an empty queue reads as
// loaded instead of leaving the section on a spinner forever.
void Apply(
		not_null<Data::ScheduledMessages*> messages,
		not_null<History*> history,
		const QVector<MTPMessage> &list,
		bool full) {
	auto received = base::flat_set<not_null<HistoryItem*>>();
	auto clear = base::flat_set<not_null<HistoryItem*>>();
	auto &data = messages->_data.emplace(
		history,
		Data::ScheduledMessages::List()).first->second;
	for (const auto &message : list) {
		if (const auto item = messages->append(history, data, message)) {
			received.emplace(item);
		}
	}
	if (full) {
		for (const auto &owned : data.items) {
			const auto item = owned.get();
			if (!item->isSending() && !received.contains(item)) {
				clear.emplace(item);
			}
		}
	}
	// The same marker upstream's parse leaves behind: list() reads it to tell
	// "loaded and empty" from "not loaded yet", and without it the section
	// shows a spinner after the last reminder is cancelled. The clear timer
	// stays untouched, so nothing ages this entry out from under the section.
	auto &request = messages->_requests[history];
	request.requestId = 0;
	request.lastReceived = crl::now();
	messages->updated(history, received, clear);
}

} // namespace CustomBackend::Scheduled
