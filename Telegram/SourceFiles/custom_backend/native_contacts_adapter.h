/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "base/basic_types.h"

#include "mtproto/core_types.h"

class QImage;
class PeerData;
class UserData;

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
} // namespace details
} // namespace MTP

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace CustomBackend::Contacts {

void FillEditBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> window,
	not_null<UserData*> user,
	bool focusNote = false);

void Block(not_null<PeerData*> peer);
void Unblock(not_null<PeerData*> peer, Fn<void(bool success)> done);

void UploadPhoto(
	not_null<UserData*> user,
	QImage &&image,
	Fn<void()> done);

// contacts.updateContactNote: the profile's "Delete note" sends it directly.
[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

} // namespace CustomBackend::Contacts
