/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <QtCore/QJsonObject>

class PeerData;

namespace Data {
class WallPaper;
}

namespace Main {
class Session;
}

namespace Window {
class SessionController;
}

namespace CustomBackend::Wallpapers {


void RequestGallery(not_null<Main::Session*> session, Fn<void()> done);

void Remove(not_null<Main::Session*> session, const Data::WallPaper &paper);

void Upload(
	not_null<Main::Session*> session,
	const QImage &image,
	Fn<void(std::optional<Data::WallPaper>)> done);

void SaveForPeer(
	not_null<PeerData*> peer,
	const Data::WallPaper &paper,
	bool both);
void SaveDefault(
	not_null<Main::Session*> session,
	const Data::WallPaper &paper);
void ResetForPeer(not_null<PeerData*> peer);
void SetNoneForPeer(not_null<PeerData*> peer);

[[nodiscard]] Data::WallPaper NoBackgroundPaper();
[[nodiscard]] bool IsNoBackground(const Data::WallPaper &paper);

[[nodiscard]] bool ChooseNoBackground(
	not_null<Window::SessionController*> controller,
	PeerData *forPeer,
	const Data::WallPaper &paper);

void RequestDefault(not_null<Main::Session*> session);

void ApplyForPeer(not_null<PeerData*> peer, const QJsonObject &wallpaper);

void ClearSession(not_null<Main::Session*> session);

}
