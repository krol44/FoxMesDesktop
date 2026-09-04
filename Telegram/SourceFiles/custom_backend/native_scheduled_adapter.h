/*
FoxMes bridge: scheduled messages.
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QtCore/QJsonObject>
#include <QtCore/QVector>

class History;
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

// Delivers scheduled messages ahead of their time.
void SendNow(not_null<PeerData*> peer, const QVector<MTPint> &ids);

// Cancels scheduled messages.
void Delete(not_null<PeerData*> peer, const QVector<MTPint> &ids);

// Applies a live reminder event from the WebSocket. Returns false when the
// type is not a reminder event, so the caller can go on matching.
bool ApplyEvent(
	not_null<Main::Session*> session,
	const QString &type,
	const QJsonObject &data);

} // namespace CustomBackend::Scheduled
