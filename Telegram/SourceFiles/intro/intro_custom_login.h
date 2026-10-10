#pragma once

#include "base/timer.h"
#include "intro/intro_step.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>

namespace Ui {
class FlatLabel;
class RoundButton;
}

namespace Intro::details {

class FoxMesHeader;
class FoxMesCodeView;

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
    void cancelled() override;
    void finished() override;
    rpl::producer<QString> nextButtonText() const override;
    bool hasBack() const override;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
	void startLogin(const QString &status);
	void schedulePoll(int seconds);
	void poll();
	void complete(const QJsonDocument &doc);
	void stop();
	void copyCode();
	void setStatus(const QString &text);

	rpl::variable<QString> _link;
	object_ptr<FoxMesHeader> _header;
	object_ptr<Ui::FlatLabel> _about;
	object_ptr<FoxMesCodeView> _code;
	object_ptr<Ui::RoundButton> _copy;
	object_ptr<Ui::FlatLabel> _status;
	base::Timer _startTimer;
	base::Timer _pollTimer;
	QString _request;
	QString _verificationUrl;
	QDateTime _expiresAt;
	int _interval = 5;
	int _startDelay = 2;
	int _generation = 0;
	bool _stopped = false;
};

}
