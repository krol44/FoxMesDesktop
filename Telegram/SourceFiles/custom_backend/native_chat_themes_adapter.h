/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <QtCore/QString>

class PeerData;

namespace Main {
class Session;
}

namespace CustomBackend::ChatThemes {


void Request(not_null<Main::Session*> session);

void Save(not_null<PeerData*> peer, const QString &emoticon);

void ApplyForPeer(not_null<PeerData*> peer, const QString &emoticon);

}
