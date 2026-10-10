#include "custom_backend/native_history_loader.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_mtp_router.h"
#include "custom_backend/native_runtime.h"

#include "base/call_delayed.h"
#include "base/timer.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
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
constexpr auto kLoadingCheckPeriod = crl::time(50);

} // namespace

void ShowHistoryLoading(
        not_null<Window::SessionController*> controller,
        not_null<Ui::ElasticScroll*> area,
        Fn<bool()> waiting) {
    const auto overlay = Ui::CreateChild<Ui::RpWidget>(area.get());
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

    const auto check = overlay->lifetime().make_state<base::Timer>([=] {
        if (!waiting()) {
            overlay->hide();
            overlay->deleteLater();
        }
    });
    check->callEach(kLoadingCheckPeriod);
    base::call_delayed(kLoadingDelay, overlay, [=] {
        if (waiting()) {
            overlay->show();
            overlay->raise();
            animation->start();
        }
    });
}

}

namespace CustomBackend::HistoryPages {
namespace {

using SerializedRequest = MTP::details::SerializedRequest;

} // namespace

bool Intercepts(const SerializedRequest &request) {
    return Mtp::RequestType(request) == mtpc_messages_getHistory;
}

void Intercept(
        not_null<MTP::Instance*> instance,
        mtpRequestId requestId,
        const SerializedRequest &request) {
    const auto answer = Mtp::Answer(instance, requestId);
    auto reader = Mtp::Reader(request);
    const auto peer = reader.read<MTPInputPeer>();
    const auto offsetId = reader.read<MTPint>().v;
    const auto offsetDate = reader.read<MTPint>().v;
    const auto addOffset = reader.read<MTPint>().v;
    const auto limit = reader.read<MTPint>().v;
    const auto session = Mtp::SessionFor(instance);
    if (!reader.ok() || !session) {
        answer.fail(u"PEER_ID_INVALID"_q);
        return;
    }
    crl::on_main(session, [=] {
        const auto bridge = BridgeFor(session);
        const auto resolved = Data::PeerFromInputMTP(&session->data(), peer);
        if (!bridge || !resolved) {
            answer.fail(u"PEER_ID_INVALID"_q);
            return;
        }
        const auto reply = [=](QVector<MTPMessage> messages) {
            answer.done(MTP_messages_messages(
                MTP_vector<MTPMessage>(std::move(messages)),
                MTP_vector<MTPForumTopic>(),
                MTP_vector<MTPChat>(),
                MTP_vector<MTPUser>()));
        };
        if (offsetDate) {
            reply({});
            return;
        }
        bridge->requestHistoryMessages(
            session->data().history(resolved),
            {
                .offsetId = MsgId(offsetId),
                .addOffset = addOffset,
                .limit = limit,
            },
            reply,
            [=](const QString &type) { answer.fail(type); });
    });
}

}
