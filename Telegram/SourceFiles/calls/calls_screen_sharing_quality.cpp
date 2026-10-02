#include "calls/calls_screen_sharing_quality.h"
#include "ui/painter.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/labels.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "styles/style_calls.h"
#include <QtCore/QPointer>
#include <QtGui/QAction>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <array>

namespace Calls {
namespace {
constexpr auto kRefreshInterval = crl::time(1000);
constexpr auto kStatsTimeout = crl::time(3000);
const std::array<const char*, 12> kLabels = {{
	"Default - Auto", "Source / ~1–5 FPS", "1440p / ~1–5 FPS",
	"Source / ~60 FPS", "1440p / ~60 FPS", "1080p / ~60 FPS",
	"Source / ~30 FPS", "1440p / ~30 FPS", "1080p / ~30 FPS",
	"Source / ~15 FPS", "1440p / ~15 FPS", "1080p / ~15 FPS"
}};
QString QualityLabel(int quality) {
	return QString::fromUtf8(kLabels[std::clamp(quality, 0, 11)]);
}
QString BitrateLabel(int bitrate) {
	return (bitrate >= 1000000)
		? QString::number(bitrate / 1000000., 'f', 1) + u" Mbps"_q
		: QString::number(bitrate / 1000., 'f', 0) + u" kbps"_q;
}
} // namespace

ScreenSharingQuality::ScreenSharingQuality(QWidget *parent, Fn<bool()> active,
		Fn<int()> quality, Fn<void(int)> select,
		Fn<void(Fn<void(tgcalls::ScreenSharingStats)>)> stats,
		Fn<QPoint(QSize)> position, std::shared_ptr<Ui::Show> show)
: AbstractButton(parent)
, _active(std::move(active))
, _quality(std::move(quality))
, _select(std::move(select))
, _stats(std::move(stats))
, _position(std::move(position))
, _show(std::move(show))
, _timer([=] { refresh(); }) {
	resize(st::callScreenQualityWidth, st::callScreenQualityHeight);
	setClickedCallback([=] { showMenu(); });
	_infoButton = Ui::CreateChild<Ui::AbstractButton>(this);
	const auto infoSize = st::callScreenQualityInfoSize;
	_infoButton->resize(infoSize, infoSize);
	_infoButton->move(width() - infoSize - st::callScreenQualityInfoPadding,
		height() - infoSize - st::callScreenQualityInfoPadding);
	_infoButton->setClickedCallback([=] { showStatistics(); });
	_infoButton->setAccessibleName(u"Screen sharing statistics"_q);
	_infoButton->paintRequest() | rpl::on_next([=] {
		Painter p(_infoButton);
		auto quality = PainterHighQualityEnabler(p);
		p.setBrush(Qt::NoBrush);
		p.setPen(_infoButton->isOver() ? st::groupCallActiveFg : st::groupCallMemberNotJoinedStatus);
		p.drawEllipse(_infoButton->rect().adjusted(2, 2, -2, -2));
		p.setFont(st::callStatus.style.font);
		p.drawText(_infoButton->rect(), Qt::AlignCenter, u"i"_q);
	}, _infoButton->lifetime());
	hide();
	_timer.callEach(kRefreshInterval);
}
ScreenSharingQuality::~ScreenSharingQuality() = default;

void ScreenSharingQuality::refreshVisibility() {
	setVisible(_active());
	refresh();
}
void ScreenSharingQuality::refreshPosition() {
	if (!_active()) return;
	const auto position = _position(size());
	if (pos() != position) move(position);
}
QString ScreenSharingQuality::accessibilityName() {
	return QualityLabel(_quality()) + u" "_q + QString::number(_current.width)
		+ u" × "_q + QString::number(_current.height) + u" / "_q
		+ QString::number(_current.fps) + u" FPS, "_q
		+ BitrateLabel(_current.bitrate);
}
void ScreenSharingQuality::refresh() {
	const auto active = _active();
	if (active != _wasActive) {
		_wasActive = active;
		setVisible(active);
		_current = {};
		_pending = false;
		++_requestGeneration;
	}
	if (!active) { _diagnosticsText = u"Screen sharing stopped."_q; return; }
	refreshPosition();
	if (!_menu || _menu->isHidden()) {
		raise();
	}
	if (_pending && crl::now() - _requestedAt < kStatsTimeout) return;
	_pending = true;
	_requestedAt = crl::now();
	const auto generation = ++_requestGeneration;
	const auto weak = QPointer<ScreenSharingQuality>(this);
	_stats([weak, generation](tgcalls::ScreenSharingStats stats) {
		if (!weak || weak->_requestGeneration != generation) return;
		weak->_pending = false;
		weak->_current = std::move(stats);
		weak->_diagnosticsText = weak->statisticsText();
		weak->update();
	});
}

QString ScreenSharingQuality::statisticsText() const {
	const auto &s = _current;
	const auto &c = s.capture;
	const auto number = [](double value) { return QString::number(value, 'f', 1); };
	const auto resolution = [](int w, int h) { return QString::number(w) + u" × "_q + QString::number(h); };
	const auto rate = [](int value) { return value < 0 ? u"—"_q : BitrateLabel(value); };
	auto text = u"Mode: "_q + QualityLabel(_quality()) + u"\n\n"_q;
	if (c.available) {
		text += u"CAPTURE → CONVERSION\n"_q
			+ u"Source: "_q + resolution(c.sourceWidth, c.sourceHeight)
			+ u" → "_q + resolution(c.width, c.height) + u"\n"_q
			+ u"Requested / captured / converted / delivered FPS: "_q
			+ QString::number(c.requestedFps) + u" / "_q + number(c.captureFps)
			+ u" / "_q + number(c.convertedFps) + u" / "_q + number(c.deliveredFps) + u"\n"_q
			+ u"Frames with changed pixels: "_q + number(c.changedFps) + u" FPS\n"_q
			+ u"Capture / scale / ARGB→I420 / delivery: "_q
			+ number(c.captureMs) + u" / "_q + number(c.scaleMs) + u" / "_q
			+ number(c.convertMs) + u" / "_q + number(c.deliverMs) + u" ms\n"_q
			+ u"Loop average / maximum: "_q + number(c.loopMs) + u" / "_q + number(c.maxLoopMs) + u" ms\n"_q
			+ u"Frame interval / scheduler lateness: "_q + number(c.intervalMs) + u" / "_q + number(c.schedulerDelayMs) + u" ms\n"_q
			+ u"Captured / delivered frames: "_q + QString::number(c.capturedFrames) + u" / "_q + QString::number(c.deliveredFrames) + u"\n"_q
			+ u"Capture errors / unchanged / skipped unchanged / buffer drops / conversion errors: "_q
			+ QString::number(c.captureErrors) + u" / "_q + QString::number(c.unchangedFrames)
			+ u" / "_q + QString::number(c.skippedUnchangedFrames)
			+ u" / "_q + QString::number(c.poolDrops) + u" / "_q + QString::number(c.conversionErrors) + u"\n\n"_q;
	} else {
		text += u"Capture measurements: unavailable / warming up\n\n"_q;
	}
	text += u"ENCODER\n"_q
		+ u"Input / encoded FPS: "_q + number(s.encoderInputFps) + u" / "_q + QString::number(s.fps) + u"\n"_q
		+ u"Encoded size: "_q + resolution(s.width, s.height) + u"\n"_q
		+ u"Codec / encoder: "_q + QString::fromStdString(s.codec) + u" / "_q + QString::fromStdString(s.encoder) + u"\n"_q
		+ u"Power efficient encoder: "_q + (s.powerEfficientKnown ? (s.powerEfficient ? u"yes"_q : u"no"_q) : u"unknown"_q) + u"\n"_q
		+ u"Encode time / usage: "_q + QString::number(s.encodeMs) + u" ms / "_q + QString::number(s.encodeUsagePercent) + u"%\n"_q
		+ u"Average QP: "_q + (s.averageQp < 0 ? u"—"_q : number(s.averageQp)) + u"\n"_q
		+ u"Encoded / key frames: "_q + QString::number(s.encodedFrames) + u" / "_q + QString::number(s.keyFrames) + u"\n"_q
		+ u"Limitation: "_q + QString::fromStdString(s.limitation) + u"\n\n"_q
		+ u"NETWORK / SENDING\n"_q
		+ u"Available (BWE) / encoder target / media: "_q + rate(s.availableBitrate) + u" / "_q + rate(s.targetBitrate) + u" / "_q + rate(s.bitrate) + u"\n"_q
		+ u"Video RTP including padding / retransmissions: "_q + rate(s.rtpBitrate) + u" / "_q + rate(s.retransmissionBitrate) + u"\n"_q
		+ u"RTT / pacer queue: "_q + (s.rttMs < 0 ? u"—"_q : QString::number(s.rttMs)) + u" / "_q + QString::number(s.pacerDelayMs) + u" ms\n"_q
		+ u"Reported loss / NACK / PLI: "_q + number(s.lossPercent) + u"% / "_q + QString::number(s.nacks) + u" / "_q + QString::number(s.plis) + u"\n"_q
		+ u"Recovery: "_q + QString::fromStdString(s.recovery) + u"; attempts: "_q + QString::number(s.recoveryAttempts) + u"\n"_q
		+ u"Recovery baseline / stable samples / cooldown: "_q + rate(s.recoveryBaselineBitrate) + u" / "_q
		+ QString::number(s.recoverySamples) + u" / "_q + QString::number(s.recoveryCooldown) + u" s\n"_q
		+ u"Bandwidth probing: "_q + QString::fromStdString(s.bandwidthProbeState)
		+ u"; attempts: "_q + QString::number(s.bandwidthProbeAttempts)
		+ u"; last probe: "_q + rate(s.bandwidthProbeBitrate) + u"\n\n"_q
		+ u"Updated every second. Timings are averages of the latest capture window; counters are cumulative. BWE and RTT cover the call. Encoded FPS does not measure receiver playback."_q;
	return text;
}

void ScreenSharingQuality::showStatistics() {
	_diagnosticsText = statisticsText();
	const auto weak = QPointer<ScreenSharingQuality>(this);
	_show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Screen sharing statistics"_q));
		box->setWidth(2 * st::callScreenQualityWidth);
		auto label = box->addRow(object_ptr<Ui::FlatLabel>(box, _diagnosticsText.value()));
		label->setSelectable(true);
		_diagnosticsText.changes() | rpl::on_next([=](const QString &text) {
			label->setText(text);
		}, label->lifetime());
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
		box->addButton(rpl::single(u"Copy"_q), [=] {
			if (weak) QGuiApplication::clipboard()->setText(weak->statisticsText());
		});
	}));
}
void ScreenSharingQuality::showMenu() {
	_menu = std::make_unique<Ui::PopupMenu>(
		parentWidget(),
		st::callDeviceSelectionMenu);
	_menu->deleteOnHide(false);
	for (auto i = 0; i != int(kLabels.size()); ++i) {
		const auto action = new QAction(QualityLabel(i), _menu->menu());
		QObject::connect(action, &QAction::triggered, this, [=] {
			_select(i);
			_current = {};
			_pending = false;
			++_requestGeneration;
			update();
		}, Qt::QueuedConnection);
		action->setCheckable(true);
		action->setChecked(i == _quality());
		_menu->addAction(base::make_unique_q<Ui::Menu::Action>(
			_menu->menu(),
			_menu->st().menu,
			action,
			nullptr,
			nullptr));
	}
	_menu->setForcedVerticalOrigin(Ui::PopupMenu::VerticalOrigin::Bottom);
	_menu->popup(mapToGlobal(QPoint(0, 0)));
}
void ScreenSharingQuality::paintEvent(QPaintEvent *event) {
	Painter p(this);
	auto quality = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(isOver() ? st::groupCallMenuBgOver : st::groupCallMembersBg);
	p.drawRoundedRect(rect(), st::callScreenQualityRadius, st::callScreenQualityRadius);
	p.setFont(st::callDeviceSelectionLabel.style.font);
	p.setPen(st::groupCallActiveFg);
	const auto left = st::callScreenQualityPadding;
	const auto width = this->width() - 2 * left;
	const auto titleHeight = height() / 3;
	const auto detailHeight = (height() - titleHeight) / 2;
	p.drawText(QRect(left, 0, width, titleHeight), Qt::AlignVCenter | Qt::AlignLeft,
		QualityLabel(_quality()) + u" ▾"_q);
	p.setFont(st::callStatus.style.font);
	p.setPen(st::groupCallMemberNotJoinedStatus);
	const auto text = (_current.width > 0)
		? QString::number(_current.width) + u" × "_q + QString::number(_current.height)
			+ u" / "_q + QString::number(_current.fps) + u" FPS"_q
		: u"— × — / — FPS"_q;
	p.drawText(QRect(left, titleHeight, width, detailHeight),
		Qt::AlignVCenter | Qt::AlignLeft, text);
	p.drawText(QRect(left, titleHeight + detailHeight, width - st::callScreenQualityInfoSize, detailHeight),
		Qt::AlignVCenter | Qt::AlignLeft,
		(_current.width > 0)
			? u"Sending: "_q + BitrateLabel(_current.bitrate)
			: u"Sending: —"_q);
}
}
