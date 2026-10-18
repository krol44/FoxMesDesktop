/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_stickers_adapter.h"

#include "api/api_common.h"
#include "base/call_delayed.h"
#include "chat_helpers/compose/compose_show.h"
#include "custom_backend/api_client.h"
#include "storage/storage_account.h"

#include <QtCore/QUuid>
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_sticker_sets_adapter.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/emoji_config.h"

namespace CustomBackend::Stickers {
namespace {

struct FavoriteMutation {
	bool desired = false;
	bool sent = false;
	bool running = false;
	bool uncertain = false;
	QString operation;
	std::shared_ptr<ChatHelpers::Show> show;
};

struct FavoriteState {
	std::vector<DocumentId> ids;
	qint64 revision = -1;
	static std::optional<FavoriteState> Parse(const QJsonObject &value) {
		if (!value.value("sticker_ids").isArray()
			|| !value.value("revision").isDouble()
			|| value.value("revision").toVariant().toLongLong() < 0
			|| value.value("limit").toInt() != 100) {
			return std::nullopt;
		}
		FavoriteState result;
		result.revision = value.value("revision").toVariant().toLongLong();
		for (const auto &entry : value.value("sticker_ids").toArray()) {
			const auto id = entry.toVariant().toLongLong();
			if (id <= 0 || id > INT32_MAX
				|| ranges::contains(result.ids, DocumentId(id))) {
				return std::nullopt;
			}
			result.ids.push_back(DocumentId(id));
		}
		if (result.ids.size() > 100) return std::nullopt;
		return result;
	}
};

struct State {
	rpl::lifetime lifetime;
	bool subscribed = false;
	FavoriteState favorites;
	std::shared_ptr<bool> favoritesLifetime = std::make_shared<bool>(true);
	base::flat_map<DocumentId, FavoriteMutation> mutations;
	bool requestingFavorites = false;
	uint64 favoritesGeneration = 0;
};

base::flat_map<not_null<Main::Session*>, State> &States() {
	static auto value = base::flat_map<not_null<Main::Session*>, State>();
	return value;
}

[[nodiscard]] State &StateFor(not_null<Main::Session*> session) {
	return States()[session];
}

void ApplyFavorites(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	if (state.favorites.revision < 0) return;
	auto &stickers = session->data().stickers();
	auto &sets = stickers.setsRef();
	auto documents = Data::StickersPack();
	for (const auto id : state.favorites.ids) {
		const auto key = StickerSets::Item{ .id = id, .sticker = true };
		if (const auto found = StickerSets::Find(
				session,
				StickerSets::DocumentIdFor(key))) {
			documents.push_back(StickerSets::Register(session, *found));
		}
	}
	auto i = sets.find(Data::Stickers::FavedSetId);
	stickers.setLastFavedUpdate(crl::now());
	if ((i == sets.end() && documents.empty())
		|| (i != sets.end() && i->second->stickers == documents)) return;
	if (documents.empty()) {
		if (i != sets.end()) sets.erase(i);
	} else {
		if (i == sets.end()) {
			i = sets.emplace(Data::Stickers::FavedSetId, std::make_unique<Data::StickersSet>(
				&session->data(), Data::Stickers::FavedSetId, uint64(0), uint64(0),
				Lang::Hard::FavedSetTitle(), QString(), 0,
				Data::StickersSetFlag::Special, TimeId(0))).first;
		}
		auto &set = *i->second;
		set.stickers = std::move(documents);
		set.count = set.stickers.size();
		set.emoji.clear();
		for (const auto document : set.stickers) {
			if (const auto emoji = Ui::Emoji::Find(document->sticker()->alt)) {
				set.emoji[emoji].push_back(document);
			}
		}
	}
	session->local().writeFavedStickers();
	stickers.notifyUpdated(Data::StickersType::Stickers);
}

void SendFavorite(not_null<Main::Session*> session, DocumentId id);

void FinishFavorite(not_null<Main::Session*> session, DocumentId id,
		const QString &operation, const QJsonObject &value, const QString &error, int status) {
	const auto owner = States().find(session);
	if (owner == States().end()) return;
	auto &state = owner->second;
	const auto i = state.mutations.find(id);
	if (i == state.mutations.end() || i->second.operation != operation) return;
	const auto parsed = error.isEmpty() && status >= 200 && status < 300
		? FavoriteState::Parse(value) : std::nullopt;
	if (parsed) {
		ReceiveFavorites(session, value);
		auto &mutation = i->second;
		mutation.running = false;
		mutation.uncertain = false;
		mutation.operation.clear();
		if (mutation.desired != mutation.sent) {
			SendFavorite(session, id);
		} else {
			state.mutations.erase(i);
		}
	} else if (status == 0 || status >= 500 || status == 408 || status == 429) {
		i->second.running = false;
		i->second.uncertain = true;
		const auto weak = base::make_weak(session);
		const auto lifetime = std::weak_ptr(state.favoritesLifetime);
		base::call_delayed(2000, session, [weak, id, lifetime] {
			if (lifetime.expired()) return;
			if (const auto session = weak.get()) SendFavorite(session, id);
		});
	} else {
		if (const auto show = i->second.show) {
			show->showToast(u"Could not update favorite stickers."_q);
		}
		state.mutations.erase(i);
	}
}

void SendFavorite(not_null<Main::Session*> session, DocumentId id) {
	const auto owner = States().find(session);
	if (owner == States().end()) return;
	const auto item = owner->second.mutations.find(id);
	if (item == owner->second.mutations.end()) return;
	auto &mutation = item->second;
	if (mutation.running) return;
	mutation.running = true;
	if (mutation.operation.isEmpty()) {
		mutation.sent = mutation.desired;
		mutation.operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	const auto weak = base::make_weak(session);
	const auto operation = mutation.operation;
	const auto lifetime = std::weak_ptr(owner->second.favoritesLifetime);
	const auto finish = [weak, id, operation, lifetime](QJsonDocument doc, QString error, int status) {
		if (lifetime.expired()) return;
		if (const auto session = weak.get()) FinishFavorite(session, id, operation, doc.object(), error, status);
	};
	if (mutation.uncertain) {
		ClientFor(session).operationResult(operation, [weak, id, operation, finish, lifetime](QJsonDocument doc, QString error, int status) {
			if (lifetime.expired()) return;
			const auto session = weak.get();
			if (!session) return;
			const auto owner = States().find(session);
			if (owner == States().end() || !owner->second.mutations.contains(id)) return;
			if (!error.isEmpty()) { finish(doc, error, status); return; }
			const auto result = doc.object();
			if (result.value("status").toString() == u"done"_q) {
				finish(QJsonDocument(result.value("result").toObject()), {}, 200);
			} else if (result.value("status").toString() == u"unknown"_q) {
				ClientFor(session).setStickerFavorite(id,
					StateFor(session).mutations[id].sent, operation, finish);
			} else {
				finish({}, u"Invalid operation response"_q, 500);
			}
		});
	} else {
		ClientFor(session).setStickerFavorite(id, mutation.sent, operation, finish);
	}
}

void Subscribe(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	if (state.subscribed) {
		return;
	}
	state.subscribed = true;
	const auto weak = base::make_weak(session);
	StickerSets::Updated(
		session
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			ApplyFavorites(strong);
		}
	}, state.lifetime);
	StickerSets::Load(session);
}

[[nodiscard]] std::optional<StickerSets::Item> CatalogSticker(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document) {
	if (!StickerSets::IsCatalogSticker(document->id)) {
		return std::nullopt;
	}
	return StickerSets::Find(session, document->id);
}

} // namespace

