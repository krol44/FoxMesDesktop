#include "custom_backend/sealed_crypto.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>

namespace CustomBackend::SealedCrypto {
namespace {

constexpr auto kSealedFrame = kFrameSize + kFrameOverhead;
constexpr auto kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

struct PkeyDeleter {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};
struct PkeyCtxDeleter {
	void operator()(EVP_PKEY_CTX *context) const {
		EVP_PKEY_CTX_free(context);
	}
};
struct CipherCtxDeleter {
	void operator()(EVP_CIPHER_CTX *context) const {
		EVP_CIPHER_CTX_free(context);
	}
};
struct MdCtxDeleter {
	void operator()(EVP_MD_CTX *context) const {
		EVP_MD_CTX_free(context);
	}
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

[[nodiscard]] const unsigned char *U(const Bytes &data) {
	return reinterpret_cast<const unsigned char*>(data.data());
}

[[nodiscard]] PkeyPtr PublicFromOctets(const Bytes &octets) {
	if (octets.size() != kPublicKeySize || octets[0] != 0x04) {
		return nullptr;
	}
	auto group = std::string("prime256v1");
	auto copy = octets;
	OSSL_PARAM params[] = {
		OSSL_PARAM_construct_utf8_string(
			OSSL_PKEY_PARAM_GROUP_NAME,
			group.data(),
			0),
		OSSL_PARAM_construct_octet_string(
			OSSL_PKEY_PARAM_PUB_KEY,
			copy.data(),
			copy.size()),
		OSSL_PARAM_construct_end(),
	};
	const auto context = PkeyCtxPtr(
		EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
	EVP_PKEY *result = nullptr;
	if (!context
		|| EVP_PKEY_fromdata_init(context.get()) != 1
		|| EVP_PKEY_fromdata(
			context.get(),
			&result,
			EVP_PKEY_PUBLIC_KEY,
			params) != 1) {
		return nullptr;
	}
	auto key = PkeyPtr(result);
	const auto check = PkeyCtxPtr(
		EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr));
	if (!check || EVP_PKEY_public_check(check.get()) != 1) {
		return nullptr;
	}
	return key;
}

[[nodiscard]] Bytes Counter(uint64_t value) {
	auto result = Bytes(kNonceSize, '\0');
	for (auto i = 0; i != 8; ++i) {
		result[kNonceSize - 1 - i] = char((value >> (8 * i)) & 0xFF);
	}
	return result;
}

} // namespace

std::string B64(const Bytes &data) {
	auto result = std::string();
	result.reserve((data.size() * 4 + 2) / 3);
	auto i = size_t(0);
	for (; i + 2 < data.size(); i += 3) {
		const auto chunk = (uint32_t(uint8_t(data[i])) << 16)
			| (uint32_t(uint8_t(data[i + 1])) << 8)
			| uint32_t(uint8_t(data[i + 2]));
		result.push_back(kAlphabet[(chunk >> 18) & 63]);
		result.push_back(kAlphabet[(chunk >> 12) & 63]);
		result.push_back(kAlphabet[(chunk >> 6) & 63]);
		result.push_back(kAlphabet[chunk & 63]);
	}
	if (const auto rest = data.size() - i; rest == 1) {
		const auto chunk = uint32_t(uint8_t(data[i])) << 16;
		result.push_back(kAlphabet[(chunk >> 18) & 63]);
		result.push_back(kAlphabet[(chunk >> 12) & 63]);
	} else if (rest == 2) {
		const auto chunk = (uint32_t(uint8_t(data[i])) << 16)
			| (uint32_t(uint8_t(data[i + 1])) << 8);
		result.push_back(kAlphabet[(chunk >> 18) & 63]);
		result.push_back(kAlphabet[(chunk >> 12) & 63]);
		result.push_back(kAlphabet[(chunk >> 6) & 63]);
	}
	return result;
}

std::optional<Bytes> UnB64(const std::string &value) {
	if (value.size() % 4 == 1) {
		return std::nullopt;
	}
	const auto decode = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '-') return 62;
		if (c == '_') return 63;
		return -1;
	};
	auto result = Bytes();
	result.reserve(value.size() * 3 / 4);
	auto buffer = uint32_t(0);
	auto bits = 0;
	for (const auto c : value) {
		const auto v = decode(c);
		if (v < 0) {
			return std::nullopt;
		}
		buffer = (buffer << 6) | uint32_t(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			result.push_back(char((buffer >> bits) & 0xFF));
		}
	}
	if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
		return std::nullopt;
	}
	return result;
}

