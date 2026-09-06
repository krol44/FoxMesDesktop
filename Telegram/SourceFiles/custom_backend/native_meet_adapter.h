/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "base/basic_types.h"

class History;
class PeerData;

namespace CustomBackend::Meet {

// FoxMes has no MTProto calls, so the handset button of the chat top bar does
// what the "create meet" action of fxl-web does: it books a meet room for
// the two participants and sends its link into the chat as an ordinary message.

// Whether the button belongs on this chat at all. Meet rooms exist only for a
// dialog with a second person, so Saved Messages and bots are out. Takes a
// nullable peer because the only caller is a top bar that may have no chat.
[[nodiscard]] bool Available(PeerData *peer);

// Books the room, sends the link, and opens it in the system browser. Repeated
// clicks while a request is in flight are ignored, so one impatient user does
// not book two rooms and post two links.
void Start(not_null<History*> history);

} // namespace CustomBackend::Meet
