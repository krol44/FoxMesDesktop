#pragma once

#include "info/profile/info_profile_badge.h"

namespace Info { class Controller; }
namespace Ui { class RpWidget; }

namespace CustomBackend::PeerStatuses {

[[nodiscard]] Info::Profile::Badge::Content BadgeContent(Info::Profile::BadgeType badge, EmojiStatusId status);
[[nodiscard]] bool KeepStatusBadge(EmojiStatusId status);
[[nodiscard]] bool HasProfileAction(not_null<PeerData*> peer);
[[nodiscard]] object_ptr<Ui::RpWidget> CreateProfileAction(not_null<Info::Controller*> controller, not_null<Ui::RpWidget*> parent, not_null<PeerData*> peer, object_ptr<Ui::RpWidget> upstream);

}
