/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_chat_request_adapter.h"

#include "base/debug_log.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_mtp_router.h"
#include "custom_backend/native_runtime.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/view/history_view_element.h"
#include "history/view/media/history_view_media_generic.h"
#include "history/view/media/history_view_unique_gift.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "ui/chat/chat_style.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/toast/toast.h"
#include "window/notifications_manager.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_history_view_about_view.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace CustomBackend::ChatRequests {
namespace {

enum class Kind {
	Accepted,
	None,
	Outgoing,
	Incoming,
	Declined,
};

struct State {
	Kind kind = Kind::Accepted;
	qint64 chatId = 0;
	TimeId retryAt = 0;
};

struct Entry {
	qint64 chatId = 0;
	QJsonObject request;
};

struct Store {
	base::flat_map<qint64, Entry> users;
	base::flat_map<qint64, QDate> registered;
	// The date each built card shows; a newer one rebuilds it.
	base::flat_map<qint64, QDate> shown;
	base::flat_set<qint64> registrationAsked;
	base::flat_set<qint64> askedBeforeList;
	base::flat_set<qint64> inFlight;
	// The local item behind each shown request notification, and requests
	// that wait for the chat list to bring their chat and its unread mark.
	base::flat_map<qint64, FullMsgId> notified;
	base::flat_set<qint64> notifyPending;
	rpl::event_stream<qint64> changes;
	bool listApplied = false;
};

[[nodiscard]] Store &StoreFor(not_null<Main::Session*> session) {
	static auto stores = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<Store>>();
	if (const auto i = stores.find(session); i != stores.end()) {
		return *i->second;
	}
	auto &result = *stores.emplace(
		session,
		std::make_unique<Store>()).first->second;
	session->lifetime().add([=] {
		stores.remove(session);
	});
	return result;
}

[[nodiscard]] qint64 UserIdOf(not_null<UserData*> user) {
	return qint64(peerToUser(user->id).bare);
}

[[nodiscard]] TimeId ParseTime(const QString &value) {
	const auto parsed = QDateTime::fromString(value, Qt::ISODate);
	return parsed.isValid() ? TimeId(parsed.toSecsSinceEpoch()) : TimeId();
}

[[nodiscard]] State StateOf(
		not_null<Main::Session*> session,
		qint64 userId) {
	const auto &store = StoreFor(session);
	if (const auto i = store.users.find(userId); i != store.users.end()) {
		const auto &request = i->second.request;
		const auto chatId = i->second.chatId;
		if (request.isEmpty()) {
			return { .kind = Kind::Accepted, .chatId = chatId };
		} else if (request.value(u"status"_q).toString() == u"declined"_q) {
			return {
				.kind = Kind::Declined,
				.chatId = chatId,
				.retryAt = ParseTime(request.value(u"retry_at"_q).toString()),
			};
		}
		const auto recipientId = request.value(
			u"recipient_id"_q).toVariant().toLongLong();
		return {
			.kind = ((recipientId == ClientFor(session).meId())
				? Kind::Incoming
				: Kind::Outgoing),
			.chatId = chatId,
		};
	}
	const auto bridge = BridgeFor(session);
	const auto hasChat = bridge
		&& bridge->hasChat(peerFromUser(UserId(userId)));
	return { .kind = (hasChat || !store.listApplied)
		? Kind::Accepted
		: Kind::None };
}

[[nodiscard]] bool RetryOpen(const State &state) {
	return (state.kind == Kind::Declined)
		&& (base::unixtime::now() >= state.retryAt);
}

[[nodiscard]] MTPPeerSettings PeerSettingsFor(
		not_null<Main::Session*> session,
		qint64 userId,
		const State &state) {
	using Flag = MTPDpeerSettings::Flag;
	const auto &store = StoreFor(session);
	auto flags = MTPDpeerSettings::Flags();
	auto month = QString();
	if (state.kind == Kind::Incoming) {
		flags |= Flag::f_block_contact;
	}
	if (state.kind != Kind::Accepted) {
		const auto i = store.registered.find(userId);
		if (i != store.registered.end() && i->second.isValid()) {
			flags |= Flag::f_registration_month;
			month = i->second.toString(u"MM.yyyy"_q);
		}
	}
	return MTP_peerSettings(
		MTP_flags(flags),
		MTP_int(0), // geo_distance
		MTPstring(), // request_chat_title
		MTP_int(0), // request_chat_date
		MTP_long(0), // business_bot_id
		MTPstring(), // business_bot_manage_url
		MTP_long(0), // charge_paid_message_stars
		MTP_string(month),
		MTPstring(), // phone_country
		MTP_int(0), // name_change_date
		MTP_int(0)); // photo_change_date
}

void HideNotification(not_null<Main::Session*> session, qint64 userId) {
	auto &store = StoreFor(session);
	const auto i = store.notified.find(userId);
	if (i == store.notified.end()) {
		return;
	}
	const auto id = i->second;
	store.notified.erase(i);
	if (const auto item = session->data().message(id)) {
		// The item is not a history entry, so destroying it alone would leave
		// the notification on screen.
		Core::App().notifications().clearFromHistory(item->history());
		item->destroy();
	}
}

// Upstream notifies only about items, and a request chat has no messages: the
// notification is a local service item kept out of the history blocks, so it
// never shows in the chat and is destroyed together with the notification.
void ShowNotification(not_null<Main::Session*> session, qint64 userId) {
	auto &store = StoreFor(session);
	if (store.notified.contains(userId)) {
		return;
	}
	const auto user = session->data().user(UserId(userId));
	const auto history = session->data().history(user);
	const auto item = history->makeMessage({
		.id = session->data().nextLocalMessageId(),
		.flags = MessageFlag::Local | MessageFlag::HideDisplayDate,
		.from = user->id,
		.date = base::unixtime::now(),
	}, PreparedServiceText{ TextWithEntities{
		tr::lng_fox_mes_chat_request_list_incoming(tr::now),
	} });
	store.notified.emplace(userId, item->fullId());
	const auto notification = Data::ItemNotification{
		.item = item,
		.type = Data::ItemNotificationType::Message,
	};
	history->pushNotification(notification);
	Core::App().notifications().schedule(notification);
}

// The notification lives while the request is incoming and the chat is still
// marked unread: the server clears the mark when the recipient opens the chat
// on any device. Only a chat.request event shows it.
void SyncNotification(
		not_null<Main::Session*> session,
		qint64 userId,
		bool show) {
	const auto history = session->data().historyLoaded(
		peerFromUser(UserId(userId)));
	const auto wanted = history
		&& history->unreadMark()
		&& (StateOf(session, userId).kind == Kind::Incoming);
	if (!wanted) {
		HideNotification(session, userId);
	} else if (show) {
		ShowNotification(session, userId);
	}
}

void Refresh(
		not_null<Main::Session*> session,
		qint64 userId,
		bool changed) {
	const auto user = session->data().user(UserId(userId));
	if (user->isSelf()) {
		return;
	}
	const auto state = StateOf(session, userId);
	user->setIsContact(state.kind == Kind::Accepted);
	user->setBarSettings(PeerSettingsFor(session, userId, state));
	if (changed) {
		SyncNotification(session, userId, false);
		StoreFor(session).changes.fire_copy(userId);
		session->changes().peerUpdated(
			user,
			Data::PeerUpdate::Flag::IsBlocked);
		// The chat list row shows ListPreview while the history is empty.
		if (const auto history = session->data().historyLoaded(user)) {
			history->updateChatListEntry();
		}
	}
}

[[nodiscard]] QString ComposeLabel(not_null<UserData*> user) {
	const auto state = StateOf(&user->session(), UserIdOf(user));
	switch (state.kind) {
	case Kind::Incoming:
		return tr::lng_fox_mes_chat_request_accept(tr::now);
	case Kind::Outgoing:
		return tr::lng_fox_mes_chat_request_sent(tr::now);
	case Kind::Declined:
		return RetryOpen(state)
			? tr::lng_fox_mes_chat_request(tr::now)
			: tr::lng_fox_mes_chat_request_declined(tr::now);
	case Kind::None:
	case Kind::Accepted:
		break;
	}
	return tr::lng_fox_mes_chat_request(tr::now);
}

[[nodiscard]] QString RetryText(const State &state) {
	return tr::lng_fox_mes_chat_request_retry(
		tr::now,
		lt_date,
		langDateTimeFull(base::unixtime::parse(state.retryAt)));
}

void ApplyResult(
		not_null<Main::Session*> session,
		const QJsonObject &result) {
	const auto userId = result.value(u"user_id"_q).toVariant().toLongLong();
	const auto chatId = result.value(u"chat_id"_q).toVariant().toLongLong();
	if (userId <= 0) {
		return;
	}
	const auto bridge = BridgeFor(session);
	if (result.value(u"hidden"_q).toBool()) {
		Forget(session, userId);
		if (bridge) {
			bridge->reloadChats();
		}
		return;
	}
	Apply(session, userId, chatId, result.value(u"request"_q));
	if (bridge && !bridge->hasChat(peerFromUser(UserId(userId)))) {
		bridge->reloadChats();
	}
}

[[nodiscard]] rpl::lifetime &ComposeLifetime(
		not_null<Ui::FlatButton*> button) {
	static auto bound = base::flat_map<Ui::FlatButton*, rpl::lifetime*>();
	if (const auto i = bound.find(button.get()); i != bound.end()) {
		return *i->second;
	}
	const auto result = button->lifetime().make_state<rpl::lifetime>();
	bound.emplace(button.get(), result);
	button->lifetime().add([raw = button.get()] {
		bound.remove(raw);
	});
	return *result;
}

void SendRequest(
		not_null<Window::SessionController*> window,
		not_null<UserData*> user) {
	const auto session = &user->session();
	const auto userId = UserIdOf(user);
	auto &store = StoreFor(session);
	if (store.inFlight.contains(userId)) {
		return;
	}
	store.inFlight.emplace(userId);
	const auto weak = base::make_weak(window);
	ClientFor(session).requestChat(userId, [=](
			QJsonDocument doc,
			QString error,
			int status) {
		StoreFor(session).inFlight.remove(userId);
		const auto strong = weak.get();
		if (error.isEmpty() && doc.isObject()) {
			ApplyResult(session, doc.object());
			return;
		}
		LOG(("FoxMes: chat request for %1 failed (%2, %3)"
			).arg(userId
			).arg(status
			).arg(error));
		const auto code = doc.isObject()
			? doc.object().value(u"code"_q).toString()
			: QString();
		if (code == u"CHAT_REQUEST_COOLDOWN"_q) {
			const auto state = State{
				.kind = Kind::Declined,
				.retryAt = ParseTime(
					doc.object().value(u"retry_at"_q).toString()),
			};
			if (strong) {
				strong->showToast(RetryText(state));
			}
			return;
		}
		if (strong) {
			strong->showToast(tr::lng_fox_mes_chat_request_failed(tr::now));
		}
	});
}

constexpr auto kInfoUserpicSize = 80;
constexpr auto kInfoLabelOpacity = 0.7;

using HistoryView::MediaGeneric;
using HistoryView::MediaGenericPart;
using HistoryView::PaintContext;

class UserpicPart final : public MediaGenericPart {
public:
	UserpicPart(not_null<UserData*> user, QMargins margins)
	: _user(user)
	, _margins(margins) {
	}

