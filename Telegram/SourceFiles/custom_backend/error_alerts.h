#pragma once

#include "ui/boxes/confirm_box.h"

namespace CustomBackend {

[[nodiscard]] bool ErrorAlertsEnabled();
[[nodiscard]] QString ErrorDisplayText(const QString &code, int status = 0);
[[nodiscard]] QString ErrorAlertText(const QString &text);
[[nodiscard]] object_ptr<Ui::GenericBox> MakeErrorAlertBox(Ui::ConfirmBoxArgs &&args);

}
