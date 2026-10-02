/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Settings::Builder {
class SectionBuilder;
}

namespace Ui {
class VerticalLayout;
}

namespace Window {
class Controller;
}

namespace CustomBackend::Updates {

void ActivateUpdate(Window::Controller *window = nullptr);
void SetupUpdateSettings(not_null<Ui::VerticalLayout*> container);
void BuildUpdateSettings(Settings::Builder::SectionBuilder &builder, bool atTop);

}
