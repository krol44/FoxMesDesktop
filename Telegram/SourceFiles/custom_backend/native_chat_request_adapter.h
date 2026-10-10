/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "base/basic_types.h"

#include "mtproto/core_types.h"

class History;
class PeerData;
class QJsonObject;
class QJsonValue;
class QString;
class QWidget;
class UserData;

namespace Main {
class Session;
} // namespace Main

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
} // namespace details
} // namespace MTP

namespace Ui {
class FlatButton;
} // namespace Ui

namespace HistoryView {
class Element;
class MediaGeneric;
class MediaGenericPart;
} // namespace HistoryView

namespace Window {
class SessionController;
} // namespace Window

namespace CustomBackend::ChatRequests {

void Apply(
	not_null<Main::Session*> session,
	qint64 userId,
	qint64 chatId,
	const QJsonValue &request);
void Forget(not_null<Main::Session*> session, qint64 userId);
void ListApplied(not_null<Main::Session*> session);
// chat.updated changed the unread mark: opening a request chat on any device
// clears it, which takes the request notification down.
void UnreadMarkApplied(not_null<History*> history);
void RememberUser(
	not_null<Main::Session*> session,
	const QJsonObject &user);
[[nodiscard]] bool HandleEvent(
	not_null<Main::Session*> session,
	const QString &type,
	const QJsonObject &data);

[[nodiscard]] bool CountsAsContact(
	not_null<Main::Session*> session,
	qint64 userId);
[[nodiscard]] bool Gated(not_null<PeerData*> peer);
[[nodiscard]] bool ComposeBlocked(not_null<PeerData*> peer);

void BindComposeButton(
	not_null<Ui::FlatButton*> button,
	not_null<PeerData*> peer);
void ComposeClicked(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> peer);

[[nodiscard]] auto GeneratePeerInfo(
	not_null<HistoryView::Element*> parent,
	not_null<UserData*> user)
-> Fn<void(
	not_null<HistoryView::MediaGeneric*>,
	Fn<void(std::unique_ptr<HistoryView::MediaGenericPart>)>)>;
// Chat list text of a request chat without messages ("Wants to chat with
// you"), empty when there is no request.
[[nodiscard]] QString ListPreview(not_null<PeerData*> peer);
// The registration date arrived after the card was built without it.
[[nodiscard]] bool PeerInfoStale(not_null<UserData*> user);

void DecorateBar(
	not_null<Ui::FlatButton*> accept,
	not_null<Ui::FlatButton*> decline,
	not_null<QWidget*> close);
void Answer(
	not_null<Window::SessionController*> window,
	not_null<UserData*> user,
	bool accept);

// messages.getPeerSettings: the contact bar asks it when it is not hidden.
[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

} // namespace CustomBackend::ChatRequests
