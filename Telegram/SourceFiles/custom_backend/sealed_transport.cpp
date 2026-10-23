#include "custom_backend/sealed_transport.h"

#include "custom_backend/dev_profile.h"
#include "custom_backend/sealed_roots.h"

#include <crl/crl_on_main.h>

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>
#include <QtCore/QSettings>
#include <QtNetwork/QNetworkAccessManager>

#include <array>
#include <atomic>

namespace CustomBackend::Sealed {
namespace {

using namespace SealedCrypto;

constexpr auto kProtectedHeaders = std::array{
	"range",
	"x-upload-id",
	"x-chunk-index",
	"x-total-chunks",
};
constexpr auto kRenewBeforeSeconds = int64_t(3600);
constexpr auto kExpiryMarginSeconds = int64_t(60);
constexpr auto kBindTimeoutMs = 30'000;

std::atomic<int64_t> ClockOffset = 0;

[[nodiscard]] std::string Std(const QByteArray &value) {
	return std::string(value.constData(), size_t(value.size()));
}

[[nodiscard]] std::string Std(const QString &value) {
	return Std(value.toUtf8());
}

[[nodiscard]] QByteArray ToQt(const Bytes &value) {
	return QByteArray(value.data(), qsizetype(value.size()));
}

[[nodiscard]] int64_t RangeStart(const QByteArray &value) {
	if (!value.startsWith("bytes ")) {
		return 0;
	}
	const auto dash = value.indexOf('-', 6);
	return (dash > 6) ? value.mid(6, dash - 6).toLongLong() : 0;
}

[[nodiscard]] QNetworkAccessManager::Operation OperationFor(
		const QByteArray &method) {
	if (method == "GET") {
		return QNetworkAccessManager::GetOperation;
	} else if (method == "POST") {
		return QNetworkAccessManager::PostOperation;
	} else if (method == "PUT") {
		return QNetworkAccessManager::PutOperation;
	} else if (method == "DELETE") {
		return QNetworkAccessManager::DeleteOperation;
	} else if (method == "HEAD") {
		return QNetworkAccessManager::HeadOperation;
	}
	return QNetworkAccessManager::CustomOperation;
}

[[nodiscard]] QSettings Settings() {
	return QSettings(u"FoxMes"_q, u"FoxMesDesktop"_q + ProfileSuffix());
}

[[nodiscard]] QString MinSerialKey(const QString &host) {
	return u"sealed/min_serial/"_q + host.toLower();
}

} // namespace

int64_t Now() {
	return QDateTime::currentSecsSinceEpoch() + ClockOffset.load();
}

void ClockSkew(int64_t serverTime) {
	if (serverTime > 0) {
		ClockOffset = serverTime - QDateTime::currentSecsSinceEpoch();
	}
}

std::string Credential::header() const {
	switch (kind) {
	case Kind::Temp: return "tid=" + tid;
	case Kind::Session: return "sid=" + std::to_string(sid);
	case Kind::Login: return "kid=" + kid + ", epk=" + epk;
	}
	return std::string();
}

std::string Credential::aad() const {
	switch (kind) {
	case Kind::Temp: return "tid=" + tid;
	case Kind::Session: return "sid=" + std::to_string(sid);
	case Kind::Login: return "kid=" + kid + ";epk=" + epk;
	}
	return std::string();
}

std::optional<Signed> Sign(const Spec &spec, const Credential &credential) {
	if (!credential.keys.valid()) {
		return std::nullopt;
	}
	auto request = spec.request;
	const auto url = request.url();
	auto parts = RequestParts{
		.method = Std(spec.method),
		.host = Std(url.host()),
		.path = Std(url.path(QUrl::FullyEncoded)),
		.query = Std(url.query(QUrl::FullyEncoded)),
		.ts = Now(),
		.cred = credential.aad(),
		.type = Std(spec.type),
	};
	if (parts.path.empty()) {
		parts.path = "/";
	}
	auto signedNames = std::string();
	for (const auto name : kProtectedHeaders) {
		if (!request.hasRawHeader(name)) {
			continue;
		}
		parts.headers.push_back({
			.name = name,
			.value = Std(request.rawHeader(name).trimmed()),
		});
		if (!signedNames.empty()) {
			signedNames += ';';
		}
		signedNames += name;
	}
	const auto nonce = Random(kNonceSize);
	const auto tag = RequestTag(credential.keys.c2s, nonce, parts);
	if (!tag || nonce.size() != size_t(kNonceSize)) {
		return std::nullopt;
	}
	auto authorization = "FoxMes v=1, " + credential.header()
		+ ", ts=" + std::to_string(parts.ts)
		+ ", n=" + B64(nonce)
		+ ", t=" + B64(*tag);
	if (!signedNames.empty()) {
		authorization += ", h=" + signedNames;
	}
	request.setRawHeader("Authorization", ToQt(authorization));
	auto result = Signed{
		.method = spec.method,
		.plainBodySize = spec.body.size(),
		.nonce = nonce,
		.responseKey = credential.keys.s2c,
	};
	if (spec.type.isEmpty()) {
		request.setHeader(QNetworkRequest::ContentTypeHeader, QVariant());
	} else {
		const auto sealed = SealFrames(
			credential.keys.c2s,
			FrameContext{
				.nonce = nonce,
				.dir = kDirC2S,
				.type = Std(spec.type),
			},
			Std(spec.body));
		if (sealed.empty()) {
			return std::nullopt;
		}
		result.body = ToQt(sealed);
		request.setRawHeader("Content-Type", kContentType);
		request.setRawHeader("X-FoxMes-Type", spec.type);
	}
	result.request = std::move(request);
	return result;
}

struct Reply::Anchor {
	std::mutex mutex;
	Reply *reply = nullptr;

