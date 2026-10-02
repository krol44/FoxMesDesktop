/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/topic_channels_prompt.h"

#include "api/api_chat_participants.h"
#include "apiwrap.h"
#include "boxes/peer_list_box.h"
#include "data/data_channel.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

namespace CustomBackend::TopicChannels {
namespace {

class ChannelsController final : public PeerListController {
public:
	ChannelsController(
		not_null<Main::Session*> session,
		std::vector<not_null<ChannelData*>> channels);

	Main::Session &session() const override;
	void prepare() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	bool trackSelectedList() override {
		return false;
	}

	[[nodiscard]] std::vector<not_null<ChannelData*>> chosen() const;

private:
	const not_null<Main::Session*> _session;
	const std::vector<not_null<ChannelData*>> _channels;

};

ChannelsController::ChannelsController(
	not_null<Main::Session*> session,
	std::vector<not_null<ChannelData*>> channels)
: _session(session)
, _channels(std::move(channels)) {
}

Main::Session &ChannelsController::session() const {
	return *_session;
}

void ChannelsController::prepare() {
	delegate()->peerListSetAboveWidget(
		object_ptr<Ui::PaddingWrap<Ui::FlatLabel>>(
			nullptr,
			object_ptr<Ui::FlatLabel>(
				nullptr,
				tr::lng_fox_mes_channels_prompt_about(),
				st::boxLabel),
			st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip)));
	for (const auto channel : _channels) {
		auto row = std::make_unique<PeerListRow>(channel);
		const auto about = channel->about().trimmed();
		row->setCustomStatus(about.isEmpty()
			? tr::lng_channel_status(tr::now)
			: about.split('\n').front());
		const auto raw = row.get();
		delegate()->peerListAppendRow(std::move(row));
		delegate()->peerListSetRowChecked(raw, true);
	}
	delegate()->peerListRefreshRows();
}

std::vector<not_null<ChannelData*>> ChannelsController::chosen() const {
	auto result = std::vector<not_null<ChannelData*>>();
	const auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count; ++i) {
		const auto row = delegate()->peerListRowAt(i);
		if (row->checked()) {
			if (const auto channel = row->peer()->asChannel()) {
				result.push_back(channel);
			}
		}
	}
	return result;
}

void ChannelsController::rowClicked(not_null<PeerListRow*> row) {
	delegate()->peerListSetRowChecked(row, !row->checked());
}

[[nodiscard]] std::vector<not_null<ChannelData*>> Unsubscribed(
		not_null<Main::Session*> session) {
	auto result = std::vector<not_null<ChannelData*>>();
	const auto &list = session->api().chatParticipants().recommendations();
	for (const auto &peer : list.list) {
		if (const auto channel = peer->asBroadcast()) {
			if (!channel->amIn()) {
				result.push_back(channel);
			}
		}
	}
	return result;
}

void Show(
		not_null<Window::SessionController*> window,
		std::vector<not_null<ChannelData*>> channels) {
	const auto session = &window->session();
	auto controller = std::make_unique<ChannelsController>(
		session,
		std::move(channels));
	const auto raw = controller.get();
	window->show(Box<PeerListBox>(std::move(controller), [=](
			not_null<PeerListBox*> box) {
		box->setTitle(tr::lng_fox_mes_channels_prompt_title());
		box->addButton(tr::lng_fox_mes_channels_prompt_subscribe(), [=] {
			for (const auto channel : raw->chosen()) {
				session->api().joinChannel(channel);
			}
			box->closeBox();
		});
		box->addButton(tr::lng_fox_mes_channels_prompt_later(), [=] {
			box->closeBox();
		});
	}));
}

} // namespace

void OfferChannels(
		not_null<Window::SessionController*> window,
		Fn<void()> shown) {
	const auto session = &window->session();
	const auto participants = &session->api().chatParticipants();
	const auto weak = base::make_weak(window);
	participants->loadRecommendations();
	participants->recommendationsLoaded(
	) | rpl::take(1) | rpl::on_next([=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto channels = Unsubscribed(session);
		if (channels.empty()) {
			return;
		}
		Show(strong, std::move(channels));
		if (shown) {
			shown();
		}
	}, window->lifetime());
}

}
