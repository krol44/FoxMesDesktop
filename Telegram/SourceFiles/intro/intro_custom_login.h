#pragma once

#include "intro/intro_step.h"

namespace Ui {
class InputField;
class RoundButton;
}

namespace Intro::details {

class CustomLoginWidget final : public Step {
public:
    CustomLoginWidget(
        QWidget *parent,
        not_null<Main::Account*> account,
        not_null<Data*> data);

    void setInnerFocus() override;
    void submit() override;
    rpl::producer<QString> nextButtonText() const override;
    bool hasBack() const override;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
	void startPairing();
	void openPairingPage();

	object_ptr<Ui::InputField> _code;
	object_ptr<Ui::RoundButton> _getCode;
	QString _pairingRequest;
	QString _pairingURL;
	bool _openPairingWhenReady = false;
	bool _busy = false;
};

} // namespace Intro::details
