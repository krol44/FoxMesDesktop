#include "intro/intro_custom_login.h"

#include "core/application.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_runtime.h"
#include "intro/intro_widget.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session_settings.h"
#include "storage/localstorage.h"
#include "storage/storage_account.h"
#include "ui/abstract_button.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "styles/style_basic.h"
#include "styles/style_intro.h"

#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>

#include <QJsonObject>
#include <QPointer>

namespace Intro::details {
namespace {

constexpr auto kLogoPath = ":/gui/art/logo_256_no_margin.png";
constexpr auto kProductName = "FoxMes";
constexpr auto kDefaultLink = "foxtail.ing/fm";
constexpr auto kMaxStartDelay = 30;

[[nodiscard]] QString LinkLabel(const QString &url) {
	const auto parsed = QUrl(url);
	if (!parsed.isValid() || parsed.host().isEmpty()) {
		return QString(kDefaultLink);
	}
	const auto port = parsed.port();
	return parsed.host()
		+ ((port > 0) ? (':' + QString::number(port)) : QString())
		+ parsed.path();
}

[[nodiscard]] int RetryAfter(const QJsonDocument &doc, int fallback) {
	const auto seconds = doc.isObject()
		? doc.object().value("_retry_after_seconds").toInt()
		: 0;
	return std::max(seconds, fallback);
}

} // namespace

class FoxMesHeader final : public Ui::RpWidget {
public:
	explicit FoxMesHeader(QWidget *parent);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	QImage _logo;

};

// Код крупно, группами по три цифры; клик копирует его без пробела.
class FoxMesCodeView final : public Ui::AbstractButton {
public:
	explicit FoxMesCodeView(QWidget *parent);

	void setCode(const QString &code);
	[[nodiscard]] const QString &code() const;

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	QString _code;

};

FoxMesHeader::FoxMesHeader(QWidget *parent) : RpWidget(parent) {
	const auto ratio = style::DevicePixelRatio();
	const auto side = st::introFoxMesLogoSize * ratio;
	_logo = QImage(QString(kLogoPath)).scaled(
		side,
		side,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	_logo.setDevicePixelRatio(ratio);
	resize(st::introFoxMesColumnWidth, st::introFoxMesHeaderHeight);
}

void FoxMesHeader::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	const auto name = QString(kProductName);
	const auto font = st::introFoxMesNameFont;
	const auto side = st::introFoxMesLogoSize;
	const auto skip = st::introFoxMesHeaderSkip;
	const auto left = (width() - side - skip - font->width(name)) / 2;

	p.drawImage(QRect(left, (height() - side) / 2, side, side), _logo);
	p.setFont(font);
	p.setPen(st::introTitleFg);
	p.drawText(
		left + side + skip,
		(height() - font->height) / 2 + font->ascent,
		name);
}

FoxMesCodeView::FoxMesCodeView(QWidget *parent) : AbstractButton(parent) {
	resize(st::introFoxMesColumnWidth, st::introFoxMesCodeHeight);
}

void FoxMesCodeView::setCode(const QString &code) {
	_code = code;
	setDisabled(_code.isEmpty());
	update();
}

const QString &FoxMesCodeView::code() const {
	return _code;
}

void FoxMesCodeView::paintEvent(QPaintEvent *e) {
	if (_code.isEmpty()) {
		return;
	}
	auto p = QPainter(this);
	const auto font = st::introFoxMesCodeFont;
	const auto half = _code.size() / 2;
	const auto first = _code.left(half);
	const auto second = _code.mid(half);
	const auto skip = st::introFoxMesCodeGroupSkip;
	const auto full = font->width(first) + skip + font->width(second);
	const auto left = (width() - full) / 2;
	const auto top = (height() - font->height) / 2 + font->ascent;
	p.setFont(font);
	p.setPen(isOver() ? st::introDescriptionFg : st::introTitleFg);
	p.drawText(left, top, first);
	p.drawText(left + font->width(first) + skip, top, second);
}

CustomLoginWidget::CustomLoginWidget(
    QWidget *parent,
    not_null<Main::Account*> account,
    not_null<Data*> data)
: Step(parent, account, data)
, _link(QString(kDefaultLink))
, _header(this)
, _about(
	this,
	tr::lng_fox_mes_login_about(lt_link, _link.value()),
	st::introFoxMesAbout)
, _code(this)
, _copy(this, tr::lng_fox_mes_copy_code(), st::introFoxMesCopyButton)
, _status(this, QString(), st::introFoxMesStatus)
, _startTimer([=] { startLogin(QString()); })
, _pollTimer([=] { poll(); }) {
	_about->resizeToWidth(st::introFoxMesColumnWidth);
	_status->resizeToWidth(st::introFoxMesColumnWidth);
	_code->setClickedCallback([=] { copyCode(); });
	_copy->setClickedCallback([=] { copyCode(); });
	_copy->setTextTransform(Ui::RoundButtonTextTransform::ToUpper);
	_copy->setFullWidth(st::introFoxMesColumnWidth);
	_copy->setDisabled(true);
	startLogin(QString());
}

CustomLoginWidget::~CustomLoginWidget() = default;

QString CustomLoginWidget::accessibilityName() {
	return QString(kProductName);
}

void CustomLoginWidget::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);

	const auto top = contentTop();
	const auto column = st::introFoxMesColumnWidth;
	const auto left = (width() - column) / 2;

	_header->moveToLeft(left, top);
	_about->moveToLeft(left, top + st::introFoxMesAboutTop);
	_code->moveToLeft(left, top + st::introFoxMesCodeTop);
	_copy->moveToLeft(left, top + st::introFoxMesCopyTop);
	_status->moveToLeft(left, top + st::introFoxMesStatusTop);
}

