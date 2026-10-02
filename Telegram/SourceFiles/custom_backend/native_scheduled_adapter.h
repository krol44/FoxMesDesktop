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
}

namespace CustomBackend::Scheduled {


[[nodiscard]] TimeId DeliveryDate(const QJsonObject &reminder);

void Request(not_null<History*> history);

void Edit(
	not_null<HistoryItem*> item,
	const QString &text,
	const QJsonArray &entities,
	std::function<void(QString)> done);

void SendNow(not_null<PeerData*> peer, const QVector<MTPint> &ids);

void Delete(not_null<PeerData*> peer, const QVector<MTPint> &ids);

bool ApplyEvent(
	not_null<Main::Session*> session,
	const QString &type,
	const QJsonObject &data);

}
