#include "custom_backend/native_send_files_adapter.h"

#include "custom_backend/native_gifs_adapter.h"
#include "custom_backend/native_stickers_adapter.h"

#include "custom_backend/native_bridge.h"
#include "custom_backend/native_runtime.h"
#include "apiwrap.h"
#include "core/file_utilities.h"
#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "data/data_document.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/image/image_prepare.h"
#include "ui/item_text_options.h"
#include "ui/text/text_entity.h"

#include <QtCore/QBuffer>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>
#include <QtGui/QImageWriter>

namespace CustomBackend {
namespace {

// The quality upstream encodes a picture with when it has to re-encode one
// (localimageloader.cpp ComputePhotoJpegBytes), progressive like it too.
constexpr auto kPhotoJpegQuality = 87;

// Resolves what the receiving client has to build natively. The same MIME can
// be a voice message or a music file, and a video can be a plain video, a
// GIF-like animation or a round video note - none of that is derivable from
// the bytes, so it is read here, at the only place that still knows the user's
// intent, and travels with the send.
[[nodiscard]] QString KindOf(
		const Ui::PreparedFile &file,
		SendMediaType type) {
	if (type == SendMediaType::Round) {
		return u"video_note"_q;
	} else if (type == SendMediaType::Audio) {
		return u"voice"_q;
	}
	using Song = Ui::PreparedFileInformation::Song;
	// A music file keeps its audio kind even when the send is not a compressed
	// one. Upstream's SendMediaType::File means "not a compressed photo", not
	// "drop the media attributes": FileLoadTask still reads the song out of the
	// prepared information and attaches it. Answering "document" here made every
	// mp3 arrive as a plain file.
	if (file.information
		&& std::get_if<Song>(&file.information->media)) {
		return u"audio"_q;
	} else if (type == SendMediaType::File) {
		return u"document"_q;
	}
	using Image = Ui::PreparedFileInformation::Image;
	// An animated picture is a GIF send, not a document. The composer gives it
	// Type::None (storage_media_prepare.cpp: an animated image cannot be laid
	// out as a photo), and the switch below answers "document" for that - so
	// every .gif went down the generic file lane and the server, which decides
	// the conversion by lane, stored it as an actual gif file. It then played
	// nowhere and could never reach the saved-GIF list.
	if (const auto image = file.information
		? std::get_if<Image>(&file.information->media)
		: nullptr; image && image->animated) {
		return u"animation"_q;
	}
	using Type = Ui::PreparedFile::Type;
	switch (file.type) {
	case Type::Photo:
		return u"photo"_q;
	case Type::Music:
		return u"audio"_q;
	case Type::Video:
		return file.isGifv() ? u"animation"_q : u"video"_q;
	}
	return u"document"_q;
}

[[nodiscard]] qint64 DurationMsOf(const Ui::PreparedFile &file) {
	if (!file.information) {
		return 0;
	}
	using Song = Ui::PreparedFileInformation::Song;
	using Video = Ui::PreparedFileInformation::Video;
	if (const auto song = std::get_if<Song>(&file.information->media)) {
		return (song->duration > 0) ? qint64(song->duration) : 0;
	} else if (const auto video = std::get_if<Video>(&file.information->media)) {
		return (video->duration > 0) ? qint64(video->duration) : 0;
	}
	return 0;
}

[[nodiscard]] QString PerformerOf(const Ui::PreparedFile &file) {
	if (!file.information) {
		return QString();
	}
	using Song = Ui::PreparedFileInformation::Song;
	if (const auto song = std::get_if<Song>(&file.information->media)) {
		return song->performer;
	}
	return QString();
}

[[nodiscard]] QString TitleOf(const Ui::PreparedFile &file) {
	if (!file.information) {
		return QString();
	}
	using Song = Ui::PreparedFileInformation::Song;
	if (const auto song = std::get_if<Song>(&file.information->media)) {
		return song->title;
	}
	return QString();
}

// Cover art out of the tag, encoded the way every other picture of a send is.
// Upstream hands the same image straight to PrepareFileThumbnail
// (localimageloader.cpp), but a thumbnail cannot travel inside our DTO: the
// attachment can only point at a file, so the cover becomes one.
[[nodiscard]] QByteArray CoverOf(const Ui::PreparedFile &file) {
	if (!file.information) {
		return QByteArray();
	}
	using Song = Ui::PreparedFileInformation::Song;
	const auto song = std::get_if<Song>(&file.information->media);
	if (!song || song->cover.isNull()) {
		return QByteArray();
	}
	const auto limit = PhotoSideLimit(false);
	const auto cover = Images::Opaque((song->cover.width() > limit)
		|| (song->cover.height() > limit)
		? song->cover.scaled(
			limit,
			limit,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation)
		: song->cover);
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	auto writer = QImageWriter(&buffer, "JPEG");
	writer.setQuality(kPhotoJpegQuality);
	writer.setProgressiveScanWrite(true);
	if (!writer.write(cover)) {
		return QByteArray();
	}
	buffer.close();
	return bytes;
}

// The image upload derives the sanitizer's format from the file name extension
// (api/files.go normalizeUploadFilename), so a re-encoded picture has to be
// renamed to what it now is: keeping "shot.png" on jpeg bytes would hand the
// sanitizer the wrong format. An unnamed one gets the timestamped name upstream
// makes for the same case.
[[nodiscard]] QString ImageName(
		const QString &displayName,
		const QString &suffix) {
	const auto base = QFileInfo(displayName).completeBaseName().trimmed();
	return base.isEmpty()
		? filedialogDefaultName(u"image"_q, suffix, QString(), true)
		: (base + suffix);
}

// Bytes for a prepared file that has neither a path nor content on disk.
// Two composer paths produce one: a clipboard picture arrives as a bare QImage
// (Storage::PrepareMediaFromImage keeps the pixels in `information` and leaves
// both fields empty), and Storage::ApplyModifications explicitly drops the path
// and the content of every picture that went through the photo editor. Upstream
// encodes those in FileLoadTask::process(), which the bridge replaces wholesale,
// so without this the upload had nothing to send: the bubble showed
// "upload.bin, 0 B" and the transfer failed with "neither path nor content".
struct MaterializedImage {
	QByteArray bytes;
	QString mime;
	QString name;
	// The picture was encoded losslessly because it cannot be laid out as a
	// photo, which is upstream's `_type = SendMediaType::File` downgrade: the
	// send has to follow it, or the server would still build an image node.
	bool asFile = false;
};

[[nodiscard]] MaterializedImage MaterializeImage(
		const Ui::PreparedFile &file,
		SendMediaType type) {
	if (!file.path.isEmpty() || !file.content.isEmpty() || !file.information) {
		return {};
	}
	using Image = Ui::PreparedFileInformation::Image;
	const auto image = std::get_if<Image>(&file.information->media);
	if (!image || image->data.isNull() || (image->data.width() <= 0)) {
		return {};
	}
	// The same two branches upstream takes, in the same order: a picture too odd
	// to be laid out as a photo falls back to being sent as a file, and the file
	// branch writes png before the image is flattened, so transparency survives.
	const auto asFile = (type == SendMediaType::File)
		|| !Ui::ValidateThumbDimensions(
			image->data.width(),
			image->data.height());
	if (asFile) {
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		if (!image->data.save(&buffer, "PNG")) {
			return {};
		}
		return {
			.bytes = std::move(bytes),
			.mime = u"image/png"_q,
			.name = ImageName(file.displayName, u".png"_q),
			.asFile = true,
		};
	}
	const auto limit = PhotoSideLimit(file.sendLargePhotos);
	const auto full = Images::Opaque((image->data.width() > limit)
		|| (image->data.height() > limit)
		? image->data.scaled(
			limit,
			limit,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation)
		: image->data);
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	auto writer = QImageWriter(&buffer, "JPEG");
	writer.setQuality(kPhotoJpegQuality);
	writer.setProgressiveScanWrite(true);
	if (!writer.write(full)) {
		return {};
	}
	buffer.close();
	return {
		.bytes = std::move(bytes),
		.mime = u"image/jpeg"_q,
		.name = ImageName(file.displayName, u".jpg"_q),
	};
}

// The user's "send as file" choice, with upstream's own meaning of it
// (apiwrap.cpp: `(type == SendMediaType::File) && (file.type == Type::Video)`).
// SendMediaType::File covers every send that is not a compressed photo, so
// reading it as force_file made the server store music and generic documents as
// "file" nodes too - and api_fox_mes/converter.go answers AsFile before it ever
// looks at the MIME, which is what turned every mp3 into a plain document.
[[nodiscard]] bool ForceFileFor(
		const Ui::PreparedFile &file,
		SendMediaType type) {
	using Type = Ui::PreparedFile::Type;
	return (type == SendMediaType::File)
		&& ((file.type == Type::Photo) || (file.type == Type::Video));
}

// The chat stores a video as it arrives instead of re-encoding it, so "video"
// here has to mean exactly what it can store: an ISO-BMFF container. Upstream
// is wider - CheckForVideo (localimageloader.cpp) also accepts .webm by
// extension - and such a file would reach the video lane only to be refused
// there. Calling it a document instead is honest: a document is what it will
// be stored as.
[[nodiscard]] bool ChatVideoContainer(
		const QString &mime,
		const QString &name) {
	if (mime == u"video/mp4"_q || mime == u"video/quicktime"_q) {
		return true;
	}
	static const auto extensions = {
		u".mp4"_q,
		u".mov"_q,
		u".m4v"_q,
	};
	for (const auto &extension : extensions) {
		if (name.endsWith(extension, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

// The container alone does not settle it: an iPhone records HEVC into exactly
// the .mov the check above accepts, and the chat can only keep h264. Nothing
// upstream carries the codec - PreparedFileInformation::Video has duration,
// audio and a thumbnail, but not this - so the header is read here. Only the
// stream table is parsed, no frame is decoded.
[[nodiscard]] bool ChatVideoCodecSupported(
		const QString &path,
		const QByteArray &content) {
	auto fileWrap = FFmpeg::ReadFileWrap();
	auto bytesWrap = FFmpeg::ReadBytesWrap();
	auto format = FFmpeg::FormatPointer();
	if (!content.isEmpty()) {
		bytesWrap = FFmpeg::ReadBytesWrap{
			.size = int64(content.size()),
			.data = reinterpret_cast<const uchar*>(content.constData()),
		};
		format = FFmpeg::MakeFormatPointer(
			&bytesWrap,
			&FFmpeg::ReadBytesWrap::Read,
			nullptr,
			&FFmpeg::ReadBytesWrap::Seek);
	} else {
		fileWrap.file.setFileName(path);
		if (!fileWrap.file.open(QIODevice::ReadOnly)) {
			return false;
		}
		format = FFmpeg::MakeFormatPointer(
			&fileWrap,
			&FFmpeg::ReadFileWrap::Read,
			nullptr,
			&FFmpeg::ReadFileWrap::Seek);
	}
	if (!format) {
		return false;
	} else if (avformat_find_stream_info(format.get(), nullptr) < 0) {
		return false;
	}
	const auto codecOf = [&](AVMediaType type) {
		const auto id = av_find_best_stream(
			format.get(),
			type,
			-1,
			-1,
			nullptr,
			0);
		return (id < 0)
			? AV_CODEC_ID_NONE
			: format->streams[id]->codecpar->codec_id;
	};
	const auto videoCodec = codecOf(AVMEDIA_TYPE_VIDEO);
	const auto audioCodec = codecOf(AVMEDIA_TYPE_AUDIO);
	if (videoCodec != AV_CODEC_ID_H264) {
		return false;
	}
	// The audio has to be playable too, and a .mov carrying PCM is common
	// enough to matter: the same set the server accepts (api/files_video_probe.go).
	switch (audioCodec) {
	case AV_CODEC_ID_NONE:
	case AV_CODEC_ID_AAC:
	case AV_CODEC_ID_MP3:
		return true;
	default:
		return false;
	}
}

// A rasterized .gif, as opposed to an mp4 the sender means as an animation.
[[nodiscard]] bool RasterizedGif(const UploadSpec &spec) {
	if (spec.mime == u"image/gif"_q) {
		return true;
	}
	const auto name = spec.displayName.isEmpty() ? spec.path : spec.displayName;
	return name.endsWith(u".gif"_q, Qt::CaseInsensitive);
}

// Demotes a send the chat cannot keep as a video to a plain document.
void DemoteUnsupportedVideo(UploadSpec &spec) {
	if (spec.kind != u"video"_q && spec.kind != u"animation"_q) {
		return;
	}
	// A gif is exempt: it is not a container the chat stores, but it is the one
	// upload the server converts for us, and demoting it here would send it
	// down the file lane where that conversion never runs.
	if (RasterizedGif(spec)) {
		return;
	}
	const auto name = spec.displayName.isEmpty() ? spec.path : spec.displayName;
	if (ChatVideoContainer(spec.mime, name)
		&& ChatVideoCodecSupported(spec.path, spec.content)) {
		return;
	}
	spec.kind = u"document"_q;
	spec.forceFile = true;
}

} // namespace

void SendFiles(
		Ui::PreparedList &&list,
		SendMediaType type,
		Api::SendAction action) {
	const auto bridge = Enabled()
		? BridgeFor(&action.history->session())
		: nullptr;
	if (!bridge) {
		return;
	}
	auto files = std::vector<UploadSpec>();
	files.reserve(list.files.size());
	auto caption = TextWithEntities();
	for (const auto &file : list.files) {
		if (caption.text.isEmpty() && !file.caption.text.isEmpty()) {
			// The composer keeps formatting as tags; PrepareForSending then
			// adds what nobody typed - bare urls, mentions, hashtags - exactly
			// as it does for a text send, because nothing downstream looks for
			// a link inside plain text.
			caption = TextWithEntities{
				file.caption.text,
				TextUtilities::ConvertTextTagsToEntities(file.caption.tags),
			};
			TextUtilities::PrepareForSending(
				caption,
				Ui::ItemTextOptions(
					action.history,
					action.history->session().user()).flags);
		}
		auto spec = UploadSpec{
			.path = file.path,
			.displayName = file.displayName,
			.mime = file.information ? file.information->filemime : QString(),
			.content = file.content,
			.forceFile = ForceFileFor(file, type),
			.kind = KindOf(file, type),
			.durationMs = DurationMsOf(file),
			.performer = PerformerOf(file),
			.title = TitleOf(file),
			.cover = CoverOf(file),
			.spoiler = file.spoiler,
		};
		DemoteUnsupportedVideo(spec);
		if (auto image = MaterializeImage(file, type); !image.bytes.isEmpty()) {
			spec.displayName = std::move(image.name);
			spec.mime = std::move(image.mime);
			spec.content = std::move(image.bytes);
			if (image.asFile) {
				spec.forceFile = true;
				spec.kind = u"document"_q;
			}
		}
		files.push_back(std::move(spec));
	}
	bridge->sendFiles(
		action.history,
		std::move(files),
		caption,
		ReplyTargetFrom(action.history, action.replyTo),
		{},
		SendOptionsFrom(action.options));
}

void SendFileContent(
		const QByteArray &content,
		SendMediaType type,
		const Api::SendAction &action) {
	const auto bridge = Enabled()
		? BridgeFor(&action.history->session())
		: nullptr;
	if (!bridge) {
		return;
	}
	// Clipboard content arrives as raw bytes with no name, and the upload is
	// typed on the server: posting everything as application/octet-stream
	// stored every pasted picture as a dimensionless generic file. Sniff the
	// real type and give the file an extension that matches it.
	const auto mime = QMimeDatabase().mimeTypeForData(content);
	const auto mimeName = mime.isValid()
		? mime.name()
		: u"application/octet-stream"_q;
	const auto suffix = mime.isValid() ? mime.preferredSuffix() : QString();
	const auto name = suffix.isEmpty()
		? u"clipboard"_q
		: (u"clipboard."_q + suffix);
	const auto forceFile = (type == SendMediaType::File);
	auto spec = UploadSpec{
		.displayName = name,
		.mime = mimeName,
		.content = content,
		.forceFile = forceFile,
		.kind = forceFile
			? u"document"_q
			: (mimeName.startsWith(u"image/"_q)
				? u"photo"_q
				: (mimeName.startsWith(u"video/"_q)
					? u"video"_q
					: u"document"_q)),
	};
	DemoteUnsupportedVideo(spec);
	auto files = std::vector<UploadSpec>();
	files.push_back(std::move(spec));
	bridge->sendFiles(
		action.history,
		std::move(files),
		TextWithEntities(),
		ReplyTargetFrom(action.history, action.replyTo),
		{},
		SendOptionsFrom(action.options));
}

void SendVoiceMessage(
		const QByteArray &content,
		const VoiceWaveform &waveform,
		crl::time duration,
		bool video,
		const Api::SendAction &action) {
	const auto bridge = Enabled()
		? BridgeFor(&action.history->session())
		: nullptr;
	if (!bridge) {
		return;
	}
	// The names and types upstream gives the same two recordings
	// (localimageloader.cpp): nothing is read off the bytes, because for a
	// recording the type is known before the first sample.
	const auto round = video;
	auto files = std::vector<UploadSpec>();
	files.push_back(UploadSpec{
		.displayName = round
			? filedialogDefaultName(u"round"_q, u".mp4"_q, QString(), true)
			: filedialogDefaultName(u"audio"_q, u".ogg"_q, QString(), true),
		.mime = round ? u"video/mp4"_q : u"audio/ogg"_q,
		.content = content,
		.kind = round ? u"video_note"_q : u"voice"_q,
		.durationMs = qint64(duration),
		// The envelope drawn in the bubble, packed the same 5 bits per sample
		// the receiving side unpacks it with (native_bridge.cpp), and base64'd
		// because the contract carries it as a string.
		.waveform = (round || waveform.isEmpty())
			? QString()
			: QString::fromLatin1(
				documentWaveformEncode5bit(waveform).toBase64()),
	});
	bridge->sendFiles(
		action.history,
		std::move(files),
		TextWithEntities(),
		ReplyTargetFrom(action.history, action.replyTo),
		{},
		SendOptionsFrom(action.options));
}

bool SendExistingDocument(
		not_null<DocumentData*> document,
		const Api::SendAction &action) {
	if (!Stickers::Send(document, action) && !Gifs::Send(document, action)) {
		return false;
	}
	// Upstream fires this from Api::SendExistingDocument, which the bridge
	// replaces whole. The composer clears its reply bar only from the stream
	// this call feeds. See ApiWrap::sendMessage.
	action.history->session().api().sendAction(action);
	return true;
}

} // namespace CustomBackend
