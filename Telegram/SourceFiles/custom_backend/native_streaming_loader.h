/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "storage/cache/storage_cache_types.h"
#include "data/data_types.h"

#include <memory>
#include <optional>

class DocumentData;
class PhotoData;

struct AVFormatContext;
struct AVPacket;

namespace Main {
class Session;
}

namespace Media::Streaming {
class Loader;
class Reader;
}

namespace CustomBackend::Streaming {


void RememberSource(not_null<DocumentData*> document, const QString &url);
void RememberPlaylist(
	not_null<DocumentData*> document,
	const QString &playlistSha256);
void ClearSession(not_null<Main::Session*> session);

void ShiftMpegTsPacket(not_null<AVFormatContext*> format, AVPacket &packet);
[[nodiscard]] int SeekMpegTs(
	not_null<AVFormatContext*> format,
	int streamIndex,
	crl::time position,
	not_null<const Media::Streaming::Reader*> reader);

void RememberFileCacheKey(
	not_null<Main::Session*> session,
	const QString &url,
	const QString &fileUniqueId,
	const QString &representation);
[[nodiscard]] std::optional<Storage::Cache::Key> FileCacheKey(const QString &url);
[[nodiscard]] Storage::Cache::Key UrlCacheKey(const QString &url);
[[nodiscard]] std::optional<MediaKey> FileLocationKey(const QString &url);
[[nodiscard]] MediaKey DocumentMediaKey(
	const QString &url,
	LocationType type,
	int32 dc,
	uint64 id);

[[nodiscard]] bool CanBeStreamed(not_null<const DocumentData*> document);

[[nodiscard]] Storage::Cache::Key BigFileCacheKey(
	not_null<const DocumentData*> document);

[[nodiscard]] std::unique_ptr<Media::Streaming::Loader> MakeLoader(
	not_null<const DocumentData*> document);

[[nodiscard]] std::unique_ptr<Media::Streaming::Loader> MakePhotoVideoLoader(
	not_null<const PhotoData*> photo);

}
