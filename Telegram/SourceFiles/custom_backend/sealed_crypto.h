#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

typedef struct evp_pkey_st EVP_PKEY;

namespace CustomBackend::SealedCrypto {

using Bytes = std::string;

inline constexpr auto kKeySize = 32;
inline constexpr auto kNonceSize = 12;
inline constexpr auto kTagSize = 16;
inline constexpr auto kPublicKeySize = 65;
inline constexpr auto kFrameSize = 65536;
inline constexpr auto kFrameOverhead = kNonceSize + kTagSize;

inline constexpr auto kDirC2S = "c2s";
inline constexpr auto kDirS2C = "s2c";

[[nodiscard]] std::string B64(const Bytes &data);
[[nodiscard]] std::optional<Bytes> UnB64(const std::string &value);
[[nodiscard]] Bytes Random(int size);
[[nodiscard]] Bytes Sha256(const Bytes &data);
[[nodiscard]] Bytes Hkdf(
	const Bytes &ikm,
	const Bytes &salt,
	const std::string &info);
[[nodiscard]] std::optional<Bytes> GcmSeal(
	const Bytes &key,
	const Bytes &nonce,
	const Bytes &plain,
	const Bytes &aad);
[[nodiscard]] std::optional<Bytes> GcmOpen(
	const Bytes &key,
	const Bytes &nonce,
	const Bytes &sealed,
	const Bytes &aad);
[[nodiscard]] bool EcdsaVerify(
	const Bytes &publicKey,
	const Bytes &message,
	const Bytes &signature);

class EcdhKey final {
public:
	EcdhKey();
	EcdhKey(EcdhKey &&other) noexcept;
	EcdhKey &operator=(EcdhKey &&other) noexcept;
	~EcdhKey();

	[[nodiscard]] static EcdhKey Generate();
	[[nodiscard]] static EcdhKey FromScalar(const Bytes &scalar);

	[[nodiscard]] bool valid() const;
	[[nodiscard]] Bytes publicKey() const;
	[[nodiscard]] std::optional<Bytes> derive(const Bytes &peerPublic) const;

private:
	explicit EcdhKey(EVP_PKEY *key);

	EVP_PKEY *_key = nullptr;

};

struct Keys {
	Bytes c2s;
	Bytes s2c;

	[[nodiscard]] bool valid() const {
		return (c2s.size() == kKeySize) && (s2c.size() == kKeySize);
	}
};

[[nodiscard]] Keys LoginKeys(
	const Bytes &shared,
	const std::string &kid,
	const std::string &epk);
[[nodiscard]] Keys SessionKeys(const Bytes &secret);
[[nodiscard]] Bytes BindSalt(const Bytes &secret);
[[nodiscard]] Bytes TempSecret(
	const Bytes &shared,
	const Bytes &bindSalt,
	int64_t sid,
	const std::string &tid,
	const std::string &clientPublic,
	const std::string &serverPublic);
[[nodiscard]] Keys TempKeys(const Bytes &temp);
[[nodiscard]] Keys WsKeys(const Keys &keys, const Bytes &nonce);

struct FrameContext {
	Bytes nonce;
	std::string dir;
	int status = 0;
	std::string type;
	std::string range;
	int64_t offset = 0;
};

[[nodiscard]] std::string FrameAad(
	const FrameContext &context,
	int64_t offset,
	bool last);
[[nodiscard]] int64_t SealedLength(int64_t plain);
[[nodiscard]] std::optional<int64_t> PlainLength(int64_t sealed);
[[nodiscard]] Bytes SealFrames(
	const Bytes &key,
	const FrameContext &context,
	const Bytes &plain,
	const std::vector<Bytes> &nonces = {});

struct HeaderLine {
	std::string name;
	std::string value;
};

[[nodiscard]] std::string HeaderBlockAad(const Bytes &requestNonce);
[[nodiscard]] std::optional<std::vector<HeaderLine>> OpenHeaderBlock(
	const Bytes &key,
	const Bytes &requestNonce,
	const std::string &value);

class FrameDecoder final {
public:
	FrameDecoder(Bytes key, FrameContext context);

	[[nodiscard]] bool feed(const char *data, int64_t size, Bytes &plain);
	[[nodiscard]] bool finish(Bytes &plain);
	[[nodiscard]] int64_t plainOffset() const {
		return _offset;
	}

private:
	[[nodiscard]] bool open(int64_t size, bool last, Bytes &plain);

	Bytes _key;
	FrameContext _context;
	Bytes _buffer;
	int64_t _offset = 0;
	bool _failed = false;
	bool _finished = false;

};

struct SignedHeader {
	std::string name;
	std::string value;
};

struct RequestParts {
	std::string method;
	std::string host;
	std::string path;
	std::string query;
	int64_t ts = 0;
	std::string cred;
	std::string type;
	std::vector<SignedHeader> headers;
};

[[nodiscard]] std::string RequestAad(const RequestParts &parts);
[[nodiscard]] std::optional<Bytes> RequestTag(
	const Bytes &key,
	const Bytes &nonce,
	const RequestParts &parts);

class WsCodec final {
public:
	WsCodec(const Keys &temp, const Bytes &nonce);

	[[nodiscard]] Bytes seal(const Bytes &plain);
	[[nodiscard]] std::optional<Bytes> open(const Bytes &message);

private:
	Keys _keys;
	uint64_t _sent = 0;
	uint64_t _received = 0;

};

[[nodiscard]] std::string IntermediateMessage(
	int64_t serial,
	const std::string &pub,
	int64_t notAfter);
[[nodiscard]] std::string LoginKeyMessage(
	const std::string &kid,
	const std::string &pub,
	int64_t notAfter);
[[nodiscard]] std::string RevocationMessage(
	int64_t minSerial,
	int64_t issuedAt);

} // namespace CustomBackend::SealedCrypto