	void draw(
			Painter &p,
			not_null<const MediaGeneric*> owner,
			const PaintContext &context,
			int outerWidth) const override {
		const auto size = style::ConvertScale(kInfoUserpicSize);
		_user->paintUserpicLeft(
			p,
			_view,
			(outerWidth - size) / 2,
			_margins.top(),
			outerWidth,
			size);
	}

	bool hasHeavyPart() override {
		return _view.cloud != nullptr;
	}

	void unloadHeavyPart() override {
		_view.cloud = nullptr;
	}

	QSize countOptimalSize() override {
		const auto size = style::ConvertScale(kInfoUserpicSize);
		return {
			_margins.left() + size + _margins.right(),
			_margins.top() + size + _margins.bottom(),
		};
	}

	QSize countCurrentSize(int newWidth) override {
		return { std::max(newWidth, maxWidth()), minHeight() };
	}

private:
	const not_null<UserData*> _user;
	const QMargins _margins;
	mutable Ui::PeerUserpicView _view;

};

class StateTextPart final : public MediaGenericPart {
public:
	StateTextPart(
		not_null<HistoryView::Element*> parent,
		not_null<UserData*> user,
		std::vector<TextWithEntities> variants,
		QMargins margins)
	: _user(user)
	, _margins(margins) {
		for (const auto &variant : variants) {
			auto text = Ui::Text::String(st::msgMinWidth);
			text.setMarkedText(st::defaultTextStyle, variant);
			_texts.push_back(std::move(text));
		}
		const auto session = &user->session();
		const auto userId = UserIdOf(user);
		StoreFor(session).changes.events(
		) | rpl::filter([=](qint64 id) {
			return id == userId;
		}) | rpl::on_next([=] {
			parent->repaint();
		}, _lifetime);
	}

