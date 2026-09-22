#pragma once

#include "intro/intro_step.h"

namespace Ui {
class InputField;
class RoundButton;
}

namespace Intro::details {

class FoxMesHeader;
class FoxMesStepDivider;

class CustomLoginWidget final : public Step {
public:
    CustomLoginWidget(
        QWidget *parent,
        not_null<Main::Account*> account,
        not_null<Data*> data);
    ~CustomLoginWidget();

    QString accessibilityName() override;

    void setInnerFocus() override;
    void submit() override;
    rpl::producer<QString> nextButtonText() const override;
    bool hasBack() const override;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
	void requestPairing(Fn<void()> done);
	void startPairing();
	void copyAuthURL();
	void openPairingPage();
	void setBusy(bool busy);

	object_ptr<FoxMesHeader> _header;
	object_ptr<FoxMesStepDivider> _firstDivider;
	object_ptr<Ui::RoundButton> _getCode;
	object_ptr<Ui::RoundButton> _copyURL;
	object_ptr<FoxMesStepDivider> _secondDivider;
	object_ptr<Ui::InputField> _code;
	QString _pairingRequest;
	QString _pairingURL;
	bool _busy = false;
};

} // namespace Intro::details
