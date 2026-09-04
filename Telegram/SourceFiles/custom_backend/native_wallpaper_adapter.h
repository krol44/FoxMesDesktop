/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <QtCore/QJsonObject>

class PeerData;

namespace Data {
class WallPaper;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace CustomBackend::Wallpapers {

// Chat wallpapers. The UI is upstream's own - BackgroundBox for the gallery and
// BackgroundPreviewBox for the preview with its Blurred toggle, dimming slider
// and "apply for me / for both" choice. Nothing here draws anything: this is
// only the transport those two widgets talk to instead of MTProto.
//
// Every MTProto call they make has a counterpart below:
//   account.getWallPapers      -> RequestGallery   (GET /wallpapers)
//   account.saveWallPaper      -> Remove           (DELETE /wallpapers/{sha})
//   account.uploadWallPaper    -> Upload           (POST /upload/wallpaper-chat)
//   account.installWallPaper   -> SaveDefault      (PUT /wallpaper)
//   messages.setChatWallPaper  -> SaveForPeer      (PUT /chats/{id}/wallpaper)

// Fills Data::Session::wallpapers() from the server and calls done() once it
// has. Answers the hook in BackgroundBox::Inner::requestPapers.
void RequestGallery(not_null<Main::Session*> session, Fn<void()> done);

// Drops one picture from the gallery.
void Remove(not_null<Main::Session*> session, const Data::WallPaper &paper);

// Uploads a picture chosen from a file and hands back the paper it became, so
// the caller can apply it exactly as it applies one picked from the gallery.
void Upload(
	not_null<Main::Session*> session,
	const QImage &image,
	Fn<void(std::optional<Data::WallPaper>)> done);

// Stores the choice. blurred and the dimming intensity ride on the paper, which
// is where upstream keeps them too.
// both applies the picture to the other side of the conversation as well,
// which is the "Apply for me and <name>" choice of the preview box.
void SaveForPeer(
	not_null<PeerData*> peer,
	const Data::WallPaper &paper,
	bool both);
void SaveDefault(
	not_null<Main::Session*> session,
	const Data::WallPaper &paper);
// Clears a chat's own picture, so it follows the per-user default again.
void ResetForPeer(not_null<PeerData*> peer);
// Gives a chat no background at all, whatever the account default is.
void SetNoneForPeer(not_null<PeerData*> peer);

// The "no background" paper: a colours-only wall paper filled with the theme's
// own window background. Deliberately not Data::ThemeWallPaper() - that one
// carries no colours, no document and no thumbnail, so the gallery draws an
// empty cell for it and, worse, a chat given it renders from an unprepared
// background image.
[[nodiscard]] Data::WallPaper NoBackgroundPaper();
[[nodiscard]] bool IsNoBackground(const Data::WallPaper &paper);

// Applies the "no background" choice. forPeer scopes it to one chat, which is a
// state of its own - not the same as clearing the chat's picture, because that
// would only bring back the account default. Answers false for any other paper,
// so the caller falls through to its normal preview flow.
[[nodiscard]] bool ChooseNoBackground(
	not_null<Window::SessionController*> controller,
	PeerData *forPeer,
	const Data::WallPaper &paper);

// Applies the stored default once the session is up, so a fresh install picks
// up the picture chosen on another device.
void RequestDefault(not_null<Main::Session*> session);

// Applies the "wallpaper" object of a chat payload, and clears the chat's own
// picture when it is absent - which is how a chat says it follows the default.
void ApplyForPeer(not_null<PeerData*> peer, const QJsonObject &wallpaper);

// Drops what this adapter remembers about a session.
void ClearSession(not_null<Main::Session*> session);

} // namespace CustomBackend::Wallpapers
