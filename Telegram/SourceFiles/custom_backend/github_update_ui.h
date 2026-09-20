/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class Controller;
} // namespace Window

namespace CustomBackend::Updates {

void ActivateUpdate(Window::Controller *window = nullptr);
void SetupUpdateSettings(not_null<Ui::VerticalLayout*> container);
void BuildUpdateSettings(Settings::Builder::SectionBuilder &builder, bool atTop);

} // namespace CustomBackend::Updates
