#include "custom_backend/native_peer_status_ui.h"
#include "custom_backend/native_peer_status_adapter.h"
#include "custom_backend/native_runtime.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_emoji_statuses.h"
#include "data/stickers/data_custom_emoji.h"
#include "info/info_controller.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "main/main_session.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "lang/lang_keys.h"
#include "styles/style_settings.h"

namespace CustomBackend::PeerStatuses {
namespace {

void Picker(not_null<Window::SessionController*> controller, not_null<Ui::RpWidget*> button, not_null<PeerData*> peer) {
    const auto panel = button->lifetime().make_state<Info::Profile::EmojiStatusPanel>();
    panel->show({
        .controller = controller,
        .button = button,
        .animationSizeTag = Data::CustomEmojiSizeTag::Normal,
        .ensureAddedEmojiId = peer->emojiStatusId(),
        .channelStatusMode = peer->isChannel(),
        .withCollectibles = false,
    });
    if (peer->isChannel()) {
        panel->someCustomChosen() | rpl::on_next([peer](auto chosen) {
            peer->owner().emojiStatuses().set(peer, chosen.id, chosen.until);
        }, button->lifetime());
    }
}

}

Info::Profile::Badge::Content BadgeContent(Info::Profile::BadgeType badge, EmojiStatusId status) {
    using Type = Info::Profile::BadgeType;
    if (status) return { Type::Premium, status };
    return { (badge == Type::Verified || (badge == Type::Premium && HidesPremiumBadge())) ? Type::None : badge, status };
}

bool KeepStatusBadge(EmojiStatusId status) { return Enabled() && bool(status); }

bool HasProfileAction(not_null<PeerData*> peer) {
    return Available(&peer->session()) && (peer->isSelf() || (peer->isChannel() && !peer->asChannel()->isMegagroup() && peer->asChannel()->amCreator()));
}

object_ptr<Ui::RpWidget> CreateProfileAction(not_null<Info::Controller*> controller, not_null<Ui::RpWidget*> parent, not_null<PeerData*> peer, object_ptr<Ui::RpWidget> upstream) {
    auto result = object_ptr<Ui::VerticalLayout>(parent);
    const auto button = result->add(object_ptr<Ui::SettingsButton>(result.data(), tr::lng_foxmes_emoji_status_title(), st::settingsButtonNoIcon));
    button->addClickHandler([=] { Picker(controller->parentController(), button, peer); });
    if (upstream) result->add(std::move(upstream));
    return std::move(result);
}

}
