#include "custom_backend/native_delete_adapter.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_scheduled_adapter.h"
#include "base/flat_map.h"
#include "data/components/scheduled_messages.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "ui/effects/thanos_effect.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <algorithm>
#include <memory>

namespace CustomBackend {
namespace {

class DeleteRequest final : public std::enable_shared_from_this<DeleteRequest> {
public:
	DeleteRequest(
		not_null<ApiClient*> client,
		qint64 chatId,
		QList<qint64> messageIds,
		QString operationId,
		std::function<void(QJsonDocument)> done);
	void send();

private:
	void resolve();
	void retry();
	void complete(QJsonDocument result);

	const QPointer<ApiClient> _client;
	const qint64 _chatId = 0;
	const QList<qint64> _messageIds;
	const QString _operationId;
	std::function<void(QJsonDocument)> _done;
	int _retryDelay = 1000;

};

DeleteRequest::DeleteRequest(
		not_null<ApiClient*> client,
		qint64 chatId,
		QList<qint64> messageIds,
		QString operationId,
		std::function<void(QJsonDocument)> done)
: _client(client.get())
, _chatId(chatId)
, _messageIds(std::move(messageIds))
, _operationId(std::move(operationId))
, _done(std::move(done)) {
}

void DeleteRequest::send() {
	if (!_client) {
		return;
	}
	const auto self = shared_from_this();
	_client->deleteMessages(
		_chatId,
		_messageIds,
		_operationId,
		[self](QJsonDocument doc, QString error, int status) {
			if (IsTransportFailure(status)
				|| status >= 500
				|| status == 408
				|| status == 429) {
				self->retry();
			} else if (error.isEmpty()
				&& doc.isObject()
				&& doc.object().value("deleted_ids").isArray()) {
				self->complete(std::move(doc));
			} else if (status >= 200 && status < 300) {
				self->retry();
			} else {
				self->complete({});
			}
		});
}

void DeleteRequest::resolve() {
	if (!_client) {
		return;
	}
	const auto self = shared_from_this();
	_client->operationResult(
		_operationId,
		[self](QJsonDocument doc, QString error, int status) {
			if (IsTransportFailure(status)
				|| status >= 500
				|| status == 408
				|| status == 429) {
				self->retry();
				return;
			}
			const auto object = doc.object();
			const auto result = object.value("result").toObject();
			if (error.isEmpty()
				&& object.value("status").toString() == u"done"_q
				&& result.value("deleted_ids").isArray()) {
				self->complete(QJsonDocument(result));
			} else if (error.isEmpty()
				&& object.value("status").toString() == u"unknown"_q) {
				self->send();
			} else if (status >= 200 && status < 300) {
				self->retry();
			} else {
				self->complete({});
			}
		});
}

void DeleteRequest::retry() {
	if (!_client) {
		return;
	}
	const auto self = shared_from_this();
	QTimer::singleShot(_retryDelay, _client.data(), [self] {
		self->resolve();
	});
	_retryDelay = std::min(_retryDelay * 2, 16'000);
}

void DeleteRequest::complete(QJsonDocument result) {
	if (const auto done = std::move(_done)) {
		done(std::move(result));
	}
}

}

void DeleteMessagesWithRecovery(
		not_null<ApiClient*> client,
		qint64 chatId,
		const QList<qint64> &messageIds,
		const QString &operationId,
		std::function<void(QJsonDocument)> done) {
	std::make_shared<DeleteRequest>(
		client,
		chatId,
		messageIds,
		operationId,
		std::move(done))->send();
}

bool NativeDeleteEffectAvailable() {
    return Ui::ThanosEffect::Supported();
}

void DeleteMessagesWithEffect(
        Main::Session *session,
        const std::vector<not_null<HistoryItem*>> &items) {
    if (!session || items.empty()) {
        return;
    }
    session->data().notifyItemsAboutToBeDestroyed(items);
    for (const auto &item : items) {
        item->destroy();
    }
}

void DeleteSelectedMessages(
        not_null<Main::Session*> session,
        const MessageIdsList &ids,
        bool revoke) {
    const auto bridge = Enabled() ? BridgeFor(session) : nullptr;
    if (!bridge) {
        return;
    }
    auto grouped = base::flat_map<not_null<History*>, std::vector<int32_t>>();
    auto local = std::vector<not_null<HistoryItem*>>();
    auto scheduled = base::flat_map<not_null<PeerData*>, QVector<MTPint>>();
    for (const auto &fullId : ids) {
        const auto item = session->data().message(fullId);
        if (!item) {
            continue;
        }
        if (item->isScheduled()) {
            if (!item->isSending() && !item->hasFailed()) {
                scheduled[item->history()->peer].push_back(MTP_int(
                    session->scheduledMessages().lookupId(item)));
            }
            local.push_back(item);
            continue;
        }
        if (!item->isRegular() || item->id.bare <= 0) {
            local.push_back(item);
            continue;
        }
        grouped[item->history()].push_back(item->id.bare);
    }
    for (auto &[history, messageIds] : grouped) {
        bridge->deleteMessages(history, messageIds, revoke);
    }
    for (const auto &[peer, reminderIds] : scheduled) {
        Scheduled::Delete(peer, reminderIds);
    }
    if (!local.empty()) {
        DeleteMessagesWithEffect(session, local);
        session->data().sendHistoryChangeNotifications();
    }
}

void DeleteMessages(
        not_null<History*> history,
        const QVector<MTPint> &ids,
        bool revoke,
        std::function<void()> finish) {
    const auto bridge = Enabled()
        ? BridgeFor(&history->session())
        : nullptr;
    if (!bridge) {
        if (finish) finish();
        return;
    }
    auto plain = std::vector<int32_t>();
    plain.reserve(ids.size());
    for (const auto &id : ids) plain.push_back(id.v);
    bridge->deleteMessages(history, std::move(plain), revoke, std::move(finish));
}

void DeleteHistory(not_null<PeerData*> peer, bool justClear, bool) {
    const auto bridge = Enabled()
        ? BridgeFor(&peer->session())
        : nullptr;
    if (!bridge) {
        return;
    }
    bridge->deleteHistory(
        peer->owner().history(peer),
        !justClear && !peer->isSelf());
}

void DeleteMessagesByDates(
        not_null<History*> history,
        TimeId minDate,
        TimeId maxDate) {
    const auto bridge = Enabled()
        ? BridgeFor(&history->session())
        : nullptr;
    if (!bridge) {
        return;
    }
    bridge->deleteMessagesByDates(history, minDate, maxDate);
}

}