void CustomLoginWidget::setInnerFocus() {
	setFocus();
}

rpl::producer<QString> CustomLoginWidget::nextButtonText() const {
	return tr::lng_fox_mes_open_link(lt_link, _link.value());
}

bool CustomLoginWidget::hasBack() const {
	const auto back = getData()->accountBeforeIntro.get();
	return (back && back->sessionExists())
		|| Core::App().domain().maybeLastOrSomeAuthedAccount();
}

void CustomLoginWidget::cancelled() {
	stop();
	Step::cancelled();
}

void CustomLoginWidget::finished() {
	stop();
	Step::finished();
}

void CustomLoginWidget::stop() {
	_stopped = true;
	++_generation;
	_startTimer.cancel();
	_pollTimer.cancel();
}

void CustomLoginWidget::setStatus(const QString &text) {
	_status->setText(text);
	_status->resizeToWidth(st::introFoxMesColumnWidth);
}

// status — что сказать пользователю о смене кода (истёк, отклонён); пусто —
// обычный первый запуск.
void CustomLoginWidget::startLogin(const QString &status) {
	if (_stopped) return;

	const auto generation = ++_generation;
	_pollTimer.cancel();
	_request.clear();
	_code->setCode(QString());
	_copy->setDisabled(true);
	setStatus(status.isEmpty() ? tr::lng_fox_mes_getting_code(tr::now) : status);

	const auto weak = QPointer<CustomLoginWidget>(this);
	CustomBackend::Client().startDevice([=](QJsonDocument doc, QString error, int code) {
		if (!weak || weak->_generation != generation) return;
		const auto object = doc.isObject() ? doc.object() : QJsonObject();
		const auto request = object.value("request").toString();
		const auto userCode = object.value("user_code").toString();
		const auto url = object.value("verification_url").toString();
		if (!error.isEmpty() || request.isEmpty() || userCode.isEmpty() || !QUrl(url).isValid()) {
			// 429 — сервер ограничивает частоту стартов с одного IP; остальное
			// считаем отсутствием связи и повторяем с нарастающей паузой.
			const auto delay = (code == 429)
				? RetryAfter(doc, weak->_startDelay)
				: weak->_startDelay;
			if (code != 429) {
				weak->setStatus(tr::lng_fox_mes_no_connection(tr::now));
			}
			weak->_startDelay = std::min(weak->_startDelay * 2, kMaxStartDelay);
			weak->_startTimer.callOnce(delay * crl::time(1000));
			return;
		}
		weak->_startDelay = 2;
		weak->_request = request;
		weak->_verificationUrl = url;
		weak->_link = LinkLabel(url);
		weak->_expiresAt = QDateTime::fromString(
			object.value("expires_at").toString(),
			Qt::ISODate);
		weak->_interval = std::max(object.value("interval").toInt(), 1);
		weak->_code->setCode(userCode);
		weak->_copy->setDisabled(false);
		if (status.isEmpty()) {
			weak->setStatus(tr::lng_fox_mes_waiting(tr::now));
		}
		weak->schedulePoll(weak->_interval);
	});
}

void CustomLoginWidget::schedulePoll(int seconds) {
	_pollTimer.callOnce(std::max(seconds, 1) * crl::time(1000));
}

void CustomLoginWidget::poll() {
	if (_stopped || _request.isEmpty()) return;
	if (_expiresAt.isValid()
		&& QDateTime::currentDateTimeUtc() >= _expiresAt) {
		startLogin(tr::lng_fox_mes_code_renewed(tr::now));
		return;
	}

	const auto generation = _generation;
	const auto weak = QPointer<CustomLoginWidget>(this);
	CustomBackend::Client().pollDevice(_request, [=](QJsonDocument doc, QString error, int code) {
		if (!weak || weak->_generation != generation) return;
		switch (code) {
		case 200:
			weak->complete(doc);
			return;
		case 202:
			weak->setStatus(tr::lng_fox_mes_waiting(tr::now));
			weak->schedulePoll(weak->_interval);
			return;
		case 429:
			weak->schedulePoll(RetryAfter(doc, weak->_interval));
			return;
		case 403:
			weak->startLogin(tr::lng_fox_mes_login_declined(tr::now));
			return;
		case 410:
			weak->startLogin(tr::lng_fox_mes_code_renewed(tr::now));
			return;
		}
		weak->setStatus(tr::lng_fox_mes_no_connection(tr::now));
		weak->schedulePoll(weak->_interval);
	});
}

void CustomLoginWidget::complete(const QJsonDocument &doc) {
	const auto user = doc.object().value("user").toObject();
	const auto id = user.value("id").toVariant().toLongLong();
	if (id <= 0) {
		startLogin(tr::lng_fox_mes_no_connection(tr::now));
		return;
	}
	stop();
	setStatus(tr::lng_fox_mes_login_done(tr::now));

	const auto raw = &account();
	CustomBackend::RememberLogin(raw, doc);
	raw->setSessionUserId(UserId(id));
	raw->createSession(
		UserId(id),
		QByteArray(),
		0,
		std::make_unique<Main::SessionSettings>());

	raw->local().writeMtpData();
	Local::sync();
}

void CustomLoginWidget::copyCode() {
	const auto code = _code->code();
	if (code.isEmpty()) return;
	if (const auto clipboard = QGuiApplication::clipboard()) {
		clipboard->setText(code);
		Ui::Toast::Show(tr::lng_fox_mes_code_copied(tr::now));
	}
}

void CustomLoginWidget::submit() {
	if (isHidden() || _verificationUrl.isEmpty()) return;
	if (!QDesktopServices::openUrl(QUrl(_verificationUrl))) {
		setStatus(tr::lng_fox_mes_browser_failed(tr::now));
	}
}

}