	void draw(
			Painter &p,
			not_null<const MediaGeneric*> owner,
			const PaintContext &context,
			int outerWidth) const override {
		const auto use = width() - _margins.left() - _margins.right();
		auto color = context.st->msgServiceFg()->c;
		color.setAlphaF(color.alphaF() * kInfoLabelOpacity);
		p.setPen(color);
		current().draw(p, {
			.position = { (outerWidth - use) / 2, _margins.top() },
			.outerWidth = outerWidth,
			.availableWidth = use,
			.align = style::al_top,
			.palette = &context.st->serviceTextPalette(),
			.now = context.now,
		});
	}

	QSize countOptimalSize() override {
		auto width = 0;
		auto height = 0;
		for (const auto &text : _texts) {
			width = std::max(width, text.maxWidth());
			height = std::max(height, text.minHeight());
		}
		return {
			_margins.left() + width + _margins.right(),
			_margins.top() + height + _margins.bottom(),
		};
	}

	QSize countCurrentSize(int newWidth) override {
		const auto skip = _margins.left() + _margins.right();
		const auto available = std::max(st::msgMinWidth, newWidth - skip);
		auto width = 0;
		auto height = 0;
		for (const auto &text : _texts) {
			const auto size = Ui::Text::CountOptimalTextSize(
				text,
				st::msgMinWidth,
				available);
			width = std::max(width, size.width());
			height = std::max(height, size.height());
		}
		return { width + skip, _margins.top() + height + _margins.bottom() };
	}

private:
	[[nodiscard]] const Ui::Text::String &current() const {
		const auto state = StateOf(&_user->session(), UserIdOf(_user));
		const auto index = [&] {
			switch (state.kind) {
			case Kind::Incoming: return 0;
			case Kind::Outgoing: return 2;
			case Kind::Declined: return RetryOpen(state) ? 3 : 4;
			case Kind::None:
			case Kind::Accepted:
				break;
			}
			return 1;
		}();
		return _texts[std::min(index, int(_texts.size()) - 1)];
	}

