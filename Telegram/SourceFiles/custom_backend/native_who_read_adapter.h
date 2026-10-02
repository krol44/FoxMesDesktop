#pragma once

#include "api/api_who_reacted.h"
#include "data/data_message_reaction_id.h"

#include <vector>

class HistoryItem;

namespace CustomBackend::WhoRead {

[[nodiscard]] std::vector<Api::WhoReadPeer> Readers(
	not_null<HistoryItem*> item);

struct Reactor {
	PeerId peer = 0;
	Data::ReactionId reaction;
};

struct Reacted {
	std::vector<Reactor> list;
	int fullCount = 0;
};

[[nodiscard]] Reacted Reactors(
	not_null<HistoryItem*> item,
	const Data::ReactionId &reaction);

}
