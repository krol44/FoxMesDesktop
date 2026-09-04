#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkRequest>

#include <crl/crl_time.h>
#include <rpl/producer.h>
#include <rpl/variable.h>

#include <functional>
#include <memory>

class QWebSocket;

namespace CustomBackend {

enum class LiveUpdatesState {
	Connected,
	Connecting,
	Waiting,
};

struct LiveUpdatesStatus {
	LiveUpdatesState state = LiveUpdatesState::Connected;
	crl::time retryAt = 0;

	[[nodiscard]] bool operator==(const LiveUpdatesStatus &other) const;
};

class LiveUpdatesConnection final : public QObject {
public:
	using RequestFactory = std::function<QNetworkRequest()>;
	using ConnectedCallback = std::function<void()>;
	using MessageCallback = std::function<void(QString)>;

	LiveUpdatesConnection(
		QObject *parent,
		RequestFactory requestFactory,
		ConnectedCallback connected,
		MessageCallback message);
	~LiveUpdatesConnection() override;

	void start();
	void stop();
	void restartNow();
	void setPresenceChat(qint64 chatId);

	[[nodiscard]] LiveUpdatesStatus status() const;
	[[nodiscard]] rpl::producer<LiveUpdatesStatus> statusValue() const;

private:
	void connectNow();
	void retryByTimer();
	void failAttempt(QWebSocket *socket);
	void socketConnected(QWebSocket *socket);
	void socketMessage(QWebSocket *socket, QString message);
	void destroySocket();
	void setStatus(LiveUpdatesStatus status);
	void sendPresence();

	RequestFactory _requestFactory;
	ConnectedCallback _connected;
	MessageCallback _message;
	rpl::variable<LiveUpdatesStatus> _status;
	QTimer _retryTimer;
	std::unique_ptr<QWebSocket> _webSocket;
	int _retryTimeout = 1;
	bool _running = false;
	bool _stopped = false;
	bool _attemptFailed = false;
	qint64 _presenceChatId = 0;

};

} // namespace CustomBackend
