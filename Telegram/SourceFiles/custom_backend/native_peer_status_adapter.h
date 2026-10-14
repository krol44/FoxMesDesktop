#pragma once

#include "custom_backend/native_mtp_router.h"
#include <QtCore/QJsonObject>

class PeerData;

namespace CustomBackend::PeerStatuses {

[[nodiscard]] bool Available(Main::Session *session);
[[nodiscard]] QJsonObject Merge(
    not_null<Main::Session*> session,
    PeerId peer,
    const QJsonObject &value);
void Apply(not_null<Main::Session*> session, not_null<PeerData*> peer, const QJsonObject &value);
void ApplyUser(not_null<Main::Session*> session, const QJsonObject &value);
[[nodiscard]] MTPEmojiStatus NativeStatus(const QJsonObject &value);
void Refresh(not_null<Main::Session*> session);
[[nodiscard]] bool ApplyEvent(not_null<Main::Session*> session, const QString &type, const QJsonObject &value);
[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId, const MTP::details::SerializedRequest &request);
void ClearSession(not_null<Main::Session*> session);

}