	void post(std::function<void(Reply*)> callback) {
		const auto lock = std::lock_guard(mutex);
		if (const auto target = reply) {
			QMetaObject::invokeMethod(target, [=] {
				callback(target);
			}, Qt::QueuedConnection);
		}
	}
};

Reply::Reply(
	QNetworkAccessManager *network,
	Spec spec,
	std::weak_ptr<Signer> signer)
: QNetworkReply(network)
, _network(network)
, _spec(std::move(spec))
, _signer(std::move(signer))
, _anchor(std::make_shared<Anchor>()) {
	_anchor->reply = this;
	_plainBodySize = _spec->body.size();
	initialize(_spec->request, _spec->method);
	_anchor->post([](Reply *reply) {
		reply->requestCredential();
	});
}

Reply::Reply(QNetworkAccessManager *network, Signed request)
: QNetworkReply(network)
, _network(network)
, _anchor(std::make_shared<Anchor>()) {
	_anchor->reply = this;
	_plainBodySize = request.plainBodySize;
	initialize(request.request, request.method);
	_anchor->post([request = std::move(request)](Reply *reply) {
		reply->send(request);
	});
}

Reply::~Reply() {
	{
		const auto lock = std::lock_guard(_anchor->mutex);
		_anchor->reply = nullptr;
	}
	dropInner();
}

void Reply::initialize(
		const QNetworkRequest &request,
		const QByteArray &method) {
	setRequest(request);
	setUrl(request.url());
	setOperation(OperationFor(method));
	if (operation() == QNetworkAccessManager::CustomOperation) {
		auto copy = request;
		copy.setAttribute(QNetworkRequest::CustomVerbAttribute, method);
		setRequest(copy);
	}
	open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

void Reply::requestCredential() {
	if (_aborted || _completed) {
		return;
	}
	const auto signer = _signer.lock();
	if (!signer) {
		finishWith(OperationCanceledError, u"session is closed"_q, 0);
		return;
	}
	signer->credential([anchor = _anchor](std::optional<Credential> value) {
		anchor->post([value = std::move(value)](Reply *reply) {
			reply->credentialReady(value);
		});
	});
}

void Reply::credentialReady(std::optional<Credential> credential) {
	if (_aborted || _completed) {
		return;
	}
	if (!credential) {
		const auto signer = _signer.lock();
		if (signer && signer->unauthorized()) {
			_rejection = u"unauthorized"_q;
			deliver(R"({"error":"unauthorized","code":"UNAUTHORIZED"})");
			finishWith(AuthenticationRequiredError, u"unauthorized"_q, 401);
		} else {
			finishWith(ProtocolFailure, u"sealed key is unavailable"_q, 0);
		}
		return;
	}
	_tid = (credential->kind == Credential::Kind::Temp)
		? credential->tid
		: std::string();
	const auto request = Sign(*_spec, *credential);
	if (!request) {
		finishWith(ProtocolFailure, u"cannot seal the request"_q, 0);
		return;
	}
	send(*request);
}

void Reply::send(Signed request) {
	if (_aborted || _completed) {
		return;
	}
	if (!_network) {
		finishWith(OperationCanceledError, u"network is gone"_q, 0);
		return;
	}
	_nonce = request.nonce;
	_responseKey = request.responseKey;
	_mode = Mode::Unknown;
	_decoder.reset();
	_rejectionBody.clear();
	const auto &method = request.method;
	auto inner = (QNetworkReply*)nullptr;
	if (method == "GET") {
		inner = _network->get(request.request);
	} else if (method == "POST") {
		inner = _network->post(request.request, request.body);
	} else if (method == "PUT") {
		inner = _network->put(request.request, request.body);
	} else if (method == "DELETE" && request.body.isEmpty()) {
		inner = _network->deleteResource(request.request);
	} else {
		inner = _network->sendCustomRequest(
			request.request,
			method,
			request.body);
	}
	_inner = inner;
	if (_ignoreSsl) {
		inner->ignoreSslErrors();
	}
	QObject::connect(inner, &QNetworkReply::metaDataChanged, this, [=] {
		if (_inner == inner) innerMetaData();
	});
	QObject::connect(inner, &QNetworkReply::readyRead, this, [=] {
		if (_inner == inner) innerReadyRead();
	});
	QObject::connect(inner, &QNetworkReply::finished, this, [=] {
		if (_inner == inner) innerFinished();
	});
	QObject::connect(inner, &QNetworkReply::sslErrors, this, [=](
			const QList<QSslError> &errors) {
		if (_inner == inner) {
			Q_EMIT sslErrors(errors);
		}
	});
	QObject::connect(inner, &QNetworkReply::uploadProgress, this, [=](
			qint64 sent,
			qint64 total) {
		if (_inner != inner) {
			return;
		}
		const auto plain = (total > 0)
			? (sent * _plainBodySize / total)
			: sent;
		Q_EMIT uploadProgress(plain, _plainBodySize);
	});
}

bool Reply::applySealedHeaders(QNetworkReply *inner) {
	const auto block = OpenHeaderBlock(
		_responseKey,
		_nonce,
		Std(inner->rawHeader("X-FoxMes-Headers")));
	if (!block) {
		return false;
	}
	for (const auto &line : *block) {
		const auto name = QByteArray::fromStdString(line.name);
		const auto value = QByteArray::fromStdString(line.value);
		const auto existing = rawHeader(name);
		setRawHeader(name, existing.isEmpty() ? value : (existing + ", " + value));
	}
	const auto lane = inner->rawHeader("x-fxl-lane");
	if (!lane.isEmpty()) {
		setRawHeader("x-fxl-lane", lane);
	}
	return true;
}

void Reply::failTransport(const QString &message) {
	dropInner();
	finishWith(ProtocolFailure, message, 0);
}

void Reply::innerMetaData() {
	const auto inner = _inner.data();
	if (!inner || _mode != Mode::Unknown) {
		return;
	}
	const auto status = inner->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status <= 0) {
		return;
	}
	if (inner->rawHeader("Content-Type").startsWith(kContentType)) {
		_mode = Mode::Sealed;
		const auto real = inner->rawHeader("X-FoxMes-Status").toInt();
		const auto type = inner->rawHeader("X-FoxMes-Type");
		const auto range = inner->rawHeader("X-FoxMes-Content-Range");
		if (real <= 0 || !applySealedHeaders(inner)) {
			failTransport(u"sealed response is damaged"_q);
			return;
		}
		_decoder.emplace(_responseKey, FrameContext{
			.nonce = _nonce,
			.dir = kDirS2C,
			.status = real,
			.type = Std(type),
			.range = Std(range),
			.offset = RangeStart(range),
		});
		_hold = (real == 401);
		if (!type.isEmpty()) {
			setRawHeader("Content-Type", type);
		}
		if (!range.isEmpty()) {
			setRawHeader("Content-Range", range);
		}
		const auto length = inner->header(
			QNetworkRequest::ContentLengthHeader);
		if (length.isValid()) {
			if (const auto plain = PlainLength(length.toLongLong())) {
				_plainTotal = *plain;
				setRawHeader("Content-Length", QByteArray::number(*plain));
				setAttribute(
					QNetworkRequest::OriginalContentLengthAttribute,
					qint64(*plain));
			}
		}
		setAttribute(
			QNetworkRequest::HttpReasonPhraseAttribute,
			inner->attribute(QNetworkRequest::HttpReasonPhraseAttribute));
		setAttribute(QNetworkRequest::HttpStatusCodeAttribute, real);
		if (!_hold) {
			Q_EMIT metaDataChanged();
		}
	} else if (status == 401) {
		_mode = Mode::Rejection;
	} else {
		failTransport(u"response is not sealed"_q);
	}
}

void Reply::innerReadyRead() {
	const auto inner = _inner.data();
	if (!inner) {
		return;
	}
	innerMetaData();
	if (_completed || !_inner) {
		return;
	}
	const auto bytes = inner->readAll();
	if (bytes.isEmpty()) {
		return;
	}
	switch (_mode) {
	case Mode::Sealed: {
		auto plain = Bytes();
		if (!_decoder->feed(bytes.constData(), bytes.size(), plain)) {
			failTransport(u"sealed response is damaged"_q);
			return;
		}
		deliver(plain);
	} break;
	case Mode::Rejection:
		_rejectionBody.append(bytes);
		break;
	case Mode::Unknown:
		failTransport(u"response is not sealed"_q);
		break;
	}
}

void Reply::deliver(const Bytes &plain) {
	if (plain.empty()) {
		return;
	}
	if (_hold) {
		_held.append(plain.data(), qsizetype(plain.size()));
		return;
	}
	_buffer.append(plain.data(), qsizetype(plain.size()));
	_plainReady += qint64(plain.size());
	Q_EMIT readyRead();
	Q_EMIT downloadProgress(_plainReady, _plainTotal);
}

void Reply::innerFinished() {
	const auto inner = _inner.data();
	if (!inner || _completed) {
		return;
	}
	innerReadyRead();
	if (_completed || !_inner) {
		return;
	}
	if (_mode == Mode::Rejection) {
		finishRejection();
		return;
	}
	const auto error = inner->error();
	const auto errorString = inner->errorString();
	if (_mode != Mode::Sealed) {
		dropInner();
		finishWith(
			(error != NoError) ? error : ProtocolFailure,
			(error != NoError) ? errorString : u"response is not sealed"_q,
			0);
		return;
	}
	auto plain = Bytes();
	if (!_decoder->finish(plain)) {
		dropInner();
		if (error != NoError) {
			finishWith(error, errorString, 0);
		} else {
			finishWith(ProtocolFailure, u"sealed response is damaged"_q, 0);
		}
		return;
	}
	deliver(plain);
	if (_hold && finishSignedRejection()) {
		return;
	}
	dropInner();
	if (error != NoError) {
		setError(error, errorString);
		Q_EMIT errorOccurred(error);
	}
	complete();
}

bool Reply::finishSignedRejection() {
	const auto object = QJsonDocument::fromJson(_held).object();
	const auto code = object.value(u"error"_q).toString();
	_hold = false;
	if (code == u"sealed_clock"_q || code == u"sealed_replay"_q) {
		_rejection = code;
		_serverTime = object.value(u"time"_q).toVariant().toLongLong();
		const auto signer = _signer.lock();
		if (code == u"sealed_clock"_q
			&& _serverTime > 0
			&& _spec
			&& signer
			&& _attempts == 0) {
			++_attempts;
			dropInner();
			ClockSkew(_serverTime);
			requestCredential();
			return true;
		}
		if (code == u"sealed_clock"_q && _serverTime > 0) {
			ClockSkew(_serverTime);
		}
		dropInner();
		finishWith(ProtocolFailure, code, 0);
		return true;
	}
	if (code == u"unauthorized"_q) {
		_rejection = code;
	}
	Q_EMIT metaDataChanged();
	const auto held = std::exchange(_held, {});
	deliver(Bytes(held.constData(), size_t(held.size())));
	return false;
}

void Reply::finishRejection() {
	const auto document = QJsonDocument::fromJson(_rejectionBody);
	const auto code = document.object().value(u"error"_q).toString();
	const auto signer = _signer.lock();
	if (code == u"sealed_temp_expired"_q && _spec && signer && _attempts == 0) {
		++_attempts;
		dropInner();
		signer->tempExpired(_tid);
		requestCredential();
		return;
	}
	const auto known = (code == u"sealed_invalid"_q)
		|| (code == u"sealed_temp_expired"_q)
		|| (code == u"unauthorized"_q);
	_rejection = known ? code : QString();
	dropInner();
	finishWith(
		ProtocolFailure,
		known ? code : u"response is not sealed"_q,
		0);
}

void Reply::finishWith(NetworkError error, const QString &message, int status) {
	if (_completed) {
		return;
	}
	setAttribute(
		QNetworkRequest::HttpStatusCodeAttribute,
		status ? QVariant(status) : QVariant());
	setError(error, message);
	Q_EMIT errorOccurred(error);
	complete();
}

void Reply::complete() {
	if (_completed) {
		return;
	}
	_completed = true;
	setFinished(true);
	Q_EMIT finished();
}

void Reply::dropInner() {
	if (const auto inner = std::exchange(_inner, nullptr).data()) {
		inner->disconnect(this);
		if (inner->isRunning()) {
			inner->abort();
		}
		inner->deleteLater();
	}
}

void Reply::abort() {
	if (_completed) {
		return;
	}
	_aborted = true;
	if (const auto inner = _inner.data()) {
		inner->abort();
	}
	if (!_completed) {
		dropInner();
		finishWith(OperationCanceledError, u"Operation canceled"_q, 0);
	}
}

void Reply::ignoreSslErrors() {
	_ignoreSsl = true;
	if (const auto inner = _inner.data()) {
		inner->ignoreSslErrors();
	}
}

void Reply::ignoreSslErrorsImplementation(const QList<QSslError> &) {
	ignoreSslErrors();
}

qint64 Reply::bytesAvailable() const {
	return _buffer.size() + QIODevice::bytesAvailable();
}

bool Reply::isSequential() const {
	return true;
}

qint64 Reply::readData(char *data, qint64 maxSize) {
	if (_buffer.isEmpty()) {
		return _completed ? -1 : 0;
	}
	const auto count = std::min(maxSize, qint64(_buffer.size()));
	memcpy(data, _buffer.constData(), size_t(count));
	_buffer.remove(0, count);
	return count;
}

Session::Session(
	QNetworkAccessManager *network,
	QUrl base,
	std::function<void(QNetworkRequest&)> prepare)
: _network(network)
, _base(std::move(base))
, _prepare(std::move(prepare)) {
}

void Session::setCredentials(int64_t sid, Bytes secret) {
	auto waiting = std::vector<Ready>();
	{
		const auto lock = std::lock_guard(_mutex);
		_sid = sid;
		_secret = std::move(secret);
		_temp.reset();
		_expires = 0;
		_renewAt = 0;
		_unauthorized = false;
		_binding = false;
		++_generation;
		waiting = std::exchange(_waiting, {});
	}
	for (auto &ready : waiting) {
		ready(std::nullopt);
	}
}

void Session::clear() {
	setCredentials(0, Bytes());
}

int64_t Session::sid() const {
	const auto lock = std::lock_guard(_mutex);
	return _sid;
}

Bytes Session::secret() const {
	const auto lock = std::lock_guard(_mutex);
	return _secret;
}

bool Session::authorized() const {
	const auto lock = std::lock_guard(_mutex);
	return (_sid > 0) && (_secret.size() == size_t(kKeySize));
}

bool Session::unauthorized() const {
	const auto lock = std::lock_guard(_mutex);
	return _unauthorized || (_sid <= 0);
}

void Session::credential(Ready ready) {
	crl::on_main([weak = weak_from_this(), ready = std::move(ready)]() mutable {
		if (const auto strong = weak.lock()) {
			strong->credentialOnMain(std::move(ready));
		} else {
			ready(std::nullopt);
		}
	});
}

void Session::prepare() {
	credential([](std::optional<Credential>) {});
}

void Session::dropTemp() {
	const auto lock = std::lock_guard(_mutex);
	_temp.reset();
	_expires = 0;
}

void Session::credentialOnMain(Ready ready) {
	auto immediate = std::optional<Credential>();
	auto startBind = false;
	auto answer = false;
	{
		const auto lock = std::lock_guard(_mutex);
		if (_sid <= 0 || _secret.size() != size_t(kKeySize)) {
			answer = true;
		} else {
			const auto now = Now();
			if (_temp && now < _expires - kExpiryMarginSeconds) {
				immediate = _temp;
				answer = true;
				startBind = !_binding && (now >= _renewAt);
			} else {
				_waiting.push_back(std::move(ready));
				startBind = !_binding;
			}
			if (startBind) {
				_binding = true;
			}
		}
	}
	if (answer) {
		ready(immediate);
	}
	if (startBind) {
		bind(false);
	}
}

void Session::tempExpired(const std::string &tid) {
	crl::on_main([weak = weak_from_this(), tid] {
		if (const auto strong = weak.lock()) {
			const auto lock = std::lock_guard(strong->_mutex);
			if (strong->_temp && strong->_temp->tid == tid) {
				strong->_temp.reset();
				strong->_expires = 0;
			}
		}
	});
}

void Session::bind(bool clockRetried) {
	auto sid = int64_t();
	auto secret = Bytes();
	auto generation = 0;
	{
		const auto lock = std::lock_guard(_mutex);
		sid = _sid;
		secret = _secret;
		generation = _generation;
	}
	if (!_network || sid <= 0) {
		bound(generation, std::nullopt, 0, false);
		return;
	}
	auto ephemeral = std::make_shared<EcdhKey>(EcdhKey::Generate());
	const auto clientPublic = B64(ephemeral->publicKey());
	auto request = QNetworkRequest(QUrl(
		_base.toString(QUrl::StripTrailingSlash) + u"/auth/temp-key"_q));
	request.setTransferTimeout(kBindTimeoutMs);
	request.setRawHeader("Accept", "application/json");
	if (_prepare) {
		_prepare(request);
	}
	const auto spec = Spec{
		.method = "POST",
		.request = request,
		.type = "application/json",
		.body = QJsonDocument(QJsonObject{
			{ u"pub"_q, QString::fromStdString(clientPublic) },
		}).toJson(QJsonDocument::Compact),
	};
	const auto sealed = Sign(spec, Credential{
		.kind = Credential::Kind::Session,
		.sid = sid,
		.keys = SessionKeys(secret),
	});
	if (!sealed) {
		bound(generation, std::nullopt, 0, false);
		return;
	}
	const auto reply = new Reply(_network, *sealed);
	const auto weak = weak_from_this();
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto body = reply->readAll();
		const auto rejection = reply->rejection();
		const auto serverTime = reply->serverTime();
		reply->deleteLater();
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		if (rejection == u"sealed_clock"_q && serverTime > 0 && !clockRetried) {
			ClockSkew(serverTime);
			strong->bind(true);
			return;
		}
		if (status != 200) {
			strong->bound(generation, std::nullopt, 0, status == 401);
			return;
		}
		const auto object = QJsonDocument::fromJson(body).object();
		const auto tid = Std(object.value(u"tid"_q).toString());
		const auto serverPublic = Std(object.value(u"pub"_q).toString());
		const auto expires = object.value(u"expires_at"_q).toVariant().toLongLong();
		const auto peer = UnB64(serverPublic);
		const auto shared = peer ? ephemeral->derive(*peer) : std::nullopt;
		if (tid.empty() || !shared || expires <= 0) {
			strong->bound(generation, std::nullopt, 0, false);
			return;
		}
		const auto temp = TempSecret(
			*shared,
			BindSalt(secret),
			sid,
			tid,
			clientPublic,
			serverPublic);
		strong->bound(generation, Credential{
			.kind = Credential::Kind::Temp,
			.tid = tid,
			.keys = TempKeys(temp),
		}, expires, false);
	});
}

