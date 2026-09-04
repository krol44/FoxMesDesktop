/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "storage/cache/storage_cache_types.h"

#include <memory>

class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace Media::Streaming {
class Loader;
} // namespace Media::Streaming

namespace CustomBackend::Streaming {

// Upstream plays a video, a GIF or a round message by streaming it: the player
// opens Media::Streaming on the MTProto file location, Media::Streaming::Reader
// pulls 128 KB parts through a Media::Streaming::Loader and stores them in the
// big file cache. Nothing is ever downloaded to a file, and the autoplay limits
// in Settings gate that stream.
//
// The only piece of it we cannot reuse is the loader: LoaderMtproto needs a
// StorageFileLocation, and an fxl-cdn attachment has none - it is an https url.
// This module is that missing loader, built on HTTP range requests, plus the
// answers the three thin hooks in data_document.cpp need to route a document
// of ours into it.

// Records where the bytes of a document live. Called wherever the bridge points
// a DocumentData at its content url; DocumentData keeps that url privately and
// hands out no getter, and this is also what tells "ours" from an upstream
// document later on.
void RememberSource(not_null<DocumentData*> document, const QString &url);
void ClearSession(not_null<Main::Session*> session);

// Answers DocumentData::canBeStreamed() for a document with no remote location.
[[nodiscard]] bool CanBeStreamed(not_null<const DocumentData*> document);

// Answers DocumentData::bigFileBaseCacheKey() for the same. Reader addresses a
// slice as key.low + sliceNumber, so the low bits are left free.
[[nodiscard]] Storage::Cache::Key BigFileCacheKey(
	not_null<const DocumentData*> document);

// The loader itself. Null for anything that is not a streamable attachment of
// ours, so the upstream branch stays in charge of every other document.
[[nodiscard]] std::unique_ptr<Media::Streaming::Loader> MakeLoader(
	not_null<const DocumentData*> document);

} // namespace CustomBackend::Streaming