	const not_null<UserData*> _user;
	const QMargins _margins;
	std::vector<Ui::Text::String> _texts;
	rpl::lifetime _lifetime;

};

} // namespace

void Apply(
		not_null<Main::Session*> session,
		qint64 userId,
		qint64 chatId,
		const QJsonValue &request) {
	if (userId <= 0) {
		return;
	}
	auto &store = StoreFor(session);
	const auto value = request.isObject() ? request.toObject() : QJsonObject();
	const auto i = store.users.find(userId);
	const auto changed = (i == store.users.end())
		|| (i->second.chatId != chatId)
		|| (i->second.request != value);
	store.users[userId] = Entry{ .chatId = chatId, .request = value };
	Refresh(session, userId, changed);
}

void Forget(not_null<Main::Session*> session, qint64 userId) {
	if (StoreFor(session).users.remove(userId)) {
		Refresh(session, userId, true);
	}
}

void ListApplied(not_null<Main::Session*> session) {
	auto &store = StoreFor(session);
	store.listApplied = true;
	for (const auto userId : base::take(store.askedBeforeList)) {
		Refresh(session, userId, true);
	}
	for (const auto userId : base::take(store.notifyPending)) {
		SyncNotification(session, userId, true);
	}
	for (const auto &[userId, id] : base::flat_map<qint64, FullMsgId>(
			store.notified)) {
		SyncNotification(session, userId, false);
	}
}

