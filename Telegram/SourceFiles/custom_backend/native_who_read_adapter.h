#pragma once

#include "api/api_who_reacted.h"
#include "data/data_message_reaction_id.h"

#include <vector>

class HistoryItem;

namespace CustomBackend::WhoRead {

// Everyone but me whose fxl-api read watermark already covers this item,
// with the read moment the server recorded. The bridge answers from the
// read state it already tracks, so there is nothing to wait for.
[[nodiscard]] std::vector<Api::WhoReadPeer> Readers(
	not_null<HistoryItem*> item);

// One reactor of a message. fxl-api has no separate "reactions list"
// endpoint: recent reactors ride along with the message, so the answer is
// read back from the reactions already applied to the item.
struct Reactor {
	PeerId peer = 0;
	Data::ReactionId reaction;
};

struct Reacted {
	std::vector<Reactor> list;
	int fullCount = 0;
};

// Pass an empty reaction for "everyone who reacted with anything".
[[nodiscard]] Reacted Reactors(
	not_null<HistoryItem*> item,
	const Data::ReactionId &reaction);

} // namespace CustomBackend::WhoRead
