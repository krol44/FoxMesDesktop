/*
This file is part of FoxMes, an unofficial desktop application
based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "custom_backend/topic_channel_links.h"

#include "base/qthelp_regex.h"
#include "base/qthelp_url.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_runtime.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace CustomBackend::TopicChannels {
namespace {

base::flat_map<GlobalMsgId, QString> SiteLinks;

base::flat_set<QString> SiteHosts = { u"foxtail.ing"_q, u"fxl.ru"_q };

[[nodiscard]] QString HostOf(const QUrl &url) {
	const auto host = url.host().toLower();
	return host.startsWith(u"www."_q) ? host.mid(4) : host;
}

} // namespace

void RememberSiteLink(
		not_null<Main::Session*> session,
		FullMsgId itemId,
		const QString &url) {
	if (url.isEmpty() || !itemId) {
		return;
	}
	SiteLinks[GlobalMsgId{ itemId, session->uniqueId() }] = url;
	if (const auto host = HostOf(QUrl(url)); !host.isEmpty()) {
		SiteHosts.emplace(host);
	}
}

QString SiteLink(not_null<HistoryItem*> item) {
	const auto i = SiteLinks.find(item->globalId());
	return (i != end(SiteLinks)) ? i->second : QString();
}

QString LocalSiteLink(const QString &url) {
	static const auto Path = QRegularExpression(
		u"^/([A-Za-z0-9_\\-]+)/(\\d+)(-[^/]*)?/?$"_q);
	static const auto Comment = QRegularExpression(u"^comment-(\\d+)$"_q);
	const auto parsed = QUrl(url.contains(u"://"_q)
		? url
		: (u"https://"_q + url));
	const auto web = (parsed.scheme() == u"https"_q)
		|| (parsed.scheme() == u"http"_q);
	if (!parsed.isValid() || !web || !SiteHosts.contains(HostOf(parsed))) {
		return QString();
	}
	const auto path = Path.match(parsed.path());
	if (!path.hasMatch()) {
		return QString();
	}
	auto result = u"tg://foxmes_topic?category=%1&topic=%2"_q
		.arg(path.captured(1), path.captured(2));
	const auto comment = Comment.match(parsed.fragment());
	if (comment.hasMatch()) {
		result += u"&comment="_q + comment.captured(1);
	}
	return result + u"&url="_q + qthelp::url_encode(url);
}

bool OpenSiteLink(
		Window::SessionController *controller,
		const qthelp::RegularExpressionMatch &match,
		const QVariant &context) {
	const auto params = qthelp::url_parse_params(
		match->captured(1),
		qthelp::UrlParamNameTransform::ToLower);
	const auto url = params.value(u"url"_q);
	const auto openInBrowser = [=] {
		if (!url.isEmpty()) {
			File::OpenUrl(url);
		}
	};
	const auto topic = params.value(u"topic"_q).toLongLong();
	if (!controller || topic <= 0) {
		openInBrowser();
		return true;
	}
	auto path = u"/channels/topics/%1/post?category=%2"_q
		.arg(topic)
		.arg(qthelp::url_encode(params.value(u"category"_q)));
	if (const auto comment = params.value(u"comment"_q); !comment.isEmpty()) {
		path += u"&comment="_q + qthelp::url_encode(comment);
	}
	const auto weak = base::make_weak(controller);
	ClientFor(&controller->session()).communityRequest("GET", path, {}, [=](
			QJsonDocument doc,
			QString error,
			int status) {
		const auto data = doc.object();
		const auto username = data.value("username").toString();
		const auto post = data.value("message_id").toVariant().toLongLong();
		if (!weak.get() || !error.isEmpty() || username.isEmpty() || post <= 0) {
			if (!error.isEmpty() && status != 404) {
				LOG(("FoxMes TopicChannels: site link %1 failed, status %2"
					).arg(path).arg(status));
			}
			openInBrowser();
			return;
		}
		auto local = u"tg://resolve?domain=%1&post=%2"_q
			.arg(qthelp::url_encode(username))
			.arg(post);
		const auto comment = data.value("comment_id").toVariant().toLongLong();
		if (comment > 0) {
			local += u"&comment=%1"_q.arg(comment);
		}
		Core::App().openLocalUrl(local, context);
	});
	return true;
}

}