Bytes Random(int size) {
	auto result = Bytes(size, '\0');
	if (RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), size) != 1) {
		return Bytes();
	}
	return result;
}

Bytes Sha256(const Bytes &data) {
	auto result = Bytes(SHA256_DIGEST_LENGTH, '\0');
	SHA256(
		U(data),
		data.size(),
		reinterpret_cast<unsigned char*>(result.data()));
	return result;
}

Bytes Hkdf(const Bytes &ikm, const Bytes &salt, const std::string &info) {
	const auto kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
	if (!kdf) {
		return Bytes();
	}
	const auto context = EVP_KDF_CTX_new(kdf);
	EVP_KDF_free(kdf);
	if (!context) {
		return Bytes();
	}
	auto digest = std::string("SHA256");
	auto ikmCopy = ikm;
	auto saltCopy = salt;
	auto infoCopy = info;
	OSSL_PARAM params[5];
	auto count = 0;
	params[count++] = OSSL_PARAM_construct_utf8_string(
		OSSL_KDF_PARAM_DIGEST,
		digest.data(),
		0);
	params[count++] = OSSL_PARAM_construct_octet_string(
		OSSL_KDF_PARAM_KEY,
		ikmCopy.data(),
		ikmCopy.size());
	if (!saltCopy.empty()) {
		params[count++] = OSSL_PARAM_construct_octet_string(
			OSSL_KDF_PARAM_SALT,
			saltCopy.data(),
			saltCopy.size());
	}
	params[count++] = OSSL_PARAM_construct_octet_string(
		OSSL_KDF_PARAM_INFO,
		infoCopy.data(),
		infoCopy.size());
	params[count] = OSSL_PARAM_construct_end();
	auto result = Bytes(kKeySize, '\0');
	const auto ok = EVP_KDF_derive(
		context,
		reinterpret_cast<unsigned char*>(result.data()),
		result.size(),
		params);
	EVP_KDF_CTX_free(context);
	return (ok == 1) ? result : Bytes();
}

std::optional<Bytes> GcmSeal(
		const Bytes &key,
		const Bytes &nonce,
		const Bytes &plain,
		const Bytes &aad) {
	if (key.size() != kKeySize || nonce.size() != kNonceSize) {
		return std::nullopt;
	}
	const auto context = CipherCtxPtr(EVP_CIPHER_CTX_new());
	auto result = Bytes(plain.size() + kTagSize, '\0');
	auto out = reinterpret_cast<unsigned char*>(result.data());
	auto length = 0;
	if (!context
		|| EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, U(key), U(nonce)) != 1
		|| (!aad.empty()
			&& EVP_EncryptUpdate(context.get(), nullptr, &length, U(aad), int(aad.size())) != 1)
		|| (!plain.empty()
			&& EVP_EncryptUpdate(context.get(), out, &length, U(plain), int(plain.size())) != 1)
		|| EVP_EncryptFinal_ex(context.get(), out + plain.size(), &length) != 1
		|| EVP_CIPHER_CTX_ctrl(
			context.get(),
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			out + plain.size()) != 1) {
		return std::nullopt;
	}
	return result;
}

std::optional<Bytes> GcmOpen(
		const Bytes &key,
		const Bytes &nonce,
		const Bytes &sealed,
		const Bytes &aad) {
	if (key.size() != kKeySize
		|| nonce.size() != kNonceSize
		|| sealed.size() < size_t(kTagSize)) {
		return std::nullopt;
	}
	const auto size = sealed.size() - kTagSize;
	auto tag = sealed.substr(size);
	const auto context = CipherCtxPtr(EVP_CIPHER_CTX_new());
	auto result = Bytes(size, '\0');
	auto out = reinterpret_cast<unsigned char*>(result.data());
	auto length = 0;
	if (!context
		|| EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, U(key), U(nonce)) != 1
		|| (!aad.empty()
			&& EVP_DecryptUpdate(context.get(), nullptr, &length, U(aad), int(aad.size())) != 1)
		|| (size > 0
			&& EVP_DecryptUpdate(context.get(), out, &length, U(sealed), int(size)) != 1)
		|| EVP_CIPHER_CTX_ctrl(
			context.get(),
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			tag.data()) != 1
		|| EVP_DecryptFinal_ex(context.get(), out + size, &length) != 1) {
		return std::nullopt;
	}
	return result;
}

