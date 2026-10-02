#pragma once

#include <functional>
#include <optional>
#include <vector>

class History;
class HistoryItem;

namespace Main {
class Session;
}

namespace Window {
class SessionNavigation;
}

namespace Data {
class Thread;
enum class ForwardOptions;
struct ResolvedForwardDraft;
}

namespace Ui {
class GenericBox;
}

namespace CustomBackend::Actions {

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

[[nodiscard]] bool TogglePin(
	not_null<Window::SessionNavigation*> navigation,
	struct FullMsgId itemId,
	bool pin);

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

}