void Session::bound(
		int generation,
		std::optional<Credential> temp,
		int64_t expires,
		bool unauthorized) {
	auto waiting = std::vector<Ready>();
	auto result = std::optional<Credential>();
	{
		const auto lock = std::lock_guard(_mutex);
		if (generation != _generation) {
			return;
		}
		_binding = false;
		if (temp) {
			_temp = std::move(temp);
			_expires = expires;
			_renewAt = expires - std::min(
				kRenewBeforeSeconds,
				std::max(expires - Now(), int64_t(0)) / 4);
		}
		_unauthorized = unauthorized;
		if (_temp && Now() < _expires - kExpiryMarginSeconds) {
			result = _temp;
		}
		waiting = std::exchange(_waiting, {});
	}
	for (auto &ready : waiting) {
		ready(result);
	}
}

FixedSigner::FixedSigner(Credential credential)
: _credential(std::move(credential)) {
}

void FixedSigner::credential(Ready ready) {
	ready(_credential);
}

void FixedSigner::tempExpired(const std::string &) {
}

bool FixedSigner::unauthorized() const {
	return false;
}

std::optional<LoginKey> VerifyLoginDocument(
		const QJsonObject &document,
		const QString &host) {
	auto roots = std::vector<RootKey>(
		kReleaseRoots.begin(),
		kReleaseRoots.end());
#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
	roots.push_back(kStageRoot);
#endif // FOXMES_ALLOW_ENDPOINT_OVERRIDE
	const auto rootKey = [&](const QString &name) -> std::optional<Bytes> {
		for (const auto &root : roots) {
			if (name.toStdString() == root.name) {
				return UnB64(std::string(root.pub));
			}
		}
		return std::nullopt;
	};
	const auto now = Now();
	const auto intermediate = document.value(u"intermediate"_q).toObject();
	const auto serial = intermediate.value(u"serial"_q).toVariant().toLongLong();
	const auto intermediatePub = Std(intermediate.value(u"pub"_q).toString());
	const auto notAfter = intermediate.value(u"not_after"_q).toVariant().toLongLong();
	const auto sig = UnB64(Std(intermediate.value(u"sig"_q).toString()));
	const auto root = rootKey(intermediate.value(u"root"_q).toString());
	if (!root
		|| !sig
		|| notAfter <= now
		|| !EcdsaVerify(
			*root,
			IntermediateMessage(serial, intermediatePub, notAfter),
			*sig)) {
		return std::nullopt;
	}
	const auto revocation = document.value(u"revocation"_q).toObject();
	const auto minSerial = revocation.value(u"min_serial"_q).toVariant().toLongLong();
	const auto issuedAt = revocation.value(u"issued_at"_q).toVariant().toLongLong();
	const auto revocationSig = UnB64(Std(revocation.value(u"sig"_q).toString()));
	const auto revocationRoot = rootKey(revocation.value(u"root"_q).toString());
	if (!revocationRoot
		|| !revocationSig
		|| !EcdsaVerify(
			*revocationRoot,
			RevocationMessage(minSerial, issuedAt),
			*revocationSig)) {
		return std::nullopt;
	}
	auto settings = Settings();
	const auto key = MinSerialKey(host);
	const auto remembered = std::max<int64_t>(
		settings.value(key).toLongLong(),
		kMinimalSerial);
	const auto required = std::max<int64_t>(remembered, minSerial);
	if (serial < required) {
		return std::nullopt;
	}
	if (minSerial > remembered) {
		settings.setValue(key, qint64(minSerial));
	}
	const auto signer = UnB64(intermediatePub);
	if (!signer) {
		return std::nullopt;
	}
	auto best = std::optional<LoginKey>();
	for (const auto &value : document.value(u"keys"_q).toArray()) {
		const auto object = value.toObject();
		const auto kid = Std(object.value(u"kid"_q).toString());
		const auto pub = Std(object.value(u"pub"_q).toString());
		const auto expires = object.value(u"not_after"_q).toVariant().toLongLong();
		const auto keySig = UnB64(Std(object.value(u"sig"_q).toString()));
		const auto raw = UnB64(pub);
		if (kid.empty()
			|| !keySig
			|| !raw
			|| raw->size() != size_t(kPublicKeySize)
			|| expires <= now
			|| !EcdsaVerify(*signer, LoginKeyMessage(kid, pub, expires), *keySig)) {
			continue;
		}
		if (!best || expires > best->notAfter) {
			best = LoginKey{ .kid = kid, .pub = pub, .notAfter = expires };
		}
	}
	return best;
}