void UnreadMarkApplied(not_null<History*> history) {
	if (const auto user = history->peer->asUser()) {
		SyncNotification(&history->session(), UserIdOf(user), false);
	}
}

void RememberUser(
		not_null<Main::Session*> session,
		const QJsonObject &user) {
	const auto userId = user.value(u"id"_q).toVariant().toLongLong();
	const auto registeredAt = user.value(u"registered_at"_q).toString();
	if (userId <= 0 || registeredAt.isEmpty()) {
		return;
	}
	const auto date = QDateTime::fromString(
		registeredAt,
		Qt::ISODate).toLocalTime().date();
	auto &store = StoreFor(session);
	if (!date.isValid() || store.registered[userId] == date) {
		return;
	}
	store.registered[userId] = date;
	Refresh(session, userId, false);
	if (const auto i = store.shown.find(userId); i != store.shown.end()
		&& i->second != date) {
		session->changes().peerUpdated(
			session->data().user(UserId(userId)),
			Data::PeerUpdate::Flag::FullInfo);
	}
}

QString ListPreview(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	if (!user || user->isSelf() || user->isBot()) {
		return QString();
	}
	const auto state = StateOf(&user->session(), UserIdOf(user));
	switch (state.kind) {
	case Kind::Incoming:
		return tr::lng_fox_mes_chat_request_list_incoming(tr::now);
	case Kind::Outgoing:
		return tr::lng_fox_mes_chat_request_list_sent(tr::now);
	case Kind::Declined:
		return tr::lng_fox_mes_chat_request_list_declined(tr::now);
	case Kind::None:
	case Kind::Accepted:
		break;
	}
	return QString();
}

bool PeerInfoStale(not_null<UserData*> user) {
	const auto &store = StoreFor(&user->session());
	const auto userId = UserIdOf(user);
	const auto shown = store.shown.find(userId);
	const auto known = store.registered.find(userId);
	return (shown != store.shown.end())
		&& (known != store.registered.end())
		&& (shown->second != known->second);
}

bool HandleEvent(
		not_null<Main::Session*> session,
		const QString &type,
		const QJsonObject &data) {
	if (type != u"chat.request"_q) {
		return false;
	}
	ApplyResult(session, data);
	const auto userId = data.value(u"user_id"_q).toVariant().toLongLong();
	const auto bridge = BridgeFor(session);
	if (userId <= 0 || StateOf(session, userId).kind != Kind::Incoming) {
	} else if (bridge && bridge->hasChat(peerFromUser(UserId(userId)))) {
		SyncNotification(session, userId, true);
	} else {
		// A new chat: its unread mark comes with the chat list reload.
		StoreFor(session).notifyPending.emplace(userId);
	}
	return true;
}

bool CountsAsContact(not_null<Main::Session*> session, qint64 userId) {
	return StateOf(session, userId).kind == Kind::Accepted;
}

bool Gated(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	if (!user || user->isSelf() || user->isBot() || user->isBlocked()) {
		return false;
	}
	const auto session = &user->session();
	const auto userId = UserIdOf(user);
	auto &store = StoreFor(session);
	if (!store.listApplied && !store.users.contains(userId)) {
		store.askedBeforeList.emplace(userId);
		return false;
	}
	return !CountsAsContact(session, userId);
}

