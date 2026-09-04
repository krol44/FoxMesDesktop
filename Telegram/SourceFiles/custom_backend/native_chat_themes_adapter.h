/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include <QtCore/QString>

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace CustomBackend::ChatThemes {

// Chat themes - the "bubble design" picker. Upstream fills the catalog from
// account.getChatThemes and writes a choice with messages.setChatTheme;
// neither answers under the bridge, so the picker showed only "No Theme".
//
// The catalog is compiled into the client. A theme here is a name (its emoji)
// plus, per light and dark, the background gradient and the outgoing bubble
// colours - no documents, no patterns, nothing to download. Only the choice
// is stored server-side, as one string per (chat, user).

// Fills Data::CloudThemes. Answers the hook in CloudThemes::refreshChatThemes.
void Request(not_null<Main::Session*> session);

// Stores the choice. Answers the hook in SendPeerThemeChangeRequest, which
// otherwise sends messages.setChatTheme into a transport that never replies.
void Save(not_null<PeerData*> peer, const QString &emoticon);

// Applies the choice from a chat payload. An empty emoticon is "no theme",
// which is a state of its own and not an absent field.
void ApplyForPeer(not_null<PeerData*> peer, const QString &emoticon);

} // namespace CustomBackend::ChatThemes
