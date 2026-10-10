/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <QtCore/QString>

class QImage;
class PeerData;

namespace Data { struct CloudTheme; class CloudThemes; }

namespace Main {
class Session;
}

namespace CustomBackend::ChatThemes {


void Request(not_null<Main::Session*> session);
void RequestUserThemes(not_null<Main::Session*> session);
void RemoveUserTheme(not_null<Main::Session*> session, quint64 id);
Fn<void()> SaveUserTheme(not_null<Main::Session*> session, const Data::CloudTheme &fields, const QByteArray &palette, const QImage &background, Fn<void(Data::CloudTheme)> done, Fn<void(QString)> fail);

void Save(not_null<PeerData*> peer, const QString &emoticon);

void ApplyForPeer(not_null<PeerData*> peer, const QString &emoticon);

}
