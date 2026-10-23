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
#include <optional>

class QWebSocket;

namespace CustomBackend::Sealed {
class SocketCodec;
struct SocketRequest;
} // namespace CustomBackend::Sealed

namespace base {
class NetworkReachability;
} // namespace base

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
	using RequestReady = std::function<void(
		std::optional<QNetworkRequest> request,
		std::shared_ptr<Sealed::SocketCodec> codec)>;
	using RequestFactory = std::function<void(bool fresh, RequestReady ready)>;
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
	void checkNow();
	void setPresenceChat(qint64 chatId);

	[[nodiscard]] LiveUpdatesStatus status() const;
	[[nodiscard]] rpl::producer<LiveUpdatesStatus> statusValue() const;

private:
	void connectNow();
	void retryByTimer();
	void failAttempt(QWebSocket *socket);
	void socketConnected(QWebSocket *socket);
	void openSocket(QWebSocket *socket, const QNetworkRequest &request);
	void socketBinary(QWebSocket *socket, const QByteArray &message);
	void socketMessage(QWebSocket *socket, QString message);
	void pongTimedOut();
	void stopHeartbeat();
	void destroySocket();
	void setStatus(LiveUpdatesStatus status);
	void sendPresence();

	RequestFactory _requestFactory;
	ConnectedCallback _connected;
	MessageCallback _message;
	rpl::variable<LiveUpdatesStatus> _status;
	QTimer _retryTimer;
	QTimer _heartbeatTimer;
	QTimer _pongTimer;
	std::unique_ptr<QWebSocket> _webSocket;
	std::shared_ptr<Sealed::SocketCodec> _codec;
	int _failuresSinceConnect = 0;
	const std::shared_ptr<base::NetworkReachability> _networkReachability;
	int _retryTimeout = 1;
	bool _running = false;
	bool _stopped = false;
	bool _attemptFailed = false;
	qint64 _presenceChatId = 0;
	rpl::lifetime _lifetime;

};

}
