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
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
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

} // namespace

// The application logo next to the product name, centered in the column.
class FoxMesHeader final : public Ui::RpWidget {
public:
	explicit FoxMesHeader(QWidget *parent);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	QImage _logo;

};

// A thin rule broken in the middle by a round badge with the step number.
class FoxMesStepDivider final : public Ui::RpWidget {
public:
	FoxMesStepDivider(QWidget *parent, int number);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const QString _number;

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

FoxMesStepDivider::FoxMesStepDivider(QWidget *parent, int number)
: RpWidget(parent)
, _number(QString::number(number)) {
	resize(st::introFoxMesColumnWidth, st::introFoxMesDividerHeight);
}

void FoxMesStepDivider::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto badge = st::introFoxMesBadgeSize;
	const auto skip = st::introFoxMesDividerSkip;
	const auto circle = QRect(
		(width() - badge) / 2,
		(height() - badge) / 2,
		badge,
		badge);

	const auto line = st::lineWidth;
	const auto lineTop = (height() - line) / 2;
	const auto lineWidth = circle.x() - skip;
	if (lineWidth > 0) {
		p.fillRect(0, lineTop, lineWidth, line, st::inputBorderFg);
		p.fillRect(
			circle.x() + badge + skip,
			lineTop,
			lineWidth,
			line,
			st::inputBorderFg);
	}

	p.setPen(Qt::NoPen);
	p.setBrush(st::activeButtonBg);
	p.drawEllipse(circle);

	p.setPen(st::activeButtonFg);
	p.setFont(st::introFoxMesBadgeFont);
	p.drawText(circle, Qt::AlignCenter, _number);
}

CustomLoginWidget::CustomLoginWidget(
    QWidget *parent,
    not_null<Main::Account*> account,
    not_null<Data*> data)
: Step(parent, account, data)
, _header(this)
, _firstDivider(this, 1)
, _getCode(this, tr::lng_fox_mes_get_code(), st::introFoxMesStepButton)
, _copyURL(this, tr::lng_fox_mes_copy_auth_url(), st::introFoxMesStepButton)
, _secondDivider(this, 2)
, _code(this, st::introPassword, tr::lng_fox_mes_code()) {
    setErrorCentered(true);
    _code->changes() | rpl::on_next([this] { hideError(); }, _code->lifetime());
    _getCode->setClickedCallback([this] { startPairing(); });
	_getCode->setTextTransform(Ui::RoundButtonTextTransform::ToUpper);
    _copyURL->setClickedCallback([this] { copyAuthURL(); });
	_copyURL->setTextTransform(Ui::RoundButtonTextTransform::ToUpper);
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
    _firstDivider->moveToLeft(left, top + st::introFoxMesDividerFirstTop);

    const auto half = (column - st::introFoxMesButtonsSkip) / 2;
    _getCode->setFullWidth(half);
    _copyURL->setFullWidth(half);
    const auto buttonsTop = top + st::introFoxMesButtonsTop;
    _getCode->moveToLeft(left, buttonsTop);
    _copyURL->moveToLeft(left + column - _copyURL->width(), buttonsTop);

    _secondDivider->moveToLeft(left, top + st::introFoxMesDividerSecondTop);
    _code->moveToLeft(
        (width() - _code->width()) / 2,
        top + st::introFoxMesFieldTop);
}

void CustomLoginWidget::setInnerFocus() {
    _code->setFocusFast();
}

rpl::producer<QString> CustomLoginWidget::nextButtonText() const {
    return tr::lng_fox_mes_connect();
}

bool CustomLoginWidget::hasBack() const {
    const auto back = getData()->accountBeforeIntro.get();
    return (back && back->sessionExists())
        || Core::App().domain().maybeLastOrSomeAuthedAccount();
}

void CustomLoginWidget::setBusy(bool busy) {
    _busy = busy;
    _getCode->setDisabled(busy);
    _copyURL->setDisabled(busy);
}

void CustomLoginWidget::requestPairing(Fn<void()> done) {
    if (_busy) return;

    hideError();
    setBusy(true);
    const auto weak = QPointer<CustomLoginWidget>(this);
    CustomBackend::Client().startDevice([weak, done](QJsonDocument doc, QString error, int) {
        if (!weak) return;
        weak->setBusy(false);
        if (!error.isEmpty() || !doc.isObject()) {
            weak->showError(error.isEmpty()
                ? tr::lng_fox_mes_link_failed()
                : rpl::single(error));
            return;
        }
        const auto object = doc.object();
        const auto request = object.value("request").toString();
        const auto url = object.value("url").toString();
        if (request.isEmpty() || url.isEmpty() || !QUrl(url).isValid()) {
            weak->showError(tr::lng_fox_mes_invalid_link());
            return;
        }
        weak->_pairingRequest = request;
        weak->_pairingURL = url;
        weak->_code->setText(QString());
        if (done) {
            done();
        }
    });
}

void CustomLoginWidget::startPairing() {
    const auto weak = QPointer<CustomLoginWidget>(this);
    requestPairing([weak] {
        if (weak) {
            weak->openPairingPage();
        }
    });
}

void CustomLoginWidget::copyAuthURL() {
    const auto copy = [](const QString &url) {
        if (const auto clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(url);
            Ui::Toast::Show(tr::lng_fox_mes_auth_url_copied(tr::now));
        }
    };
    if (!_pairingURL.isEmpty()) {
        hideError();
        copy(_pairingURL);
        return;
    }
    const auto weak = QPointer<CustomLoginWidget>(this);
    requestPairing([weak, copy] {
        if (!weak) return;
        if (weak->_pairingURL.isEmpty()) {
            weak->showError(tr::lng_fox_mes_auth_url_missing());
            return;
        }
        copy(weak->_pairingURL);
    });
}

void CustomLoginWidget::openPairingPage() {
    if (!QDesktopServices::openUrl(QUrl(_pairingURL))) {
        showError(tr::lng_fox_mes_browser_failed());
    }
}

void CustomLoginWidget::submit() {
    if (_busy || isHidden()) return;
    const auto code = _code->getLastText().trimmed();
    if (_pairingRequest.isEmpty()) {
        showError(tr::lng_fox_mes_pairing_preparing());
        return;
    }
    if (code.isEmpty()) {
        showError(tr::lng_fox_mes_code_required());
        return;
    }

    setBusy(true);
    const auto weak = QPointer<CustomLoginWidget>(this);
    CustomBackend::Client().exchangeDevice(weak->_pairingRequest, code,
        [weak](QJsonDocument doc, QString error, int) {
            if (!weak) return;
            weak->setBusy(false);
            if (!error.isEmpty() || !doc.isObject()) {
                weak->showError(error.isEmpty()
                    ? tr::lng_fox_mes_login_failed()
                    : rpl::single(error));
                weak->_code->selectAll();
                weak->_code->showError();
                return;
            }
            const auto user = doc.object().value("user").toObject();
            const auto id = user.value("id").toVariant().toLongLong();
            if (id <= 0) {
                weak->showError(tr::lng_fox_mes_invalid_user());
                return;
            }

            qWarning() << "FoxMes LOGIN OK, user id =" << id;
            CustomBackend::RememberLogin(doc);
            const auto raw = &weak->account();
            raw->setSessionUserId(UserId(id));
            raw->createSession(
                UserId(id),
                QByteArray(),
                0,
                std::make_unique<Main::SessionSettings>());

            // Creating Main::Session destroys/replaces the intro widget.
            raw->local().writeMtpData();
            Local::sync();
        });
}

} // namespace Intro::details
