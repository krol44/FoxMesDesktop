#pragma once
#include "custom_backend/native_mtp_router.h"
#include <QtCore/QJsonObject>

class QPainter;
class QRect;

namespace Ui {
struct ChatPaintContext;
}

namespace CustomBackend::Appearance {
void Load(not_null<Main::Session*> session);
void Apply(not_null<Main::Session*> session, const QJsonObject &user);
void PaintReply(PeerData *peer, const Ui::ChatPaintContext &context,
    bool inBubble, Fn<void(const Ui::ChatPaintContext &)> paint);
void PaintProfileOutline(not_null<PeerData*> peer, QPainter &p,
    const QRect &geometry, float64 progress, bool nativeOutline, Fn<void()> paint);
bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(not_null<MTP::Instance*> instance, mtpRequestId requestId,
    const MTP::details::SerializedRequest &request);
}