bool ComposeBlocked(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return (user && user->isBlocked()) || Gated(peer);
}

void BindComposeButton(
		not_null<Ui::FlatButton*> button,
		not_null<PeerData*> peer) {
	auto &lifetime = ComposeLifetime(button);
	lifetime.destroy();
	const auto user = peer->asUser();
	if (!user || user->isBot()) {
		return;
	}
	const auto session = &user->session();
	const auto userId = UserIdOf(user);
	rpl::merge(
		StoreFor(session).changes.events(
		) | rpl::filter([=](qint64 id) {
			return id == userId;
		}) | rpl::to_empty,
		session->changes().peerUpdates(
			user,
			Data::PeerUpdate::Flag::IsBlocked) | rpl::to_empty
	) | rpl::on_next([=] {
		const auto gated = Gated(user);
		button->setText((gated
			? ComposeLabel(user)
			: tr::lng_unblock_button(tr::now)).toUpper());
		button->setColorOverride(gated
			? std::make_optional(st::windowActiveTextFg->c)
			: std::nullopt);
	}, lifetime);
	if (Gated(user)) {
		button->setText(ComposeLabel(user).toUpper());
		button->setColorOverride(st::windowActiveTextFg->c);
	} else {
		button->setColorOverride(std::nullopt);
	}
}

void ComposeClicked(
		not_null<Window::SessionController*> window,
		not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	if (!user) {
		return;
	}
	const auto state = StateOf(&user->session(), UserIdOf(user));
	switch (state.kind) {
	case Kind::Incoming:
		Answer(window, user, true);
		return;
	case Kind::Outgoing:
		window->showToast(tr::lng_fox_mes_chat_request_sent_about(
			tr::now,
			lt_user,
			user->shortName()));
		return;
	case Kind::Declined:
		if (!RetryOpen(state)) {
			window->showToast(RetryText(state));
			return;
		}
		break;
	case Kind::None:
	case Kind::Accepted:
		break;
	}
	SendRequest(window, user);
}

auto GeneratePeerInfo(
	not_null<HistoryView::Element*> parent,
	not_null<UserData*> user)
-> Fn<void(
		not_null<MediaGeneric*>,
		Fn<void(std::unique_ptr<MediaGenericPart>)>)> {
	return [=](
			not_null<MediaGeneric*> media,
			Fn<void(std::unique_ptr<MediaGenericPart>)> push) {
		const auto fadedFg = [](const PaintContext &context) {
			auto result = context.st->msgServiceFg()->c;
			result.setAlphaF(result.alphaF() * kInfoLabelOpacity);
			return result;
		};
		const auto normalFg = [](const PaintContext &context) {
			return context.st->msgServiceFg()->c;
		};
		const auto margins = st::newPeerSubtitleMargin;
		push(std::make_unique<UserpicPart>(
			user,
			QMargins(margins.left(), margins.left() + margins.top(), margins.right(), 0)));
		push(std::make_unique<HistoryView::MediaGenericTextPart>(
			tr::bold(user->name()),
			st::newPeerTitleMargin));
		push(std::make_unique<HistoryView::TextPartColored>(
			tr::lng_new_contact_not_contact(tr::now, tr::marked),
			margins,
			fadedFg));

		const auto name = tr::bold(user->shortName());
		const auto state = StateOf(&user->session(), UserIdOf(user));
		const auto retry = langDateTimeFull(
			base::unixtime::parse(state.retryAt));
		push(std::make_unique<StateTextPart>(
			parent,
			user,
			std::vector<TextWithEntities>{
				tr::lng_fox_mes_chat_request_card_incoming(
					tr::now,
					lt_user,
					name,
					tr::marked),
				tr::lng_fox_mes_chat_request_card_none(
					tr::now,
					lt_user,
					name,
					tr::marked),
				tr::lng_fox_mes_chat_request_card_sent(
					tr::now,
					lt_user,
					name,
					tr::marked),
				tr::lng_fox_mes_chat_request_card_declined(
					tr::now,
					lt_user,
					name,
					tr::marked),
				tr::lng_fox_mes_chat_request_card_cooldown(
					tr::now,
					lt_user,
					name,
					lt_date,
					tr::marked(retry),
					tr::marked),
			},
			margins));

		const auto session = &user->session();
		const auto userId = UserIdOf(user);
		auto &store = StoreFor(session);
		const auto i = store.registered.find(userId);
		const auto known = (i != store.registered.end())
			? i->second
			: QDate();
		store.shown[userId] = known;
		if (!known.isValid() && store.registrationAsked.emplace(userId).second) {
			// The chat list may carry the peer without its date.
			const auto weak = base::make_weak(session);
			ClientFor(session).user(userId, [=](
					QJsonDocument doc,
					QString error,
					int) {
				const auto strong = weak.get();
				if (strong && error.isEmpty() && doc.isObject()) {
					RememberUser(strong, doc.object());
				}
			});
		}
		if (i != store.registered.end() && i->second.isValid()) {
			auto entries = std::vector<HistoryView::AttributeTable::Entry>();
			entries.push_back({
				tr::lng_new_contact_registration(tr::now),
				tr::bold(langMonthOfYearFull(
					i->second.month(),
					i->second.year())),
			});
			push(std::make_unique<HistoryView::AttributeTable>(
				std::move(entries),
				QMargins(margins.left(), margins.top(), margins.right(), margins.left() + margins.bottom()),
				fadedFg,
				normalFg));
		}
	};
}

