#pragma once

#include "custom_backend/sealed_crypto.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QNetworkAccessManager;

namespace CustomBackend::Sealed {

using SealedCrypto::Bytes;
using SealedCrypto::Keys;

inline constexpr auto kContentType = "application/vnd.foxmes.sealed";

[[nodiscard]] int64_t Now();
void ClockSkew(int64_t serverTime);

struct Credential {
	enum class Kind {
		Temp,
		Session,
		Login,
	};
	Kind kind = Kind::Temp;
	std::string tid;
	int64_t sid = 0;
	std::string kid;
	std::string epk;
	Keys keys;

	[[nodiscard]] std::string header() const;
	[[nodiscard]] std::string aad() const;
};

struct Spec {
	QByteArray method;
	QNetworkRequest request;
	QByteArray type;
	QByteArray body;
};

struct Signed {
	QNetworkRequest request;
	QByteArray method;
	QByteArray body;
	qint64 plainBodySize = 0;
	Bytes nonce;
	Bytes responseKey;
};

[[nodiscard]] std::optional<Signed> Sign(
	const Spec &spec,
	const Credential &credential);

class Signer {
public:
	using Ready = std::function<void(std::optional<Credential>)>;

	virtual ~Signer() = default;

	virtual void credential(Ready ready) = 0;
	virtual void tempExpired(const std::string &tid) = 0;
	[[nodiscard]] virtual bool unauthorized() const = 0;
};

class Reply final : public QNetworkReply {
public:
	Reply(
		QNetworkAccessManager *network,
		Spec spec,
		std::weak_ptr<Signer> signer);
	Reply(QNetworkAccessManager *network, Signed request);
	~Reply();

	void abort() override;
	void ignoreSslErrors() override;
	[[nodiscard]] qint64 bytesAvailable() const override;
	[[nodiscard]] bool isSequential() const override;

	[[nodiscard]] QString rejection() const {
		return _rejection;
	}
	[[nodiscard]] int64_t serverTime() const {
		return _serverTime;
	}

protected:
	qint64 readData(char *data, qint64 maxSize) override;
	void ignoreSslErrorsImplementation(const QList<QSslError> &errors) override;

private:
	enum class Mode {
		Unknown,
		Sealed,
		Rejection,
	};
	struct Anchor;

	void initialize(const QNetworkRequest &request, const QByteArray &method);
	void requestCredential();
	void credentialReady(std::optional<Credential> credential);
	void send(Signed request);
	void innerMetaData();
	void innerReadyRead();
	void innerFinished();
	void finishRejection();
	[[nodiscard]] bool finishSignedRejection();
	void failTransport(const QString &message);
	void finishWith(NetworkError error, const QString &message, int status);
	[[nodiscard]] bool applySealedHeaders(QNetworkReply *inner);
	void deliver(const Bytes &plain);
	void complete();
	void dropInner();

	QPointer<QNetworkAccessManager> _network;
	std::optional<Spec> _spec;
	std::weak_ptr<Signer> _signer;
	std::shared_ptr<Anchor> _anchor;
	QPointer<QNetworkReply> _inner;
	std::optional<SealedCrypto::FrameDecoder> _decoder;
	Bytes _nonce;
	Bytes _responseKey;
	std::string _tid;
	QByteArray _buffer;
	QByteArray _rejectionBody;
	QByteArray _held;
	bool _hold = false;
	qint64 _plainReady = 0;
	qint64 _plainTotal = -1;
	qint64 _plainBodySize = 0;
	Mode _mode = Mode::Unknown;
	QString _rejection;
	int64_t _serverTime = 0;
	int _attempts = 0;
	bool _ignoreSsl = false;
	bool _aborted = false;
	bool _completed = false;

};

class Session final
	: public Signer
	, public std::enable_shared_from_this<Session> {
public:
	Session(
		QNetworkAccessManager *network,
		QUrl base,
		std::function<void(QNetworkRequest&)> prepare);

	void setCredentials(int64_t sid, Bytes secret);
	void clear();

	[[nodiscard]] int64_t sid() const;
	[[nodiscard]] Bytes secret() const;
	[[nodiscard]] bool authorized() const;

	void credential(Ready ready) override;
	void tempExpired(const std::string &tid) override;
	[[nodiscard]] bool unauthorized() const override;

	void prepare();
	void dropTemp();

private:
	void credentialOnMain(Ready ready);
	void bind(bool clockRetried);
	void bound(
		int generation,
		std::optional<Credential> temp,
		int64_t expires,
		bool unauthorized);

	QPointer<QNetworkAccessManager> _network;
	QUrl _base;
	std::function<void(QNetworkRequest&)> _prepare;
	mutable std::mutex _mutex;
	int64_t _sid = 0;
	Bytes _secret;
	std::optional<Credential> _temp;
	int64_t _expires = 0;
	int64_t _renewAt = 0;
	bool _binding = false;
	bool _unauthorized = false;
	int _generation = 0;
	std::vector<Ready> _waiting;

};

class FixedSigner final : public Signer {
public:
	explicit FixedSigner(Credential credential);

	void credential(Ready ready) override;
	void tempExpired(const std::string &tid) override;
	[[nodiscard]] bool unauthorized() const override;

private:
	Credential _credential;

};

struct LoginKey {
	std::string kid;
	std::string pub;
	int64_t notAfter = 0;
};

[[nodiscard]] std::optional<LoginKey> VerifyLoginDocument(
	const QJsonObject &document,
	const QString &host);
[[nodiscard]] std::optional<Credential> StartLogin(const LoginKey &key);

[[nodiscard]] QNetworkReply *Get(
	QNetworkAccessManager &network,
	const QNetworkRequest &request,
	const std::weak_ptr<Signer> &signer);

class SocketCodec final {
public:
	SocketCodec(const Keys &temp, const Bytes &nonce);

	[[nodiscard]] QByteArray seal(const QByteArray &plain);
	[[nodiscard]] std::optional<QByteArray> open(const QByteArray &message);

private:
	SealedCrypto::WsCodec _codec;

};

struct SocketRequest {
	QNetworkRequest request;
	std::shared_ptr<SocketCodec> codec;
};

[[nodiscard]] std::optional<SocketRequest> SignSocket(
	QNetworkRequest request,
	const Credential &credential);

} // namespace CustomBackend::Sealed
