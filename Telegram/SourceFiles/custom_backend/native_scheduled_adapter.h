/*
FoxMes bridge: scheduled messages.
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QVector>

#include <functional>

class History;
class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace CustomBackend::Scheduled {

// A scheduled message in FoxMes is a reminder: a row of its own queue, private
// to its author until it fires, then delivered by the server through the same
// send path a live message takes. This adapter is the whole transport of that
// queue on the client side; upstream keeps its list, its section and its menu.

// The date a reminder carries as a native message. A timed reminder is dated
// by the moment it will be delivered, because that is the date the scheduled
// list sorts and shows. A when-online one has no time at all and takes
// upstream's sentinel, which HasScheduledDate and the bottom info already
// read as "when online".
[[nodiscard]] TimeId DeliveryDate(const QJsonObject &reminder);

// Replaces the transport of Data::ScheduledMessages::request(). An unanswered
// MTProto request would hold its slot forever under the bridge, so this is not
// an optimisation: the hook is what keeps the request from leaking.
void Request(not_null<History*> history);

// Replaces the transport of an edit made from the scheduled list. A reminder
// is not a chat_messages row: it is addressed by its own id, on its own
// endpoint. The id upstream carries on a scheduled item is the local one it
// mints for the queue (ServerMaxMsgId plus the reminder id), so the message
// edit path would both address the wrong table and overflow the column the
// message ids live in.
void Edit(
	not_null<HistoryItem*> item,
	const QString &text,
	const QJsonArray &entities,
	std::function<void(QString)> done);

void SendNow(not_null<PeerData*> peer, const QVector<MTPint> &ids);

void Delete(not_null<PeerData*> peer, const QVector<MTPint> &ids);

// Applies a live reminder event from the WebSocket. Returns false when the
// type is not a reminder event, so the caller can go on matching.
bool ApplyEvent(
	not_null<Main::Session*> session,
	const QString &type,
	const QJsonObject &data);

} // namespace CustomBackend::Scheduled
