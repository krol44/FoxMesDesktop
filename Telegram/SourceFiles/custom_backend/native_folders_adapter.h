#pragma once
#include "custom_backend/native_mtp_router.h"
#include <QtCore/QJsonObject>

namespace Data { class ChatFilter; }

namespace CustomBackend::Folders {
void BindEditor(not_null<Main::Session*> session,
    const Data::ChatFilter &initial,
    Fn<void(const Data::ChatFilter &)> &done,
    Fn<void(const Data::ChatFilter &, Fn<void(Data::ChatFilter)>)> &saveAnd);
bool Supported(Main::Session *session);
bool SharingEnabled(Main::Session *session);
void Refresh(not_null<Main::Session*> session, bool reset = false);
void Receive(not_null<Main::Session*> session, const QJsonObject &state);
bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId,
    const MTP::details::SerializedRequest &request);
}