void DecorateBar(
		not_null<Ui::FlatButton*> accept,
		not_null<Ui::FlatButton*> decline,
		not_null<QWidget*> close) {
	accept->setText(tr::lng_fox_mes_chat_request_accept(tr::now).toUpper());
	decline->setText(
		tr::lng_fox_mes_chat_request_decline(tr::now).toUpper());
	close->hide();
}

void Answer(
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		bool accept) {
	const auto session = &user->session();
	const auto userId = UserIdOf(user);
	const auto state = StateOf(session, userId);
	auto &store = StoreFor(session);
	if (state.kind != Kind::Incoming
		|| state.chatId <= 0
		|| store.inFlight.contains(userId)) {
		return;
	}
	store.inFlight.emplace(userId);
	const auto weak = base::make_weak(window);
	ClientFor(session).answerChatRequest(state.chatId, accept, [=](
			QJsonDocument doc,
			QString error,
			int status) {
		StoreFor(session).inFlight.remove(userId);
		if (error.isEmpty() && doc.isObject()) {
			ApplyResult(session, doc.object());
			return;
		}
		LOG(("FoxMes: chat request answer for %1 failed (%2, %3)"
			).arg(userId
			).arg(status
			).arg(error));
		if (const auto strong = weak.get()) {
			strong->showToast(tr::lng_fox_mes_chat_request_failed(tr::now));
		}
	});
}

bool Intercepts(const MTP::details::SerializedRequest &request) {
	return Mtp::RequestType(request) == mtpc_messages_getPeerSettings;
}

void Intercept(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const MTP::details::SerializedRequest &request) {
	const auto answer = Mtp::Answer(instance, requestId);
	const auto session = Mtp::SessionFor(instance);
	auto reader = Mtp::Reader(request);
	const auto input = reader.read<MTPInputPeer>();
	const auto userId = input.match([](const MTPDinputPeerUser &data) {
		return UserId(data.vuser_id().v);
	}, [](const auto &) {
		return UserId();
	});
	if (!session || !reader.ok()) {
		answer.fail(u"PEER_ID_INVALID"_q);
		return;
	}
	const auto id = qint64(userId.bare);
	const auto state = id ? StateOf(session, id) : State();
	answer.done(MTP_messages_peerSettings(
		PeerSettingsFor(session, id, state),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>()));
}

} // namespace CustomBackend::ChatRequests
