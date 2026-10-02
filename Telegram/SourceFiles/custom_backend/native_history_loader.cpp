#include "custom_backend/native_history_loader.h"

#include "logs.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"

#include "base/call_delayed.h"
#include "data/data_msg_id.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "ui/chat/chat_style.h"
#include "ui/effects/radial_animation.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/elastic_scroll.h"
#include "window/window_session_controller.h"
#include "styles/style_window.h"

namespace CustomBackend {
namespace {

constexpr auto kLoadingDelay = crl::time(200);

[[nodiscard]] Fn<void()> ShowLoading(
        const HistoryLoadingView &view,
        not_null<History*> history) {
    const auto area = view.area.get();
    const auto controller = view.controller.get();
    const auto overlay = Ui::CreateChild<Ui::RpWidget>(area);
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay->hide();
    area->sizeValue() | rpl::on_next([=](QSize size) {
        overlay->setGeometry(QRect(QPoint(), size));
    }, overlay->lifetime());

    const auto &st = st::windowShellLoading;
    const auto animation = overlay->lifetime().make_state<
        Ui::InfiniteRadialAnimation>([=] { overlay->update(); }, st);
    overlay->paintRequest() | rpl::on_next([=] {
        auto p = QPainter(overlay);
        auto hq = PainterHighQualityEnabler(p);
        const auto chat = controller->chatStyle();
        const auto padding = st.thickness * 2;
        const auto outer = st.size.width() + 2 * padding;
        const auto bubble = QRect(
            (overlay->width() - outer) / 2,
            (overlay->height() - outer) / 2,
            outer,
            outer);
        p.setPen(Qt::NoPen);
        p.setBrush(chat->msgServiceBg());
        p.drawEllipse(bubble);
        Ui::InfiniteRadialAnimation::Draw(
            p,
            animation->computeState(),
            bubble.topLeft() + QPoint(padding, padding),
            st.size,
            overlay->width(),
            QPen(chat->msgServiceFg()),
            st.thickness);
    }, overlay->lifetime());

    const auto weak = QPointer<Ui::RpWidget>(overlay);
    const auto remove = [=] {
        if (const auto strong = weak.data()) {
            strong->hide();
            strong->deleteLater();
        }
    };
    controller->activeChatChanges(
    ) | rpl::filter([=](const Dialogs::Key &key) {
        return key.history() != history.get();
    }) | rpl::take(1) | rpl::on_next(remove, overlay->lifetime());
    base::call_delayed(kLoadingDelay, overlay, [=] {
        overlay->show();
        overlay->raise();
        animation->start();
    });
    return remove;
}

} // namespace

bool WidgetHistoryRequest(
        not_null<Main::Session*> session,
        not_null<History*> history,
        MsgId showAtMsgId,
        Data::LoadDirection direction,
        Fn<void()> done,
        Fn<void(const MTP::Error &)> failed,
        std::optional<HistoryLoadingView> loading) {
    if (!Enabled()) {
        return false;
    }
    const auto bridge = BridgeFor(session);
    if (!bridge) {
        return false;
    }
    auto aroundId = ShowAtTheEndMsgId.bare;
    if (showAtMsgId > 0 && showAtMsgId < ServerMaxMsgId) {
        aroundId = showAtMsgId.bare;
    } else if (showAtMsgId == ShowAtUnreadMsgId) {
        if (const auto around = history->loadAroundId()) {
            aroundId = around.bare;
        }
    }
    if (loading) {
        const auto remove = ShowLoading(*loading, history);
        done = [=, done = std::move(done)] {
            remove();
            if (done) done();
        };
        failed = [=, failed = std::move(failed)](const MTP::Error &error) {
            remove();
            if (failed) failed(error);
        };
    }
    bridge->loadHistory(
        history,
        aroundId,
        direction,
        std::move(done),
        std::move(failed));
    return true;
}

}
