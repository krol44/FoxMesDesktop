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
}

namespace MTP {
class Instance;
namespace details {
class SerializedRequest;
}
}

namespace CustomBackend::Mtp {

[[nodiscard]] bool Intercepts(const MTP::details::SerializedRequest &request);
void Intercept(
	not_null<MTP::Instance*> instance,
	mtpRequestId requestId,
	const MTP::details::SerializedRequest &request);

[[nodiscard]] mtpTypeId RequestType(
	const MTP::details::SerializedRequest &request);
[[nodiscard]] Main::Session *SessionFor(not_null<MTP::Instance*> instance);

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

class Answer final {
public:
	Answer(not_null<MTP::Instance*> instance, mtpRequestId requestId);

	template <typename Bare>
	void done(const tl::boxed<Bare> &result) const {
		auto reply = mtpBuffer();
		result.write(reply);
		deliver(std::move(reply));
	}

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

[[nodiscard]] QString ErrorType(
	const QJsonDocument &doc,
	const QString &error,
	int status,
	const QString &fallback404,
	const QString &fallback403);

}
