#include "custom_backend/appearance_editor.h"
#include "custom_backend/native_appearance_adapter.h"
#include "base/flat_map.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"

namespace CustomBackend::Appearance {
namespace {
struct Draft {
    QJsonObject name, profile;
    std::vector<QPointer<Ui::RoundButton>> buttons;
    bool saving = false;
};
base::flat_map<Ui::GenericBox*, std::shared_ptr<Draft>> drafts;

QJsonObject Choice(uint8 color, DocumentId icon) {
    return {
        { "color_id", color == 255 ? QJsonValue(QJsonValue::Null) : QJsonValue(int(color)) },
        { "background_emoji_id", icon ? QJsonValue(qint64(icon)) : QJsonValue(QJsonValue::Null) },
    };
}
} // namespace

void BindEditor(not_null<Ui::GenericBox*> box,
        not_null<Ui::RoundButton*> button, std::shared_ptr<ChatHelpers::Show> show,
        not_null<PeerData*> peer, rpl::producer<uint8> color,
        rpl::producer<DocumentId> icon, bool profile) {
    if (!peer->isSelf()) return;
    auto &slot = drafts[box];
    if (!slot) {
        slot = std::make_shared<Draft>();
        slot->name = Choice(peer->colorIndex(), peer->backgroundEmojiId());
        slot->profile = Choice(peer->colorProfileIndex().value_or(255), peer->profileBackgroundEmojiId());
        box->lifetime().add([raw = box.get()] { drafts.remove(raw); });
    }
    const auto draft = slot;
    draft->buttons.push_back(button.get());
    rpl::combine(std::move(color), std::move(icon)) | rpl::on_next(
        [draft, profile](uint8 color, DocumentId icon) {
            (profile ? draft->profile : draft->name) = Choice(color, icon);
        }, box->lifetime());
    button->setClickedCallback([draft, box = QPointer<Ui::GenericBox>(box.get()), show, peer] {
        if (draft->saving || !box) return;
        draft->saving = true;
        for (const auto &button : draft->buttons) if (button) button->setDisabled(true);
        Save(&peer->session(), { { "name", draft->name }, { "profile", draft->profile } },
            [draft, box, show](bool ok) {
                draft->saving = false;
                if (!box) return;
                if (ok) {
                    box->closeBox();
                } else {
                    for (const auto &button : draft->buttons) if (button) button->setDisabled(false);
                    show->showToast(tr::lng_passkey_cable_error_title(tr::now));
                }
            });
    });
}
} // namespace CustomBackend::Appearance
