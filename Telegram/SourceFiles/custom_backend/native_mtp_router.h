/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "mtproto/core_types.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QString>

namespace Main {
class Session;
} // namespace Main

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
} // namespace details
} // namespace MTP

namespace CustomBackend::Mtp {

// Requests answered at the MTProto boundary instead of at their call sites.
//
// Upstream makes the same request from many places, and a hook at each would
// repeat itself. MTP::Instance hands every request whose constructor an
// adapter claims to that adapter instead of a DC; the adapter reads the TL
// request, asks fxl-api, builds the TL answer upstream expects and delivers
// it through the regular response path (Instance::processCallback). To
// upstream the server simply answered. Requests nobody claims keep today's
// behaviour: under the bridge they are never sent anywhere.
//
// Adapters: native_conference_adapter (phone.*) and native_channels_adapter
// (channels.*, groups and channels).
[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

[[nodiscard]] mtpTypeId RequestType(
	const MTP::details::SerializedRequest &request);
[[nodiscard]] Main::Session *SessionFor(not_null<MTP::Instance*> instance);

// A request is read field by field with the field types' own readers: the
// generated request classes can read themselves but keep their fields
// private, so there is nothing to read them into.
class Reader final {
public:
	explicit Reader(const MTP::details::SerializedRequest &request);

	template <typename Type>
	[[nodiscard]] Type read() {
		auto result = Type();
		if (_ok && !result.read(_from, _end)) {
			_ok = false;
		}
		return result;
	}

	[[nodiscard]] int32 flags() {
		return read<MTPint>().v;
	}

	[[nodiscard]] bool ok() const {
		return _ok;
	}

private:
	const mtpPrime *_from = nullptr;
	const mtpPrime *_end = nullptr;
	bool _ok = true;

};

// Delivers an answer the way a Telegram server's would arrive. The instance
// can die while fxl-api thinks (logout), hence the guard.
class Answer final {
public:
	Answer(not_null<MTP::Instance*> instance, mtpRequestId requestId);

	// Boxed only: a reply starts with its constructor id, and a bare type
	// written here reaches upstream as an unparsable answer.
	template <typename Bare>
	void done(const tl::boxed<Bare> &result) const {
		auto reply = mtpBuffer();
		result.write(reply);
		deliver(std::move(reply));
	}

	// A bare result (what MTP_... constructors return) is boxed on the way:
	// the overload above is more specialized, so a boxed one never lands here.
	template <typename Bare>
	void done(const Bare &result) const {
		done(tl::boxed<Bare>(result));
	}

	void fail(const QString &type, int code = 400) const;

private:
	void deliver(mtpBuffer &&reply) const;

	QPointer<MTP::Instance> _instance;
	mtpRequestId _requestId = 0;

};

// fxl-api answers a refusal with the MTProto error type in "code", so upstream
// gets exactly the error it knows how to react to. fallback404 and
// fallback403 name the type for an answer that carries none.
[[nodiscard]] QString ErrorType(
	const QJsonDocument &doc,
	const QString &error,
	int status,
	const QString &fallback404,
	const QString &fallback403);

} // namespace CustomBackend::Mtp
