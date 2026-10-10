#pragma once

#include "rpl/producer.h"

namespace Ui {
class GenericBox;
class RoundButton;
}
namespace ChatHelpers {
class Show;
}

namespace CustomBackend::Appearance {
void BindEditor(not_null<Ui::GenericBox*> box,
    not_null<Ui::RoundButton*> button, std::shared_ptr<ChatHelpers::Show> show,
    not_null<PeerData*> peer, rpl::producer<uint8> color,
    rpl::producer<DocumentId> icon, bool profile);
}