bool EcdsaVerify(
		const Bytes &publicKey,
		const Bytes &message,
		const Bytes &signature) {
	const auto key = PublicFromOctets(publicKey);
	const auto context = MdCtxPtr(EVP_MD_CTX_new());
	return key
		&& context
		&& EVP_DigestVerifyInit_ex(
			context.get(),
			nullptr,
			"SHA256",
			nullptr,
			nullptr,
			key.get(),
			nullptr) == 1
		&& EVP_DigestVerify(
			context.get(),
			U(signature),
			signature.size(),
			U(message),
			message.size()) == 1;
}

EcdhKey::EcdhKey() = default;

EcdhKey::EcdhKey(EVP_PKEY *key) : _key(key) {
}

EcdhKey::EcdhKey(EcdhKey &&other) noexcept
: _key(std::exchange(other._key, nullptr)) {
}

EcdhKey &EcdhKey::operator=(EcdhKey &&other) noexcept {
	if (this != &other) {
		EVP_PKEY_free(_key);
		_key = std::exchange(other._key, nullptr);
	}
	return *this;
}

EcdhKey::~EcdhKey() {
	EVP_PKEY_free(_key);
}

EcdhKey EcdhKey::Generate() {
	return EcdhKey(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
}

EcdhKey EcdhKey::FromScalar(const Bytes &scalar) {
	if (scalar.size() != kKeySize) {
		return EcdhKey();
	}
	auto group = std::string("prime256v1");
	auto reversed = Bytes(scalar.rbegin(), scalar.rend());
	OSSL_PARAM params[] = {
		OSSL_PARAM_construct_utf8_string(
			OSSL_PKEY_PARAM_GROUP_NAME,
			group.data(),
			0),
		OSSL_PARAM_construct_BN(
			OSSL_PKEY_PARAM_PRIV_KEY,
			reinterpret_cast<unsigned char*>(reversed.data()),
			reversed.size()),
		OSSL_PARAM_construct_end(),
	};
	const auto context = PkeyCtxPtr(
		EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
	EVP_PKEY *result = nullptr;
	if (!context
		|| EVP_PKEY_fromdata_init(context.get()) != 1
		|| EVP_PKEY_fromdata(
			context.get(),
			&result,
			EVP_PKEY_KEYPAIR,
			params) != 1) {
		return EcdhKey();
	}
	return EcdhKey(result);
}

bool EcdhKey::valid() const {
	return _key != nullptr;
}

Bytes EcdhKey::publicKey() const {
	if (!_key) {
		return Bytes();
	}
	auto result = Bytes(kPublicKeySize, '\0');
	auto length = size_t(0);
	if (EVP_PKEY_get_octet_string_param(
			_key,
			OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY,
			reinterpret_cast<unsigned char*>(result.data()),
			result.size(),
			&length) != 1
		|| length != size_t(kPublicKeySize)) {
		return Bytes();
	}
	return result;
}

std::optional<Bytes> EcdhKey::derive(const Bytes &peerPublic) const {
	const auto peer = PublicFromOctets(peerPublic);
	if (!_key || !peer) {
		return std::nullopt;
	}
	const auto context = PkeyCtxPtr(
		EVP_PKEY_CTX_new_from_pkey(nullptr, _key, nullptr));
	auto length = size_t(0);
	if (!context
		|| EVP_PKEY_derive_init(context.get()) != 1
		|| EVP_PKEY_derive_set_peer_ex(context.get(), peer.get(), 1) != 1
		|| EVP_PKEY_derive(context.get(), nullptr, &length) != 1) {
		return std::nullopt;
	}
	auto result = Bytes(length, '\0');
	if (EVP_PKEY_derive(
			context.get(),
			reinterpret_cast<unsigned char*>(result.data()),
			&length) != 1
		|| length != size_t(kKeySize)) {
		return std::nullopt;
	}
	result.resize(length);
	return result;
}

Keys LoginKeys(
		const Bytes &shared,
		const std::string &kid,
		const std::string &epk) {
	const auto salt = Bytes("foxmes-login-v1");
	return {
		.c2s = Hkdf(shared, salt, "c2s|" + kid + "|" + epk),
		.s2c = Hkdf(shared, salt, "s2c|" + kid + "|" + epk),
	};
}

Keys SessionKeys(const Bytes &secret) {
	const auto salt = Bytes("foxmes-sealed-v1");
	return {
		.c2s = Hkdf(secret, salt, "c2s"),
		.s2c = Hkdf(secret, salt, "s2c"),
	};
}

Bytes BindSalt(const Bytes &secret) {
	return Hkdf(secret, Bytes("foxmes-sealed-v1"), "bind");
}

Bytes TempSecret(
		const Bytes &shared,
		const Bytes &bindSalt,
		int64_t sid,
		const std::string &tid,
		const std::string &clientPublic,
		const std::string &serverPublic) {
	return Hkdf(
		shared,
		bindSalt,
		"foxmes-temp-v1|" + std::to_string(sid) + "|" + tid
			+ "|" + clientPublic + "|" + serverPublic);
}

Keys TempKeys(const Bytes &temp) {
	const auto salt = Bytes("foxmes-temp-v1");
	return {
		.c2s = Hkdf(temp, salt, "c2s"),
		.s2c = Hkdf(temp, salt, "s2c"),
	};
}

Keys WsKeys(const Keys &keys, const Bytes &nonce) {
	const auto salt = Bytes("foxmes-ws-v1");
	const auto info = B64(nonce);
	return {
		.c2s = Hkdf(keys.c2s, salt, info),
		.s2c = Hkdf(keys.s2c, salt, info),
	};
}

std::string HeaderBlockAad(const Bytes &requestNonce) {
	return "foxmes-headers-v1\n" + B64(requestNonce);
}

std::optional<std::vector<HeaderLine>> OpenHeaderBlock(
		const Bytes &key,
		const Bytes &requestNonce,
		const std::string &value) {
	const auto raw = UnB64(value);
	if (!raw || raw->size() < size_t(kNonceSize + kTagSize)) {
		return std::nullopt;
	}
	const auto plain = GcmOpen(
		key,
		raw->substr(0, kNonceSize),
		raw->substr(kNonceSize),
		HeaderBlockAad(requestNonce));
	if (!plain) {
		return std::nullopt;
	}
	auto result = std::vector<HeaderLine>();
	auto start = size_t(0);
	while (start < plain->size()) {
		auto end = plain->find('\n', start);
		if (end == std::string::npos) {
			end = plain->size();
		}
		const auto line = plain->substr(start, end - start);
		start = end + 1;
		if (line.empty()) {
			continue;
		}
		const auto colon = line.find(':');
		if (colon == std::string::npos || colon == 0) {
			return std::nullopt;
		}
		result.push_back({
			.name = line.substr(0, colon),
			.value = line.substr(colon + 1),
		});
	}
	return result;
}

std::string FrameAad(
		const FrameContext &context,
		int64_t offset,
		bool last) {
	return "foxmes-frame-v1\n" + B64(context.nonce) + "\n" + context.dir
		+ "\n" + std::to_string(context.status) + "\n" + context.type
		+ "\n" + context.range + "\n" + std::to_string(offset)
		+ "\n" + (last ? "1" : "0");
}

int64_t SealedLength(int64_t plain) {
	auto frames = plain / kFrameSize;
	if (plain % kFrameSize != 0 || plain == 0) {
		++frames;
	}
	return plain + frames * kFrameOverhead;
}

std::optional<int64_t> PlainLength(int64_t sealed) {
	if (sealed < kFrameOverhead) {
		return std::nullopt;
	}
	const auto frames = (sealed + kSealedFrame - 1) / kSealedFrame;
	const auto plain = sealed - frames * kFrameOverhead;
	if (plain < 0 || SealedLength(plain) != sealed) {
		return std::nullopt;
	}
	return plain;
}

Bytes SealFrames(
		const Bytes &key,
		const FrameContext &context,
		const Bytes &plain,
		const std::vector<Bytes> &nonces) {
	auto result = Bytes();
	result.reserve(SealedLength(int64_t(plain.size())));
	auto offset = int64_t(0);
	auto index = size_t(0);
	do {
		const auto size = std::min<int64_t>(
			kFrameSize,
			int64_t(plain.size()) - offset);
		const auto last = (offset + size == int64_t(plain.size()));
		const auto nonce = (index < nonces.size())
			? nonces[index]
			: Random(kNonceSize);
		const auto sealed = GcmSeal(
			key,
			nonce,
			plain.substr(offset, size),
			FrameAad(context, context.offset + offset, last));
		if (!sealed || nonce.size() != size_t(kNonceSize)) {
			return Bytes();
		}
		result += nonce;
		result += *sealed;
		offset += size;
		++index;
	} while (offset < int64_t(plain.size()));
	return result;
}

FrameDecoder::FrameDecoder(Bytes key, FrameContext context)
: _key(std::move(key))
, _context(std::move(context))
, _offset(_context.offset) {
}

bool FrameDecoder::open(int64_t size, bool last, Bytes &plain) {
	const auto nonce = _buffer.substr(0, kNonceSize);
	const auto opened = GcmOpen(
		_key,
		nonce,
		_buffer.substr(kNonceSize, size - kNonceSize),
		FrameAad(_context, _offset, last));
	if (!opened) {
		_failed = true;
		return false;
	}
	_buffer.erase(0, size);
	_offset += int64_t(opened->size());
	plain += *opened;
	return true;
}

bool FrameDecoder::feed(const char *data, int64_t size, Bytes &plain) {
	if (_failed || _finished) {
		return !_failed && (size == 0);
	}
	_buffer.append(data, size);
	while (int64_t(_buffer.size()) > kSealedFrame) {
		if (!open(kSealedFrame, false, plain)) {
			return false;
		}
	}
	return true;
}

bool FrameDecoder::finish(Bytes &plain) {
	if (_failed) {
		return false;
	} else if (_finished) {
		return true;
	}
	_finished = true;
	if (int64_t(_buffer.size()) < kFrameOverhead) {
		_failed = true;
		return false;
	}
	return open(int64_t(_buffer.size()), true, plain);
}

std::string RequestAad(const RequestParts &parts) {
	auto host = parts.host;
	std::transform(host.begin(), host.end(), host.begin(), [](char c) {
		return char(std::tolower(static_cast<unsigned char>(c)));
	});
	auto result = "foxmes-req-v1\n" + parts.method + "\n" + host + "\n"
		+ parts.path + "\n" + parts.query + "\n" + std::to_string(parts.ts)
		+ "\n" + parts.cred + "\n" + parts.type + "\n";
	for (const auto &header : parts.headers) {
		result += header.name + ":" + header.value + "\n";
	}
	return result;
}

std::optional<Bytes> RequestTag(
		const Bytes &key,
		const Bytes &nonce,
		const RequestParts &parts) {
	return GcmSeal(key, nonce, Bytes(), RequestAad(parts));
}

WsCodec::WsCodec(const Keys &temp, const Bytes &nonce)
: _keys(WsKeys(temp, nonce)) {
}

Bytes WsCodec::seal(const Bytes &plain) {
	const auto sealed = GcmSeal(
		_keys.c2s,
		Counter(_sent),
		plain,
		Bytes("foxmes-ws-v1\n") + kDirC2S);
	++_sent;
	return sealed.value_or(Bytes());
}

std::optional<Bytes> WsCodec::open(const Bytes &message) {
	auto result = GcmOpen(
		_keys.s2c,
		Counter(_received),
		message,
		Bytes("foxmes-ws-v1\n") + kDirS2C);
	if (result) {
		++_received;
	}
	return result;
}

std::string IntermediateMessage(
		int64_t serial,
		const std::string &pub,
		int64_t notAfter) {
	return "foxmes-intermediate-v1\n" + std::to_string(serial) + "\n" + pub
		+ "\n" + std::to_string(notAfter);
}

std::string LoginKeyMessage(
		const std::string &kid,
		const std::string &pub,
		int64_t notAfter) {
	return "foxmes-login-key-v1\n" + kid + "\n" + pub + "\n"
		+ std::to_string(notAfter);
}

std::string RevocationMessage(int64_t minSerial, int64_t issuedAt) {
	return "foxmes-revocation-v1\n" + std::to_string(minSerial) + "\n"
		+ std::to_string(issuedAt);
}

} // namespace CustomBackend::SealedCrypto
