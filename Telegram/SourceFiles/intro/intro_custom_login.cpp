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
#include "styles/style_intro.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include <QDesktopServices>
#include <QUrl>

#include <QJsonObject>
#include <QPointer>

namespace Intro::details {

CustomLoginWidget::CustomLoginWidget(
    QWidget *parent,
    not_null<Main::Account*> account,
    not_null<Data*> data)
: Step(parent, account, data)
, _code(this, st::introPassword, tr::lng_fox_mes_code())
, _getCode(this, tr::lng_fox_mes_get_code(), st::introFoxMesCodeButton) {
    setTitleText(tr::lng_fox_mes_title());
    setErrorCentered(true);
    _code->changes() | rpl::on_next([this] { hideError(); }, _code->lifetime());
    _getCode->setClickedCallback([this] { openPairingPage(); });
	_getCode->setTextTransform(Ui::RoundButtonTextTransform::ToUpper);
}

void CustomLoginWidget::resizeEvent(QResizeEvent *e) {
    Step::resizeEvent(e);
    _code->moveToLeft(contentLeft(), contentTop() + st::introStepFieldTop);
    _getCode->moveToLeft(
        (width() - _getCode->width()) / 2,
        _code->y() + _code->height() + 12);
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

void CustomLoginWidget::startPairing() {
    _busy = true;
    const auto weak = QPointer<CustomLoginWidget>(this);
    CustomBackend::Client().startDevice([weak](QJsonDocument doc, QString error, int) {
        if (!weak) return;
        weak->_busy = false;
        if (!error.isEmpty() || !doc.isObject()) {
			weak->_openPairingWhenReady = false;
			weak->showError(tr::lng_fox_mes_link_failed());
            return;
        }
        const auto object = doc.object();
        weak->_pairingRequest = object.value("request").toString();
        weak->_pairingURL = object.value("url").toString();
        if (weak->_pairingRequest.isEmpty() || weak->_pairingURL.isEmpty()) {
			weak->_openPairingWhenReady = false;
			weak->showError(tr::lng_fox_mes_invalid_link());
            return;
        }
		if (weak->_openPairingWhenReady) {
			weak->_openPairingWhenReady = false;
			weak->openPairingPage();
		}
    });
}

void CustomLoginWidget::openPairingPage() {
    if (_pairingURL.isEmpty()) {
        _openPairingWhenReady = true;
        if (!_busy) startPairing();
        return;
    }
    const auto url = QUrl(_pairingURL);
    if (!url.isValid() || !QDesktopServices::openUrl(url)) {
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

    _busy = true;
    const auto weak = QPointer<CustomLoginWidget>(this);
    CustomBackend::Client().exchangeDevice(weak->_pairingRequest, code,
        [weak](QJsonDocument doc, QString error, int) {
            if (!weak) return;
            weak->_busy = false;
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
