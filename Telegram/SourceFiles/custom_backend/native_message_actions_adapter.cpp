/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/native_message_actions_adapter.h"

#include "base/debug_log.h"
#include "base/weak_ptr.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "boxes/pin_messages_box.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"

#include <QtCore/QString>

namespace CustomBackend::Actions {
namespace {

void ShowToast(Main::Session *session, PeerData *peer, const QString &text) {
	if (!session || !peer) return;
	if (const auto controller = session->tryResolveWindow(peer)) {
		controller->showToast(text);
	}
}

} // namespace

bool Forward(
		not_null<Main::Session*> session,
		std::vector<not_null<HistoryItem*>> items,
		const std::vector<not_null<History*>> &to,
		std::function<void()> done) {
	const auto bridge = BridgeFor(session);
	if (!bridge || items.empty() || to.empty()) {
		// There is no MTP fallback under the bridge, so a caller that
		// only checks this return value and returns must not leave the
		// user without feedback.
		if (!to.empty()) {
			ShowToast(session, to.front()->peer, u"Failed to forward"_q);
		}
		return false;
	}
	auto ids = std::vector<int32_t>();
	ids.reserve(items.size());
	for (const auto &item : items) {
		const auto id = item->id.bare;
		if (!item->isRegular() || id <= 0 || id > INT32_MAX) {
			ShowToast(
				session,
				to.front()->peer,
				u"Can't forward this message yet"_q);
			return false;
		}
		ids.push_back(int32_t(id));
	}
	const auto source = items.front()->history();
	auto left = std::make_shared<int>(int(to.size()));
	auto failed = std::make_shared<bool>(false);
	for (const auto &target : to) {
		bridge->forwardMessages(source, target, ids, [session, peer = target->peer, left, failed, done](
				QString error) {
			if (!error.isEmpty()) {
				*failed = true;
				// ShowToast() is silent when no window resolves for the
				// peer, so the log is the only trace of a failed forward.
				LOG(("FoxMes forward failed: %1").arg(error));
				ShowToast(
					session,
					peer.get(),
					u"Failed to forward: "_q + error);
			}
			if (--*left == 0 && done && !*failed) {
				done();
			}
		});
	}
	return true;
}

bool TogglePin(
		not_null<Window::SessionNavigation*> navigation,
		FullMsgId itemId,
		bool pin) {
	const auto item = navigation->session().data().message(itemId);
	const auto bridge = BridgeFor(&navigation->session());
	if (!bridge || !item || !item->isRegular()) {
		return false;
	}
	const auto session = &navigation->session();
	const auto history = item->history();
	const auto messageId = itemId.msg;
	const auto finish = [=](QString error) mutable {
		if (!error.isEmpty()) {
			ShowToast(
				session,
				history->peer,
				u"Failed to update pin: "_q + error);
		}
	};
	if (pin) {
		// Upstream's own box: it carries the "Also pin for {user}" checkbox
		// and the "pinning an older message" wording, and its confirm button
		// routes back here through PinFromBox().
		navigation->parentController()->show(
			Box(PinMessageBox, item),
			Ui::LayerOption::CloseOther);
		return true;
	}
	navigation->parentController()->show(
		Ui::MakeConfirmBox({
			.text = tr::lng_pinned_unpin_sure(),
			.confirmed = [=](Fn<void()> &&close) {
				close();
				bridge->unpinMessage(history, messageId, finish);
			},
			.confirmText = tr::lng_pinned_unpin(),
		}),
		Ui::LayerOption::CloseOther);
	return true;
}

void PinFromBox(
		not_null<Ui::GenericBox*> box,
		not_null<HistoryItem*> item,
		bool forEveryone) {
	const auto session = &item->history()->session();
	const auto bridge = BridgeFor(session);
	const auto history = item->history();
	if (!bridge || !item->isRegular()) {
		ShowToast(session, history->peer, u"Can't pin this message yet"_q);
		box->closeBox();
		return;
	}
	const auto weak = base::make_weak(box.get());
	bridge->pinMessage(history, item->id, forEveryone, [=](QString error) {
		if (!error.isEmpty()) {
			ShowToast(
				session,
				history->peer,
				u"Failed to pin: "_q + error);
		}
		// Upstream closes the box on both done and fail, never before the
		// answer: closing early would hide a pin that did not happen.
		if (const auto strong = weak.get()) {
			strong->closeBox();
		}
	});
}

void UnpinMessages(
		not_null<Window::SessionNavigation*> navigation,
		std::vector<FullMsgId> items,
		std::function<void()> onConfirmed) {
	const auto session = &navigation->session();
	const auto bridge = BridgeFor(session);
	if (!bridge || items.empty()) {
		return;
	}
	const auto count = int(items.size());
	// Guarded like upstream: the box outlives the click, and a session that
	// went away must not be touched from the confirmation callback.
	const auto confirmed = crl::guard(session, [=](Fn<void()> &&close) {
		close();
		for (const auto &itemId : items) {
			const auto item = session->data().message(itemId);
			if (!item || !item->isPinned()) {
				continue;
			}
			const auto history = item->history();
			bridge->unpinMessage(history, itemId.msg, [=](QString error) {
				if (!error.isEmpty()) {
					ShowToast(
						session,
						history->peer,
						u"Failed to unpin: "_q + error);
				}
			});
		}
		if (onConfirmed) {
			onConfirmed();
		}
	});
	navigation->parentController()->show(
		Ui::MakeConfirmBox({
			.text = ((count > 1)
				? tr::lng_pinned_unpin_many_sure(tr::now, lt_count, count)
				: tr::lng_pinned_unpin_sure(tr::now)),
			.confirmed = confirmed,
			.confirmText = tr::lng_pinned_unpin(),
		}),
		Ui::LayerOption::CloseOther);
}

void UnpinAll(
		not_null<Window::SessionNavigation*> navigation,
		not_null<Data::Thread*> thread) {
	const auto session = &navigation->session();
	const auto bridge = BridgeFor(session);
	if (!bridge) {
		return;
	}
	const auto weak = base::make_weak(thread);
	const auto confirmed = crl::guard(navigation, [=](Fn<void()> &&close) {
		close();
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto history = strong->owningHistory();
		bridge->unpinAllMessages(history, [=](QString error) {
			if (!error.isEmpty()) {
				ShowToast(
					session,
					history->peer,
					u"Failed to unpin: "_q + error);
			}
		});
	});
	navigation->parentController()->show(
		Ui::MakeConfirmBox({
			.text = tr::lng_pinned_unpin_all_sure(),
			.confirmed = confirmed,
			.confirmText = tr::lng_pinned_unpin(),
		}),
		Ui::LayerOption::CloseOther);
}

} // namespace CustomBackend::Actions