std::optional<Credential> StartLogin(const LoginKey &key) {
	const auto ephemeral = EcdhKey::Generate();
	const auto peer = UnB64(key.pub);
	const auto shared = peer ? ephemeral.derive(*peer) : std::nullopt;
	if (!shared) {
		return std::nullopt;
	}
	const auto epk = B64(ephemeral.publicKey());
	return Credential{
		.kind = Credential::Kind::Login,
		.kid = key.kid,
		.epk = epk,
		.keys = LoginKeys(*shared, key.kid, epk),
	};
}

QNetworkReply *Get(
		QNetworkAccessManager &network,
		const QNetworkRequest &request,
		const std::weak_ptr<Signer> &signer) {
	if (signer.expired()) {
		return network.get(request);
	}
	return new Reply(&network, Spec{
		.method = "GET",
		.request = request,
	}, signer);
}

SocketCodec::SocketCodec(const Keys &temp, const Bytes &nonce)
: _codec(temp, nonce) {
}

QByteArray SocketCodec::seal(const QByteArray &plain) {
	return ToQt(_codec.seal(Std(plain)));
}

std::optional<QByteArray> SocketCodec::open(const QByteArray &message) {
	const auto result = _codec.open(Std(message));
	return result ? std::make_optional(ToQt(*result)) : std::nullopt;
}

std::optional<SocketRequest> SignSocket(
		QNetworkRequest request,
		const Credential &credential) {
	const auto sealed = Sign(Spec{
		.method = "GET",
		.request = std::move(request),
	}, credential);
	if (!sealed) {
		return std::nullopt;
	}
	return SocketRequest{
		.request = sealed->request,
		.codec = std::make_shared<SocketCodec>(credential.keys, sealed->nonce),
	};
}

} // namespace CustomBackend::Sealed
