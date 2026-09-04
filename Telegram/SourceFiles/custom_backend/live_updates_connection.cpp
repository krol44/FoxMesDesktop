#include "custom_backend/live_updates_connection.h"

#include <QtNetwork/QAbstractSocket>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslError>
#include <QtNetwork/QSslSocket>
#include <QtWebSockets/QWebSocket>

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace CustomBackend {
namespace {

constexpr auto kMaximumRetryTimeout = 64'000;

constexpr int NextRetryTimeout(int timeout) {
	if (timeout < 3) {
		return timeout + 1;
	} else if (timeout == 3) {
		return 1000;
	}
	return std::min(timeout * 2, kMaximumRetryTimeout);
}

static_assert(NextRetryTimeout(1) == 2);
static_assert(NextRetryTimeout(2) == 3);
static_assert(NextRetryTimeout(3) == 1000);
static_assert(NextRetryTimeout(1000) == 2000);
static_assert(NextRetryTimeout(2000) == 4000);
static_assert(NextRetryTimeout(4000) == 8000);
static_assert(NextRetryTimeout(8000) == 16000);
static_assert(NextRetryTimeout(16000) == 32000);
static_assert(NextRetryTimeout(32000) == 64000);
static_assert(NextRetryTimeout(64000) == 64000);

} // namespace

bool LiveUpdatesStatus::operator==(const LiveUpdatesStatus &other) const {
	return (state == other.state) && (retryAt == other.retryAt);
}

LiveUpdatesConnection::LiveUpdatesConnection(
	QObject *parent,
	RequestFactory requestFactory,
	ConnectedCallback connected,
	MessageCallback message)
: QObject(parent)
, _requestFactory(std::move(requestFactory))
, _connected(std::move(connected))
, _message(std::move(message)) {
	_retryTimer.setSingleShot(true);
	QObject::connect(
		&_retryTimer,
		&QTimer::timeout,
		this,
		&LiveUpdatesConnection::retryByTimer);
}

LiveUpdatesConnection::~LiveUpdatesConnection() {
	stop();
}

void LiveUpdatesConnection::start() {
	if (_running || _stopped) {
		return;
	}
	_running = true;
	_retryTimeout = 1;
	connectNow();
}

void LiveUpdatesConnection::stop() {
	_running = false;
	_stopped = true;
	_attemptFailed = true;
	_retryTimer.stop();
	destroySocket();
	_retryTimeout = 1;
	setStatus({ LiveUpdatesState::Connected });
}

void LiveUpdatesConnection::restartNow() {
	if (!_running) {
		return;
	}
	_attemptFailed = true;
	_retryTimer.stop();
	destroySocket();
	_retryTimeout = 1;
	_attemptFailed = false;
	setStatus({
		.state = LiveUpdatesState::Waiting,
		.retryAt = crl::now() + _retryTimeout,
	});
	_retryTimer.start(_retryTimeout);
}

void LiveUpdatesConnection::setPresenceChat(qint64 chatId) {
	chatId = std::max<qint64>(0, chatId);
	if (_presenceChatId == chatId) {
		return;
	}
	_presenceChatId = chatId;
	sendPresence();
}

LiveUpdatesStatus LiveUpdatesConnection::status() const {
	return _status.current();
}

rpl::producer<LiveUpdatesStatus> LiveUpdatesConnection::statusValue() const {
	return _status.value();
}

void LiveUpdatesConnection::connectNow() {
	if (!_running) {
		return;
	}
	_attemptFailed = true;
	destroySocket();
	_attemptFailed = false;
	setStatus({ LiveUpdatesState::Connecting });

	_webSocket = std::make_unique<QWebSocket>();
	const auto socket = _webSocket.get();
	QObject::connect(socket, &QWebSocket::connected, this, [=] {
		socketConnected(socket);
	});
	QObject::connect(
		socket,
		&QWebSocket::textMessageReceived,
		this,
		[=](const QString &message) {
			socketMessage(socket, message);
		});
	QObject::connect(
		socket,
		&QWebSocket::errorOccurred,
		this,
		[=](QAbstractSocket::SocketError) {
			failAttempt(socket);
		});
	QObject::connect(socket, &QWebSocket::disconnected, this, [=] {
		failAttempt(socket);
	});
	const auto request = _requestFactory();
	// QWebSocket does not read the ssl configuration off the request it is
	// opened with, it uses its own - so the factory's choice (dev endpoints
	// verify no peer, see ApiClient) has to be carried over by hand.
	const auto configuration = request.sslConfiguration();
	socket->setSslConfiguration(configuration);
	if (configuration.peerVerifyMode() == QSslSocket::VerifyNone) {
		QObject::connect(
			socket,
			&QWebSocket::sslErrors,
			this,
			[=](const QList<QSslError> &) {
				socket->ignoreSslErrors();
			});
	}
	socket->open(request);
}

void LiveUpdatesConnection::retryByTimer() {
	if (!_running) {
		return;
	}
	_retryTimeout = NextRetryTimeout(_retryTimeout);
	connectNow();
}

void LiveUpdatesConnection::failAttempt(QWebSocket *socket) {
	if (!_running || _webSocket.get() != socket || _attemptFailed) {
		return;
	}
	_attemptFailed = true;
	setStatus({
		.state = LiveUpdatesState::Waiting,
		.retryAt = crl::now() + _retryTimeout,
	});
	_retryTimer.start(_retryTimeout);
}

void LiveUpdatesConnection::socketConnected(QWebSocket *socket) {
	if (!_running || _webSocket.get() != socket) {
		return;
	}
	_attemptFailed = false;
	_retryTimer.stop();
	_retryTimeout = 1;
	setStatus({ LiveUpdatesState::Connected });
	if (_connected) {
		_connected();
	}
	sendPresence();
}

void LiveUpdatesConnection::sendPresence() {
	if (!_running
		|| !_webSocket
		|| _webSocket->state() != QAbstractSocket::ConnectedState) {
		return;
	}
	_webSocket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
		{ "type", "presence.set" },
		{ "chat_id", _presenceChatId },
		{ "active", _presenceChatId > 0 },
	}).toJson(QJsonDocument::Compact)));
}

void LiveUpdatesConnection::socketMessage(
		QWebSocket *socket,
		QString message) {
	if (!_running
		|| _webSocket.get() != socket
		|| _attemptFailed
		|| !_message) {
		return;
	}
	_message(std::move(message));
}

void LiveUpdatesConnection::destroySocket() {
	if (!_webSocket) {
		return;
	}
	// The socket is destroyed from inside its own textMessageReceived handler
	// whenever an event restarts the connection (a sequence gap, gap.detected),
	// and QWebSocket keeps reading the frames it is in the middle of after the
	// signal returns. So it has to outlive the current stack frame: its signals
	// are dropped right away and the object itself goes to the event loop.
	const auto socket = _webSocket.release();
	socket->disconnect(this);
	socket->abort();
	// Parented only now, after the unique_ptr gave up the ownership, so that a
	// deletion still pending when this connection dies happens as a child.
	socket->setParent(this);
	socket->deleteLater();
}

void LiveUpdatesConnection::setStatus(LiveUpdatesStatus status) {
	_status = status;
}

} // namespace CustomBackend
