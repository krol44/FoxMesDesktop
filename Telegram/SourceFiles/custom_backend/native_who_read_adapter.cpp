#include "custom_backend/native_who_read_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "data/data_message_reactions.h"
#include "data/data_peer_id.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

namespace CustomBackend::WhoRead {

std::vector<Api::WhoReadPeer> Readers(not_null<HistoryItem*> item) {
	auto result = std::vector<Api::WhoReadPeer>();
	// Only an outgoing message has readers to show; an incoming one was read
	// by me and upstream never asks about it.
	if (!item->out() || !item->isRegular()) {
		return result;
	}
	const auto history = item->history();
	const auto bridge = BridgeFor(&history->session());
	if (!bridge) {
		return result;
	}
	// A private chat renders one line that is nothing but the read moment,
	// so a reader without a timestamp would show an empty row; leaving them
	// out makes upstream say "read time is hidden" instead. A group list is
	// about who read, not when, so there an unknown moment is still useful.
	const auto needDate = history->peer->isUser();
	const auto readers = bridge->readersThrough(history, item->id.bare);
	result.reserve(readers.size());
	for (const auto &reader : readers) {
		if (needDate && !reader.date) {
			continue;
		}
		result.push_back(Api::WhoReadPeer{
			.peer = peerFromUser(UserId(reader.userId)),
			.date = reader.date,
		});
	}
	return result;
}

Reacted Reactors(
		not_null<HistoryItem*> item,
		const Data::ReactionId &reaction) {
	auto result = Reacted();
	for (const auto &one : item->reactions()) {
		if (reaction.empty() || one.id == reaction) {
			result.fullCount += one.count;
		}
	}
	for (const auto &[id, recent] : item->recentReactions()) {
		if (!reaction.empty() && id != reaction) {
			continue;
		}
		for (const auto &one : recent) {
			result.list.push_back(Reactor{
				.peer = one.peer->id,
				.reaction = id,
			});
		}
	}
	return result;
}

} // namespace CustomBackend::WhoRead