void RequestFavorites(not_null<Main::Session*> session) {
	Subscribe(session);
	auto &state = StateFor(session);
	if (state.requestingFavorites) return;
	state.requestingFavorites = true;
	const auto generation = state.favoritesGeneration;
	const auto lifetime = std::weak_ptr(state.favoritesLifetime);
	const auto weak = base::make_weak(session);
	ClientFor(session).stickerFavorites([weak, generation, lifetime](QJsonDocument doc, QString error, int status) {
		if (lifetime.expired()) return;
		const auto session = weak.get();
		if (!session) return;
		const auto owner = States().find(session);
		if (owner == States().end()) return;
		auto &state = owner->second;
		if (generation != state.favoritesGeneration) return;
		state.requestingFavorites = false;
		if (error.isEmpty() && status == 200) ReceiveFavorites(session, doc.object());
	});
}

void ReceiveFavorites(not_null<Main::Session*> session, const QJsonObject &value) {
	const auto parsed = FavoriteState::Parse(value);
	auto &state = StateFor(session);
	if (!parsed || parsed->revision < state.favorites.revision) return;
	state.favorites = *parsed;
	ApplyFavorites(session);
}

void ResetFavorites(not_null<Main::Session*> session) {
	auto &state = StateFor(session);
	state.requestingFavorites = false;
	++state.favoritesGeneration;
	RequestFavorites(session);
}

void ToggleFavorite(std::shared_ptr<ChatHelpers::Show> show,
		not_null<DocumentData*> document, bool favorited) {
	const auto session = &document->session();
	const auto item = CatalogSticker(session, document);
	if (!item) {
		if (show) show->showToast(u"Could not update favorite stickers."_q);
		return;
	}
	auto &mutation = StateFor(session).mutations[item->id];
	mutation.desired = favorited;
	mutation.show = std::move(show);
	SendFavorite(session, item->id);
}

bool IsSticker(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document) {
	return CatalogSticker(session, document).has_value();
}

void ClearSession(not_null<Main::Session*> session) {
	if (States().find(session) == States().end()) {
		return;
	}
	States().remove(session);
}

bool Send(
		not_null<DocumentData*> document,
		const Api::SendAction &action) {
	const auto history = action.history;
	if (!history) {
		return false;
	}
	const auto session = &history->session();
	const auto item = CatalogSticker(session, document);
	if (!item) {
		return false;
	}
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return false;
	}
	bridge->sendSticker(
		history,
		*item,
		ReplyTargetFrom(history, action.replyTo),
		QString(),
		SendOptionsFrom(action.options));
	return true;
}

}
