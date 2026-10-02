#pragma once
#include "ui/abstract_button.h"
#include "base/timer.h"
#include "rpl/variable.h"
#include "tgcalls/ScreenSharing.h"

namespace Ui { class PopupMenu; class Show; }
namespace Calls {
class ScreenSharingQuality final : public Ui::AbstractButton {
public:
	ScreenSharingQuality(QWidget *parent, Fn<bool()> active, Fn<int()> quality,
		Fn<void(int)> select, Fn<void(Fn<void(tgcalls::ScreenSharingStats)>)> stats,
		Fn<QPoint(QSize)> position, std::shared_ptr<Ui::Show> show);
	~ScreenSharingQuality();
	void refreshVisibility();
	void refreshPosition();
	QString accessibilityName() override;
protected:
	void paintEvent(QPaintEvent *event) override;
private:
	void refresh();
	void showMenu();
	void showStatistics();
	QString statisticsText() const;
	Fn<bool()> _active;
	Fn<int()> _quality;
	Fn<void(int)> _select;
	Fn<void(Fn<void(tgcalls::ScreenSharingStats)>)> _stats;
	Fn<QPoint(QSize)> _position;
	std::shared_ptr<Ui::Show> _show;
	base::Timer _timer;
	std::unique_ptr<Ui::PopupMenu> _menu;
	Ui::AbstractButton *_infoButton = nullptr;
	rpl::variable<QString> _diagnosticsText;
	tgcalls::ScreenSharingStats _current;
	bool _wasActive = false;
	bool _pending = false;
	crl::time _requestedAt = 0;
	uint64 _requestGeneration = 0;
};
}
