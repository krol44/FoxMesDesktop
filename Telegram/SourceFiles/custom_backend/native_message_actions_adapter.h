#pragma once

#include <functional>
#include <optional>
#include <vector>

class History;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionNavigation;
} // namespace Window

namespace Data {
class Thread;
enum class ForwardOptions;
struct ResolvedForwardDraft;
} // namespace Data

namespace Ui {
class GenericBox;
} // namespace Ui

namespace CustomBackend::Actions {

// Forwards messages through the FoxMes REST bridge. Returns false when the
// bridge is unavailable or the draft is unusable; Forward() shows the error
// toast itself in that case, since there is no MTP fallback under the
// bridge. Callers must call this and return unconditionally when the
// bridge is enabled, never falling through to their upstream MTP path.
[[nodiscard]] bool Forward(
	not_null<Main::Session*> session,
	std::vector<not_null<HistoryItem*>> items,
	const std::vector<not_null<History*>> &to,
	std::function<void()> done = nullptr,
	bool dropAuthor = false,
	std::optional<int> videoTimestamp = std::nullopt);

void ForwardDraft(
	not_null<Main::Session*> session,
	Data::ResolvedForwardDraft &&draft,
	not_null<History*> target,
	FnMut<void()> &&done);

void ForwardToThreads(
	not_null<Main::Session*> session,
	std::vector<not_null<HistoryItem*>> items,
	const std::vector<not_null<Data::Thread*>> &to,
	Data::ForwardOptions options,
	std::optional<int> videoTimestamp,
	std::function<void()> done);

// Pins/unpins a message through the FoxMes REST bridge. Returns false when
// the bridge is unavailable; the caller must then run its upstream path.
[[nodiscard]] bool TogglePin(
	not_null<Window::SessionNavigation*> navigation,
	struct FullMsgId itemId,
	bool pin);

// Sends the pin chosen in upstream's PinMessageBox. forEveryone is the state
// of its "Also pin for {user}" checkbox: a private-chat pin is personal unless
// it is ticked, which is what upstream expresses as f_pm_oneside. The box is
// closed on the server answer, success or failure, like the upstream path.
void PinFromBox(
	not_null<Ui::GenericBox*> box,
	not_null<HistoryItem*> item,
	bool forEveryone);

void UnpinMessages(
	not_null<Window::SessionNavigation*> navigation,
	std::vector<struct FullMsgId> items,
	std::function<void()> onConfirmed);

void UnpinAll(
	not_null<Window::SessionNavigation*> navigation,
	not_null<Data::Thread*> thread);

} // namespace CustomBackend::Actions
