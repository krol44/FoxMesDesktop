#include "custom_backend/native_bridge.h"

#include "api/api_text_entities.h"
#include "apiwrap.h"

#include "custom_backend/native_chat_themes_adapter.h"
#include "custom_backend/native_gifs_adapter.h"
#include "custom_backend/native_wallpaper_adapter.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_calls_adapter.h"
#include "custom_backend/native_community.h"
#include "custom_backend/native_conference_adapter.h"
#include "custom_backend/native_delete_adapter.h"
#include "custom_backend/native_reactions_adapter.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_scheduled_adapter.h"
#include "custom_backend/native_streaming_loader.h"
#include "custom_backend/native_topic_channels.h"
#include "custom_backend/topic_channel_links.h"
#include "custom_backend/topic_channels_prompt.h"
#include "data/components/scheduled_messages.h"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "base/qthelp_url.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/file_location.h"
#include "data/notify/data_notify_settings.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/data_document_media.h"
#include "data/data_folder.h"
#include "data/data_history_messages.h"
#include "data/data_lastseen_status.h"
#include "data/data_message_reaction_id.h"
#include "data/data_message_reactions.h"
#include "data/data_messages.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_send_action.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "history/view/history_view_element.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_edition.h"
#include "history/history_item_helpers.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_facade.h"
#include "storage/storage_account.h"
#include "storage/storage_shared_media.h"
#include "ui/effects/thanos_effect.h"
#include "ui/item_text_options.h"
#include "ui/text/text_entity.h"
#include "ui/image/image_location.h"
#include "ui/image/image_location_factory.h"
#include "window/window_session_controller.h"
#include "mainwindow.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <memory>
#include <utility>

namespace CustomBackend {
namespace {

void ShowSettingsToast(Main::Session *session, PeerData *peer, const QString &text) {
    if (!session || !peer) return;
    if (const auto controller = session->tryResolveWindow(peer)) {
        controller->showToast(text);
    }
}

MessageKey SeenKey(qint64 chatId, qint64 messageId) {
    return MessageKey{ .chatId = chatId, .messageId = messageId };
}

constexpr auto kAttachmentMediaIdOffset = qint64(1000000000000000LL);

constexpr auto kSendNoStatus = 0;

constexpr auto kSendServerAccepted = 200;

constexpr auto kMaxSendReplays = 3;

[[nodiscard]] bool SendMayBeRetried(int status) {
    return !status || (status == 429) || (status >= 500);
}

[[nodiscard]] QString NormalizedLinkTarget(QString value) {
    value = value.trimmed();
    for (const auto &scheme : { u"https://"_q, u"http://"_q }) {
        if (value.startsWith(scheme, Qt::CaseInsensitive)) {
            value = value.mid(scheme.size());
            break;
        }
    }
    while (value.endsWith('/')) {
        value.chop(1);
    }
    return value;
}

[[nodiscard]] bool LinkTargetIsTheText(
        const QString &target,
        const QString &text,
        int offset,
        int length) {
    if (offset < 0 || length <= 0 || offset + length > text.size()) {
        return false;
    }
    const auto covered = text.mid(offset, length);
    return !NormalizedLinkTarget(covered).isEmpty()
        && !NormalizedLinkTarget(target).compare(
            NormalizedLinkTarget(covered),
            Qt::CaseInsensitive);
}

constexpr auto kDeliveredBatchDelayMs = 400;

constexpr auto kSlowHistoryPage = crl::time(700);

constexpr auto kReactionUsageRefreshDelayMs = 10 * 1000;

constexpr auto kPinnedMessagesLimit = 30;

constexpr auto kDeferredMessageEventsLimit = 256;

[[nodiscard]] std::vector<DocumentId> ReactionUsageIds(
        const QJsonValue &value) {
    auto result = std::vector<DocumentId>();
    for (const auto &entry : value.toArray()) {
        const auto id = DocumentId(entry.toVariant().toLongLong());
        if (id && !ranges::contains(result, id)) {
            result.push_back(id);
        }
    }
    return result;
}

QString HlsPlaylist(const QJsonObject &attachment) {
    return (attachment.value("stream_size").toVariant().toLongLong() > 0)
        ? attachment.value("sha256").toString().trimmed().toLower()
        : QString();
}

QString AttachmentMime(const QJsonObject &attachment) {
    if (!HlsPlaylist(attachment).isEmpty()) {
        return u"video/mp2t"_q;
    }
    auto result = attachment.value("mime").toString().trimmed();
    if (!result.isEmpty()) {
        return result;
    }
    const auto name = attachment.value("name").toString();
    result = QMimeDatabase().mimeTypeForFile(name).name();
    return result.isEmpty() ? u"application/octet-stream"_q : result;
}

QByteArray LoadUploadBytes(const UploadSpec &file) {
    if (!file.content.isEmpty()) {
        return file.content;
    }
    if (file.path.isEmpty()) {
        return QByteArray();
    }
    QFile local(file.path);
    if (!local.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return local.readAll();
}

QJsonObject AttachmentObjectFromUpload(
        qint64 localId,
        const UploadSpec &file,
        const QByteArray &bytes) {
    const auto info = QFileInfo(file.path);
    const auto name = !file.displayName.trimmed().isEmpty()
        ? file.displayName.trimmed()
        : !info.fileName().isEmpty()
        ? info.fileName()
        : u"upload.bin"_q;
    const auto stableId = (localId < 0) ? -localId : (localId > 0 ? localId : 1);
    const auto size = file.path.isEmpty()
        ? qint64(bytes.size())
        : info.size();
    auto result = QJsonObject{
        {"id", QString::number(stableId)},
        {"name", name},
        {"mime", file.mime.trimmed()},
        {"size", QString::number(size)},
    };
    if (!file.kind.isEmpty()) result.insert("kind", file.kind);
    if (file.durationMs > 0) result.insert("duration_ms", file.durationMs);
    if (!file.waveform.isEmpty()) result.insert("waveform", file.waveform);
    if (!file.performer.isEmpty()) result.insert("performer", file.performer);
    if (!file.title.isEmpty()) result.insert("title", file.title);
    if (file.spoiler) result.insert("spoiler", true);
    return result;
}

bool UploadIsPhoto(const UploadSpec &file) {
    if (file.forceFile) {
        return false;
    }
    auto mime = file.mime.trimmed();
    if (mime.isEmpty()) {
        const auto name = file.displayName.trimmed().isEmpty()
            ? file.path
            : file.displayName.trimmed();
        mime = QMimeDatabase().mimeTypeForFile(name).name();
    }
    return mime.startsWith(u"image/"_q);
}

qint64 LocalAttachmentMediaId(qint64 localId) {
    const auto stableId = (localId < 0) ? -localId : (localId > 0 ? localId : 1);
    return kAttachmentMediaIdOffset + stableId;
}

void EnsureBarSettingsKnown(PeerData *peer) {
    if (peer && !peer->barSettings()) {
        peer->setBarSettings(PeerBarSettings());
    }
}

FullReplyTo ReplyToFromServerId(History *history, ReplyTarget replyTo) {
    if (!history
        || replyTo.messageId <= 0
        || replyTo.messageId > INT32_MAX) {
        return {};
    }
    const auto peer = replyTo.peer ? replyTo.peer : history->peer->id;
    return FullReplyTo{
        .messageId = FullMsgId(peer, MsgId(int32(replyTo.messageId))),
    };
}

constexpr auto kUnknownPhotoSide = 320;

constexpr auto kEphemeralKindPhoto = 1;
constexpr auto kEphemeralKindVideo = 2;
constexpr auto kEphemeralKindVoice = 5;
constexpr auto kEphemeralKindVideoNote = 6;

[[nodiscard]] std::optional<MTPMessageMedia> EphemeralExpiredMedia(
        const QJsonObject &ephemeral,
        int ttlSeconds) {
    const auto kind = ephemeral.value("media_kind").toInt();
    if (kind == kEphemeralKindPhoto) {
        using Flag = MTPDmessageMediaPhoto::Flag;
        return MTP_messageMediaPhoto(
            MTP_flags(Flag::f_ttl_seconds),
            MTPPhoto(),
            MTP_int(ttlSeconds),
            MTPDocument());
    }
    using Flag = MTPDmessageMediaDocument::Flag;
    auto flags = Flag::f_ttl_seconds | Flag();
    if (kind == kEphemeralKindVideo) {
        flags |= Flag::f_video;
    } else if (kind == kEphemeralKindVoice) {
        flags |= Flag::f_voice;
    } else if (kind == kEphemeralKindVideoNote) {
        flags |= Flag::f_round;
    } else {
        return std::nullopt;
    }
    return MTP_messageMediaDocument(
        MTP_flags(flags),
        MTPDocument(),
        MTPVector<MTPDocument>(),
        MTPPhoto(),
        MTPint(),
        MTP_int(ttlSeconds));
}

[[nodiscard]] int EphemeralTtlSeconds(const QJsonObject &ephemeral) {
    if (ephemeral.value("mode").toString() == u"once"_q) {
        return kMediaTtlOnce;
    }
    const auto seconds = ephemeral.value("ttl_seconds").toInt();
    return (seconds > 0) ? seconds : kMediaTtlOnce;
}

bool AttachmentIsPhoto(const QJsonObject &attachment, bool forceFile) {
    if (forceFile || attachment.value("as_file").toBool()) {
        return false;
    }
    return AttachmentMime(attachment).startsWith(u"image/"_q);
}

constexpr auto kCdnSmallSide = 100;
constexpr auto kCdnThumbnailSide = 320;
constexpr auto kCdnLargeSide = 1280;

bool IsCdnOriginalUrl(const QString &url) {
    static const auto kShaPath = QRegularExpression(
        u"^/[0-9a-fA-F]{64}/?$"_q);
    const auto parsed = QUrl(url);
    return parsed.isValid()
        && !parsed.host().isEmpty()
        && !parsed.hasQuery()
        && kShaPath.match(parsed.path()).hasMatch();
}

bool MimeHasCdnPreview(const QString &mime) {
    return (mime == u"image/jpeg"_q)
        || (mime == u"image/jpg"_q)
        || (mime == u"image/png"_q)
        || (mime == u"image/webp"_q);
}

QString CdnPreviewUrl(const QString &url, int side) {
    auto base = url;
    while (base.endsWith('/')) {
        base.chop(1);
    }
    return base
        + u"/-/preview/%1x%1/quality/smart/format/webp"_q.arg(side);
}

QSize CdnPreviewSize(int width, int height, int side) {
    if (width <= 0 || height <= 0 || std::max(width, height) <= side) {
        return QSize(width, height);
    }
    const auto scale = float64(side) / std::max(width, height);
    return QSize(
        std::max(1, int(std::round(width * scale))),
        std::max(1, int(std::round(height * scale))));
}

bool FitsSide(int width, int height, int side) {
    return (width > 0) && (height > 0) && (std::max(width, height) <= side);
}

ImageWithLocation RemoteImage(
        const QString &url,
        int width,
        int height,
        int side,
        bool resizable,
        Main::Session *session = nullptr,
        const QString &fileUniqueId = QString()) {
    if (url.isEmpty()) {
        return ImageWithLocation{
            .location = ImageLocation(DownloadLocation(), width, height),
        };
    } else if (!resizable
        || !IsCdnOriginalUrl(url)
        || FitsSide(width, height, side)) {
        if (session) {
            Streaming::RememberFileCacheKey(session, url, fileUniqueId, u"original"_q);
        }
        return ImageWithLocation{
            .location = ImageLocation(
                DownloadLocation{ PlainUrlLocation{ url } },
                width,
                height),
        };
    }
    const auto size = CdnPreviewSize(width, height, side);
    const auto previewUrl = CdnPreviewUrl(url, side);
    if (session) {
        Streaming::RememberFileCacheKey(session, previewUrl, fileUniqueId,
            u"preview-webp-smart-v1-%1"_q.arg(side));
    }
    return ImageWithLocation{
        .location = ImageLocation(
            DownloadLocation{ PlainUrlLocation{ previewUrl } },
            size.width(),
            size.height()),
    };
}

void UpdateRemotePhotoImages(
        not_null<PhotoData*> photo,
        const QString &url,
        int width,
        int height,
        bool resizable,
        const QString &fileUniqueId = QString()) {
    const auto sized = resizable && !url.isEmpty() && IsCdnOriginalUrl(url);
    const auto image = [&](int side) {
        return (sized && !FitsSide(width, height, side))
            ? RemoteImage(url, width, height, side, true, &photo->session(), fileUniqueId)
            : ImageWithLocation();
    };
    photo->updateImages(
        QByteArray(),
        image(kCdnSmallSide),
        image(kCdnThumbnailSide),
        RemoteImage(url, width, height, kCdnLargeSide, resizable, &photo->session(), fileUniqueId),
        ImageWithLocation(),
        ImageWithLocation(),
        0);
}

bool AttachmentPlaysOnce(const QJsonObject &attachment) {
    const auto kind = attachment.value("kind").toString();
    return (kind == u"voice"_q) || (kind == u"video_note"_q);
}

void CacheLocalAttachmentPhoto(
        not_null<Main::Session*> session,
        const QJsonObject &attachment,
        const QImage &image,
        const QByteArray &bytes) {
    const auto fileUniqueId = attachment.value("file_unique_id").toString();
    const auto url = attachment.value("url").toString().trimmed();
    if (QUuid(fileUniqueId).isNull() || url.isEmpty()) {
        return;
    }
    const auto resizable = MimeHasCdnPreview(AttachmentMime(attachment))
        && IsCdnOriginalUrl(url);
    for (const auto side : { kCdnSmallSide, kCdnThumbnailSide, kCdnLargeSide }) {
        const auto remote = RemoteImage(url, image.width(), image.height(),
            side, resizable, session, fileUniqueId);
        auto cached = bytes;
        if (resizable && !FitsSide(image.width(), image.height(), side)) {
            cached.clear();
            QBuffer buffer(&cached);
            buffer.open(QIODevice::WriteOnly);
            image.scaled(CdnPreviewSize(image.width(), image.height(), side),
                Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG");
        }
        session->data().cache().putIfEmpty(remote.location.file().cacheKey(),
            Storage::Cache::Database::TaggedValue(std::move(cached), Data::kImageCacheTag));
        if (!resizable || FitsSide(image.width(), image.height(), side)) {
            break;
        }
    }
}

MTPPhoto AttachmentPhoto(
        not_null<Main::Session*> session,
        const QJsonObject &attachment,
        const QByteArray &bytes) {
    const auto attachmentId = attachment.value("id").toVariant().toLongLong();
    const auto mediaId = kAttachmentMediaIdOffset + attachmentId;
    auto image = QImage();
    if (!bytes.isEmpty()) {
        image.loadFromData(bytes);
    }
    auto width = attachment.value("width").toInt();
    auto height = attachment.value("height").toInt();
    if (!image.isNull()) {
        width = image.width();
        height = image.height();
    }
    if (width <= 0 || height <= 0) {
        width = height = kUnknownPhotoSide;
    }
    auto sizes = QVector<MTPPhotoSize>();
    auto thumbs = PreparedPhotoThumbs();
    if (!image.isNull()) {
        const auto preview = image.scaled(
            320,
            320,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
        auto fullBytes = QByteArray();
        QBuffer buffer(&fullBytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPG", 87);
        sizes.push_back(MTP_photoSize(
            MTP_string("m"),
            MTP_int(preview.width()),
            MTP_int(preview.height()),
            MTP_int(0)));
        thumbs.emplace('m', PreparedPhotoThumb{ .image = preview });
        thumbs.emplace('y', PreparedPhotoThumb{
            .image = image,
            .bytes = fullBytes,
        });
    }
    sizes.push_back(MTP_photoSize(
        MTP_string("y"),
        MTP_int(width),
        MTP_int(height),
        MTP_int(0)));
    const auto photo = MTP_photo(
        MTP_flags(0),
        MTP_long(mediaId),
        MTP_long(mediaId),
        MTP_bytes(),
        MTP_int(base::unixtime::now()),
        MTP_vector<MTPPhotoSize>(sizes),
        MTPVector<MTPVideoSize>(),
        MTP_int(0));
    if (!thumbs.empty()) {
        CacheLocalAttachmentPhoto(session, attachment, image, bytes);
        session->data().processPhoto(photo, thumbs);
        return photo;
    }
    const auto url = attachment.value("url").toString().trimmed();
    UpdateRemotePhotoImages(
        session->data().photo(mediaId),
        url,
        width,
        height,
        MimeHasCdnPreview(AttachmentMime(attachment)),
        attachment.value("file_unique_id").toString());
    return photo;
}

void ApplyAttachmentSource(
        not_null<DocumentData*> document,
        const QJsonObject &attachment) {
    const auto url = attachment.value("url").toString().trimmed();
    if (!url.isEmpty()) {
        const auto previousLocation = document->location(true);
        const auto playlist = HlsPlaylist(attachment);
        Streaming::RememberFileCacheKey(&document->session(), url,
            attachment.value("file_unique_id").toString(),
            playlist.isEmpty() ? u"original"_q : u"hls-stream"_q);
        document->setContentUrl(url);
        Streaming::RememberSource(document, url);
        Streaming::RememberPlaylist(document, playlist);
        if (Streaming::FileCacheKey(url) && previousLocation.check()) {
            document->session().local().writeFileLocation(document->mediaKey(), previousLocation);
        }
    }
    const auto poster = attachment.value("poster_url").toString().trimmed();
    if (poster.isEmpty()) {
        return;
    }
    auto width = attachment.value("width").toInt();
    auto height = attachment.value("height").toInt();
    if (width <= 0 || height <= 0) {
        width = height = kUnknownPhotoSide;
    }
    document->updateThumbnails(
        InlineImageLocation(),
        RemoteImage(poster, width, height, kCdnLargeSide, true,
            &document->session(), attachment.value("poster_file_unique_id").toString()),
        ImageWithLocation(),
        false);
}

MTPMessageMedia MediaFromAttachment(
        not_null<Main::Session*> session,
        const QJsonObject &attachment,
        const LocalAttachment &local,
        std::optional<int> videoTimestamp = std::nullopt,
        int ttlSeconds = 0) {
    const auto &bytes = local.bytes;
    const auto attachmentId = attachment.value("id").toVariant().toLongLong();
    const auto mediaId = kAttachmentMediaIdOffset + attachmentId;
    auto name = attachment.value("name").toString().isEmpty()
        ? u"attachment"_q
        : attachment.value("name").toString();
    const auto mime = AttachmentMime(attachment);
    auto size = attachment.value("size").toVariant().toLongLong() > 0
        ? attachment.value("size").toVariant().toLongLong()
        : bytes.size();
    if (!HlsPlaylist(attachment).isEmpty()) {
        size = attachment.value("stream_size").toVariant().toLongLong();
        name = QFileInfo(name).completeBaseName() + u".m2ts"_q;
    }
    if (AttachmentIsPhoto(attachment, local.forceFile)) {
        using Flag = MTPDmessageMediaPhoto::Flag;
        auto photoFlags = Flag::f_photo | Flag();
        if (attachment.value("spoiler").toBool()) {
            photoFlags |= Flag::f_spoiler;
        }
        if (ttlSeconds > 0) {
            photoFlags |= Flag::f_ttl_seconds;
        }
        return MTP_messageMediaPhoto(
            MTP_flags(photoFlags),
            AttachmentPhoto(session, attachment, bytes),
            MTP_int(ttlSeconds),
            MTPDocument());
    }

    auto width = attachment.value("width").toInt();
    auto height = attachment.value("height").toInt();
    if (width <= 0 || height <= 0) {
        width = height = kUnknownPhotoSide;
    }
    const auto kind = attachment.value("kind").toString();
    const auto durationSeconds = attachment
        .value("duration_ms").toVariant().toLongLong() / 1000.;

    auto attributes = QVector<MTPDocumentAttribute>();
    attributes.push_back(MTP_documentAttributeFilename(MTP_string(name)));
    if (kind == u"voice"_q || kind == u"audio"_q) {
        using Flag = MTPDdocumentAttributeAudio::Flag;
        auto flags = Flag() | Flag();
        auto waveform = QByteArray();
        if (kind == u"voice"_q) {
            flags |= Flag::f_voice;
            waveform = QByteArray::fromBase64(
                attachment.value("waveform").toString().toUtf8());
            if (!waveform.isEmpty()) {
                flags |= Flag::f_waveform;
            }
        }
        auto title = attachment.value("title").toString().trimmed();
        if (title.isEmpty()) {
            title = attachment.value("name").toString();
        }
        const auto performer = attachment.value("performer").toString();
        if (!title.isEmpty()) flags |= Flag::f_title;
        if (!performer.isEmpty()) flags |= Flag::f_performer;
        attributes.push_back(MTP_documentAttributeAudio(
            MTP_flags(flags),
            MTP_int(int(durationSeconds)),
            MTP_string(title),
            MTP_string(performer),
            MTP_bytes(waveform)));
    } else if (kind == u"video"_q
        || kind == u"animation"_q
        || kind == u"video_note"_q
        || (kind.isEmpty() && mime.startsWith(u"video/"_q))) {
        using Flag = MTPDdocumentAttributeVideo::Flag;
        auto flags = Flag::f_supports_streaming | Flag();
        if (kind == u"video_note"_q) {
            flags = Flag::f_round_message | Flag::f_supports_streaming;
        }
        attributes.push_back(MTP_documentAttributeVideo(
            MTP_flags(flags),
            MTP_double(durationSeconds),
            MTP_int(width),
            MTP_int(height),
            MTPint(),
            MTPdouble(),
            MTPstring()));
        if (kind == u"animation"_q) {
            attributes.push_back(MTP_documentAttributeAnimated());
        }
    } else if (mime.startsWith(u"image/"_q)) {
        attributes.push_back(MTP_documentAttributeImageSize(
            MTP_int(width),
            MTP_int(height)));
    }
    const auto document = MTP_document(
        MTP_flags(0),
        MTP_long(mediaId),
        MTP_long(0),
        MTP_bytes(),
        MTP_int(base::unixtime::now()),
        MTP_string(mime),
        MTP_long(size),
        MTPVector<MTPPhotoSize>(),
        MTPVector<MTPVideoSize>(),
        MTP_int(0),
        MTP_vector<MTPDocumentAttribute>(attributes));
    const auto data = session->data().document(mediaId);
    if (!local.path.isEmpty() && QFileInfo::exists(local.path)) {
        data->setLocation(Core::FileLocation(local.path));
    }
    ApplyAttachmentSource(data, attachment);
    Gifs::RememberSource(
        session,
        mediaId,
        attachment.value("sha256").toString());
    session->data().processDocument(document);
    if (!bytes.isEmpty()) {
        data->setDataAndCache(bytes);
    }
    using Flag = MTPDmessageMediaDocument::Flag;
    auto mediaFlags = Flag::f_document | Flag();
    if (attachment.value("spoiler").toBool()) {
        mediaFlags |= Flag::f_spoiler;
    }
    if (kind == u"video_note"_q) {
        mediaFlags |= Flag::f_round;
    } else if (kind == u"voice"_q) {
        mediaFlags |= Flag::f_voice;
    } else if (kind == u"video"_q || kind == u"animation"_q) {
        mediaFlags |= Flag::f_video;
    }
    if (videoTimestamp.has_value()) {
        mediaFlags |= Flag::f_video_timestamp;
    }
    if (ttlSeconds > 0) {
        mediaFlags |= Flag::f_ttl_seconds;
    }
    return MTP_messageMediaDocument(
        MTP_flags(mediaFlags),
        document,
        MTPVector<MTPDocument>(),
        MTPPhoto(),
        MTP_int(videoTimestamp.value_or(0)),
        MTP_int(ttlSeconds));
}

constexpr auto kWebPageMediaIdOffset = qint64(2000000000000000LL);
constexpr auto kWebPageMediaIdRange = qint64(1000000000000000LL);

qint64 WebPageStableId(const QString &url) {
    const auto digest = QCryptographicHash::hash(
        url.toUtf8(),
        QCryptographicHash::Sha256);
    auto value = quint64(0);
    const auto size = std::min(qsizetype(8), digest.size());
    for (auto i = qsizetype(0); i != size; ++i) {
        value = (value << 8) | quint8(digest.at(i));
    }
    return kWebPageMediaIdOffset + qint64(value % quint64(kWebPageMediaIdRange));
}

MTPPhoto WebPagePhoto(
        not_null<Main::Session*> session,
        const QString &url,
        int width,
        int height) {
    const auto mediaId = WebPageStableId(url);
    if (width <= 0 || height <= 0) {
        width = height = kUnknownPhotoSide;
    }
    const auto photo = MTP_photo(
        MTP_flags(0),
        MTP_long(mediaId),
        MTP_long(mediaId),
        MTP_bytes(),
        MTP_int(base::unixtime::now()),
        MTP_vector<MTPPhotoSize>(QVector<MTPPhotoSize>{
            MTP_photoSize(
                MTP_string("y"),
                MTP_int(width),
                MTP_int(height),
                MTP_int(0)),
        }),
        MTPVector<MTPVideoSize>(),
        MTP_int(0));
    UpdateRemotePhotoImages(
        session->data().photo(mediaId),
        url,
        width,
        height,
        true);
    return photo;
}

std::optional<MTPMessageMedia> MediaFromWebPage(
        not_null<Main::Session*> session,
        const QJsonObject &webPage) {
    const auto url = webPage.value("url").toString().trimmed();
    if (url.isEmpty()) {
        return std::nullopt;
    }
    const auto id = WebPageStableId(url);
    const auto displayUrl = webPage.value("display_url").toString().trimmed();
    if (webPage.value("pending").toBool()) {
        return std::nullopt;
    }
    const auto type = webPage.value("type").toString().trimmed();
    const auto siteName = webPage.value("site_name").toString().trimmed();
    const auto title = webPage.value("title").toString().trimmed();
    const auto description = webPage.value("description").toString().trimmed();
    const auto imageUrl = webPage.value("image_url").toString().trimmed();
    if (type.isEmpty()
        && siteName.isEmpty()
        && title.isEmpty()
        && description.isEmpty()
        && imageUrl.isEmpty()) {
        return std::nullopt;
    }
    using Flag = MTPDwebPage::Flag;
    auto flags = MTPDwebPage::Flags();
    if (!type.isEmpty()) flags |= Flag::f_type;
    if (!siteName.isEmpty()) flags |= Flag::f_site_name;
    if (!title.isEmpty()) flags |= Flag::f_title;
    if (!description.isEmpty()) flags |= Flag::f_description;
    if (!imageUrl.isEmpty()) {
        flags |= Flag::f_photo;
        flags |= Flag::f_has_large_media;
    }
    using MediaFlag = MTPDmessageMediaWebPage::Flag;
    auto mediaFlags = MTPDmessageMediaWebPage::Flags();
    const auto large = webPage.value("large").toBool();
    const auto small = webPage.value("small").toBool();
    if (large || small || webPage.value("above").toBool()) {
        mediaFlags |= MediaFlag::f_manual;
    }
    if (small) {
        mediaFlags |= MediaFlag::f_force_small_media;
    } else if (large) {
        mediaFlags |= MediaFlag::f_force_large_media;
    }
    return MTP_messageMediaWebPage(
        MTP_flags(mediaFlags),
        MTP_webPage(
            MTP_flags(flags),
            MTP_long(id),
            MTP_string(url),
            MTP_string(displayUrl.isEmpty() ? url : displayUrl),
            MTP_int(0),
            MTP_string(type),
            MTP_string(siteName),
            MTP_string(title),
            MTP_string(description),
            imageUrl.isEmpty()
                ? MTPPhoto()
                : WebPagePhoto(
                    session,
                    imageUrl,
                    webPage.value("image_width").toInt(),
                    webPage.value("image_height").toInt()),
            MTPstring(),
            MTPstring(),
            MTPint(),
            MTPint(),
            MTPint(),
            MTPstring(),
            MTPDocument(),
            MTPPage(),
            MTPVector<MTPWebPageAttribute>()));
}

QString UserDisplay(const QJsonObject &user) {
    const auto display = user.value("display_name").toString().trimmed();
    return display.isEmpty() ? user.value("username").toString() : display;
}

QString DefaultNotifyScope(Data::DefaultNotify type) {
	switch (type) {
	case Data::DefaultNotify::Group: return u"group"_q;
	case Data::DefaultNotify::Broadcast: return u"channel"_q;
	case Data::DefaultNotify::User: break;
	}
	return u"user"_q;
}

std::optional<Data::DefaultNotify> DefaultNotifyType(const QString &scope) {
	if (scope.isEmpty() || scope == u"user"_q) {
		return Data::DefaultNotify::User;
	} else if (scope == u"group"_q) {
		return Data::DefaultNotify::Group;
	} else if (scope == u"channel"_q) {
		return Data::DefaultNotify::Broadcast;
	}
	return std::nullopt;
}

constexpr auto kChatPhotoVideoSide = 800;

PhotoId AvatarPhotoId(const QString &revision) {
	const auto digest = QCryptographicHash::hash(
		revision.toUtf8(),
		QCryptographicHash::Sha256);
	auto value = uint64_t(0);
	const auto size = std::min(qsizetype(8), digest.size());
	for (auto i = qsizetype(0); i != size; ++i) {
		value = (value << 8) | uint8_t(digest.at(i));
	}
	return PhotoId(value ? value : 1);
}

PhotoId ChatPhotoId(const QString &url, const QString &videoUrl) {
	return AvatarPhotoId(videoUrl.isEmpty() ? url : (url + '\n' + videoUrl));
}

Data::AllowedReactions ParseAllowedReactions(
        not_null<Main::Session*> session,
        const QJsonObject &chat) {
    Data::AllowedReactions result;
    result.type = Data::AllowedReactionsType::Some;
    const auto chatMax = chat.value("max_selected").toInt();
    result.maxCount = chatMax > 0
        ? chatMax
        : Reactions::MaxSelectedReactions(session);
    const auto reactions = chat.value("available_reactions").toArray();
    for (const auto &entry : reactions) {
        const auto emoji = entry.toObject().value("emoji").toString();
        if (!emoji.isEmpty()) {
            result.some.push_back(Data::ReactionId{ emoji });
        }
    }
    if (result.some.empty()) {
        result.type = Data::AllowedReactionsType::All;
    }
    return result;
}

MTPMessageReplyHeader ReplyHeaderFrom(
        const QJsonObject &message,
        PeerId externalPeer) {
    const auto replyId = message.value("reply_to_id").toVariant().toLongLong();
    if (replyId <= 0 || replyId > INT32_MAX) {
        return MTPMessageReplyHeader();
    }
    using Flag = MTPDmessageReplyHeader::Flag;
    const auto external = message.value("reply_to").toObject();
    if (external.isEmpty() || !externalPeer) {
        const auto root = message.value("thread_root_id").toVariant().toLongLong();
        const auto inThread = root > 0 && root <= INT32_MAX && root != replyId;
        return MTP_messageReplyHeader(
            MTP_flags(Flag::f_reply_to_msg_id
                | (inThread ? Flag::f_reply_to_top_id : Flag())),
            MTP_int(int(replyId)),
            MTPPeer(),
            MTPMessageFwdHeader(),
            MTPMessageMedia(),
            MTP_int(inThread ? int(root) : 0),
            MTP_string(QString()),
            MTPVector<MTPMessageEntity>(),
            MTPint(),
            MTPint(),
            MTP_bytes(QByteArray()));
    }
    const auto authorId = external.value("author_id").toVariant().toLongLong();
    const auto authorName = external.value("author_name").toString();
    const auto quote = external.value("text").toString();
    const auto channelId = external.value("channel").toObject()
        .value("id").toVariant().toLongLong();
    const auto signature = external.value("post_author").toString();
    auto flags = Flag::f_reply_to_msg_id | Flag::f_reply_to_peer_id | Flag();
    if (!authorName.isEmpty() || authorId > 0 || channelId > 0) {
        flags |= Flag::f_reply_from;
    }
    if (!quote.isEmpty()) {
        flags |= Flag::f_quote_text;
    }
    using FwdFlag = MTPDmessageFwdHeader::Flag;
    auto fwdFlags = MTPDmessageFwdHeader::Flags();
    if (authorId > 0 || channelId > 0) {
        fwdFlags |= FwdFlag::f_from_id;
    }
    if (!authorName.isEmpty()) {
        fwdFlags |= FwdFlag::f_from_name;
    }
    if (channelId > 0) {
        fwdFlags |= FwdFlag::f_channel_post;
        if (!signature.isEmpty()) {
            fwdFlags |= FwdFlag::f_post_author;
        }
    }
    return MTP_messageReplyHeader(
        MTP_flags(flags),
        MTP_int(int(replyId)),
        peerToMTP(externalPeer),
        MTP_messageFwdHeader(
            MTP_flags(fwdFlags),
            (channelId > 0
                ? MTP_peerChannel(MTP_long(channelId))
                : (authorId > 0)
                ? MTP_peerUser(MTP_long(authorId))
                : MTPPeer()),
            MTP_string(authorName),
            MTP_int(0),
            (channelId > 0 ? MTP_int(int(replyId)) : MTPint()),
            MTP_string(signature),
            MTPPeer(),
            MTPint(),
            MTPPeer(),
            MTP_string(QString()),
            MTPint(),
            MTP_string(QString())),
        MTPMessageMedia(),
        MTPint(),
        MTP_string(quote),
        MTPVector<MTPMessageEntity>(),
        MTPint(),
        MTPint(),
        MTP_bytes(QByteArray()));
}

} // namespace

ReplyTarget ReplyTargetFrom(History *history, const FullReplyTo &replyTo) {
    if (!replyTo.messageId) {
        return {};
    }
    const auto peer = (history && replyTo.messageId.peer == history->peer->id)
        ? PeerId()
        : replyTo.messageId.peer;
    return ReplyTarget{ .messageId = replyTo.messageId.msg.bare, .peer = peer };
}

NativeBridge::NativeBridge(Main::Session *session)
: _session(session) {
	_readRetryTimer.setSingleShot(true);
	connect(&_readRetryTimer, &QTimer::timeout, this, [this] { flushReadJournal(); });
	_deliveredTimer.setSingleShot(true);
	connect(&_deliveredTimer, &QTimer::timeout, this, [this] { flushDelivered(); });
	loadReadJournal();
    Ui::ThanosEffect::WarmUp();
    _session->user()->addFlags(UserDataFlag::Premium);
    const auto me = CurrentUser(_session);
    if (!me.isEmpty()) {
        ensureUser(me, false);
    }
    refreshSelf();
    for (const auto type : {
            Data::DefaultNotify::User,
            Data::DefaultNotify::Group,
            Data::DefaultNotify::Broadcast }) {
        const auto cached = LoadDefaultNotifyCache(
            _session,
            DefaultNotifyScope(type));
        _defaultNotify[size_t(type)].revision = cached.value(
            "settings_revision").toVariant().toLongLong();
        applyDefaultNotifySettings(
            type,
            cached.value("mute_until").toVariant().toLongLong(),
            cached.value("sound_none").toBool());
    }
    loadDefaultNotifySettings();
    Wallpapers::RequestDefault(_session);
    ChatThemes::Request(_session);
    loadContacts();
    refreshReactionsCatalog();
    QTimer::singleShot(0, this, [this] {
        loadCachedChats();
        reloadChats();
    });
	_eventSeq = std::max<qint64>(0, client().eventSequence());
	_liveUpdates = std::make_unique<LiveUpdatesConnection>(
		this,
		[=] { return client().websocketRequest(_eventSeq); },
		[=] {
			reloadChats();
			updatePresence();
			resendPendingSends();
			if (Reactions::Catalog(_session).empty()) {
				refreshReactionsCatalog();
			}
		},
		[=](QString message) { onWebSocketMessage(message); });
	for (const auto &controller : _session->windows()) {
		trackWindow(controller);
	}
	if (!client().accessToken().isEmpty()) {
		startLiveUpdates();
		QTimer::singleShot(0, this, [this] { flushReadJournal(); });
	}
}

NativeBridge::~NativeBridge() {
	_readRetryTimer.stop();
	for (auto &entryPair : _readJournal) {
		auto &entry = entryPair.second;
		for (auto &completion : entry.completions) {
			if (completion) completion();
		}
	}
	_liveUpdates->stop();
}

ApiClient &NativeBridge::client() const {
    return ClientFor(_session);
}

int32_t NativeBridge::unixTime(const QString &value) {
	if (value.isEmpty()) {
		return 0;
	}
	const auto parsed = QDateTime::fromString(value, Qt::ISODate);
	return parsed.isValid()
		? int32_t(parsed.toSecsSinceEpoch())
		: 0;
}

QString NativeBridge::renderMessageText(const QJsonObject &message) {
    return message.value("text").toString();
}

MTPVector<MTPMessageEntity> NativeBridge::renderMessageEntities(
        const QJsonObject &message) {
    const auto list = message.value("entities").toArray();
    if (list.isEmpty()) {
        return MTPVector<MTPMessageEntity>();
    }
    const auto text = renderMessageText(message);
    auto result = QVector<MTPMessageEntity>();
    result.reserve(list.size());
    for (const auto &value : list) {
        if (!value.isObject()) continue;
        const auto entity = value.toObject();
        const auto offset = entity.value("offset").toInt();
        const auto length = entity.value("length").toInt();
        if (offset < 0 || length <= 0) continue;
        const auto type = entity.value("type").toString();
        const auto data = entity.value("data").toString();
        const auto from = MTP_int(offset);
        const auto count = MTP_int(length);
        if (type == u"bold"_q) {
            result.push_back(MTP_messageEntityBold(from, count));
        } else if (type == u"italic"_q) {
            result.push_back(MTP_messageEntityItalic(from, count));
        } else if (type == u"underline"_q) {
            result.push_back(MTP_messageEntityUnderline(from, count));
        } else if (type == u"strikethrough"_q) {
            result.push_back(MTP_messageEntityStrike(from, count));
        } else if (type == u"code"_q) {
            result.push_back(MTP_messageEntityCode(from, count));
        } else if (type == u"spoiler"_q) {
            result.push_back(MTP_messageEntitySpoiler(from, count));
        } else if (type == u"pre"_q) {
            result.push_back(MTP_messageEntityPre(from, count, MTP_string(data)));
        } else if (type == u"blockquote"_q) {
            result.push_back(MTP_messageEntityBlockquote(
                MTP_flags(MTPDmessageEntityBlockquote::Flags()),
                from,
                count));
        } else if (type == u"text_url"_q) {
            if (data.isEmpty()) continue;
            result.push_back(LinkTargetIsTheText(data, text, offset, length)
                ? MTP_messageEntityUrl(from, count)
                : MTP_messageEntityTextUrl(from, count, MTP_string(data)));
        } else if (type == u"url"_q) {
            result.push_back(MTP_messageEntityUrl(from, count));
        } else if (type == u"mention"_q) {
            result.push_back(MTP_messageEntityMention(from, count));
        } else if (type == u"hashtag"_q) {
            result.push_back(MTP_messageEntityHashtag(from, count));
        } else if (type == u"email"_q) {
            result.push_back(MTP_messageEntityEmail(from, count));
        } else if (type == u"custom_emoji"_q) {
            const auto emojiId = DocumentId(
                entity.value("emoji_id").toVariant().toLongLong());
            if (!emojiId) continue;
            result.push_back(MTP_messageEntityCustomEmoji(
                from,
                count,
                MTP_long(emojiId)));
        }
    }
    return result.isEmpty()
        ? MTPVector<MTPMessageEntity>()
        : MTP_vector<MTPMessageEntity>(result);
}

std::optional<MTPMessageMedia> NativeBridge::WebPageMedia(
        Main::Session *session,
        const QJsonObject &webPage) {
    return session ? MediaFromWebPage(session, webPage) : std::nullopt;
}

QJsonArray NativeBridge::entitiesToJson(
        const EntitiesInText &entities,
        const QString &text,
        const Data::WebPageDraft &webPage) const {
    const auto tuned = webPage.removed
        || webPage.invert
        || webPage.forceLargeMedia
        || webPage.forceSmallMedia;
    auto previewAssigned = false;
    auto result = QJsonArray();
    for (const auto &entity : entities) {
        auto type = QString();
        auto data = QString();
        auto emojiId = DocumentId(0);
        switch (entity.type()) {
        case EntityType::Bold: type = u"bold"_q; break;
        case EntityType::Italic: type = u"italic"_q; break;
        case EntityType::Underline: type = u"underline"_q; break;
        case EntityType::StrikeOut: type = u"strikethrough"_q; break;
        case EntityType::Code: type = u"code"_q; break;
        case EntityType::Spoiler: type = u"spoiler"_q; break;
        case EntityType::Pre:
            type = u"pre"_q;
            data = entity.data();
            break;
        case EntityType::Blockquote: type = u"blockquote"_q; break;
        case EntityType::Mention: type = u"mention"_q; break;
        case EntityType::CustomEmoji:
            type = u"custom_emoji"_q;
            emojiId = Data::ParseCustomEmojiData(entity.data());
            data = Reactions::AssetUrlFor(_session, emojiId);
            break;
        case EntityType::CustomUrl:
            type = u"text_url"_q;
            data = entity.data();
            break;
        case EntityType::Url:
            type = u"text_url"_q;
            data = ((entity.offset() >= 0)
                && (entity.length() > 0)
                && (entity.offset() + entity.length() <= text.size()))
                ? qthelp::validate_url(
                    text.mid(entity.offset(), entity.length()))
                : QString();
            if (data.isEmpty()) continue;
            break;
        default: continue;
        }
        if (entity.length() <= 0) continue;
        auto object = QJsonObject{
            {"type", type},
            {"offset", entity.offset()},
            {"length", entity.length()},
        };
        if (!data.isEmpty()) object.insert("data", data);
        if (emojiId) object.insert("emoji_id", qint64(emojiId));
        if (tuned && !previewAssigned && (type == u"text_url"_q)) {
            const auto chosen = webPage.url.isEmpty()
                || !NormalizedLinkTarget(webPage.url).compare(
                    NormalizedLinkTarget(data),
                    Qt::CaseInsensitive);
            if (chosen) {
                previewAssigned = true;
                if (webPage.removed) object.insert("preview_disabled", true);
                if (webPage.invert) object.insert("preview_above", true);
                if (webPage.forceLargeMedia) object.insert("preview_large", true);
                if (webPage.forceSmallMedia) object.insert("preview_small", true);
            }
        }
        result.append(object);
    }
    return result;
}

namespace {

struct LocalAuthor {
    MessageFlags flags;
    PeerId from;
    QString postAuthor;
};

LocalAuthor LocalMessageAuthor(not_null<History*> history) {
    const auto action = Api::SendAction(history);
    auto flags = NewMessageFlags(history->peer);
    Api::FillMessagePostFlags(action, history->peer, flags);
    return {
        .flags = flags,
        .from = NewMessageFromId(action),
        .postAuthor = NewMessagePostAuthor(action),
    };
}

} // namespace

HistoryItem *NativeBridge::createPendingTextMessage(
        History *history,
        const QString &text,
        const EntitiesInText &entities,
        ReplyTarget replyTo,
        MsgId localMessageId) {
    if (!history) {
        return nullptr;
    }
    const auto author = LocalMessageAuthor(history);
    auto flags = author.flags;
    if (replyTo) {
        flags |= MessageFlag::HasReplyInfo;
    }
    return history->addNewLocalMessage({
        .id = localMessageId,
        .flags = flags,
        .from = author.from,
        .replyTo = ReplyToFromServerId(history, replyTo),
        .date = base::unixtime::now(),
        .postAuthor = author.postAuthor,
    }, TextWithEntities{ text, entities }, MTP_messageMediaEmpty()).get();
}

HistoryItem *NativeBridge::createPendingFileMessage(
        History *history,
        const UploadSpec &file,
        const TextWithEntities &caption,
        ReplyTarget replyTo,
        const LocalAttachment &local,
        uint64 groupedId,
        int mediaTtlSeconds,
        std::shared_ptr<Data::DocumentMedia> &keepMedia) {
    if (!history) {
        return nullptr;
    }
    const auto author = LocalMessageAuthor(history);
    auto flags = author.flags;
    if (replyTo) {
        flags |= MessageFlag::HasReplyInfo;
    }
    if (mediaTtlSeconds != 0) {
        flags |= MessageFlag::MediaIsUnread;
    }
    const auto localId = _session->data().nextLocalMessageId();
    const auto attachment = AttachmentObjectFromUpload(
        localId.bare,
        file,
        local.bytes);
    if (!local.bytes.isEmpty()
        && local.path.isEmpty()
        && !AttachmentIsPhoto(attachment, local.forceFile)) {
        keepMedia = _session->data().document(
            LocalAttachmentMediaId(localId.bare))->createMediaView();
    }
    return history->addNewLocalMessage({
        .id = localId,
        .flags = flags,
        .from = author.from,
        .replyTo = ReplyToFromServerId(history, replyTo),
        .date = base::unixtime::now(),
        .postAuthor = author.postAuthor,
        .groupedId = groupedId,
    }, caption, MediaFromAttachment(
        _session,
        attachment,
        local,
        std::nullopt,
        mediaTtlSeconds)).get();
}

void NativeBridge::rememberPendingSend(
        not_null<HistoryItem*> item,
        QString clientNonce,
        ReplyTarget replyTo,
        QString text,
        EntitiesInText entities,
        Data::WebPageDraft webPage,
        TextWithEntities caption,
        std::vector<UploadSpec> files,
        LocalAttachment local,
        bool clearDraft,
        MsgId draftTopicRootId,
        PeerId draftMonoforumPeerId) {
    auto request = PendingSendRequest();
    request.history = item->history().get();
    request.clientNonce = clientNonce;
    request.text = std::move(text);
    request.entities = std::move(entities);
    request.webPage = std::move(webPage);
    request.caption = std::move(caption);
    request.replyTo = replyTo;
    request.draftTopicRootId = draftTopicRootId;
    request.draftMonoforumPeerId = draftMonoforumPeerId;
    request.draftSaving = clearDraft;
    request.files = std::move(files);
    request.localAttachment = std::move(local);
    request.inFlight = true;
    _pendingSendNonceToLocalId[request.clientNonce] = item->id.bare;
    _pendingSends[item->id.bare] = std::move(request);
}

void NativeBridge::failPendingSend(qint64 localId, int status) {
    if (!localId) {
        return;
    }
    const auto i = _pendingSends.find(localId);
    if (i == _pendingSends.end()) {
        return;
    }
    if (i->second.cancelledAfterCommit) {
        finishPendingDraftSave(localId, base::unixtime::now());
        clearPendingSend(localId);
        return;
    }
    if (!SendMayBeRetried(status)) {
        const auto history = i->second.history;
        finishPendingDraftSave(localId, base::unixtime::now());
        clearPendingSend(localId);
        if (history) {
            if (const auto item = _session->data().message(
                    history->peer,
                    MsgId(localId))) {
                item->destroy();
            }
        }
        _session->data().sendHistoryChangeNotifications();
        return;
    }
    i->second.inFlight = false;
    i->second.committing = false;
    i->second.cancelUpload = nullptr;
    if (const auto history = i->second.history) {
        if (const auto item = _session->data().message(history->peer, MsgId(localId))) {
            if (item->isSending()) {
                item->sendFailed();
            }
        }
    }
    finishPendingDraftSave(localId, base::unixtime::now());
    _session->data().sendHistoryChangeNotifications();
}

void NativeBridge::finishPendingDraftSave(qint64 localId, TimeId savedAt) {
    const auto i = _pendingSends.find(localId);
    if (i == _pendingSends.end() || !i->second.draftSaving) {
        return;
    }
    i->second.draftSaving = false;
    if (const auto history = i->second.history) {
        history->finishSavingCloudDraft(
            i->second.draftTopicRootId,
            i->second.draftMonoforumPeerId,
            savedAt);
    }
}

void NativeBridge::clearPendingSend(qint64 localId) {
    const auto i = _pendingSends.find(localId);
    if (i == _pendingSends.end()) {
        return;
    }
    _sendReplays.erase(i->second.clientNonce);
    _pendingSendNonceToLocalId.erase(i->second.clientNonce);
    _pendingSends.erase(i);
}

void NativeBridge::ensureUser(const QJsonObject &user, bool contact) {
    const auto id = user.value("id").toVariant().toLongLong();
    if (id <= 0) return;
    const auto data = _session->data().user(UserId(id));
    data->setName(
        UserDisplay(user),
        QString(),
        QString(),
        user.value("username").toString());
    if (!data->isLoaded()) {
        data->setLoadedStatus(PeerData::LoadedStatus::Normal);
    }
    data->addFlags(UserDataFlag::CanPinMessages);
    EnsureBarSettingsKnown(data);
    if (contact && id != client().meId()) {
        data->setIsContact(true);
    }
	const auto avatarId = user.value("avatar_id").toString().trimmed();
	const auto avatarUrl = user.value("avatar_url").toString().trimmed();
	const auto revision = avatarId.isEmpty() ? avatarUrl : avatarId;
	const auto previous = _avatarIds.find(id);
	if (previous != _avatarIds.end() && previous->second == revision) {
		return;
	}
	_avatarIds[id] = revision;
	if (avatarUrl.isEmpty()) {
		data->setUserpic(PhotoId(), ImageLocation(), false);
	} else {
		const auto photoId = AvatarPhotoId(revision);
		data->setUserpic(
			photoId,
			ImageLocation(
				DownloadLocation{ PlainUrlLocation{ avatarUrl } },
				160,
				160),
			false);
		UpdateRemotePhotoImages(
			_session->data().photo(photoId),
			avatarUrl,
			kUnknownPhotoSide,
			kUnknownPhotoSide,
			false);
	}
	_session->changes().peerUpdated(
		data,
		Data::PeerUpdate::Flag::Photo);
}

void NativeBridge::refreshSelf() {
	const auto weak = QPointer<NativeBridge>(this);
	client().me([weak](QJsonDocument doc, QString error, int) {
		if (!weak || !error.isEmpty() || !doc.isObject()) {
			return;
		}
		const auto user = doc.object();
		if (user.value("id").toVariant().toLongLong() <= 0) {
			return;
		}
		RememberUser(weak->_session, user);
		weak->ensureUser(user, false);
		auto ephemeral = false;
		for (const auto &value : user.value("capabilities").toArray()) {
			if (value.toString() == u"ephemeral-media-v1"_q) {
				ephemeral = true;
			}
		}
		weak->_ephemeralMediaSupported = ephemeral;
		weak->_canCreateGroups = user.value("can_create_group").toBool();
		weak->_canCreateChannels = user.value("can_create_channel").toBool();
		weak->_topicChannelsPrompt = false;
		for (const auto &value : user.value("prompts").toArray()) {
			if (value.toString() == u"topic-channels"_q) {
				weak->_topicChannelsPrompt = true;
			}
		}
		weak->offerTopicChannels();
	});
}

void NativeBridge::offerTopicChannels() {
	if (!_topicChannelsPrompt || _topicChannelsPromptOffered) {
		return;
	}
	const auto window = _session->tryResolveWindow();
	if (!window) {
		return;
	}
	_topicChannelsPromptOffered = true;
	const auto weak = QPointer<NativeBridge>(this);
	TopicChannels::OfferChannels(window, [weak] {
		if (!weak) {
			return;
		}
		weak->_topicChannelsPrompt = false;
		weak->client().communityRequest(
			"POST",
			u"/me/prompts/topic-channels/seen"_q,
			{},
			[](QJsonDocument, QString error, int status) {
				if (!error.isEmpty()) {
					LOG(("FoxMes: topic channels prompt not marked, "
						"status %1, %2").arg(status).arg(error));
				}
			});
	});
}

void NativeBridge::startLiveUpdates() {
	if (_eventSeq > 0) {
		_liveUpdates->start();
		return;
	}
	const auto weak = QPointer<NativeBridge>(this);
	client().me([weak](QJsonDocument doc, QString error, int) {
		if (!weak) {
			return;
		}
		const auto seq = doc.object().value(
			"event_seq").toVariant().toLongLong();
		if (error.isEmpty() && seq > 0 && weak->_eventSeq == 0) {
			weak->_eventSeq = seq;
			weak->client().setEventSequence(seq);
			RememberEventSequence(weak->_session, seq);
		}
		weak->_liveUpdates->start();
	});
}

void NativeBridge::loadContacts() {
	_contactsDone = true;
	finishInitialLoadIfReady();
}

void NativeBridge::refreshReactionsCatalog() {
    if (_reactionsCatalogLoading) {
        return;
    }
    _reactionsCatalogLoading = true;
    const auto weak = QPointer<NativeBridge>(this);
    client().reactionsCatalog([weak](
            QJsonDocument doc,
            QString error,
            int status) {
        if (!weak) {
            return;
        }
        weak->_reactionsCatalogLoading = false;
        if (!error.isEmpty() || !doc.isObject()) {
            LOG(("FoxMes: reactions catalog failed (%1, %2)"
                ).arg(status
                ).arg(error));
            return;
        }
        auto values = std::vector<Reactions::CatalogItem>();
        const auto object = doc.object();
        Reactions::SetUsageLists(
            weak->_session,
            ReactionUsageIds(object.value("top_reactions")),
            ReactionUsageIds(object.value("recent_reactions")));
        const auto array = object.value("available_reactions").toArray();
        for (const auto &entry : array) {
            const auto reaction = entry.toObject();
            const auto id = DocumentId(
                reaction.value("id").toVariant().toLongLong());
            const auto emoji = reaction.value("emoji").toString();
            if (!id || emoji.isEmpty()) {
                continue;
            }
            values.push_back(Reactions::CatalogItem{
                .id = id,
                .emoji = emoji,
                .assetUrl = reaction.value("asset_url").toString(),
                .category = reaction.value("category").toString(),
            });
        }
        if (!values.empty()) {
            Reactions::SetAvailableCatalog(weak->_session, values);
            const auto maxSelected = object.value("max_selected").toInt();
            if (maxSelected > 0) {
                Reactions::SetMaxSelectedReactions(
                    weak->_session,
                    maxSelected);
            }
            weak->scheduleReactionsRefresh();
        }
    });
}

void NativeBridge::scheduleReactionsRefresh() {
    if (_reactionsRefreshScheduled) {
        return;
    }
    _reactionsRefreshScheduled = true;
    const auto weak = QPointer<NativeBridge>(this);
    crl::on_main(this, [weak] {
        if (!weak) {
            return;
        }
        weak->_reactionsRefreshScheduled = false;
        weak->_session->data().reactions().refreshDefault();
    });
}

void NativeBridge::refreshReactionUsage() {
    const auto weak = QPointer<NativeBridge>(this);
    client().reactionUsage([weak](QJsonDocument doc, QString error, int) {
        if (!weak || !error.isEmpty() || !doc.isObject()) {
            return;
        }
        const auto object = doc.object();
        Reactions::SetUsageLists(
            weak->_session,
            ReactionUsageIds(object.value("top_reactions")),
            ReactionUsageIds(object.value("recent_reactions")));
        weak->scheduleReactionsRefresh();
    });
}

void NativeBridge::scheduleReactionUsageRefresh() {
    if (_reactionUsageRefreshScheduled) {
        return;
    }
    _reactionUsageRefreshScheduled = true;
    QTimer::singleShot(kReactionUsageRefreshDelayMs, this, [this] {
        _reactionUsageRefreshScheduled = false;
        refreshReactionUsage();
    });
}

PeerData *NativeBridge::peerForChat(const QJsonObject &chat) {
    const auto chatId = chat.value("id").toVariant().toLongLong();
    if (chatId <= 0) return nullptr;

    const auto type = chat.value("type").toString();
    if (type == u"encrypted"_q) {
        return nullptr;
    }
    const auto community = Community::IsCommunity(chat);
    if (!community && type != u"saved"_q && type != u"direct"_q) {
        return nullptr;
    }

    const auto members = chat.value("members").toArray();
    for (const auto &entry : members) {
        if (entry.isObject()) ensureUser(entry.toObject(), true);
    }

    PeerData *peer = nullptr;
    if (type == u"saved"_q) {
        peer = _session->user().get();
    } else if (type == u"direct"_q) {
        const auto me = client().meId();
        qint64 otherId = 0;
        QJsonObject other;
        for (const auto &entry : members) {
            const auto object = entry.toObject();
            const auto id = object.value("id").toVariant().toLongLong();
            if (id > 0 && id != me) {
                otherId = id;
                other = object;
                break;
            }
        }
        if (!otherId && !members.isEmpty()) {
            other = members.at(0).toObject();
            otherId = other.value("id").toVariant().toLongLong();
        }
        if (otherId <= 0) return nullptr;
        const auto user = _session->data().user(UserId(otherId));
        if (!other.isEmpty()) ensureUser(other, true);
        peer = user;
    } else {
        return applyCommunityChat(chat);
    }

    if (peer) {
        applyChatConfig(peer, chat);
        _chatByPeer[peer->id.value] = chatId;
        _peerByChat[chatId] = peer->id.value;
    }
    return peer;
}

ChannelData *NativeBridge::applyCommunityChat(
        const QJsonObject &chat,
        bool member) {
    const auto chatId = chat.value("id").toVariant().toLongLong();
    if (chatId <= 0) {
        return nullptr;
    }
    const auto me = client().meId();
    const auto linked = TopicChannels::WithDiscussion(chat);
    const auto channel = _session->data().processChat(
        Community::Channel(linked, me, member))->asChannel();
    if (!channel) {
        return nullptr;
    }
    _communityChats[chatId] = linked;
    if (TopicChannels::IsTopicChannel(linked)) {
        applyCommunityChat(TopicChannels::DiscussionChat(linked), false);
    }
    const auto full = Community::ChannelFull(
        linked,
        me,
        communityNotifySettings(chatId, chat),
        AutoDeletePeriod(channel));
    Data::ApplyChannelUpdate(channel, full.c_channelFull());
    applyCommunityPhoto(
        channel,
        chat.value("photo_url").toString(),
        chat.value("photo_video_url").toString(),
        chat.value("photo_video_size").toInt(),
        chat.value("photo_video_start_ts").toDouble());
    EnsureBarSettingsKnown(channel);
    if (!member
        && TopicChannels::IsTopicChannel(linked)
        && !_bottomLoadedChats.contains(chatId)) {
        const auto history = _session->data().history(channel);
        if (history->loadedAtBottom()) {
            history->setNotLoadedAtBottom();
        }
    }
    if (member) {
        applyChatConfig(channel, chat);
        _chatByPeer[channel->id.value] = chatId;
        _peerByChat[chatId] = channel->id.value;
    }
    return channel;
}

bool NativeBridge::handleCommunityEvent(
        const QString &type,
        const QJsonObject &data) {
    if (type == u"chat.created"_q) {
        const auto chat = data.value("chat").toObject();
        if (Community::IsCommunity(chat) && peerForChat(chat)) {
            if (const auto history = historyForChatId(
                    chat.value("id").toVariant().toLongLong())) {
                history->updateChatListExistence();
            }
        }
        reloadChats();
        return true;
    } else if (type == u"ephemeral.created"_q) {
        _session->api().applyUpdates(MTP_updates(
            MTP_vector<MTPUpdate>(1, MTP_updateNewEphemeralMessage(
                welcomeMessage(data, false))),
            MTP_vector<MTPUser>(),
            MTP_vector<MTPChat>(),
            MTP_int(base::unixtime::now()),
            MTP_int(0)));
        return true;
    } else if (type == u"welcome.updated"_q) {
        _session->api().applyUpdates(MTP_updates(
            MTP_vector<MTPUpdate>(1, MTP_updateNewEphemeralMessage(
                welcomeMessage(data.value("message").toObject(), true))),
            MTP_vector<MTPUser>(),
            MTP_vector<MTPChat>(),
            MTP_int(base::unixtime::now()),
            MTP_int(0)));
        return true;
    }
    const auto chatId = data.value("chat_id").toVariant().toLongLong();
    auto chat = communityChat(chatId);
    if (chat.isEmpty()) {
        return false;
    }
    if (type == u"chat.profile_updated"_q) {
        applyCommunityProfile(data);
        return true;
    } else if (type == u"chat.member_added"_q
        || type == u"chat.member_removed"_q) {
        chat.insert("participants_count", data.value("participants_count"));
        const auto channel = applyCommunityChat(chat);
        if (channel && channel->mgInfo) {
            channel->mgInfo->lastParticipantsStatus
                |= MegagroupInfo::LastParticipantsCountOutdated;
            _session->changes().peerUpdated(
                channel,
                Data::PeerUpdate::Flag::Members);
        }
        return true;
    } else if (type == u"chat.message_views"_q) {
        auto updates = QVector<MTPUpdate>();
        for (const auto &entry : data.value("views").toArray()) {
            const auto object = entry.toObject();
            updates.push_back(MTP_updateChannelMessageViews(
                MTP_long(chatId),
                MTP_int(object.value("id").toInt()),
                MTP_int(object.value("views").toInt())));
        }
        if (!updates.isEmpty()) {
            _session->api().applyUpdates(MTP_updates(
                MTP_vector<MTPUpdate>(std::move(updates)),
                MTP_vector<MTPUser>(),
                MTP_vector<MTPChat>(),
                MTP_int(base::unixtime::now()),
                MTP_int(0)));
        }
        return true;
    } else if (type == u"chat.call_updated"_q) {
        const auto call = data.value("call");
        if (call.isObject()) {
            chat.insert("call", call);
        } else {
            chat.remove("call");
        }
        applyCommunityChat(chat);
        return true;
    }
    return false;
}

MTPEphemeralMessage NativeBridge::welcomeMessage(
        const QJsonObject &welcome,
        bool isTemplate) const {
    using Flag = MTPDephemeralMessage::Flag;
    const auto chatId = welcome.value("chat_id").toVariant().toLongLong();
    auto entities = renderMessageEntities(welcome);
    auto flags = MTPDephemeralMessage::Flags(Flag::f_peer_id);
    if (isTemplate) {
        flags |= Flag::f_welcome_template | Flag::f_out;
    }
    if (!entities.v.isEmpty()) {
        flags |= Flag::f_entities;
    }
    return MTP_ephemeralMessage(
        MTP_flags(flags),
        MTP_int(welcome.value("id").toInt()),
        MTP_peerUser(MTP_long(
            welcome.value("from_id").toVariant().toLongLong())),
        MTP_peerChannel(MTP_long(chatId)),
        MTP_long(client().meId()),
        MTPint(),
        MTP_int(unixTime(welcome.value("date").toString())),
        MTP_string(welcome.value("text").toString()),
        std::move(entities),
        MTPMessageMedia(),
        MTPReplyMarkup(),
        MTPMessageReplyHeader(),
        MTPRichMessage(),
        MTPlong(),
        MTPint());
}

void NativeBridge::applyCommunityProfile(const QJsonObject &profile) {
    const auto chatId = profile.value("chat_id").toVariant().toLongLong();
    auto chat = communityChat(chatId);
    if (chat.isEmpty()) {
        return;
    }
    for (const auto key : {
            "title",
            "about",
            "username",
            "photo_url",
            "photo_video_url",
            "photo_video_size",
            "photo_video_start_ts",
            "history_hidden",
            "signatures",
            "participants_count",
            "reactions" }) {
        if (profile.contains(QLatin1String(key))) {
            chat.insert(QLatin1String(key), profile.value(QLatin1String(key)));
        }
    }
    applyCommunityChat(chat);
}

void NativeBridge::uploadCommunityPhoto(
        ChannelData *channel,
        QImage &&image,
        std::function<void()> done,
        QByteArray video,
        double videoStartTs,
        std::function<void()> fail) {
    const auto chatId = qint64(peerToChannel(channel->id).bare);
    auto bytes = QByteArray();
    {
        auto buffer = QBuffer(&bytes);
        image.save(&buffer, "JPEG", 87);
    }
    LOG(("FoxMes: PUT /chats/%1/photo, image %2 bytes, video %3 bytes"
        ).arg(chatId
        ).arg(bytes.size()
        ).arg(video.size()));
    const auto weak = QPointer<NativeBridge>(this);
    client().communityPhoto(chatId, bytes, video, videoStartTs, [
            weak,
            chatId,
            done = std::move(done),
            fail = std::move(fail)](
            QJsonDocument doc,
            QString error,
            int status) {
        if (!weak || !error.isEmpty() || !doc.isObject()) {
            LOG(("FoxMes: PUT /chats/%1/photo failed (%2, %3)"
                ).arg(chatId
                ).arg(status
                ).arg(error));
            if (fail) {
                fail();
            }
            return;
        }
        weak->applyCommunityProfile(doc.object());
        if (done) {
            done();
        }
    });
}

void NativeBridge::reapplyCommunityChat(qint64 chatId) {
    const auto i = _communityChats.find(chatId);
    if (i == _communityChats.end()) {
        return;
    }
    const auto peer = _session->data().peerLoaded(
        peerFromChannel(ChannelId(chatId)));
    if (const auto channel = peer ? peer->asChannel() : nullptr) {
        applyCommunityPhoto(
            channel,
            i->second.value("photo_url").toString(),
            i->second.value("photo_video_url").toString(),
            i->second.value("photo_video_size").toInt(),
            i->second.value("photo_video_start_ts").toDouble());
        applyChatConfig(channel, i->second);
    }
}

QJsonArray NativeBridge::entitiesJson(
        const MTPVector<MTPMessageEntity> &entities,
        const QString &text) const {
    return entitiesToJson(
        Api::EntitiesFromMTP(_session, entities.v),
        text);
}

QJsonObject NativeBridge::communityChat(qint64 chatId) const {
    const auto i = _communityChats.find(chatId);
    return (i != _communityChats.end()) ? i->second : QJsonObject();
}

MTPPeerNotifySettings NativeBridge::communityNotifySettings(
        qint64 chatId,
        const QJsonObject &chat) const {
    const auto settings = chat.value("notification_settings").toObject();
    const auto known = _notificationByChat.find(chatId);
    auto muteUntil = (known != _notificationByChat.end())
        ? known->second.muteUntil
        : std::clamp(
            qint64(settings.value("mute_until").toVariant().toLongLong()),
            qint64(0),
            qint64(INT32_MAX));
    const auto showPreviews = (known != _notificationByChat.end())
        ? known->second.showPreviews
        : settings.value("show_previews").toBool(true);
    auto soundNone = (known != _notificationByChat.end())
        ? known->second.soundNone
        : settings.value("sound_none").toBool(false);
    const auto peer = _session->data().peerLoaded(
        peerFromChannel(ChannelId(chatId)));
    if (peer && !peer->notify().settingsUnknown()) {
        muteUntil = std::clamp(
            qint64(peer->notify().muteUntil().value_or(0)),
            qint64(0),
            qint64(INT32_MAX));
        const auto sound = peer->notify().sound();
        soundNone = sound && sound->none;
    }
    using NotifyFlag = MTPDpeerNotifySettings::Flag;
    return MTP_peerNotifySettings(
        MTP_flags(NotifyFlag::f_show_previews
            | NotifyFlag::f_other_sound
            | (muteUntil > 0 ? NotifyFlag::f_mute_until : NotifyFlag(0))),
        MTP_bool(showPreviews),
        MTPBool(),
        MTP_int(int(muteUntil)),
        MTPNotificationSound(),
        MTPNotificationSound(),
        soundNone ? MTP_notificationSoundNone() : MTP_notificationSoundDefault(),
        MTPBool(),
        MTPBool(),
        MTPNotificationSound(),
        MTPNotificationSound(),
        MTPNotificationSound());
}

void NativeBridge::applyCommunityPhoto(
        ChannelData *channel,
        const QString &url,
        const QString &videoUrl,
        int videoSize,
        double videoStartTs) {
    const auto video = (url.isEmpty() || videoSize <= 0)
        ? QString()
        : videoUrl;
    const auto photoId = url.isEmpty()
        ? PhotoId()
        : ChatPhotoId(url, video);
    const auto location = url.isEmpty()
        ? ImageLocation()
        : ImageLocation(DownloadLocation{ PlainUrlLocation{ url } }, 160, 160);
    if (channel->userpicPhotoId() == photoId
        && channel->userpicLocation() == location) {
        return;
    }
    if (url.isEmpty()) {
        channel->setUserpic(PhotoId(), ImageLocation(), false);
    } else {
        applyRemoteChatPhoto(url, video, videoSize, videoStartTs);
        channel->setUserpic(photoId, location, !video.isEmpty());
    }
    _session->changes().peerUpdated(channel, Data::PeerUpdate::Flag::Photo);
}

PhotoId NativeBridge::applyRemoteChatPhoto(
        const QString &url,
        const QString &videoUrl,
        int videoSize,
        double videoStartTs) {
    const auto video = (videoSize > 0) ? videoUrl : QString();
    const auto photoId = ChatPhotoId(url, video);
    const auto photo = _session->data().photo(photoId);
    if (photo->date()) {
        return photoId;
    }
    UpdateRemotePhotoImages(
        photo,
        url,
        kUnknownPhotoSide,
        kUnknownPhotoSide,
        false);
    if (!video.isEmpty()) {
        photo->updateImages(
            QByteArray(),
            ImageWithLocation(),
            ImageWithLocation(),
            ImageWithLocation(),
            ImageWithLocation(),
            ImageWithLocation{
                .location = ImageLocation(
                    DownloadLocation{ PlainUrlLocation{ video } },
                    kChatPhotoVideoSide,
                    kChatPhotoVideoSide),
                .bytesCount = videoSize,
            },
            crl::time(std::llround(videoStartTs * 1000.)));
    }
    photo->setFields(base::unixtime::now(), false);
    return photoId;
}

MTPMessageAction NativeBridge::communityPhotoAction(
        const QJsonObject &action) {
    const auto url = action.value("photo_url").toString();
    if (url.isEmpty()) {
        return MTP_messageActionChatDeletePhoto();
    }
    const auto photoId = applyRemoteChatPhoto(
        url,
        action.value("video_url").toString(),
        action.value("video_size").toInt(),
        action.value("video_start_ts").toDouble());
    return MTP_messageActionChatEditPhoto(MTP_photo(
        MTP_flags(0),
        MTP_long(photoId),
        MTP_long(0),
        MTP_bytes(),
        MTP_int(0),
        MTP_vector<MTPPhotoSize>(1, MTP_photoSizeEmpty(MTP_string())),
        MTPVector<MTPVideoSize>(),
        MTP_int(0)));
}

void NativeBridge::applyChatConfig(PeerData *peer, const QJsonObject &chat) {
    if (!peer) {
        return;
    }
    EnsureBarSettingsKnown(peer);
    peer->setMessagesTTL(AutoDeletePeriod(peer));
    const auto chatId = chat.value("id").toVariant().toLongLong();
    const auto permissions = chat.value("permissions").toObject();
    const auto canSend = permissions.value("can_send").toBool(true);
    const auto allowed = ParseAllowedReactions(_session, chat);
    applyNotificationSettings(
        peer,
        chatId,
        chat.value("notification_settings").toObject());
    Wallpapers::ApplyForPeer(peer, chat.value("wallpaper").toObject());
    ChatThemes::ApplyForPeer(peer, chat.value("theme_emoticon").toString());
    if (const auto group = peer->asChat()) {
        auto flags = ChatDataFlags();
        if (chat.value("owner_id").toVariant().toLongLong() == client().meId()) {
            flags |= ChatDataFlag::Creator;
        }
        group->setFlags(flags);
        group->setAdminRights(group->amCreator()
            ? ChatAdminRight::DeleteMessages
                | ChatAdminRight::EditMessages
                | ChatAdminRight::BanUsers
                | ChatAdminRight::InviteByLinkOrAdd
                | ChatAdminRight::PinMessages
            : ChatAdminRights());
        group->setDefaultRestrictions(canSend ? ChatRestrictions() : Data::AllSendRestrictions());
        group->setAllowedReactions(allowed);
    }
}

void NativeBridge::applyPeerNotifySettings(
        PeerData *peer,
        qint64 muteUntil,
        bool showPreviews,
        bool soundNone) {
    if (!peer) {
        return;
    }
    using NotifyFlag = MTPDpeerNotifySettings::Flag;
    const auto notifyFlags = NotifyFlag::f_show_previews
        | NotifyFlag::f_other_sound
        | (muteUntil > 0 ? NotifyFlag::f_mute_until : NotifyFlag(0));
    _session->data().notifySettings().apply(peer->id, MTP_peerNotifySettings(
        MTP_flags(notifyFlags),
        MTP_bool(showPreviews),
        MTPBool(),
        MTP_int(int(muteUntil)),
        MTPNotificationSound(),
        MTPNotificationSound(),
        soundNone ? MTP_notificationSoundNone() : MTP_notificationSoundDefault(),
        MTPBool(),
        MTPBool(),
        MTPNotificationSound(),
        MTPNotificationSound(),
        MTPNotificationSound()));
}

void NativeBridge::applyNotificationSettings(
        PeerData *peer,
        qint64 chatId,
        const QJsonObject &notifications) {
    if (!peer || chatId <= 0) {
        return;
    }
    const auto revision = static_cast<qint64>(
        notifications.value("settings_revision").toVariant().toLongLong());
    const auto known = _notificationByChat.find(chatId);
    const auto knownRevision = (known != _notificationByChat.end())
        ? known->second.revision
        : qint64(0);
    if (known != _notificationByChat.end() && revision <= knownRevision) {
        return;
    }
    auto muteUntil = static_cast<qint64>(
        notifications.value("mute_until").toVariant().toLongLong());
    const auto showPreviews = notifications.value("show_previews").toBool(true);
    const auto soundNone = notifications.value("sound_none").toBool(false);
    if (muteUntil > INT32_MAX) {
        muteUntil = INT32_MAX;
    } else if (muteUntil < 0) {
        muteUntil = 0;
    }
    _notificationByChat[chatId] = {
        muteUntil,
        showPreviews,
        soundNone,
        qMax(revision, knownRevision),
    };
    applyPeerNotifySettings(peer, muteUntil, showPreviews, soundNone);
}

void NativeBridge::applyDefaultNotifySettings(
        Data::DefaultNotify type,
        qint64 muteUntil,
        bool soundNone) {
    if (muteUntil > INT32_MAX) {
        muteUntil = INT32_MAX;
    } else if (muteUntil < 0) {
        muteUntil = 0;
    }
    _defaultNotify[size_t(type)].muteUntil = muteUntil;
    _defaultNotify[size_t(type)].soundNone = soundNone;
    using NotifyFlag = MTPDpeerNotifySettings::Flag;
    const auto settings = MTP_peerNotifySettings(
        MTP_flags(NotifyFlag::f_mute_until
            | NotifyFlag::f_show_previews
            | NotifyFlag::f_other_sound),
        MTP_bool(true),
        MTPBool(),
        MTP_int(int(muteUntil)),
        MTPNotificationSound(),
        MTPNotificationSound(),
        soundNone ? MTP_notificationSoundNone() : MTP_notificationSoundDefault(),
        MTPBool(),
        MTPBool(),
        MTPNotificationSound(),
        MTPNotificationSound(),
        MTPNotificationSound());
    _session->data().notifySettings().apply(type, settings);
}

void NativeBridge::applyDefaultNotifySettingsPayload(
        const QJsonObject &settings) {
    if (settings.isEmpty()) {
        return;
    }
    const auto type = DefaultNotifyType(settings.value("scope").toString());
    if (!type) {
        return;
    }
    auto &state = _defaultNotify[size_t(*type)];
    const auto revision = settings.value("settings_revision").toVariant().toLongLong();
    if (revision < state.revision) {
        return;
    }
    state.revision = revision;
    applyDefaultNotifySettings(
        *type,
        settings.value("mute_until").toVariant().toLongLong(),
        settings.value("sound_none").toBool());
    SaveDefaultNotifyCache(_session, settings);
}

void NativeBridge::loadDefaultNotifySettings() {
    const auto weak = QPointer<NativeBridge>(this);
    for (const auto type : {
            Data::DefaultNotify::User,
            Data::DefaultNotify::Group,
            Data::DefaultNotify::Broadcast }) {
        client().defaultNotificationSettings(
            DefaultNotifyScope(type),
            [weak](QJsonDocument doc, QString error, int) {
                if (!weak || !error.isEmpty() || !doc.isObject()) {
                    return;
                }
                weak->applyDefaultNotifySettingsPayload(doc.object());
            });
    }
}

void NativeBridge::reloadChats() {
    const auto generation = ++_chatsRequestGeneration;
    const auto weak = QPointer<NativeBridge>(this);
    client().chatsLight([weak, generation](QJsonDocument doc, QString error, int status) {
        if (!weak || generation != weak->_chatsRequestGeneration) return;
        if (status == 401) {
            const auto account = &weak->_session->account();
            ClearLogin(weak->_session);
            crl::on_main(account, [account] { account->foxmesLoggedOut(); });
            return;
        }
        weak->_chatsDone = true;
        weak->finishInitialLoadIfReady();
        if (!error.isEmpty() || !doc.isArray()) {
            weak->_session->data().chatsListDone(nullptr);
            weak->runAfterChatsReload(false);
            return;
        }
        weak->applyChats(doc);
        SaveChatsCache(weak->_session, doc.toJson(QJsonDocument::Compact));
        weak->_session->data().chatsListDone(nullptr);
        weak->_session->data().sendHistoryChangeNotifications();
        weak->runAfterChatsReload(true);
    });
}

void NativeBridge::runAfterChatsReload(bool listLoaded) {
    for (auto &callback : base::take(_afterChatsReload)) {
        callback(listLoaded);
    }
}

void NativeBridge::applyChats(const QJsonDocument &doc) {
    if (!doc.isArray()) return;
    auto missing = _peerByChat;
    for (const auto &entry : doc.array()) {
        missing.erase(entry.toObject().value("id").toVariant().toLongLong());
    }
    for (const auto &[chatId, peerId] : missing) {
        removeChat(chatId);
    }
    _pinnedRanks.clear();
    for (const auto &entry : doc.array()) {
        if (!entry.isObject()) continue;
        const auto chat = entry.toObject();
        const auto chatId = chat.value("id").toVariant().toLongLong();
        const auto peer = peerForChat(chat);
        if (!peer) continue;
        const auto history = _session->data().history(peer);

        if (chat.value("archived").toBool()) {
            history->setFolder(_session->data().folder(Data::Folder::kId));
        } else {
            history->clearFolder();
        }
        history->setUnreadMark(chat.value("marked_unread").toBool());
        if (chat.value("pinned").toBool()) {
            _pinnedRanks[chatId] = qMax<qint64>(
                chat.value("pinned_rank").toVariant().toLongLong(), 1);
        }

        if (const auto draft = chat.value("draft").toObject(); !draft.isEmpty()) {
            _draftRevisionByChat[chatId] = draft.value("revision").toVariant().toLongLong();
        } else {
            _draftRevisionByChat[chatId] = 0;
        }

        const auto last = chat.value("last_message").toObject();
        const auto updated = chat.value("updated_at").toString();
        const auto date = !last.isEmpty()
            ? unixTime(last.value("created_at").toString())
            : unixTime(updated);

        history->setChatListTimeId(date);
        history->setUnreadCount(chat.value("unread_count").toInt());
        applyReceiptSnapshot(history, chatId, chat.value("receipt_state").toObject());
        if (!last.isEmpty()) {
            if (!_bottomLoadedChats.contains(chatId) && history->loadedAtBottom()) {
                history->setNotLoadedAtBottom();
            }
            const auto lastId = last.value("id").toVariant().toLongLong();
            applyMessage(history, last, false, NewMessageType::Existing);
            if (lastId > 0 && lastId <= INT32_MAX) {
                history->applyDialogTopMessage(MsgId(int32(lastId)));
            }
        }
        history->updateChatListExistence();
    }
    rebuildPinnedOrder();
	updatePresence();
	drainDeferredMessageEvents();
}

void NativeBridge::deferMessageEvent(
        const QString &type,
        const QJsonObject &data) {
    if (_deferredMessageEvents.size() >= kDeferredMessageEventsLimit) {
        _deferredMessageEvents.erase(_deferredMessageEvents.begin());
    }
    _deferredMessageEvents.push_back({ type, data });
}

void NativeBridge::drainDeferredMessageEvents() {
    if (_deferredMessageEvents.empty()) {
        return;
    }
    auto pending = base::take(_deferredMessageEvents);
    auto applied = false;
    for (auto &deferred : pending) {
        const auto chatId = deferred.data
            .value("chat_id").toVariant().toLongLong();
        const auto history = historyForChatId(chatId);
        if (!history) {
            _deferredMessageEvents.push_back(std::move(deferred));
            continue;
        }
        applyMessage(
            history,
            deferred.data,
            deferred.type == u"message.updated"_q,
            deferred.type == u"message.created"_q
                ? NewMessageType::Unread
                : NewMessageType::Existing);
        applied = true;
    }
    if (applied) {
        _session->data().sendHistoryChangeNotifications();
    }
}

void NativeBridge::trackWindow(Window::SessionController *controller) {
	if (!controller || !_trackedWindows.emplace(controller).second) {
		return;
	}
	const auto weak = QPointer<NativeBridge>(this);
	controller->activeChatValue(
	) | rpl::on_next([weak](Dialogs::Key) {
		if (weak) {
			weak->updatePresence();
		}
	}, controller->lifetime());
	controller->widget()->windowActiveValue(
	) | rpl::on_next([weak, controller](bool active) {
		if (weak) {
			weak->_windowActive[controller] = active;
			weak->updatePresence();
		}
	}, controller->lifetime());
	controller->lifetime().add([weak, controller] {
		if (weak) {
			weak->_trackedWindows.erase(controller);
			weak->_windowActive.erase(controller);
			weak->updatePresence();
		}
	});
	updatePresence();
	offerTopicChannels();
}

void NativeBridge::updatePresence() {
	auto chatId = qint64(0);
	for (const auto &controller : _session->windows()) {
		const auto i = _windowActive.find(controller);
		if (i == _windowActive.end() || !i->second) {
			continue;
		}
		if (const auto history = controller->activeChatCurrent().history()) {
			chatId = chatIdFor(history);
		}
		break;
	}
	if (_presenceChatId == chatId) {
		return;
	}
	_presenceChatId = chatId;
	if (_liveUpdates) {
		_liveUpdates->setPresenceChat(chatId);
	}
}

void NativeBridge::applyPresence(const QJsonObject &data) {
	const auto chatId = data.value("chat_id").toVariant().toLongLong();
	const auto userId = data.value("user_id").toVariant().toLongLong();
	if (chatId <= 0 || userId <= 0 || userId == client().meId()) {
		return;
	}
	const auto observed = data.value("revision").toVariant().toLongLong();
	auto &observedByUser = _presenceObservedAt[chatId];
	const auto i = observedByUser.find(userId);
	if (i != observedByUser.end()
		&& observed > 0
		&& i->second > observed) {
		return;
	}
	observedByUser[userId] = observed;
	const auto history = historyForChatId(chatId);
	if (!history || history->peer->id != peerFromUser(UserId(userId))) {
		return;
	}
	const auto user = history->peer->asUser();
	if (!user) {
		return;
	}
	const auto lastSeen = data.value("last_seen_at_ms")
		.toVariant().toLongLong() / 1000;
	const auto status = data.value("online").toBool()
		? Data::LastseenStatus::OnlineTill(base::unixtime::now() + 90)
		: (lastSeen > 0)
		? Data::LastseenStatus::OnlineTill(TimeId(lastSeen))
		: Data::LastseenStatus::LongAgo();
	if (user->updateLastseen(status)) {
		_session->changes().peerUpdated(
			user,
			Data::PeerUpdate::Flag::OnlineStatus);
	}
}

void NativeBridge::loadCachedChats() {
    const auto json = LoadChatsCache(_session);
    if (json.isEmpty()) return;
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return;
    applyChats(doc);
    _session->data().sendHistoryChangeNotifications();
}

void NativeBridge::finishInitialLoadIfReady() {
    if (_contactsDone
        && _chatsDone
        && !_session->data().contactsLoaded().current()) {
        _session->data().contactsLoaded() = true;
    }
}

bool NativeBridge::hasChat(PeerId peerId) const {
    return _chatByPeer.find(peerId.value) != _chatByPeer.end();
}

void NativeBridge::removeChat(qint64 chatId) {
    const auto i = _peerByChat.find(chatId);
    if (i == _peerByChat.end()) {
        return;
    }
    const auto peerId = PeerId(i->second);
    _peerByChat.erase(i);
    const auto peer = _chatByPeer.find(peerId.value);
    if (peer == _chatByPeer.end() || peer->second != chatId) {
        return;
    }
    _chatByPeer.erase(peer);
    _pinnedRanks.erase(chatId);
    _bottomLoadedChats.erase(chatId);
    _loadedChats.erase(chatId);
    _nextHistoryBefore.erase(chatId);
    const auto preview = _communityChats.find(chatId);
    const auto keepPreview = (preview != _communityChats.end())
        && TopicChannels::IsTopicChannel(preview->second);
    const auto previewChat = keepPreview ? preview->second : QJsonObject();
    _communityChats.erase(chatId);
    if (const auto history = _session->data().historyLoaded(peerId)) {
        _session->data().deleteConversationLocally(history->peer);
    }
    if (keepPreview) {
        applyCommunityChat(previewChat, false);
    }
    const auto cached = QJsonDocument::fromJson(LoadChatsCache(_session));
    if (cached.isArray()) {
        auto remaining = QJsonArray();
        for (const auto &entry : cached.array()) {
            if (entry.toObject().value("id").toVariant().toLongLong() != chatId) {
                remaining.push_back(entry);
            }
        }
        SaveChatsCache(_session, QJsonDocument(remaining).toJson(QJsonDocument::Compact));
    }
}

qint64 NativeBridge::chatIdFor(History *history) const {
    if (!history) return 0;
    const auto i = _chatByPeer.find(history->peer->id.value);
    return (i == _chatByPeer.end()) ? 0 : i->second;
}

qint64 NativeBridge::historyChatIdFor(History *history) const {
    if (const auto chatId = chatIdFor(history)) {
        return chatId;
    }
    const auto channel = history ? history->peer->asChannel() : nullptr;
    if (!channel) {
        return 0;
    }
    const auto chatId = qint64(peerToChannel(channel->id).bare);
    return TopicChannels::IsTopicChannel(communityChat(chatId)) ? chatId : 0;
}

PeerId NativeBridge::peerForChatId(qint64 chatId) const {
    if (!chatId) {
        return PeerId();
    }
    const auto i = _peerByChat.find(chatId);
    return (i == _peerByChat.end()) ? PeerId() : PeerId(i->second);
}

History *NativeBridge::historyForChatId(qint64 chatId) const {
    const auto i = _peerByChat.find(chatId);
    if (i == _peerByChat.end()) return nullptr;
    return _session->data().historyLoaded(PeerId(i->second));
}

History *NativeBridge::historyForChat(qint64 chatId) const {
    return historyForChatId(chatId);
}

void NativeBridge::resolveChatId(
        History *history,
        std::function<void(qint64)> done) {
    ensureChat(history, std::move(done));
}

void NativeBridge::ensureChat(History *history, std::function<void(qint64)> done) {
    if (!history) return;
    if (const auto current = chatIdFor(history)) {
        if (done) done(current);
        return;
    }
    const auto user = history->peer->asUser();
	if (!user) {
		if (done) done(0);
		return;
	}
	const auto peerKey = history->peer->id.value;
	const auto [pending, starting] = _pendingChatCallbacks.emplace(peerKey, std::vector<std::function<void(qint64)>>());
	auto &callbacks = pending->second;
	if (done) callbacks.push_back(std::move(done));
	if (!starting) return;
    if (user->isSelf()) {
        const auto weak = QPointer<NativeBridge>(this);
		client().savedChat([weak, history, peerKey](
                QJsonDocument doc, QString error, int) mutable {
			if (!weak) return;
			auto callbacks = std::move(weak->_pendingChatCallbacks[peerKey]);
			weak->_pendingChatCallbacks.erase(peerKey);
			if (!history || !error.isEmpty() || !doc.isObject()) {
				for (const auto &callback : callbacks) callback(0);
				return;
			}
            const auto chat = doc.object();
            weak->peerForChat(chat);
            const auto chatId = chat.value("id").toVariant().toLongLong();
            if (!history->folderKnown()) history->clearFolder();
            history->updateChatListExistence();
			for (const auto &callback : callbacks) callback(chatId);
            weak->reloadChats();
        });
        return;
    }
    const auto otherId = qint64(peerToUser(history->peer->id).bare);
    const auto weak = QPointer<NativeBridge>(this);
	client().createDirect(otherId, [weak, history, peerKey](
            QJsonDocument doc,
            QString error,
            int) mutable {
        if (!weak) return;
		auto callbacks = std::move(weak->_pendingChatCallbacks[peerKey]);
		weak->_pendingChatCallbacks.erase(peerKey);
        if (!error.isEmpty() || !doc.isObject()) {
			for (const auto &callback : callbacks) callback(0);
			return;
		}
        const auto chat = doc.object();
        weak->peerForChat(chat);
        const auto chatId = chat.value("id").toVariant().toLongLong();
        if (history && !history->folderKnown()) history->clearFolder();
        if (history) history->updateChatListExistence();
		for (const auto &callback : callbacks) callback(chatId);
        weak->reloadChats();
    });
}

PeerId NativeBridge::ensureQuotedChannel(const QJsonObject &channel) {
    const auto channelId = channel.value("id").toVariant().toLongLong();
    if (channelId <= 0) {
        return PeerId();
    }
    const auto known = _session->data().channelLoaded(ChannelId(channelId));
    if (!known || !known->isLoaded()) {
        const auto stub = QJsonObject{
            { "id", channelId },
            { "type", u"channel"_q },
            { "title", channel.value("title").toString() },
            { "username", channel.value("username").toString() },
        };
        if (const auto peer = _session->data().processChat(
                Community::Channel(stub, client().meId(), false))
                    ->asChannel()) {
            applyCommunityPhoto(peer, channel.value("photo_url").toString());
        }
    }
    return peerFromChannel(ChannelId(channelId));
}

bool NativeBridge::isOwnMessage(
        not_null<History*> history,
        const QJsonObject &message) const {
    if (const auto channel = history->peer->asBroadcast()) {
        return channel->amCreator();
    }
    return message.value("sender_id").toVariant().toLongLong()
        == client().meId();
}

HistoryItem *NativeBridge::applyMessage(
        History *history,
        const QJsonObject &message,
        bool replaceExisting) {
    return applyMessage(
        history,
        message,
        replaceExisting,
        NewMessageType::Existing);
}

std::optional<MTPMessage> NativeBridge::prepareReminder(
        History *history,
        const QJsonObject &reminder) {
    auto prepared = prepareMessage(history, reminder, {}, true);
    if (!prepared) {
        return std::nullopt;
    }
    return std::move(prepared->mtp);
}

std::optional<NativeBridge::PreparedMessage> NativeBridge::prepareMessage(
        History *history,
        const QJsonObject &message,
        const LocalAttachment &local,
        bool reminder) {
    const auto chatId = message.value("chat_id").toVariant().toLongLong();
    const auto messageId = message.value("id").toVariant().toLongLong();
    if (chatId <= 0 || messageId <= 0 || messageId > INT32_MAX) return std::nullopt;

    const auto senderObject = message.value("sender").toObject();
    if (!senderObject.isEmpty()) ensureUser(senderObject, true);
    const auto senderId = message.value("sender_id").toVariant().toLongLong();
    if (senderId <= 0 && !history->peer->isBroadcast()) return std::nullopt;

    if (const auto call = message.value("call"); call.isObject()) {
        return prepareCallMessage(
            history,
            message,
            call.toObject(),
            messageId,
            senderId);
    }
    if (const auto action = message.value("action"); action.isObject()) {
        return prepareServiceMessage(
            history,
            message,
            action.toObject(),
            messageId,
            senderId);
    }

    using Flag = MTPDmessage::Flag;
    auto mtpFlags = Flag::f_from_id | Flag();
    if (isOwnMessage(history, message)) {
        mtpFlags |= Flag::f_out;
    }
    const auto reactionsArray = message.value("reactions").toArray();
    if (!reactionsArray.isEmpty()) {
        mtpFlags |= Flag::f_reactions;
    }
    const auto replyId = message.value("reply_to_id").toVariant().toLongLong();
    const auto replyTo = message.value("reply_to").toObject();
    const auto replyChannel = replyTo.value("channel").toObject();
    const auto replyPeerId = replyChannel.isEmpty()
        ? peerForChatId(replyTo.value("chat_id").toVariant().toLongLong())
        : ensureQuotedChannel(replyChannel);
    const auto replyHeader = ReplyHeaderFrom(message, replyPeerId);
    if (replyId > 0) {
        mtpFlags |= Flag::f_reply_to;
        const auto replyPeer = replyPeerId
            ? _session->data().peerLoaded(replyPeerId)
            : history->peer.get();
        if (replyId <= INT32_MAX
            && replyPeer
            && !_session->data().message(replyPeer, MsgId(int32(replyId)))) {
            requestMessageData(replyPeer, MsgId(int32(replyId)), {});
        }
    }
    const auto editedAt = message.value("edited_at").toString();
    if (!editedAt.isEmpty()) {
        mtpFlags |= Flag::f_edit_date;
    }
    if (message.value("silent").toBool()) {
        mtpFlags |= Flag::f_silent;
    }
    if (message.value("from_scheduled").toBool()) {
        mtpFlags |= Flag::f_from_scheduled;
    }
    if (message.value("mentioned").toBool()) {
        mtpFlags |= Flag::f_mentioned;
    }
    auto richMessage = MTPRichMessage();
    if (const auto article = message.value("article"); article.isObject()) {
        richMessage = TopicChannels::RichMessage(_session, article.toObject());
        mtpFlags |= Flag::f_rich_message;
    }
    TopicChannels::RememberSiteLink(
        _session,
        FullMsgId(history->peer->id, MsgId(messageId)),
        message.value("site_url").toString());
    auto replies = MTPMessageReplies();
    if (const auto value = message.value("replies"); value.isObject()) {
        replies = TopicChannels::Replies(value.toObject(), chatId);
        mtpFlags |= Flag::f_replies;
    }

    auto fwdHeader = MTPMessageFwdHeader();
    const auto forwarded = message.value("forwarded_from").toObject();
    if (!forwarded.isEmpty()) {
        const auto authorId = forwarded.value("author_id").toVariant().toLongLong();
        const auto authorName = forwarded.value("author_name").toString();
        const auto sourceDate = unixTime(forwarded.value("source_date").toString());
        using FwdFlag = MTPDmessageFwdHeader::Flag;
        auto fwdFlags = MTPDmessageFwdHeader::Flags();
        auto from = MTPPeer();
        auto fromName = MTPstring();
        auto channelPost = MTPint();
        auto postAuthor = MTPstring();
        const auto channel = forwarded.value("channel").toObject();
        const auto channelId = channel.value("id").toVariant().toLongLong();
        if (channelId > 0) {
            fwdFlags |= FwdFlag::f_from_id | FwdFlag::f_channel_post;
            from = MTP_peerChannel(MTP_long(channelId));
            channelPost = MTP_int(forwarded.value("message_id").toInt());
            const auto signature = forwarded.value("post_author").toString();
            if (!signature.isEmpty()) {
                fwdFlags |= FwdFlag::f_post_author;
                postAuthor = MTP_string(signature);
            }
            ensureQuotedChannel(channel);
        } else if (authorId > 0) {
            fwdFlags |= FwdFlag::f_from_id;
            from = MTP_peerUser(MTP_long(authorId));
            if (!_session->data().user(UserId(authorId))->isLoaded()) {
                ensureUser(QJsonObject{
                    { "id", authorId },
                    { "display_name", authorName },
                    { "username", QString() },
                }, true);
            }
        } else if (!authorName.isEmpty()) {
            fwdFlags |= FwdFlag::f_from_name;
            fromName = MTP_string(authorName);
        }
        fwdHeader = MTP_messageFwdHeader(
            MTP_flags(fwdFlags),
            from,
            fromName,
            MTP_int(sourceDate),
            channelPost,
            postAuthor,
            MTPPeer(),
            MTPint(),
            MTPPeer(),
            MTPstring(),
            MTPint(),
            MTPstring());
        mtpFlags |= Flag::f_fwd_from;
    }

    const auto groupedId = message
        .value("grouped_id").toVariant().toLongLong();
    if (groupedId != 0) {
        mtpFlags |= Flag::f_grouped_id;
    }

    const auto attachments = message.value("attachments").toArray();
    if (attachments.size() > 1) {
        LOG(("FoxMes: message %1 in chat %2 has %3 attachments, only the "
            "first one is rendered"
            ).arg(messageId).arg(chatId).arg(attachments.size()));
    }
    const auto ephemeral = message.value("ephemeral").toObject();
    const auto ephemeralTtl = ephemeral.isEmpty()
        ? 0
        : EphemeralTtlSeconds(ephemeral);
    const auto attachment = attachments.isEmpty()
        ? QJsonObject()
        : attachments.at(0).toObject();
    const auto webPage = message.value("web_page").toObject();
    auto media = (!ephemeral.isEmpty() && attachment.isEmpty())
        ? EphemeralExpiredMedia(ephemeral, ephemeralTtl)
        : attachment.isEmpty()
        ? MediaFromWebPage(_session, webPage)
        : std::optional<MTPMessageMedia>(
            MediaFromAttachment(
                _session,
                attachment,
                local,
                message.value("video_timestamp").isDouble()
                    ? std::make_optional(message.value("video_timestamp").toInt())
                    : std::nullopt,
                ephemeralTtl));
    if (media && webPage.value("above").toBool()) {
        mtpFlags |= Flag::f_invert_media;
    }
    if (media) {
        mtpFlags |= Flag::f_media;
    }
    if (!(mtpFlags & Flag::f_out)
        && AttachmentPlaysOnce(attachment)
        && (messageId > history->inboxReadTillId().bare)) {
        mtpFlags |= Flag::f_media_unread;
    }
    if (!ephemeral.isEmpty()
        && ephemeral.value("state").toString() == u"pending"_q) {
        mtpFlags |= Flag::f_media_unread;
    }
    auto entities = renderMessageEntities(message);
    if (!entities.v.isEmpty()) {
        mtpFlags |= Flag::f_entities;
    }
    const auto createdAt = reminder
        ? Scheduled::DeliveryDate(message)
        : unixTime(message.value("created_at").toString());
    auto fromId = MTP_peerUser(MTP_long(senderId));
    auto postAuthor = MTPstring();
    auto views = MTPint();
    if (const auto channel = history->peer->asBroadcast()) {
        mtpFlags &= ~Flag::f_from_id;
        mtpFlags |= Flag::f_post;
        fromId = MTPPeer();
        mtpFlags |= Flag::f_views;
        views = MTP_int(message.value("views").toInt());
        const auto signature = channel->addsSignature()
            ? message.value("post_author").toString()
            : QString();
        if (!signature.isEmpty()) {
            mtpFlags |= Flag::f_post_author;
            postAuthor = MTP_string(signature);
        }
    }
    auto mtp = MTP_message(
        MTP_flags(mtpFlags),
        MTP_int(int(messageId)),
        fromId,
        MTPint(),
        MTPstring(),
        peerToMTP(history->peer->id),
        MTPPeer(),
        fwdHeader,
        MTPlong(),
        MTPlong(),
        MTPPeer(),
        replyHeader,
        MTP_int(createdAt),
        MTP_string(renderMessageText(message)),
        media ? *media : MTP_messageMediaEmpty(),
        MTPReplyMarkup(),
        std::move(entities),
        views,
        MTPint(),
        replies,
        MTP_int(editedAt.isEmpty() ? 0 : unixTime(editedAt)),
        postAuthor,
        MTP_long(groupedId),
        Reactions::Build(_session, reactionsArray, client().meId()),
        MTPVector<MTPRestrictionReason>(),
        MTPint(),
        MTPint(),
        MTPlong(),
        MTPFactCheck(),
        MTPint(),
        MTPlong(),
        MTPSuggestedPost(),
        MTPint(),
        MTPstring(),
        richMessage);
    if (!reminder) {
        _messageRevisions[SeenKey(chatId, messageId)] = qMax<qint64>(
            message.value("revision").toVariant().toLongLong(),
            1);
    }
    return PreparedMessage{
        .mtp = std::move(mtp),
        .messageId = MsgId(int32(messageId)),
        .senderId = senderId,
    };
}

MTPMessageMedia NativeBridge::AttachmentMedia(
        not_null<Main::Session*> session,
        const QJsonObject &attachment) {
    return MediaFromAttachment(session, attachment, LocalAttachment());
}

std::optional<MTPMessage> NativeBridge::prepareAnswerMessage(
        History *history,
        const QJsonObject &message) {
    auto prepared = history ? prepareMessage(history, message) : std::nullopt;
    return prepared
        ? std::make_optional(std::move(prepared->mtp))
        : std::nullopt;
}

HistoryItem *NativeBridge::applyThreadMessage(
        History *history,
        const QJsonObject &message,
        NewMessageType type) {
    auto prepared = history ? prepareMessage(history, message) : std::nullopt;
    if (!prepared || prepared->mtp.type() != mtpc_message) {
        return nullptr;
    }
    if (const auto existing = _session->data().message(
            history->peer,
            prepared->messageId)) {
        existing->applyEdition(HistoryMessageEdition(
            _session,
            prepared->mtp.c_message()));
        applyMessagePayloadState(existing, message);
        return existing;
    }
    const auto item = history->addNewMessage(
        prepared->messageId,
        prepared->mtp,
        MessageFlags(),
        type);
    applyMessagePayloadState(item, message);
    return item;
}

std::optional<NativeBridge::PreparedMessage> NativeBridge::prepareServiceMessage(
        History *history,
        const QJsonObject &message,
        const QJsonObject &action,
        qint64 messageId,
        qint64 senderId) {
    _messageRevisions[SeenKey(
        message.value("chat_id").toVariant().toLongLong(),
        messageId)] = 1;
    const auto broadcast = history->peer->isBroadcast();
    using Flag = MTPDmessageService::Flag;
    auto flags = MTPDmessageService::Flags(
        broadcast ? Flag::f_post : Flag::f_from_id);
    if (isOwnMessage(history, message)) {
        flags |= Flag::f_out;
    }
    return PreparedMessage{
        .mtp = MTP_messageService(
            MTP_flags(flags),
            MTP_int(int32(messageId)),
            broadcast ? MTPPeer() : MTP_peerUser(MTP_long(senderId)),
            peerToMTP(history->peer->id),
            MTPPeer(),
            MTPMessageReplyHeader(),
            MTP_int(unixTime(message.value("created_at").toString())),
            (action.value("type").toString() == u"photo_changed"_q
                ? communityPhotoAction(action)
                : Community::Action(_session, action, senderId, broadcast)),
            MTPMessageReactions(),
            MTPint()),
        .messageId = MsgId(int32(messageId)),
        .senderId = senderId,
    };
}

std::optional<NativeBridge::PreparedMessage> NativeBridge::prepareCallMessage(
        History *history,
        const QJsonObject &message,
        const QJsonObject &call,
        qint64 messageId,
        qint64 senderId) {
    _messageRevisions[SeenKey(
        message.value("chat_id").toVariant().toLongLong(),
        messageId)] = qMax<qint64>(
            message.value("revision").toVariant().toLongLong(),
            1);
    if (call.value("conference").toBool()) {
        return PreparedMessage{
            .mtp = Calls::BuildConferenceMessage(
                history->peer->id,
                (senderId == client().meId()),
                MsgId(int32(messageId)),
                senderId,
                call,
                unixTime(message.value("created_at").toString())),
            .messageId = MsgId(int32(messageId)),
            .senderId = senderId,
        };
    }
    return PreparedMessage{
        .mtp = Calls::BuildCallMessage(
            history->peer->id,
            (senderId == client().meId()),
            MsgId(int32(messageId)),
            senderId,
            call.value("call_id").toVariant().toLongLong(),
            call.value("reason").toString(),
            call.value("duration").toInt(),
            call.value("video").toBool(),
            unixTime(message.value("created_at").toString())),
        .messageId = MsgId(int32(messageId)),
        .senderId = senderId,
    };
}

HistoryItem *NativeBridge::applyMessage(
        History *history,
        const QJsonObject &message,
        bool replaceExisting,
        NewMessageType type,
        qint64 pendingLocalIdHint) {
    if (!history) return nullptr;
    if (!history->folderKnown()) history->clearFolder();

    const auto chatId = message.value("chat_id").toVariant().toLongLong();
    const auto messageId = message.value("id").toVariant().toLongLong();
    if (chatId <= 0 || messageId <= 0 || messageId > INT32_MAX) return nullptr;

    const auto clientNonce = message.value("client_nonce").toString().trimmed();
    HistoryItem *pendingLocal = nullptr;
    qint64 pendingLocalId = 0;
    if (pendingLocalIdHint) {
        if (const auto i = _pendingSends.find(pendingLocalIdHint);
            i != _pendingSends.end() && i->second.history == history) {
            pendingLocalId = pendingLocalIdHint;
            pendingLocal = _session->data().message(
                history->peer,
                MsgId(pendingLocalId));
        }
    }
    if (!pendingLocal && !clientNonce.isEmpty()) {
        if (const auto i = _pendingSendNonceToLocalId.find(clientNonce);
            i != _pendingSendNonceToLocalId.end()) {
            pendingLocalId = i->second;
            pendingLocal = _session->data().message(
                history->peer,
                MsgId(pendingLocalId));
        }
    }

    const auto id = MsgId(int32(messageId));
    if (const auto existing = _session->data().message(history->peer, id)) {
        const auto attachExisting = (type == NewMessageType::Unread)
            && !existing->mainView();
        if (!replaceExisting && !attachExisting) {
            if (pendingLocal && pendingLocal != existing) {
                pendingLocal->markEphemeralSent();
                pendingLocal->destroy();
                finishPendingDraftSave(
                    pendingLocalId,
                    unixTime(message.value("created_at").toString()));
                clearPendingSend(pendingLocalId);
            }
            applyMessagePayloadState(existing, message);
            _seenMessages.emplace(SeenKey(chatId, messageId));
            return existing;
        }
        if (replaceExisting) {
            auto prepared = prepareMessage(history, message);
            if (!prepared) {
                return existing;
            }
            if (prepared->mtp.type() == mtpc_messageService) {
                existing->applyEdition(prepared->mtp.c_messageService());
                applyMessagePayloadState(existing, message);
                _seenMessages.emplace(SeenKey(chatId, messageId));
                return existing;
            }
            existing->applyEdition(HistoryMessageEdition(
                _session,
                prepared->mtp.c_message()));
            applyMessagePayloadState(existing, message);
            _seenMessages.emplace(SeenKey(chatId, messageId));
            return existing;
        }
    } else if (!replaceExisting && !pendingLocal) {
        if (!_seenMessages.emplace(SeenKey(chatId, messageId)).second) return nullptr;
    }

    auto local = LocalAttachment();
    if (pendingLocalId) {
        if (const auto i = _pendingSends.find(pendingLocalId);
            i != _pendingSends.end()) {
            local = i->second.localAttachment;
        }
    }
    auto prepared = prepareMessage(history, message, local);
    if (!prepared) return nullptr;

    HistoryItem *item = nullptr;
    if (pendingLocal) {
        pendingLocal->setRealId(id);
        pendingLocal->updateDate(unixTime(message.value("created_at").toString()));
        pendingLocal->applySentMessage(prepared->mtp.c_message());
        if (const auto media = pendingLocal->media()) {
            if (const auto document = media->document()) {
                const auto attachments = message.value("attachments").toArray();
                if (!attachments.isEmpty()) {
                    ApplyAttachmentSource(
                        document,
                        attachments.at(0).toObject());
                }
            }
        }
        finishPendingDraftSave(
            pendingLocalId,
            unixTime(message.value("created_at").toString()));
        clearPendingSend(pendingLocalId);
        item = pendingLocal;
    } else {
        const auto own = isOwnMessage(history, message);
        const auto unreadBefore = history->unreadCountKnown()
            ? std::optional<int>(history->unreadCount())
            : std::nullopt;
        item = history->addNewMessage(
            id,
            prepared->mtp,
            MessageFlags(),
            type);
        if (own && unreadBefore && history->unreadCountKnown()
            && history->unreadCount() != *unreadBefore) {
            history->setUnreadCount(*unreadBefore);
        }
    }
    applyMessagePayloadState(item, message);
    history->setChatListTimeId(unixTime(message.value("created_at").toString()));
    history->updateChatListExistence();
    _seenMessages.emplace(SeenKey(chatId, messageId));
    if (item && !isOwnMessage(history, message)) {
        queueDelivered(chatId, messageId);
    }
    return item;
}

void NativeBridge::queueDelivered(qint64 chatId, qint64 messageId) {
    if (chatId <= 0 || messageId <= 0) {
        return;
    }
    auto &ids = _pendingDelivered[chatId];
    if (std::find(ids.begin(), ids.end(), messageId) != ids.end()) {
        return;
    }
    ids.push_back(messageId);
    if (!_deliveredTimer.isActive()) {
        _deliveredTimer.start(kDeliveredBatchDelayMs);
    }
}

void NativeBridge::flushDelivered() {
    auto pending = std::move(_pendingDelivered);
    _pendingDelivered.clear();
    for (auto &[chatId, ids] : pending) {
        if (ids.isEmpty()) {
            continue;
        }
        client().markDelivered(chatId, ids);
    }
}

HistoryItem *NativeBridge::applyDependencyMessage(
		History *history,
		const QJsonObject &message) {
	if (!history) {
		return nullptr;
	}
	const auto messageId = message.value("id").toVariant().toLongLong();
	if (messageId <= 0 || messageId > INT32_MAX) {
		return nullptr;
	}
	const auto id = MsgId(int32(messageId));
	if (const auto existing = _session->data().message(history->peer, id)) {
		applyMessagePayloadState(existing, message);
		return existing;
	}
	const auto prepared = prepareMessage(history, message);
	if (!prepared) {
		return nullptr;
	}
	const auto item = history->addNewMessage(
		id,
		prepared->mtp,
		MessageFlags(),
		NewMessageType::Existing);
	applyMessagePayloadState(item, message);
	return item;
}

void NativeBridge::requestMessageData(
		PeerData *peer,
		MsgId messageId,
		std::function<void()> done) {
	if (!messageId || messageId.bare <= 0) {
		if (done) {
			done();
		}
		return;
	}
	if (peer && _session->data().message(peer, messageId)) {
		if (done) {
			done();
		}
		return;
	}
	const auto [request, inserted] = _messageDataCallbacks.try_emplace(
		messageId.bare);
	auto &callbacks = request->second;
	if (done) {
		callbacks.push_back(std::move(done));
	}
	if (!inserted) {
		return;
	}
	const auto weak = QPointer<NativeBridge>(this);
	client().messageById(
		messageId.bare,
		[weak, peer, messageId](
				QJsonDocument document,
				QString error,
				int) {
			if (!weak) {
				return;
			}
			auto callbacks = std::move(
				weak->_messageDataCallbacks[messageId.bare]);
			weak->_messageDataCallbacks.erase(messageId.bare);
			if (error.isEmpty() && document.isObject()) {
				const auto object = document.object();
				const auto chatId = object.value("chat_id")
					.toVariant().toLongLong();
				auto history = weak->historyForChatId(chatId);
				if (!history && peer) {
					history = weak->_session->data().history(peer);
				}
				if (weak->applyDependencyMessage(history, object)) {
					weak->_session->data().sendHistoryChangeNotifications();
				}
			}
			for (const auto &callback : callbacks) {
				callback();
			}
		});
}

void AttachEphemeralMediaUrl(
        Main::Session *session,
        HistoryItem *item,
        const QString &url) {
    if (!session || !item || url.isEmpty()) {
        return;
    }
    const auto media = item->media();
    if (!media) {
        return;
    }
    if (const auto document = media->document()) {
        Streaming::RememberSource(document, url);
        return;
    }
    if (const auto photo = media->photo()) {
        UpdateRemotePhotoImages(
            photo,
            url,
            photo->width() > 0 ? photo->width() : kUnknownPhotoSide,
            photo->height() > 0 ? photo->height() : kUnknownPhotoSide,
            true);
    }
}

void NativeBridge::applyMessagePayloadState(
        HistoryItem *item,
        const QJsonObject &message) {
    applyMessageReactions(item, message);
    applyEphemeralState(item, message);
}

void NativeBridge::applyEphemeralState(
        HistoryItem *item,
        const QJsonObject &message) {
    if (!item) {
        return;
    }
    const auto ephemeral = message.value("ephemeral").toObject();
    if (ephemeral.isEmpty()) {
        return;
    }
    const auto state = ephemeral.value("state").toString();
    if (state == u"expired"_q) {
        item->clearMediaAsExpired();
        return;
    }
    if (state != u"opened"_q) {
        return;
    }
    if (ephemeral.value("mode").toString() != u"timer"_q) {
        if (item->out()) {
            item->clearMediaAsExpired();
        }
        return;
    }
    const auto ttl = ephemeral.value("ttl_seconds").toInt();
    const auto deadline = unixTime(ephemeral.value("expires_at").toString());
    if (ttl > 0 && deadline > 0) {
        item->applyMediaContentsRead(deadline - ttl);
    }
}

void NativeBridge::applyEphemeralViewed(
        HistoryItem *item,
        const QJsonObject &message) {
    if (!item) {
        return;
    }
    const auto ephemeral = message.value("ephemeral").toObject();
    if (ephemeral.isEmpty()) {
        return;
    }
    if (!item->isUnreadMedia() && !item->isUnreadMention()) {
        return;
    }
    item->markMediaAndMentionRead();
    _session->data().requestItemRepaint(item);
    item->applyMediaContentsRead(unixTime(
        ephemeral.value("opened_at").toString()));
}

void MarkEphemeralViewed(
        const base::flat_set<not_null<HistoryItem*>> &items) {
    for (const auto &item : items) {
        const auto media = item->media();
        if (!media || !media->ttlSeconds() || item->out()) {
            continue;
        }
        const auto session = &item->history()->session();
        const auto id = item->fullId();
        ClientFor(session).markEphemeralViewed(
            id.msg.bare,
            [session, id](QJsonDocument doc, QString error, int status) {
                if (!error.isEmpty() || !doc.isObject()) {
                    return;
                }
                const auto strong = session->data().message(id);
                if (const auto bridge = BridgeFor(session); bridge && strong) {
                    bridge->applyEphemeralState(strong, doc.object());
                }
            });
    }
}

void NativeBridge::applyMessageReactions(
        HistoryItem *item,
        const QJsonObject &message) {
    if (!item) {
        return;
    }
    const auto revisionValue = message.value("reaction_revision");
    if (!revisionValue.isUndefined() && !revisionValue.isNull()) {
        const auto revision = revisionValue.toVariant().toLongLong();
        const auto known = _reactionReplace.find(item->id.bare);
        if (known != _reactionReplace.end()) {
            if (revision < known->second.revision) {
                return;
            }
            known->second.revision = revision;
        } else if (revision > 0) {
            _reactionReplace[item->id.bare].revision = revision;
        }
    }
    auto reactionsArray = message.value("reactions").toArray();
    if (reactionsArray.isEmpty()) {
        reactionsArray = message.value("reactions").toObject().value("recent").toArray();
    }
    const auto reactions = Reactions::Build(
        _session,
        reactionsArray,
        client().meId());
    if (reactions.data().vresults().v.isEmpty()) {
        item->updateReactions(nullptr);
    } else {
        item->updateReactions(&reactions);
    }
}

void NativeBridge::removeMessage(qint64 chatId, qint64 messageId) {
    removeMessageFrom(historyForChatId(chatId), chatId, messageId);
}

void NativeBridge::removeMessageFrom(
        History *history,
        qint64 chatId,
        qint64 messageId) {
    if (messageId <= 0 || messageId > INT32_MAX) return;
    if (!history) {
        LOG(("FoxMes: cannot remove message %1, chat %2 has no history"
            ).arg(messageId).arg(chatId));
        return;
    }
    _seenMessages.erase(SeenKey(chatId, messageId));
    _messageRevisions.erase(SeenKey(chatId, messageId));
    const auto item = _session->data().message(
        history->peer,
        MsgId(int32(messageId)));
    if (!item) {
        return;
    }
    DeleteMessagesWithEffect(_session, { item });
    history->updateChatListExistence();
    _session->data().sendHistoryChangeNotifications();
}

void NativeBridge::loadHistory(
        History *history,
        qint64 aroundId,
        Data::LoadDirection direction,
        HistoryLoaded done,
        HistoryFailed failed) {
    loadHistoryOf(
        history,
        aroundId,
        direction,
        std::move(done),
        std::move(failed),
        false);
}

void NativeBridge::loadHistoryOf(
        History *history,
        qint64 aroundId,
        Data::LoadDirection direction,
        HistoryLoaded done,
        HistoryFailed failed,
        bool chatsReloaded) {
    const auto chatId = historyChatIdFor(history);
    if (!chatId) {
        if (!history) {
            if (done) done();
            return;
        }
        const auto weak = QPointer<NativeBridge>(this);
        if (history->peer->isUser()) {
			ensureChat(history, [=](qint64 chatId) {
				if (!weak) return;
				if (chatId <= 0) {
					if (done) done();
					return;
				}
				weak->loadHistoryOf(history, aroundId, direction, done, failed, chatsReloaded);
            });
            return;
        }
        if (!chatsReloaded) {
            _afterChatsReload.push_back([=](bool listLoaded) {
                if (!weak) {
                    return;
                } else if (!listLoaded) {
                    if (done) done();
                    return;
                }
                weak->loadHistoryOf(history, aroundId, direction, done, failed, true);
            });
            reloadChats();
            return;
        }
        if (failed) {
            failed(MTP::Error::Local(
                u"CHANNEL_PRIVATE"_q,
                u"not a chat of this account"_q));
        } else if (done) {
            done();
        }
        return;
    }
    if (!history->folderKnown()) history->clearFolder();
	if (chatIdFor(history)) {
		syncPinnedMessages(history, chatId);
	}
	loadHistoryPage(history, aroundId, direction, std::move(done));
}

void NativeBridge::loadHistoryPage(
		History *history,
		qint64 aroundId,
		Data::LoadDirection direction,
		HistoryLoaded done) {
	const auto chatId = historyChatIdFor(history);
	if (!chatId) {
		if (done) done();
		return;
	}
	auto requestDirection = direction;
	auto requestAroundId = aroundId;
	const auto wantsTail = (aroundId == ShowAtTheEndMsgId.bare)
		|| (aroundId >= (ServerMaxMsgId.bare - 1));
	const auto openAtEnd = wantsTail
		&& (requestDirection == Data::LoadDirection::Around);
	if (wantsTail) {
		requestAroundId = 0;
	}
	if (openAtEnd) {
		requestDirection = Data::LoadDirection::Before;
	} else if (requestDirection == Data::LoadDirection::Before
		&& requestAroundId <= 0) {
		if (const auto i = _nextHistoryBefore.find(chatId); i != _nextHistoryBefore.end()) {
			requestAroundId = i->second;
		} else if (_loadedChats.find(chatId) != _loadedChats.end()) {
			if (done) done();
			return;
		}
	}
	const auto loadingKey = HistoryLoadKey{
		chatId,
		requestAroundId,
		requestDirection,
		openAtEnd,
	};
	if (auto it = _loadingChats.find(loadingKey); it != _loadingChats.end()) {
		it->second.push_back(std::move(done));
		return;
	}
	_loadingChats.emplace(loadingKey, std::vector<HistoryLoaded>{});

	const auto weak = QPointer<NativeBridge>(this);
	const auto requested = crl::now();
	client().messages(chatId, requestAroundId, 80, requestDirection, [=, done = std::move(done)](QJsonDocument doc, QString error, int) mutable {
		const auto received = crl::now();
		auto completed = false;
		auto completeAll = [&]() mutable {
			if (std::exchange(completed, true)) return;
			if (const auto total = crl::now() - requested
				; total >= kSlowHistoryPage) {
				LOG(("FoxMes: slow history page for chat %1: %2 ms "
					"(server %3 ms, %4 items)"
					).arg(chatId
					).arg(total
					).arg(received - requested
					).arg(doc.object().value("items").toArray().size()));
			}
			if (weak) {
				if (auto it = weak->_loadingChats.find(loadingKey); it != weak->_loadingChats.end()) {
					auto waiters = std::move(it->second);
					weak->_loadingChats.erase(it);
					for (auto &waiter : waiters) {
						waiter();
					}
				}
			}
			if (done) done();
		};
		if (!weak || !history) {
			completeAll();
			return;
		}
		if (!error.isEmpty() || !doc.isObject()) {
			completeAll();
			return;
		}
		const auto response = doc.object();

		auto older = QVector<MTPMessage>();
		auto newer = QVector<MTPMessage>();
		auto applied = base::flat_map<MsgId, QJsonObject>();
		for (const auto &entry : response.value("items").toArray()) {
			if (!entry.isObject()) continue;
			const auto message = entry.toObject();
			const auto messageId = message.value("id").toVariant().toLongLong();
			if (messageId <= 0) continue;
			if (messageId > INT32_MAX) {
				LOG(("FoxMes: message id %1 in chat %2 exceeds MsgId range, "
					"message skipped").arg(messageId).arg(chatId));
				continue;
			}
			const auto clientNonce = message.value("client_nonce").toString().trimmed();
			if (!clientNonce.isEmpty()
				&& weak->_pendingSendNonceToLocalId.contains(clientNonce)) {
				weak->applyMessage(history, message, false);
				continue;
			}
			auto prepared = weak->prepareMessage(history, message);
			if (!prepared) continue;
			const auto id = prepared->messageId;
			const auto belongsToOlder = openAtEnd
				|| (id.bare <= MsgId(requestAroundId));
			(belongsToOlder ? older : newer).push_back(
				std::move(prepared->mtp));
			applied.emplace(id, message);
			weak->_seenMessages.emplace(SeenKey(chatId, messageId));
		}
		if (!response.contains("has_more_before")
			|| !response.contains("has_more_after")) {
			LOG(("FoxMes: history page for chat %1 has no directional flags"
				).arg(chatId));
			completeAll();
			return;
		}
		const auto hasMoreBefore = response.value("has_more_before").toBool();
		const auto hasMoreAfter = response.value("has_more_after").toBool();
		const auto nextBefore = response.value("next_before").toVariant().toLongLong();
		const auto beforeExhausted = !hasMoreBefore;
		const auto afterExhausted = openAtEnd || !hasMoreAfter;
		const auto reverse = [](QVector<MTPMessage> &list) {
			std::reverse(list.begin(), list.end());
		};
		if (openAtEnd) {
			history->addNewerSlice(QVector<MTPMessage>());
			reverse(older);
			history->addOlderSlice(older);
		} else {
			if (!older.isEmpty() || beforeExhausted) {
				reverse(older);
				history->addOlderSlice(older);
			}
			if (!newer.isEmpty() || afterExhausted) {
				reverse(newer);
				history->addNewerSlice(newer);
			}
		}
		if (afterExhausted) {
			weak->_bottomLoadedChats.insert(chatId);
		}

		std::vector<MsgId> messageIds;
		messageIds.reserve(applied.size());
		MsgId firstId = 0;
		MsgId lastId = 0;
		for (const auto &[id, message] : applied) {
			const auto item = weak->_session->data().message(history->peer, id);
			if (!item) continue;
			weak->applyMessagePayloadState(item, message);
			messageIds.push_back(id);
			if (!firstId || id < firstId) firstId = id;
			if (id > lastId) lastId = id;
		}

		if (!messageIds.empty()) {
			auto range = MsgRange{ firstId, lastId };
			switch (requestDirection) {
			case Data::LoadDirection::Before:
				range.till = (requestAroundId > 0)
					? qMax(range.till, MsgId(requestAroundId))
					: ServerMaxMsgId;
				break;
			case Data::LoadDirection::After:
				if (requestAroundId > 0) {
					range.from = qMin(range.from, MsgId(requestAroundId));
				}
				if (!hasMoreAfter) {
					range.till = ServerMaxMsgId;
				}
				break;
			case Data::LoadDirection::Around:
				if (requestAroundId > 0) {
					range.from = qMin(range.from, MsgId(requestAroundId));
					range.till = qMax(range.till, MsgId(requestAroundId));
				}
				if (!hasMoreAfter) {
					range.till = ServerMaxMsgId;
				}
				break;
			}
			if (openAtEnd) {
				range.till = ServerMaxMsgId;
			}
			if (!hasMoreBefore && requestDirection != Data::LoadDirection::After) {
				range.from = 0;
			}
			history->messages().addSlice(std::move(messageIds), range, {});
		} else if (openAtEnd
			&& requestDirection == Data::LoadDirection::Before
			&& requestAroundId <= 0) {
			history->messages().addSlice({}, MsgRange{ 0, ServerMaxMsgId }, {});
		}
		if (requestDirection == Data::LoadDirection::Before && hasMoreBefore && nextBefore > 0) {
			weak->_nextHistoryBefore[chatId] = nextBefore;
		} else {
			weak->_nextHistoryBefore.erase(chatId);
			if (requestDirection == Data::LoadDirection::Before) {
				weak->_loadedChats.insert(chatId);
				history->markLoadedAtTop();
			}
		}
		weak->_session->data().sendHistoryChangeNotifications();
		completeAll();
    });
}

SendOptions SendOptionsFrom(const Api::SendOptions &options) {
    auto result = SendOptions{
        .silent = options.silent,
        .mediaTtlSeconds = int(options.ttlSeconds),
    };
    if (options.scheduled == Api::kScheduledUntilOnlineTimestamp) {
        result.deliverWhenOnline = true;
    } else if (options.scheduled > 0) {
        result.deliverAt = options.scheduled;
    }
    return result;
}

void NativeBridge::sendMessage(
        Api::MessageToSend &&message,
        std::optional<MsgId> localMessageId) {
    const auto replyTo = ReplyTargetFrom(
        message.action.history,
        message.action.replyTo);
    auto prepared = TextWithEntities{
        message.textWithTags.text,
        TextUtilities::ConvertTextTagsToEntities(message.textWithTags.tags),
    };
    TextUtilities::PrepareForSending(
        prepared,
        Ui::ItemTextOptions(message.action.history, _session->user()).flags);
    if (TopicChannels::IsDiscussionPeer(message.action.history->peer)) {
        TextUtilities::Trim(prepared);
        sendComment(
            message.action.history,
            prepared,
            replyTo,
            message.action.replyTo.topicRootId,
            localMessageId);
        return;
    }
    const auto options = SendOptionsFrom(message.action.options);
    if (options.scheduled()) {
        scheduleText(
            message.action.history,
            prepared.text,
            prepared.entities,
            replyTo,
            options,
            message.action.clearDraft,
            message.action.replyTo.topicRootId,
            message.action.replyTo.monoforumPeerId);
        return;
    }
    sendText(
        message.action.history,
        prepared.text,
        prepared.entities,
        message.webPage,
        replyTo,
        localMessageId,
        message.action.clearDraft,
        message.action.replyTo.topicRootId,
        message.action.replyTo.monoforumPeerId,
        QString(),
        options);
}

void NativeBridge::saveDraftToCloudDelayed(Data::Thread *thread) {
    if (!thread) {
        return;
    }
    const auto history = thread->owningHistory().get();
    const auto topicRootId = thread->topicRootId();
    const auto monoforumPeerId = thread->monoforumPeerId();
    const auto localDraft = history->localDraft(topicRootId, monoforumPeerId);
    const auto text = localDraft ? localDraft->textWithTags.text : QString();
    const auto replyToId = (localDraft && localDraft->reply.messageId)
        ? localDraft->reply.messageId.msg.bare
        : 0;
    history->createCloudDraft(topicRootId, monoforumPeerId, localDraft);
    history->startSavingCloudDraft(topicRootId, monoforumPeerId);
    const auto key = history->peer->id.value
        ^ uint64_t(uint32_t(topicRootId.bare))
        ^ (monoforumPeerId.value << 1);
    const auto generation = ++_draftSaveGenerations[key];
    const auto weak = QPointer<NativeBridge>(this);
    ensureChat(history, [weak, history, topicRootId, monoforumPeerId, text, replyToId, key, generation](qint64 chatId) {
        if (!weak) {
            return;
        }
        if (chatId <= 0) {
            history->finishSavingCloudDraft(
                topicRootId,
                monoforumPeerId,
                base::unixtime::now());
            if (weak->_draftSaveGenerations[key] == generation) {
                history->clearCloudDraft(topicRootId, monoforumPeerId);
            }
            return;
        }
        const auto baseRevision = weak->_draftRevisionByChat[chatId];
        weak->client().setDraft(chatId, text, replyToId, baseRevision,
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            [weak, history, chatId, topicRootId, monoforumPeerId, key, generation](QJsonDocument doc, QString error, int status) {
                if (!weak) {
                    return;
                }
                history->finishSavingCloudDraft(
                    topicRootId,
                    monoforumPeerId,
                    base::unixtime::now());
                if (weak->_draftSaveGenerations[key] != generation) {
                    return;
                }
                if (error.isEmpty() && doc.isObject()) {
                    const auto revision = doc.object().value("draft").toObject().value("revision").toVariant().toLongLong();
                    if (revision > 0) {
                        weak->_draftRevisionByChat[chatId] = revision;
                    }
                    history->draftSavedToCloud(topicRootId, monoforumPeerId);
                } else if (status == 409 && doc.isObject()) {
                    const auto draft = doc.object().value("current").toObject();
                    const auto revision = draft.value("revision").toVariant().toLongLong();
                    if (revision > 0) {
                        weak->_draftRevisionByChat[chatId] = revision;
                    }
                    const auto draftText = draft.value("text").toString();
                    const auto replyTo = MsgId(draft.value("reply_to_id").toVariant().toLongLong());
                    const auto replyPeerId = weak->peerForChatId(
                        draft.value("reply_to_chat_id")
                            .toVariant()
                            .toLongLong());
                    auto value = std::make_unique<Data::Draft>();
                    value->textWithTags = TextWithTags{ draftText, TextWithTags::Tags() };
                    value->reply.messageId = FullMsgId(
                        replyPeerId ? replyPeerId : history->peer->id,
                        replyTo);
                    value->reply.topicRootId = topicRootId;
                    value->reply.monoforumPeerId = monoforumPeerId;
                    value->date = base::unixtime::now();
                    history->setCloudDraft(std::move(value));
                } else {
                    history->clearCloudDraft(topicRootId, monoforumPeerId);
                }
            });
    });
}

void NativeBridge::sendText(
        History *history,
        const QString &text,
        const EntitiesInText &entities,
        Data::WebPageDraft webPage,
        ReplyTarget replyTo,
        std::optional<MsgId> localMessageId,
        bool clearDraft,
        MsgId draftTopicRootId,
        PeerId draftMonoforumPeerId,
        const QString &reuseClientNonce,
        const SendOptions &options) {
    auto prepared = TextWithEntities{ text, entities };
    TextUtilities::Trim(prepared);
    const auto trimmed = prepared.text;
    const auto trimmedEntities = prepared.entities;
    if (!history || trimmed.isEmpty()) return;
    if (TopicChannels::IsDiscussionPeer(history->peer)) {
        sendComment(
            history,
            prepared,
            replyTo,
            draftTopicRootId,
            localMessageId,
            reuseClientNonce);
        return;
    }
    const auto item = createPendingTextMessage(
        history,
        trimmed,
        trimmedEntities,
        replyTo,
        localMessageId
            ? *localMessageId
            : _session->data().nextLocalMessageId());
    if (!item) {
        return;
    }
    const auto clientNonce = reuseClientNonce.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : reuseClientNonce;
    if (clearDraft) {
        const auto key = history->peer->id.value
            ^ uint64_t(uint32_t(draftTopicRootId.bare))
            ^ (draftMonoforumPeerId.value << 1);
        ++_draftSaveGenerations[key];
        history->clearCloudDraft(draftTopicRootId, draftMonoforumPeerId);
        history->startSavingCloudDraft(draftTopicRootId, draftMonoforumPeerId);
    }
    rememberPendingSend(
        item,
        clientNonce,
        replyTo,
        trimmed,
        trimmedEntities,
        webPage,
        TextWithEntities(),
        {},
        {},
        clearDraft,
        draftTopicRootId,
        draftMonoforumPeerId);
    _session->data().sendHistoryChangeNotifications();
    const auto weak = QPointer<NativeBridge>(this);
    const auto localId = item->id.bare;
    ensureChat(history, [weak, history, trimmed, trimmedEntities, webPage, replyTo, clientNonce, localId, options](qint64 chatId) {
        if (!weak || !history) return;
        if (chatId <= 0) {
            weak->failPendingSend(localId, kSendNoStatus);
            return;
        }
        const auto clearDraft = weak->_pendingSends.contains(localId)
            && weak->_pendingSends.at(localId).draftSaving;
        const auto clearDraftRevision = weak->_draftRevisionByChat[chatId];
        if (const auto i = weak->_pendingSends.find(localId)
            ; i != weak->_pendingSends.end()) {
            i->second.committing = true;
        }
        const auto outgoing = weak->entitiesToJson(
            trimmedEntities,
            trimmed,
            webPage);
        weak->client().sendMessageWithDraftRevision(chatId, trimmed, replyTo.messageId, {}, clientNonce, clearDraft, clearDraftRevision, [weak, history, chatId, localId, clearDraft](QJsonDocument doc, QString error, int status) {
            if (!weak || !history) return;
            if (!error.isEmpty() || !doc.isObject()) {
                weak->failPendingSend(localId, status);
                return;
            }
            if (weak->finishCancelledCommit(localId, doc.object())) {
                return;
            }
            weak->applyMessage(
                history,
                doc.object(),
                false,
                NewMessageType::Existing,
                localId);
            if (weak->_pendingSends.contains(localId)) {
                weak->failPendingSend(localId, kSendServerAccepted);
            }
            if (clearDraft) {
                weak->_draftRevisionByChat[chatId] = 0;
            }
            if (!history->folderKnown()) history->clearFolder();
            history->setUnreadCount(0);
            weak->_session->data().sendHistoryChangeNotifications();
        }, false, {}, {}, outgoing, options);
    });
}

void NativeBridge::scheduleText(
        History *history,
        const QString &text,
        const EntitiesInText &entities,
        ReplyTarget replyTo,
        const SendOptions &options,
        bool clearDraft,
        MsgId draftTopicRootId,
        PeerId draftMonoforumPeerId) {
    auto prepared = TextWithEntities{ text, entities };
    TextUtilities::Trim(prepared);
    if (!history || prepared.text.isEmpty()) {
        return;
    }
    if (clearDraft) {
        const auto key = history->peer->id.value
            ^ uint64_t(uint32_t(draftTopicRootId.bare))
            ^ (draftMonoforumPeerId.value << 1);
        ++_draftSaveGenerations[key];
        history->clearCloudDraft(draftTopicRootId, draftMonoforumPeerId);
    }
    const auto weak = QPointer<NativeBridge>(this);
    const auto raw = history;
    const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto outgoing = entitiesToJson(
        prepared.entities,
        prepared.text,
        Data::WebPageDraft());
    ensureChat(raw, [weak, raw, prepared, replyTo, options, nonce, operationId, outgoing, clearDraft](qint64 chatId) {
        if (!weak || !raw) return;
        if (chatId <= 0) {
            LOG(("FoxMes: scheduled send has no chat id"));
            return;
        }
        if (clearDraft) {
            weak->clearCloudDraftFor(chatId);
        }
        weak->client().createReminder(
            chatId,
            prepared.text,
            outgoing,
            replyTo.messageId,
            {},
            nonce,
            false,
            {},
            {},
            options,
            operationId,
            [weak, raw](QJsonDocument doc, QString error, int status) {
                if (!weak || !raw) return;
                weak->applyReminderResponse(raw, doc, error, status);
            });
    });
}

void NativeBridge::applyReminderResponse(
        History *history,
        const QJsonDocument &doc,
        const QString &error,
        int status) {
    if (!error.isEmpty() || !doc.isObject()) {
        LOG(("FoxMes: scheduled send failed (%1, %2)"
            ).arg(status).arg(error));
        ShowSettingsToast(
            _session,
            history->peer,
            error.isEmpty() ? u"Failed to schedule the message"_q : error);
        return;
    }
    const auto object = doc.object();
    const auto items = object.value("items").toArray();
    if (object.value("delivered").toBool()) {
        for (const auto &value : items) {
            if (value.isObject()) {
                applyMessage(
                    history,
                    value.toObject(),
                    false,
                    NewMessageType::Unread);
            }
        }
        _session->data().sendHistoryChangeNotifications();
        return;
    }
    auto list = QVector<MTPMessage>();
    list.reserve(items.size());
    for (const auto &value : items) {
        if (!value.isObject()) {
            continue;
        }
        if (auto message = prepareReminder(history, value.toObject())) {
            list.push_back(std::move(*message));
        }
    }
    Scheduled::Apply(&_session->scheduledMessages(), history, list, false);
}

void NativeBridge::clearCloudDraftFor(qint64 chatId) {
    const auto weak = QPointer<NativeBridge>(this);
    const auto revision = _draftRevisionByChat[chatId];
    client().setDraft(
        chatId,
        QString(),
        0,
        revision,
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        [weak, chatId](QJsonDocument doc, QString error, int) {
            if (!weak) return;
            const auto next = doc.object()
                .value("draft").toObject()
                .value("revision").toVariant().toLongLong();
            weak->_draftRevisionByChat[chatId] = (next > 0) ? next : 0;
        });
}

void NativeBridge::dropOptimisticItems(
        History *history,
        const std::vector<qint64> &localIds) {
    if (!history) {
        return;
    }
    for (const auto localId : localIds) {
        if (const auto item = _session->data().message(
                history->peer->id,
                MsgId(localId))) {
            _session->data().destroyMessageWithCacheCleanup(item);
        }
    }
}

void NativeBridge::sendFiles(
        History *history,
        std::vector<UploadSpec> files,
        const TextWithEntities &caption,
        ReplyTarget replyTo,
        const std::vector<QString> &reuseClientNonces,
        const SendOptions &options) {
    if (!history || files.empty()) return;
    auto trimmedCaption = caption;
    TextUtilities::Trim(trimmedCaption);
    const auto discussion = TopicChannels::IsDiscussionPeer(history->peer);
    const auto groupedId = (files.size() > 1)
        ? base::RandomValue<uint64>()
        : uint64(0);
    auto localIds = std::vector<qint64>();
    auto nonces = std::vector<QString>();
    localIds.reserve(files.size());
    nonces.reserve(files.size());
    for (auto index = 0; index != int(files.size()); ++index) {
        const auto &file = files[index];
        auto local = LocalAttachment{
            .bytes = (UploadIsPhoto(file) || !file.content.isEmpty())
                ? LoadUploadBytes(file)
                : QByteArray(),
            .path = file.path,
            .forceFile = file.forceFile,
        };
        auto keepMedia = std::shared_ptr<Data::DocumentMedia>();
        const auto item = createPendingFileMessage(
            history,
            file,
            index ? TextWithEntities() : trimmedCaption,
            replyTo,
            local,
            groupedId,
            options.mediaTtlSeconds,
            keepMedia);
        if (!item) {
            for (const auto localId : localIds) {
                failPendingSend(localId, kSendNoStatus);
            }
            return;
        }
        auto nonce = (index < int(reuseClientNonces.size())
            && !reuseClientNonces[index].isEmpty())
            ? reuseClientNonces[index]
            : QUuid::createUuid().toString(QUuid::WithoutBraces);
        rememberPendingSend(
            item,
            nonce,
            replyTo,
            QString(),
            EntitiesInText(),
            Data::WebPageDraft(),
            index ? TextWithEntities() : trimmedCaption,
            std::vector<UploadSpec>{ file },
            std::move(local));
        _pendingSends[item->id.bare].groupedId = groupedId;
        _pendingSends[item->id.bare].localMedia = std::move(keepMedia);
        localIds.push_back(item->id.bare);
        nonces.push_back(std::move(nonce));
    }
    _session->data().sendHistoryChangeNotifications();
    const auto weak = QPointer<NativeBridge>(this);
    const auto forceFile = files.front().forceFile;
    auto upload = [weak, history, files = std::move(files), trimmedCaption, replyTo, localIds, nonces, forceFile, options](UploadTarget target) mutable {
        if (!weak || !history) return;
        const auto chatId = target.chatId;
        const auto failAll = [weak, history, localIds](
                int status,
                QString error = QString()) {
            if (!SendMayBeRetried(status) && !error.isEmpty()) {
                ShowSettingsToast(weak->_session, history->peer, error);
            }
            for (const auto localId : localIds) {
                weak->failPendingSend(localId, status);
            }
        };
        if (chatId <= 0 && target.topicId <= 0) {
            LOG(("NativeBridge: file send has no chat id"));
            failAll(kSendNoStatus);
            return;
        }
        auto index = std::make_shared<size_t>(0);
        auto ids = std::make_shared<QList<qint64>>();
        auto posters = std::make_shared<QMap<qint64, QString>>();
        auto meta = std::make_shared<QMap<qint64, AttachmentMeta>>();
        auto next = std::make_shared<std::function<void()>>();
        *next = [weak, history, target, chatId, files = std::move(files), trimmedCaption, replyTo, localIds, nonces, forceFile, options, failAll, index, ids, posters, meta, next]() mutable {
            if (!weak || !history) return;
            if (*index >= files.size()) {
                for (const auto localId : localIds) {
                    if (const auto i = weak->_pendingSends.find(localId)
                        ; i != weak->_pendingSends.end()) {
                        i->second.committing = true;
                        i->second.cancelUpload = nullptr;
                    }
                }
                if (target.topicId > 0) {
                    auto body = TopicChannels::CommentBody(
                        weak.data(),
                        history,
                        trimmedCaption);
                    auto attachments = QJsonArray();
                    for (const auto id : *ids) {
                        attachments.push_back(id);
                    }
                    body.insert("attachment_ids", attachments);
                    body.insert("force_file", forceFile);
                    body.insert("attachment_posters", AttachmentPostersJson(*posters));
                    body.insert("attachment_meta", AttachmentMetaJson(*meta));
                    TopicChannels::PostThreadComment(
                        weak.data(),
                        history,
                        MsgId(replyTo.messageId),
                        MsgId(),
                        std::move(body),
                        nonces.front(),
                        [weak, history, localIds](QString error, int status) {
                            if (weak) {
                                weak->finishCommentSend(history, localIds, error, status);
                            }
                        });
                    return;
                }
                auto items = QList<ApiClient::AlbumItem>();
                items.reserve(ids->size());
                for (auto i = 0; i != ids->size(); ++i) {
                    const auto attachmentId = (*ids)[i];
                    items.push_back(ApiClient::AlbumItem{
                        .clientNonce = nonces[i],
                        .attachmentId = attachmentId,
                        .meta = meta->value(attachmentId),
                    });
                }
                const auto outgoing = weak->entitiesToJson(
                    trimmedCaption.entities,
                    trimmedCaption.text);
                if (options.scheduled()) {
                    weak->client().createReminderAlbum(
                        chatId,
                        trimmedCaption.text,
                        outgoing,
                        replyTo.messageId,
                        items,
                        forceFile,
                        *posters,
                        options,
                        QUuid::createUuid().toString(QUuid::WithoutBraces),
                        [weak, history, localIds, failAll](
                                QJsonDocument doc,
                                QString error,
                                int status) {
                            if (!weak || !history) return;
                            if (!error.isEmpty() || !doc.isObject()) {
                                LOG(("NativeBridge: scheduled file send failed (%1, %2)"
                                    ).arg(status).arg(error));
                                failAll(status, error);
                                return;
                            }
                            failAll(kSendServerAccepted);
                            weak->dropOptimisticItems(history, localIds);
                            weak->applyReminderResponse(history, doc, {}, status);
                            weak->_session->data().sendHistoryChangeNotifications();
                        });
                    return;
                }
                weak->client().sendAlbum(chatId, trimmedCaption.text, outgoing, replyTo.messageId, items, forceFile, *posters,
                    [weak, history, localIds, failAll](QJsonDocument doc, QString error, int status) {
                        if (!weak || !history) return;
                        if (!error.isEmpty() || !doc.isObject()) {
                            LOG(("NativeBridge: file message send failed (%1, %2)"
                                ).arg(status).arg(error));
                            failAll(status, error);
                            return;
                        }
                        const auto response = doc.object();
                        const auto items = response.value("items").toArray();
                        if (items.isEmpty()) {
                            failAll(status);
                            return;
                        }
                        for (const auto &value : items) {
                            if (!value.isObject()) continue;
                            const auto object = value.toObject();
                            const auto nonce = object
                                .value("client_nonce").toString().trimmed();
                            const auto known = !nonce.isEmpty()
                                && weak->_pendingSendNonceToLocalId.contains(
                                    nonce);
                            if (known) {
                                const auto localId = weak
                                    ->_pendingSendNonceToLocalId.at(nonce);
                                if (weak->finishCancelledCommit(
                                        localId,
                                        object)) {
                                    continue;
                                }
                            }
                            weak->applyMessage(
                                history,
                                object,
                                false,
                                known
                                    ? NewMessageType::Existing
                                    : NewMessageType::Unread);
                        }
                        failAll(kSendServerAccepted);
                        weak->_session->data().sendHistoryChangeNotifications();
                    }, options);
                return;
            }
            const auto fileIndex = (*index)++;
            const auto file = files[fileIndex];
            const auto localId = localIds[fileIndex];
            const auto isPhoto = UploadIsPhoto(file);
            const auto uploaded = [weak, next, ids, posters, meta, file, target, failAll](QJsonDocument doc, QString error, int status) {
                if (!weak) return;
                if (!error.isEmpty() || !doc.isObject()) {
                    LOG(("NativeBridge: attachment upload failed (%1, %2)"
                        ).arg(status).arg(error));
                    failAll(status, error);
                    return;
                }
                const auto data = doc.object().value("data").toObject();
                const auto id = data.value("id").toVariant().toLongLong();
                if (id <= 0) {
                    LOG(("NativeBridge: attachment upload returned no id (%1)"
                        ).arg(status));
                    failAll(status);
                    return;
                }
                ids->push_back(id);
                if (!file.kind.isEmpty()
                    || file.durationMs > 0
                    || !file.waveform.isEmpty()
                    || !file.performer.isEmpty()
                    || !file.title.isEmpty()
                    || file.spoiler) {
                    meta->insert(id, AttachmentMeta{
                        .kind = file.kind,
                        .durationMs = file.durationMs,
                        .waveform = file.waveform,
                        .performer = file.performer,
                        .title = file.title,
                        .spoiler = file.spoiler,
                    });
                }
                const auto poster = data.value("poster").toString().trimmed();
                if (!poster.isEmpty()) {
                    posters->insert(id, poster);
                }
                if (file.cover.isEmpty()) {
                    (*next)();
                    return;
                }
                weak->client().uploadData(
                    u"cover.jpg"_q,
                    file.cover,
                    u"image/jpeg"_q,
                    target,
                    false,
                    [weak, next, posters, id](
                            QJsonDocument doc,
                            QString error,
                            int status) {
                        if (!weak) return;
                        const auto url = doc.object()
                            .value("data").toObject()
                            .value("url").toString().trimmed();
                        if (error.isEmpty() && !url.isEmpty()) {
                            posters->insert(id, url);
                        } else {
                            LOG(("NativeBridge: cover upload failed (%1, %2)"
                                ).arg(status).arg(error));
                        }
                        (*next)();
                    },
                    {},
                    u"photo"_q);
            };
            const auto onProgress = [weak, localId, isPhoto](
                    qint64 sent,
                    qint64 total) {
                if (!weak || total <= 0) return;
                const auto mediaId = LocalAttachmentMediaId(localId);
                auto &owner = weak->_session->data();
                if (isPhoto) {
                    const auto photo = owner.photo(mediaId);
                    if (!photo->uploadingData) {
                        photo->uploadingData
                            = std::make_unique<Data::UploadState>(total);
                    }
                    photo->uploadingData->size = total;
                    photo->uploadingData->offset = sent;
                    owner.requestPhotoViewRepaint(photo);
                } else {
                    const auto document = owner.document(mediaId);
                    if (!document->uploadingData) {
                        document->uploadingData
                            = std::make_unique<Data::UploadState>(total);
                    }
                    document->uploadingData->size = total;
                    document->uploadingData->offset = sent;
                    owner.requestDocumentViewRepaint(document);
                }
            };
            auto cancel = ApiClient::CancelHandle();
            if (!file.path.isEmpty()) {
                cancel = weak->client().uploadFile(
                    file.path,
                    file.mime,
                    target,
                    file.forceFile,
                    uploaded,
                    onProgress,
                    file.kind);
            } else if (!file.content.isEmpty()) {
                cancel = weak->client().uploadData(
                    file.displayName.isEmpty() ? u"upload.bin"_q : file.displayName,
                    file.content,
                    file.mime,
                    target,
                    file.forceFile,
                    uploaded,
                    onProgress,
                    file.kind);
            } else {
                LOG(("NativeBridge: attachment has neither path nor content"));
                failAll(kSendNoStatus);
                return;
            }
            for (const auto pendingId : localIds) {
                if (const auto i = weak->_pendingSends.find(pendingId)
                    ; i != weak->_pendingSends.end()) {
                    i->second.cancelUpload = cancel;
                }
            }
        };
        (*next)();
    };
    if (discussion) {
        upload(UploadTarget{
            .topicId = TopicChannels::UploadTopicOf(
                history,
                MsgId(replyTo.messageId)),
        });
    } else {
        ensureChat(history, [upload = std::move(upload)](qint64 chatId) mutable {
            upload(UploadTarget{ .chatId = chatId });
        });
    }
}

void NativeBridge::sendComment(
        History *history,
        const TextWithEntities &text,
        ReplyTarget replyTo,
        MsgId rootId,
        std::optional<MsgId> localMessageId,
        const QString &reuseClientNonce) {
    if (!history || text.text.isEmpty()) {
        return;
    }
    if (!replyTo && rootId) {
        replyTo.messageId = rootId.bare;
    }
    const auto item = createPendingTextMessage(
        history,
        text.text,
        text.entities,
        replyTo,
        localMessageId
            ? *localMessageId
            : _session->data().nextLocalMessageId());
    if (!item) {
        return;
    }
    const auto nonce = reuseClientNonce.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : reuseClientNonce;
    rememberPendingSend(
        item,
        nonce,
        replyTo,
        text.text,
        text.entities,
        Data::WebPageDraft(),
        TextWithEntities(),
        {});
    const auto localId = item->id.bare;
    _session->data().sendHistoryChangeNotifications();
    const auto weak = QPointer<NativeBridge>(this);
    TopicChannels::PostThreadComment(
        this,
        history,
        MsgId(replyTo.messageId),
        rootId,
        TopicChannels::CommentBody(this, history, text),
        nonce,
        [weak, history, localId](QString error, int status) {
            if (weak) {
                weak->finishCommentSend(history, { localId }, error, status);
            }
        });
}

void NativeBridge::finishCommentSend(
        History *history,
        const std::vector<qint64> &localIds,
        const QString &error,
        int status) {
    if (error.isEmpty()) {
        for (const auto localId : localIds) {
            clearPendingSend(localId);
        }
        dropOptimisticItems(history, localIds);
    } else {
        if (!SendMayBeRetried(status) && history) {
            ShowSettingsToast(
                _session,
                history->peer,
                TopicChannels::FailureText(error));
        }
        for (const auto localId : localIds) {
            failPendingSend(localId, status);
        }
    }
    _session->data().sendHistoryChangeNotifications();
}

void NativeBridge::cancelSend(HistoryItem *item) {
    if (!item) {
        return;
    }
    const auto localId = item->id.bare;
    const auto i = _pendingSends.find(localId);
    if (i == _pendingSends.end()) {
        return;
    }
    if (i->second.committing) {
        i->second.cancelledAfterCommit = true;
        i->second.cancelUpload = nullptr;
        return;
    }
    const auto cancel = i->second.cancelUpload;
    clearPendingSend(localId);
    if (cancel) {
        cancel();
    }
}

bool NativeBridge::finishCancelledCommit(
        qint64 localId,
        const QJsonObject &message) {
    const auto i = _pendingSends.find(localId);
    if (i == _pendingSends.end() || !i->second.cancelledAfterCommit) {
        return false;
    }
    const auto history = i->second.history;
    const auto messageId = message.value("id").toVariant().toLongLong();
    clearPendingSend(localId);
    if (!history || messageId <= 0 || messageId > INT32_MAX) {
        return true;
    }
    deleteMessages(history, { int32_t(messageId) }, true);
    return true;
}

bool NativeBridge::pendingSendReplayable(
        const PendingSendRequest &request) const {
    if (request.inFlight
        || request.cancelledAfterCommit
        || !request.history) {
        return false;
    }
    const auto replays = _sendReplays.find(request.clientNonce);
    if (replays != _sendReplays.end() && replays->second >= kMaxSendReplays) {
        return false;
    }
    for (const auto &file : request.files) {
        if (file.content.isEmpty() && !QFileInfo::exists(file.path)) {
            return false;
        }
    }
    return true;
}

void NativeBridge::resendPendingSends() {
    if (_pendingSends.empty()) {
        return;
    }
    auto replay = std::vector<qint64>();
    for (const auto &[localId, request] : _pendingSends) {
        if (pendingSendReplayable(request)) {
            replay.push_back(localId);
        }
    }
    std::sort(replay.begin(), replay.end());
    for (auto i = replay.begin(); i != replay.end();) {
        const auto found = _pendingSends.find(*i);
        if (found == _pendingSends.end()) {
            ++i;
            continue;
        }
        const auto &first = found->second;
        auto group = std::vector<qint64>{ *i };
        auto next = i + 1;
        if (const auto groupedId = first.groupedId) {
            while (next != replay.end()) {
                const auto other = _pendingSends.find(*next);
                if (other == _pendingSends.end()
                    || other->second.groupedId != groupedId) {
                    break;
                }
                group.push_back(*next);
                ++next;
            }
        }
        i = next;
        resendPendingGroup(group);
    }
}

void NativeBridge::resendPendingGroup(const std::vector<qint64> &localIds) {
    if (localIds.empty()) {
        return;
    }
    const auto first = _pendingSends.find(localIds.front());
    if (first == _pendingSends.end()) {
        return;
    }
    const auto history = first->second.history;
    const auto caption = first->second.caption;
    const auto replyTo = first->second.replyTo;
    const auto text = first->second.text;
    const auto entities = first->second.entities;
    const auto webPage = first->second.webPage;

    auto files = std::vector<UploadSpec>();
    auto nonces = std::vector<QString>();
    auto items = std::vector<not_null<HistoryItem*>>();
    for (const auto localId : localIds) {
        const auto i = _pendingSends.find(localId);
        if (i == _pendingSends.end() || i->second.history != history) {
            return;
        }
        const auto item = _session->data().message(
            history->peer,
            MsgId(localId));
        if (!item || (!item->isSending() && !item->hasFailed())) {
            return;
        }
        items.push_back(item);
        nonces.push_back(i->second.clientNonce);
        for (const auto &file : i->second.files) {
            files.push_back(file);
        }
    }
    auto replays = std::vector<int>();
    replays.reserve(nonces.size());
    for (const auto &nonce : nonces) {
        const auto i = _sendReplays.find(nonce);
        replays.push_back((i == _sendReplays.end() ? 0 : i->second) + 1);
    }
    LOG(("FoxMes: resending %1 message(s) from %2 after reconnect"
        ).arg(localIds.size()).arg(localIds.front()));
    for (const auto localId : localIds) {
        clearPendingSend(localId);
    }
    for (const auto &item : items) {
        item->destroy();
    }
    if (files.empty()) {
        sendText(
            history,
            text,
            entities,
            webPage,
            replyTo,
            std::nullopt,
            false,
            MsgId(),
            PeerId(),
            nonces.front());
    } else {
        sendFiles(history, std::move(files), caption, replyTo, nonces);
    }
    for (auto i = 0; i != int(nonces.size()); ++i) {
        _sendReplays[nonces[i]] = replays[i];
    }
}

bool NativeBridge::retryFailedMessage(HistoryItem *item) {
    if (!item || !item->hasFailed()) {
        return false;
    }
    const auto i = _pendingSends.find(item->id.bare);
    if (i == _pendingSends.end()) {
        return false;
    }
    const auto request = i->second;
    clearPendingSend(item->id.bare);
    item->destroy();
    if (!request.history) {
        return false;
    }
    if (request.files.empty()) {
        sendText(
            request.history,
            request.text,
            request.entities,
            request.webPage,
            request.replyTo,
            std::nullopt,
            false,
            MsgId(),
            PeerId(),
            request.clientNonce);
    } else {
        sendFiles(
            request.history,
            request.files,
            request.caption,
            request.replyTo,
            { request.clientNonce });
    }
    return true;
}

void NativeBridge::editText(
        HistoryItem *item,
        const TextWithEntities &text,
        Data::WebPageDraft webPage,
        std::function<void(QString)> done) {
    auto edited = text;
    TextUtilities::Trim(edited);
    if (!item || edited.text.isEmpty()) {
        if (done) done(u"MESSAGE_EMPTY"_q);
        return;
    }
    if (TopicChannels::IsDiscussionPeer(item->history()->peer)) {
        TopicChannels::EditComment(this, item, edited, std::move(done));
        return;
    }
    const auto history = item->history().get();
    const auto messageId = item->id.bare;
    if (!_pendingEdits.emplace(messageId).second) {
        if (done) done(u"MESSAGE_EDIT_PENDING"_q);
        return;
    }
    if (item->isScheduled()) {
        const auto weak = QPointer<NativeBridge>(this);
        Scheduled::Edit(
            item,
            edited.text,
            entitiesToJson(edited.entities, edited.text, webPage),
            [weak, messageId, done = std::move(done)](QString error) {
                if (weak) {
                    weak->_pendingEdits.erase(messageId);
                }
                if (done) done(std::move(error));
            });
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    const auto chatId = chatIdFor(history);
    const auto revision = [&] {
        const auto i = _messageRevisions.find(SeenKey(chatId, messageId));
        return (i == _messageRevisions.end()) ? qint64(1) : i->second;
    }();
    client().editMessage(messageId, edited.text, entitiesToJson(edited.entities, edited.text, webPage), revision, [weak, history, messageId, done = std::move(done)](
            QJsonDocument doc,
            QString error,
            int status) mutable {
        if (!weak) return;
        weak->_pendingEdits.erase(messageId);
        if (error.isEmpty() && doc.isObject()) {
            weak->applyMessage(history, doc.object(), true);
            weak->_session->data().sendHistoryChangeNotifications();
            weak->reloadChats();
        } else if (status == 409 && doc.isObject()) {
            const auto current = doc.object().value("current").toObject();
            if (!current.isEmpty()) {
                weak->applyMessage(history, current, true);
                weak->_session->data().sendHistoryChangeNotifications();
            }
            error = u"MESSAGE_REVISION_CONFLICT"_q;
        }
        if (done) done(std::move(error));
    });
}

void NativeBridge::deleteMessages(
        History *history,
        const std::vector<int32_t> &ids,
        bool revoke,
        std::function<void()> done) {
    Q_UNUSED(revoke);
    auto finishOnce = std::make_shared<bool>(false);
    const auto finish = [done = std::move(done), finishOnce] {
        if (done && !std::exchange(*finishOnce, true)) done();
    };
    if (!history) {
        finish();
        return;
    }
    const auto chatId = chatIdFor(history);
    if (chatId <= 0) {
        finish();
        return;
    }
    auto messageIds = QList<qint64>();
    for (const auto raw : ids) {
        if (raw > 0 && _pendingDeletes.emplace(raw).second) messageIds.push_back(raw);
    }
    if (messageIds.isEmpty()) {
        finish();
        return;
    }
    for (const auto messageId : messageIds) {
        removeMessageFrom(history, chatId, messageId);
    }
    const auto weak = QPointer<NativeBridge>(this);
    const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const std::function<void(QJsonDocument)> applyResult = [weak, history, chatId, messageIds, finish](QJsonDocument doc) {
        if (!weak) {
            finish();
            return;
        }
        auto acknowledged = false;
        if (doc.isObject()) {
            const auto obj = doc.object();
            const auto readIds = [&](const QString field) {
                auto out = QList<qint64>();
                for (const auto &value : obj.value(field).toArray()) {
                    const auto messageId = value.toVariant().toLongLong();
                    if (messageId > 0) out.push_back(messageId);
                }
                return out;
            };
            for (const auto messageId : readIds("deleted_ids")) {
                weak->_pendingDeletes.erase(messageId);
                acknowledged = true;
            }
            const auto skippedPinned = readIds("skipped_pinned_ids");
            const auto refused = skippedPinned + readIds("rejected_ids");
            if (!skippedPinned.isEmpty() && history) {
                ShowSettingsToast(
                    weak->_session,
                    history->peer,
                    u"Unpin the message before deleting it"_q);
            }
            for (const auto messageId : refused) {
                weak->_pendingDeletes.erase(messageId);
                weak->_seenMessages.erase(SeenKey(chatId, messageId));
                weak->_messageRevisions.erase(SeenKey(chatId, messageId));
            }
            if (!refused.isEmpty() && history) {
                LOG(("FoxMes: %1 deletions refused in chat %2, reloading"
                    ).arg(refused.size()).arg(chatId));
                weak->_bottomLoadedChats.erase(chatId);
                weak->_loadedChats.erase(chatId);
                weak->_nextHistoryBefore.erase(chatId);
                weak->loadHistoryPage(
                    history,
                    ShowAtTheEndMsgId.bare,
                    Data::LoadDirection::Around,
                    nullptr);
            }
        }
        for (const auto messageId : messageIds) {
            weak->_pendingDeletes.erase(messageId);
        }
        if (!acknowledged) {
            LOG(("FoxMes: batch delete in chat %1 confirmed no ids "
                "(response object: %2)"
                ).arg(chatId).arg(doc.isObject() ? 1 : 0));
        } else {
            weak->reloadChats();
        }
        finish();
    };
    client().deleteMessages(chatId, messageIds, operationId, [weak, chatId, operationId, applyResult, finish](
            QJsonDocument doc,
            QString error,
            int status) mutable {
        if (!error.isEmpty() && (status == 0 || status >= 500)) {
            if (weak) {
                weak->client().operationResult(operationId, [applyResult](QJsonDocument result, QString, int) {
                    if (result.isObject() && result.object().value("status").toString() == "done") {
                        applyResult(QJsonDocument(result.object().value("result").toObject()));
                    } else {
                        applyResult(QJsonDocument());
                    }
                });
            } else {
                finish();
            }
            return;
        }
        applyResult((error.isEmpty() && doc.isObject()) ? doc : QJsonDocument());
    });
}

void NativeBridge::removeHistoryThrough(
        qint64 chatId,
        qint64 throughMessageId,
        const QJsonArray &skippedPinnedIds) {
    if (throughMessageId <= 0 || throughMessageId > INT32_MAX) {
        return;
    }
    const auto history = historyForChatId(chatId);
    if (!history) {
        return;
    }
    auto skipped = std::unordered_set<qint64>();
    for (const auto &value : skippedPinnedIds) {
        skipped.emplace(value.toVariant().toLongLong());
    }
    auto remove = std::vector<not_null<HistoryItem*>>();
    auto removedIds = std::vector<qint64>();
    for (const auto &block : history->blocks) {
        for (const auto &view : block->messages) {
            const auto item = view->data();
            const auto id = item->id.bare;
            if (id > 0
                && id <= throughMessageId
                && !skipped.contains(id)) {
                remove.push_back(item);
                removedIds.push_back(id);
            }
        }
    }
    if (!remove.empty()) {
        DeleteMessagesWithEffect(_session, remove);
        for (const auto id : removedIds) {
            _seenMessages.erase(SeenKey(chatId, id));
            _messageRevisions.erase(SeenKey(chatId, id));
        }
        history->updateChatListExistence();
        _session->data().sendHistoryChangeNotifications();
    }
}

void NativeBridge::deleteHistory(History *history, bool deleteConversation) {
    if (!history) {
        return;
    }
    const auto chatId = chatIdFor(history);
    if (chatId <= 0) {
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    const auto completed = [weak, history, chatId, deleteConversation](
            QJsonDocument doc,
            QString error,
            int) {
        if (!weak || !history) {
            return;
        }
        if (!error.isEmpty() || !doc.isObject()) {
            ShowSettingsToast(weak->_session, history->peer, error.isEmpty()
                ? u"Delete failed"_q
                : error);
            return;
        }
        if (deleteConversation) {
            weak->removeChat(chatId);
        } else {
            const auto object = doc.object();
            weak->removeHistoryThrough(
                chatId,
                object.value("through_message_id").toVariant().toLongLong(),
                object.value("skipped_pinned_ids").toArray());
            weak->_bottomLoadedChats.erase(chatId);
        }
        weak->reloadChats();
    };
    if (deleteConversation) {
        client().deleteChat(chatId, completed);
    } else {
        client().deleteHistory(chatId, completed);
    }
}

void NativeBridge::deleteMessagesByDates(
        History *history,
        qint64 minDate,
        qint64 maxDate,
        std::function<void()> done) {
    const auto chatId = chatIdFor(history);
    if (!history || chatId <= 0) {
        if (done) done();
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().deleteMessagesByDate(chatId, minDate, maxDate,
        [weak, history, chatId, done = std::move(done)](
                QJsonDocument doc,
                QString error,
                int) mutable {
            if (weak && history && error.isEmpty() && doc.isObject()) {
                const auto object = doc.object();
                for (const auto &value : object.value("deleted_ids").toArray()) {
                    weak->removeMessage(chatId, value.toVariant().toLongLong());
                }
                weak->reloadChats();
            } else if (weak && history && !error.isEmpty()) {
                ShowSettingsToast(weak->_session, history->peer, error);
            }
            if (done) done();
        });
}

void NativeBridge::applyReadState(
        qint64 chatId,
        qint64 readerId,
        qint64 readThroughId,
        TimeId readAt,
        int unreadCount,
        qint64 stateRevision) {
    if (chatId <= 0 || readerId <= 0 || readThroughId < 0) return;
    const auto key = SeenKey(chatId, readerId);
    if (const auto i = _appliedReadStates.find(key); i != _appliedReadStates.end()) {
        if (stateRevision < i->second.stateRevision
            || readThroughId < i->second.readThroughId) {
            return;
        }
        if (!readAt) {
            readAt = i->second.readAt;
        }
    }
    _appliedReadStates[key] = AppliedReadState{
        .readThroughId = readThroughId,
        .stateRevision = stateRevision,
        .readAt = readAt,
    };

    const auto history = historyForChatId(chatId);
    if (!history) return;
    if (readerId == client().meId()) {
        if (readThroughId > 0 && readThroughId <= INT32_MAX) {
            history->inboxRead(MsgId(int32(readThroughId)), qMax(unreadCount, 0));
        } else {
            history->setUnreadCount(qMax(unreadCount, 0));
        }
    } else if (readThroughId > 0 && readThroughId <= INT32_MAX
        && history->peer->id == peerFromUser(UserId(readerId))) {
        history->outboxRead(MsgId(int32(readThroughId)));
    }
    _session->data().sendHistoryChangeNotifications();
}

void NativeBridge::applyReceiptSnapshot(
        History *history,
        qint64 chatId,
        const QJsonObject &receiptState) {
    if (!history || receiptState.isEmpty()) return;
    const auto apply = [this, chatId](const QJsonObject &state) {
        applyReadState(
            chatId,
            state.value("reader_id").toVariant().toLongLong(),
            state.value("read_through_id").toVariant().toLongLong(),
            unixTime(state.value("read_at").toString()),
            state.value("unread_count").toInt(),
            state.value("state_revision").toVariant().toLongLong());
    };
    apply(receiptState.value("inbox").toObject());
    const auto outbox = receiptState.value("outbox").toObject();
    apply(outbox);

    if (history->peer->isSelf()) {
        const auto through = outbox.value("read_through_id").toVariant().toLongLong();
        if (through > 0 && through <= INT32_MAX) {
            history->outboxRead(MsgId(int32(through)));
        }
    }
}

std::vector<NativeBridge::ChatReader> NativeBridge::readersThrough(
        History *history,
        qint64 messageId) const {
    auto result = std::vector<ChatReader>();
    const auto chatId = chatIdFor(history);
    if (chatId <= 0 || messageId <= 0) {
        return result;
    }
    const auto me = client().meId();
    for (const auto &[key, state] : _appliedReadStates) {
        if (key.chatId != chatId
            || key.messageId == me
            || state.readThroughId < messageId) {
            continue;
        }
        result.push_back(ChatReader{
            .userId = key.messageId,
            .date = state.readAt,
        });
    }
    std::sort(result.begin(), result.end(), [](
            const ChatReader &a,
            const ChatReader &b) {
        return a.date > b.date;
    });
    return result;
}

void NativeBridge::loadReadJournal() {
    const auto stored = LoadReadJournal(_session);
    for (auto i = stored.begin(); i != stored.end(); ++i) {
        const auto chatId = i.key().toLongLong();
        const auto desired = i.value().toVariant().toLongLong();
        if (chatId > 0 && desired > 0) {
            _readJournal[chatId].desired = desired;
        }
    }
}

void NativeBridge::persistReadJournal() const {
    auto stored = QJsonObject();
    for (const auto &[chatId, entry] : _readJournal) {
        if (entry.desired > 0) {
            stored.insert(QString::number(chatId), QString::number(entry.desired));
        }
    }
    SaveReadJournal(_session, stored);
}

void NativeBridge::enqueueRead(
        qint64 chatId,
        qint64 desired,
        std::function<void()> done) {
    if (chatId <= 0 || desired <= 0) {
        if (done) done();
        return;
    }
    auto &entry = _readJournal[chatId];
    entry.desired = qMax(entry.desired, desired);
    if (done) entry.completions.push_back(std::move(done));
    persistReadJournal();
    if (!entry.inFlight && !_readJournalPaused) {
        entry.retryAtMs = 0;
        sendJournalRead(chatId);
    }
}

void NativeBridge::sendJournalRead(qint64 chatId) {
    const auto i = _readJournal.find(chatId);
    if (i == _readJournal.end() || i->second.inFlight || _readJournalPaused) return;
    auto &entry = i->second;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (entry.retryAtMs > now) {
        scheduleReadRetry();
        return;
    }
    const auto sent = entry.desired;
    entry.inFlight = true;
    const auto weak = QPointer<NativeBridge>(this);
    client().markRead(chatId, sent, [weak, chatId](
            QJsonDocument document,
            QString error,
            int status) mutable {
        if (!weak) return;
        const auto i = weak->_readJournal.find(chatId);
        if (i == weak->_readJournal.end()) return;
        auto &entry = i->second;
        entry.inFlight = false;
        auto completions = std::move(entry.completions);
        entry.completions.clear();
        auto finishCompletions = [completions = std::move(completions)]() mutable {
            for (auto &completion : completions) {
                if (completion) completion();
            }
        };

        if (error.isEmpty() && status >= 200 && status < 300 && document.isObject()) {
            const auto state = document.object();
            const auto acknowledged = state.value("read_through_id").toVariant().toLongLong();
            weak->applyReadState(
                state.value("chat_id").toVariant().toLongLong(),
                state.value("reader_id").toVariant().toLongLong(),
                acknowledged,
                unixTime(state.value("read_at").toString()),
                state.value("unread_count").toInt(),
                state.value("state_revision").toVariant().toLongLong());
            if (acknowledged >= entry.desired) {
                weak->_readJournal.erase(i);
                weak->persistReadJournal();
                weak->scheduleReadRetry();
                finishCompletions();
                return;
            }
            entry.backoffSeconds = 1;
            entry.retryAtMs = 0;
            weak->sendJournalRead(chatId);
            finishCompletions();
            return;
        }

        if (status == 401) {
            weak->persistReadJournal();
            finishCompletions();
            weak->pauseReadJournalForUnauthorized();
            return;
        }
        if (status == 400 || status == 403 || status == 404) {
            weak->_readJournal.erase(i);
            weak->persistReadJournal();
            weak->reloadChats();
            weak->scheduleReadRetry();
            finishCompletions();
            return;
        }

        auto delay = entry.backoffSeconds;
        if (status == 429 && document.isObject()) {
            delay = qMax(delay, document.object().value("_retry_after_seconds").toInt());
        }
        delay = qBound(1, delay, 60);
        entry.retryAtMs = QDateTime::currentMSecsSinceEpoch() + qint64(delay) * 1000;
        entry.backoffSeconds = qMin(delay * 2, 60);
        weak->scheduleReadRetry();
        finishCompletions();
    });
}

void NativeBridge::scheduleReadRetry() {
    _readRetryTimer.stop();
    if (_readJournalPaused) return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto earliest = qint64(0);
    for (const auto &entryPair : _readJournal) {
        const auto &entry = entryPair.second;
        if (entry.inFlight) continue;
        const auto retryAt = qMax(entry.retryAtMs, now);
        if (!earliest || retryAt < earliest) earliest = retryAt;
    }
    if (earliest) {
        _readRetryTimer.start(int(qMin<qint64>(earliest - now, INT_MAX)));
    }
}

void NativeBridge::flushReadJournal() {
    if (_readJournalPaused) return;
    auto chatIds = std::vector<qint64>();
    chatIds.reserve(_readJournal.size());
    for (const auto &[chatId, entry] : _readJournal) {
        if (!entry.inFlight) chatIds.push_back(chatId);
    }
    for (const auto chatId : chatIds) sendJournalRead(chatId);
    scheduleReadRetry();
}

void NativeBridge::pauseReadJournalForUnauthorized() {
    if (_readJournalPaused) return;
    _readJournalPaused = true;
    _readRetryTimer.stop();
    const auto account = &_session->account();
    ClearLogin(_session);
    crl::on_main(account, [account] { account->foxmesLoggedOut(); });
}

void NativeBridge::markRead(History *history, qint64 messageId) {
    const auto chatId = chatIdFor(history);
    if (!chatId || !history) return;
    if (!messageId && history->lastMessage()) messageId = history->lastMessage()->id.bare;
    if (messageId <= 0) return;
    enqueueRead(chatId, messageId);
}

void NativeBridge::readHistory(History *history, qint64 tillId, std::function<void()> done) {
    const auto chatId = chatIdFor(history);
    if (!chatId || !history || tillId <= 0) {
        if (done) done();
        return;
    }
    enqueueRead(chatId, tillId, std::move(done));
}

void NativeBridge::sendTyping(History *history) {
    const auto chatId = chatIdFor(history);
    if (chatId > 0) client().typing(chatId);
}

void NativeBridge::setChatArchived(History *history, bool archived, std::function<void()> done) {
    const auto chatId = chatIdFor(history);
    if (!history || chatId <= 0) {
        if (done) done();
        return;
    }
    const auto wasPinned = history->isPinnedDialog(FilterId());
    const auto weak = QPointer<NativeBridge>(this);
    client().setChatArchived(chatId, archived, [weak, history, archived, wasPinned, done = std::move(done)](QJsonDocument, QString error, int) mutable {
        if (!weak || !history || !error.isEmpty()) {
            if (weak && history && !error.isEmpty()) {
                weak->reloadChats();
                ShowSettingsToast(
                    weak->_session,
                    history->peer,
                    u"Failed to update archive: "_q + error);
            }
            if (done) done();
            return;
        }
        if (archived) {
            history->setFolder(weak->_session->data().folder(Data::Folder::kId));
        } else {
            history->clearFolder();
        }
        if (done) done();
        weak->_session->data().sendHistoryChangeNotifications();
        if (wasPinned) {
            weak->_session->data().notifyPinnedDialogsOrderUpdated();
        }
    });
}

void NativeBridge::setChatPinned(
        History *history,
        bool pinned,
        std::function<void(bool success)> done) {
    const auto finish = [done = std::move(done)](bool success) {
        if (done) done(success);
    };
    if (!history) {
        finish(false);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    ensureChat(history, [weak, history, pinned, finish](qint64 chatId) mutable {
        if (!weak || !history || chatId <= 0) {
            finish(false);
            return;
        }
        weak->client().setChatPinned(chatId, pinned, [weak, history, pinned, finish](
                QJsonDocument,
                QString error,
                int) mutable {
            if (!weak || !history) {
                finish(false);
                return;
            }
            if (!error.isEmpty()) {
                weak->_session->data().setChatPinned(history, FilterId(), !pinned);
                weak->reloadChats();
                ShowSettingsToast(
                    weak->_session,
                    history->peer,
                    u"Failed to update pin: "_q + error);
                finish(false);
                return;
            }
            finish(true);
        });
    });
}

void NativeBridge::savePinnedOrder(Data::Folder *folder) {
    const auto &order = _session->data().pinnedChatsOrder(folder);
    auto ids = QList<qint64>();
    for (const auto &key : order) {
        if (const auto history = key.history()) {
            if (const auto chatId = chatIdFor(history)) {
                ids.append(chatId);
            }
        }
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().savePinnedOrder(ids, [weak](QJsonDocument, QString error, int) {
        if (!weak) return;
        if (!error.isEmpty()) {
            weak->reloadChats();
            return;
        }
        weak->_session->data().notifyPinnedDialogsOrderUpdated();
    });
}

void NativeBridge::setChatUnreadMark(History *history, bool marked) {
    if (!history) return;
    const auto weak = QPointer<NativeBridge>(this);
    ensureChat(history, [weak, marked](qint64 chatId) {
        if (!weak || chatId <= 0) return;
        weak->client().setChatUnreadMark(chatId, marked, [weak](QJsonDocument, QString error, int) {
            if (!weak || error.isEmpty()) return;
            weak->reloadChats();
        });
    });
}

void NativeBridge::rebuildPinnedOrder() {
    auto ordered = std::vector<std::pair<qint64, qint64>>();
    for (const auto &[chatId, rank] : _pinnedRanks) {
        if (rank > 0 && historyForChatId(chatId)) {
            ordered.push_back({rank, chatId});
        }
    }
    std::sort(ordered.begin(), ordered.end());
    std::unordered_set<qint64> wanted;
    for (const auto &[rank, chatId] : ordered) {
        wanted.insert(chatId);
    }
    auto &owner = _session->data();
    bool changed = false;
    for (const auto &key : owner.pinnedChatsOrder(static_cast<Data::Folder *>(nullptr))) {
        const auto history = key.history();
        if (!history) continue;
        if (!wanted.contains(chatIdFor(history)) && history->isPinnedDialog(FilterId())) {
            owner.setChatPinned(history, FilterId(), false);
            changed = true;
        }
    }
    for (const auto &[rank, chatId] : ordered) {
        if (const auto history = historyForChatId(chatId)) {
            if (!history->isPinnedDialog(FilterId())) {
                owner.setChatPinned(history, FilterId(), true);
                changed = true;
            }
        }
    }
    if (changed) {
        owner.notifyPinnedDialogsOrderUpdated();
    }
}

void NativeBridge::applyChatLookPatch(const QJsonObject &data) {
    const auto history = historyForChatId(
        data.value("chat_id").toVariant().toLongLong());
    if (!history) {
        reloadChats();
        return;
    }
    if (data.contains(u"wallpaper"_q)) {
        Wallpapers::ApplyForPeer(
            history->peer,
            data.value("wallpaper").toObject());
    }
    if (data.contains(u"theme"_q)) {
        ChatThemes::ApplyForPeer(
            history->peer,
            data.value("theme").toObject().value("theme_emoticon").toString());
    }
}

void NativeBridge::applyChatSettingsPatch(const QJsonObject &data) {
    const auto chatId = data.value("id").toVariant().toLongLong();
    if (chatId <= 0) return;
    const auto history = historyForChatId(chatId);
    if (!history) {
        reloadChats();
        return;
    }
    bool changed = false;
    bool needsReload = false;
    bool pinnedOrderStale = false;
    if (data.contains(u"marked_unread"_q)) {
        history->setUnreadMark(data.value("marked_unread").toBool());
        changed = true;
    }
    if (data.contains(u"archived"_q)) {
        pinnedOrderStale = history->isPinnedDialog(FilterId());
        if (data.value("archived").toBool()) {
            history->setFolder(_session->data().folder(Data::Folder::kId));
        } else {
            history->clearFolder();
        }
        history->updateChatListExistence();
        changed = true;
    }
    if (data.contains(u"pinned"_q)) {
        const auto pinned = data.value("pinned").toBool();
        if (!pinned) {
            if (_pinnedRanks.erase(chatId) > 0 || history->isPinnedDialog(FilterId())) {
                rebuildPinnedOrder();
                changed = true;
            }
        } else if (data.contains(u"pinned_rank"_q)) {
            const auto rank = qMax<qint64>(
                data.value("pinned_rank").toVariant().toLongLong(), 1);
            const auto old = _pinnedRanks.find(chatId);
            if (old == _pinnedRanks.end() || old->second != rank) {
                _pinnedRanks[chatId] = rank;
                rebuildPinnedOrder();
                changed = true;
            }
        } else {
            needsReload = true;
        }
    }
    if (data.contains(u"notification_settings"_q)) {
        applyNotificationSettings(
            history->peer,
            chatId,
            data.value("notification_settings").toObject());
        changed = true;
    }
    if (needsReload) {
        if (pinnedOrderStale) {
            _session->data().notifyPinnedDialogsOrderUpdated();
        }
        reloadChats();
        return;
    }
    if (changed) {
        _session->data().sendHistoryChangeNotifications();
    }
    if (pinnedOrderStale) {
        _session->data().notifyPinnedDialogsOrderUpdated();
    }
}

void NativeBridge::saveNotificationSettings(PeerData *peer) {
	if (!peer) {
		return;
	}
	const auto i = _chatByPeer.find(peer->id.value);
	if (i == _chatByPeer.end()) {
		return;
	}
	const auto chatId = i->second;
	auto muteUntil = static_cast<qint64>(peer->notify().muteUntil().value_or(0));
	if (muteUntil > INT32_MAX) {
		muteUntil = INT32_MAX;
	} else if (muteUntil < 0) {
		muteUntil = 0;
	}
	const auto sound = peer->notify().sound();
	const auto soundNone = sound && sound->none;
	auto canonicalIt = _notificationByChat.find(chatId);
	const auto showPreviews = (canonicalIt != _notificationByChat.end())
		? canonicalIt->second.showPreviews
		: true;
	const auto weak = QPointer<NativeBridge>(this);
	client().setNotificationSettings(chatId, muteUntil, showPreviews, soundNone,
		[weak, peerId = peer->id, chatId, muteUntil, showPreviews, soundNone](
				QJsonDocument doc, QString error, int) {
			if (!weak) {
				return;
			}
			if (!error.isEmpty() || !doc.isObject()) {
				const auto snap = weak->_notificationByChat.find(chatId);
				const auto known = (snap != weak->_notificationByChat.end());
				const auto revertMute = known ? snap->second.muteUntil : 0;
				const auto revertPreviews = known ? snap->second.showPreviews : true;
				const auto revertSound = known ? snap->second.soundNone : false;
				if (const auto peer = weak->_session->data().peerLoaded(peerId)) {
					weak->applyPeerNotifySettings(
						peer,
						revertMute,
						revertPreviews,
						revertSound);
					weak->_session->data().sendHistoryChangeNotifications();
				}
				return;
			}
			auto &snapshot = weak->_notificationByChat[chatId];
			snapshot.muteUntil = muteUntil;
			snapshot.showPreviews = showPreviews;
			snapshot.soundNone = soundNone;
			snapshot.revision = qMax(
				snapshot.revision,
				static_cast<qint64>(doc.object().value(
					"settings_revision").toVariant().toLongLong()));
		});
}

void NativeBridge::saveDefaultNotifySettings(Data::DefaultNotify type) {
    const auto &settings = _session->data().notifySettings();
    const auto &value = settings.defaultSettings(type);
    auto muteUntil = static_cast<qint64>(value.muteUntil().value_or(0));
    if (muteUntil > INT32_MAX) {
        muteUntil = INT32_MAX;
    } else if (muteUntil < 0) {
        muteUntil = 0;
    }
    const auto sound = value.sound();
    const auto soundNone = sound && sound->none;
    const auto weak = QPointer<NativeBridge>(this);
    const auto revert = _defaultNotify[size_t(type)];
    client().setDefaultNotificationSettings(
        DefaultNotifyScope(type),
        muteUntil,
        soundNone,
        QString(),
        [weak, type, revert](QJsonDocument doc, QString error, int) {
            if (!weak) {
                return;
            }
            if (!error.isEmpty() || !doc.isObject()) {
                weak->applyDefaultNotifySettings(
                    type,
                    revert.muteUntil,
                    revert.soundNone);
                return;
            }
            weak->applyDefaultNotifySettingsPayload(doc.object());
        });
}

void NativeBridge::setReactions(
        HistoryItem *item,
        const std::vector<DocumentId> &emojiIds) {
    if (!item || item->id.bare <= 0) return;
    if (TopicChannels::IsDiscussionPeer(item->history()->peer)) {
        TopicChannels::SetCommentReactions(this, item, emojiIds);
        return;
    }
    const auto history = item->history().get();
    const auto messageId = item->id.bare;
    auto &state = _reactionReplace[messageId];
    state.desired = emojiIds;
    if (state.inFlight) {
        return;
    }
    dispatchReactionReplace(history, messageId);
}

void NativeBridge::dispatchReactionReplace(History *history, qint64 messageId) {
    const auto it = _reactionReplace.find(messageId);
    if (it == _reactionReplace.end() || it->second.inFlight || !history) {
        return;
    }
    auto &state = it->second;
    if (messageId <= 0 || messageId > INT32_MAX) {
        return;
    }
    state.inFlight = true;
    const auto sent = state.desired;
    const auto weak = QPointer<NativeBridge>(this);
    client().setReactions(messageId, sent, state.revision,
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        [weak, history, messageId, sent](QJsonDocument doc, QString error, int status) {
            if (!weak) return;
            const auto reactionIt = weak->_reactionReplace.find(messageId);
            if (reactionIt == weak->_reactionReplace.end()) return;
            auto &state = reactionIt->second;
            state.inFlight = false;
            const auto applyCanonical = [&](const QJsonObject &data) {
                if (const auto revision = data.value("reaction_revision").toVariant().toLongLong(); revision > state.revision) {
                    state.revision = revision;
                }
                if (const auto existing = weak->_session->data().message(
                        history->peer,
                        MsgId(int32(messageId)))) {
                    weak->applyMessagePayloadState(existing, data);
                }
                weak->_session->data().sendHistoryChangeNotifications();
            };
            if (!error.isEmpty() || !doc.isObject()) {
                if (status == 409 && doc.isObject()) {
                    applyCanonical(doc.object().value("current").toObject());
                } else {
                    LOG(("FoxMes: reaction replace failed for message %1: "
                        "%2 (status %3)"
                        ).arg(messageId).arg(error).arg(status));
                    weak->reloadMessageReactions(history, messageId);
                }
            } else {
                applyCanonical(doc.object());
            }
            if (status == 409 || (error.isEmpty() && doc.isObject())) {
                if (!weak->_reactionReplace.contains(messageId)
                        || weak->_reactionReplace[messageId].desired != sent) {
                    weak->dispatchReactionReplace(history, messageId);
                }
            }
            if (error.isEmpty() && doc.isObject()) {
                weak->scheduleReactionUsageRefresh();
            }
        });
}

void NativeBridge::reloadMessageReactions(History *history, qint64 messageId) {
    if (!history || messageId <= 0 || messageId > INT32_MAX) {
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().messageById(messageId, [weak, history, messageId](
            QJsonDocument doc, QString error, int) {
        if (!weak || !error.isEmpty() || !doc.isObject()) {
            return;
        }
        if (const auto existing = weak->_session->data().message(
                history->peer,
                MsgId(int32(messageId)))) {
            weak->applyMessagePayloadState(existing, doc.object());
            weak->_session->data().sendHistoryChangeNotifications();
        }
    });
}

void NativeBridge::searchMessages(
        History *history,
        const QString &query,
        qint64 before,
        int limit,
        std::function<void(std::vector<int32_t>, bool hasMore, int total)> done) {
    const auto chatId = chatIdFor(history);
    if (!history || !chatId || query.trimmed().isEmpty()) {
        if (done) done({}, false, 0);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().searchMessages(query.trimmed(), chatId, before, limit,
        [weak, history, done = std::move(done)](
                QJsonDocument doc, QString error, int) mutable {
            auto ids = std::vector<int32_t>();
            auto hasMore = false;
            auto total = 0;
            auto items = QJsonArray();
            if (!error.isEmpty()) {
                if (done) done(std::move(ids), false, 0);
                return;
            }
            if (doc.isObject()) {
                const auto object = doc.object();
                items = object.value("items").toArray();
                hasMore = object.value("has_more").toBool();
                total = object.value("total").toInt();
            } else if (doc.isArray()) {
                items = doc.array();
            }
            for (const auto &entry : items) {
                if (!entry.isObject()) continue;
                const auto message = entry.toObject();
                const auto id = message.value("id").toVariant().toLongLong();
                if (id <= 0 || id > INT32_MAX) continue;
                weak->applyMessage(history, message, false);
                ids.push_back(int32_t(id));
            }
            if (!items.isEmpty()) {
                weak->_session->data().sendHistoryChangeNotifications();
            }
            if (total < int(ids.size())) {
                total = int(ids.size());
            }
            if (done) done(std::move(ids), hasMore, total);
        });
}

void NativeBridge::searchAllChats(
        const QString &query,
        History *inHistory,
        qint64 before,
        int limit,
        std::function<void(SearchPage)> done) {
    if (query.trimmed().isEmpty()) {
        if (done) done({});
        return;
    }
    const auto chatId = inHistory ? chatIdFor(inHistory) : qint64(0);
    if (inHistory && !chatId) {
        if (done) done({});
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().searchMessages(query.trimmed(), chatId, before, limit, [
        weak,
        done = std::move(done)
    ](QJsonDocument doc, QString error, int) mutable {
        auto page = SearchPage();
        if (!weak || !error.isEmpty()) {
            if (done) done(std::move(page));
            return;
        }
        auto items = QJsonArray();
        if (doc.isObject()) {
            const auto object = doc.object();
            items = object.value("items").toArray();
            page.hasMore = object.value("has_more").toBool();
            page.total = object.value("total").toInt();
            page.nextBefore = object.value("next_before").toVariant().toLongLong();
        } else if (doc.isArray()) {
            items = doc.array();
        }
        for (const auto &entry : items) {
            if (!entry.isObject()) continue;
            const auto message = entry.toObject();
            const auto chat = message.value("chat_id").toVariant().toLongLong();
            const auto history = weak->historyForChat(chat);
            if (!history) continue;
            auto prepared = weak->prepareMessage(history, message, {}, false);
            if (!prepared) continue;
            page.messages.push_back(std::move(prepared->mtp));
            page.nextBefore = prepared->messageId.bare;
        }
        if (page.total < page.messages.size()) {
            page.total = int(page.messages.size());
        }
        if (done) done(std::move(page));
    });
}

void NativeBridge::requestGlobalMedia(
        const QString &kind,
        const QString &query,
        qint64 before,
        int limit,
        std::function<void(GlobalMediaPage)> done) {
    if (kind.isEmpty()) {
        if (done) done({});
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().globalMedia(kind, query, before, limit, [
        weak,
        done = std::move(done)
    ](QJsonDocument doc, QString error, int) mutable {
        auto page = GlobalMediaPage();
        if (!weak || !error.isEmpty() || !doc.isObject()) {
            if (done) done(std::move(page));
            return;
        }
        const auto object = doc.object();
        const auto items = object.value("items").toArray();
        page.hasMore = object.value("has_more").toBool();
        page.total = object.value("total").toInt();
        for (const auto &entry : items) {
            if (!entry.isObject()) continue;
            const auto message = entry.toObject();
            const auto chat = message.value("chat_id").toVariant().toLongLong();
            const auto history = weak->historyForChat(chat);
            if (!history) continue;
            const auto item = weak->applyMessage(history, message, false);
            if (!item) continue;
            page.ids.push_back(item->fullId());
            page.nextBefore = item->id.bare;
        }
        if (!items.isEmpty()) {
            weak->_session->data().sendHistoryChangeNotifications();
        }
        if (page.total < int(page.ids.size())) {
            page.total = int(page.ids.size());
        }
        if (done) done(std::move(page));
    });
}

void NativeBridge::requestChatMedia(
        History *history,
        const QString &kind,
        const QString &query,
        qint64 before,
        qint64 after,
        qint64 around,
        int limit,
        std::function<void(MediaPage)> done) {
    const auto chatId = chatIdFor(history);
    if (!history || !chatId || kind.isEmpty()) {
        if (done) done({});
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().chatMedia(chatId, kind, query, before, after, around, limit, [
        weak,
        history,
        done = std::move(done)
    ](QJsonDocument doc, QString error, int) mutable {
        auto page = MediaPage();
        if (!weak || !error.isEmpty() || !doc.isObject()) {
            page.failed = true;
            if (done) done(std::move(page));
            return;
        }
        const auto object = doc.object();
        const auto items = object.value("items").toArray();
        page.hasMoreBefore = object.value("has_more_before").toBool();
        page.hasMoreAfter = object.value("has_more_after").toBool();
        page.total = object.value("total").toInt();
        for (const auto &entry : items) {
            if (!entry.isObject()) continue;
            const auto message = entry.toObject();
            const auto id = message.value("id").toVariant().toLongLong();
            if (id <= 0 || id > INT32_MAX) continue;
            weak->applyMessage(history, message, false);
            page.ids.push_back(int32_t(id));
        }
        if (!items.isEmpty()) {
            weak->_session->data().sendHistoryChangeNotifications();
        }
        if (done) done(std::move(page));
    });
}

void NativeBridge::forwardMessages(
        History *source,
        History *target,
        const std::vector<int32_t> &ids,
        MsgId threadRootId,
        bool dropAuthor,
        std::optional<int> videoTimestamp,
        std::function<void(QString error)> done) {
    if (!source || !target || ids.empty()) {
        if (done) done(u"nothing to forward"_q);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    const auto completion = std::make_shared<std::function<void(QString)>>(std::move(done));
    const auto discussionOf = TopicChannels::IsDiscussionPeer(source->peer)
        ? TopicChannels::ChannelOfDiscussion(
            qint64(peerToChannel(source->peer->id).bare))
        : qint64();
    const auto readable = discussionOf
        ? discussionOf
        : historyChatIdFor(source);
    const auto rootId = discussionOf ? qint64(threadRootId.bare) : qint64();
    auto withSource = [weak, source, target, ids, rootId, dropAuthor, videoTimestamp, completion](qint64 sourceChatId) mutable {
        if (!weak || !source || !target) {
            if (*completion) (*completion)(u"forward cancelled"_q);
            return;
        }
        if (sourceChatId <= 0) {
            if (*completion) (*completion)(u"source chat unavailable"_q);
            return;
        }
        weak->ensureChat(target, [weak, target, ids, sourceChatId, rootId, dropAuthor, videoTimestamp, completion](qint64 targetChatId) mutable {
            if (!weak || !target) {
                if (*completion) (*completion)(u"forward cancelled"_q);
                return;
            }
            if (targetChatId <= 0) {
                if (*completion) (*completion)(u"target chat unavailable"_q);
                return;
            }
            auto messageIds = QList<qint64>();
            messageIds.reserve(int(ids.size()));
            for (const auto id : ids) messageIds.push_back(id);
            const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto attempt = std::make_shared<int>(0);
            const auto request = std::make_shared<std::function<void()>>();
            *request = [weak, target, targetChatId, sourceChatId, rootId, messageIds, dropAuthor, videoTimestamp, operationId, completion, attempt, request] {
                if (!weak || !target) {
                    *request = {};
                    if (*completion) (*completion)(u"forward cancelled"_q);
                    return;
                }
                ++*attempt;
                weak->client().forwardMessages(
                    targetChatId,
                    sourceChatId,
                    rootId,
                    messageIds,
                    dropAuthor,
                    videoTimestamp,
                    operationId,
                    [weak, target, completion, attempt, request](
                            QJsonDocument doc,
                            QString error,
                            int) {
                        if (!weak || !target) {
                            *request = {};
                            if (*completion) (*completion)(u"forward cancelled"_q);
                            return;
                        }
                        auto failures = QJsonArray();
                        if (doc.isObject()) {
                            const auto object = doc.object();
                            const auto items = object.value("items").toArray();
                            failures = object.value("failures").toArray();
                            for (const auto &entry : items) {
                                if (entry.isObject()) {
                                    weak->applyMessage(target, entry.toObject(), false);
                                }
                            }
                            weak->_session->data().sendHistoryChangeNotifications();
                            weak->reloadChats();
                        }
                        if (*attempt < 2 && (!error.isEmpty() || !failures.isEmpty())) {
                            (*request)();
                            return;
                        }
                        if (error.isEmpty() && !failures.isEmpty()) {
                            error = u"FORWARD_PARTIAL_FAILED"_q;
                        }
                        *request = {};
                        if (*completion) (*completion)(std::move(error));
                    });
            };
            (*request)();
        });
    };
    if (readable) {
        withSource(readable);
    } else {
        ensureChat(source, std::move(withSource));
    }
}

void NativeBridge::pinMessage(
        History *history,
        MsgId messageId,
        bool forEveryone,
        std::function<void(QString error)> done) {
    if (!history || messageId <= 0) {
        if (done) done(u"bad message"_q);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    ensureChat(history, [weak, history, messageId, forEveryone, done = std::move(done)](qint64 chatId) mutable {
        if (!weak || !history) return;
        if (chatId <= 0) {
            if (done) done(u"chat unavailable"_q);
            return;
        }
        const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        weak->client().pinMessage(chatId, messageId.bare, forEveryone, operationId,
            [weak, history, chatId, done = std::move(done)](QJsonDocument doc, QString error, int) mutable {
                if (!weak) return;
                if (error.isEmpty() && doc.isObject()) {
                    weak->applyPinnedState(history, chatId, doc.object());
                }
                if (done) done(std::move(error));
            });
    });
}

void NativeBridge::unpinMessage(
        History *history,
        MsgId messageId,
        std::function<void(QString error)> done) {
    if (!history || messageId <= 0) {
        if (done) done(u"bad message"_q);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    ensureChat(history, [weak, history, messageId, done = std::move(done)](qint64 chatId) mutable {
        if (!weak || !history) return;
        if (chatId <= 0) {
            if (done) done(u"chat unavailable"_q);
            return;
        }
        const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        weak->client().unpinMessage(chatId, messageId.bare, operationId,
            [weak, history, chatId, done = std::move(done)](QJsonDocument doc, QString error, int) mutable {
                if (!weak) return;
                if (error.isEmpty() && doc.isObject()) {
                    weak->applyPinnedState(history, chatId, doc.object());
                }
                if (done) done(std::move(error));
            });
    });
}

void NativeBridge::unpinAllMessages(
        History *history,
        std::function<void(QString error)> done) {
    if (!history) {
        if (done) done(u"bad chat"_q);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    ensureChat(history, [weak, history, done = std::move(done)](qint64 chatId) mutable {
        if (!weak || !history) return;
        if (chatId <= 0) {
            if (done) done(u"chat unavailable"_q);
            return;
        }
        const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        weak->client().unpinAllMessages(chatId, operationId,
            [weak, history, chatId, done = std::move(done)](QJsonDocument doc, QString error, int) mutable {
                if (!weak) return;
                if (error.isEmpty() && doc.isObject()) {
                    weak->applyPinnedState(history, chatId, doc.object());
                }
                if (done) done(std::move(error));
            });
    });
}

void NativeBridge::applyPinnedState(
        History *history,
        qint64 chatId,
        const QJsonObject &state) {
    if (!history || chatId <= 0) {
        return;
    }
    const auto revision = state.value("pin_revision").toVariant().toLongLong();
    const auto knownRevision = _pinRevisions.find(chatId);
    if (knownRevision != _pinRevisions.end() && revision < knownRevision->second) {
        return;
    }
    _pinRevisions[chatId] = revision;

    auto ids = std::vector<qint64>();
    for (const auto &value : state.value("message_ids").toArray()) {
        const auto id = value.toVariant().toLongLong();
        if (id > 0 && id <= INT32_MAX) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());

    for (const auto &value : state.value("messages").toArray()) {
        const auto object = value.toObject();
        if (!object.isEmpty()) {
            applyMessage(history, object, false);
        }
    }

    const auto peer = history->peer;
    auto &owner = _session->data();
    auto &storage = _session->storage();
    const auto previous = _pinnedIds.find(chatId);
    if (previous != _pinnedIds.end()) {
        for (const auto id : previous->second) {
            if (std::binary_search(ids.begin(), ids.end(), id)) {
                continue;
            }
            const auto messageId = MsgId(int32(id));
            if (const auto item = owner.message(peer, messageId)) {
                item->setIsPinned(false);
            }
            storage.remove(Storage::SharedMediaRemoveOne(
                peer->id,
                MsgId(0),
                PeerId(0),
                Storage::SharedMediaType::Pinned,
                messageId));
        }
    }

    auto slice = std::vector<MsgId>();
    slice.reserve(ids.size());
    for (const auto id : ids) {
        slice.push_back(MsgId(int32(id)));
    }
    storage.add(Storage::SharedMediaAddSlice(
        peer->id,
        MsgId(0),
        PeerId(0),
        Storage::SharedMediaType::Pinned,
        std::move(slice),
        MsgRange{ MsgId(0), ServerMaxMsgId },
        int(ids.size())));
    for (const auto id : ids) {
        if (const auto item = owner.message(peer, MsgId(int32(id)))) {
            item->setIsPinned(true);
        }
    }
    history->setHasPinnedMessages(!ids.empty());
    _pinnedIds[chatId] = std::move(ids);
    owner.sendHistoryChangeNotifications();
}

void NativeBridge::syncPinnedMessages(History *history, qint64 chatId) {
    if (!history || chatId <= 0) return;
    if (!_pinnedSyncedChats.emplace(chatId).second) return;
    const auto weak = QPointer<NativeBridge>(this);
    client().pinnedMessages(chatId, kPinnedMessagesLimit,
        [weak, history, chatId](QJsonDocument doc, QString error, int) {
            if (!weak) return;
            if (!history || !error.isEmpty() || !doc.isObject()) {
                weak->_pinnedSyncedChats.erase(chatId);
                return;
            }
            weak->applyPinnedState(history, chatId, doc.object());
        });
}

void NativeBridge::requestPinnedMessages(History *history) {
    if (!history) return;
    const auto chatId = chatIdFor(history);
    if (chatId <= 0) return;
    syncPinnedMessages(history, chatId);
}

void NativeBridge::updateProfile(
        const QString &displayName,
        std::function<void(QJsonObject, QString)> done) {
    if (CustomBackend::DisableWhile) {
        if (done) done(QJsonObject(), u"PROFILE_EDIT_DISABLED"_q);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    client().updateMe(displayName.trimmed(),
        [weak, done = std::move(done)](QJsonDocument doc, QString error, int) mutable {
            auto user = QJsonObject();
            if (weak && error.isEmpty() && doc.isObject()) {
                user = doc.object();
                RememberUser(weak->_session, user);
                weak->ensureUser(user, false);
            }
            if (done) done(std::move(user), std::move(error));
        });
}

void NativeBridge::handleEvent(const QJsonObject &event) {
    const auto sequence = event.value("seq").toVariant().toLongLong();
    const auto type = event.value("type").toString();
    const auto data = event.value("data").toObject();
	if (sequence > 0 && _eventSeq > 0 && sequence > (_eventSeq + 1)) {
		resyncAfterGap(sequence, sequence);
		return;
	}

    if (Conferences::HandleEvent(_session, type, data)) {
        return;
    }
    if (Calls::HandleEvent(_session, type, data)) {
        return;
    }

    if (type == u"message.created"_q || type == u"message.updated"_q) {
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        if (const auto history = historyForChatId(chatId)) {
            applyMessage(
                history,
                data,
                type == u"message.updated"_q,
                type == u"message.created"_q
                    ? NewMessageType::Unread
                    : NewMessageType::Existing);
            _session->data().sendHistoryChangeNotifications();
        } else {
            deferMessageEvent(type, data);
            reloadChats();
        }
    } else if (type == u"reaction.updated"_q) {
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        const auto messageId = data.value("id").toVariant().toLongLong();
        if (chatId > 0 && messageId > 0 && messageId <= INT32_MAX) {
            if (const auto history = historyForChatId(chatId)) {
                if (const auto item = _session->data().message(
                        history->peer,
                        MsgId(int32(messageId)))) {
                    applyMessagePayloadState(item, data);
                    _session->data().sendHistoryChangeNotifications();
                }
            }
        }
    } else if (type == u"topic_channel.post_updated"_q) {
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        const auto messageId = data.value("id").toVariant().toLongLong();
        const auto history = historyForChatId(chatId);
        if (history && messageId > 0 && messageId <= INT32_MAX) {
            applyMessage(
                history,
                TopicChannels::KeepOwnReactions(
                    _session->data().message(history->peer, MsgId(int32(messageId))),
                    data),
                true,
                NewMessageType::Existing);
            _session->data().sendHistoryChangeNotifications();
        }
    } else if (type == u"topic_channel.comment"_q) {
        TopicChannels::ApplyCommentEvent(this, data);
        _session->data().sendHistoryChangeNotifications();
    } else if (type == u"topic_channel.discussion_read"_q) {
        TopicChannels::ApplyDiscussionRead(this, data);
    } else if (type == u"message.viewed"_q || type == u"message.expired"_q) {
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        const auto messageId = data.value("id").toVariant().toLongLong();
        if (chatId > 0 && messageId > 0 && messageId <= INT32_MAX) {
            if (const auto history = historyForChatId(chatId)) {
                if (const auto item = _session->data().message(
                        history->peer,
                        MsgId(int32(messageId)))) {
                    if (type == u"message.viewed"_q) {
                        applyEphemeralViewed(item, data);
                    } else {
                        item->clearMediaAsExpired();
                    }
                    _session->data().sendHistoryChangeNotifications();
                }
            }
        }
    } else if (type == u"message.deleted"_q) {
        removeMessage(
            data.value("chat_id").toVariant().toLongLong(),
            data.value("id").toVariant().toLongLong());
	} else if (type == u"message.delivered"_q) {
    } else if (type == u"read.updated"_q) {
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        auto readerId = data.value("reader_id").toVariant().toLongLong();
        if (!readerId) readerId = data.value("user_id").toVariant().toLongLong();
        auto readThroughId = data.value("read_through_id").toVariant().toLongLong();
        if (!readThroughId) readThroughId = data.value("message_id").toVariant().toLongLong();
        applyReadState(
            chatId,
            readerId,
            readThroughId,
            unixTime(data.value("read_at").toString()),
            data.value("unread_count").toInt(),
            data.value("state_revision").toVariant().toLongLong());
    } else if (type == u"chat.read"_q) {
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        auto readerId = data.value("reader_id").toVariant().toLongLong();
        if (!readerId) readerId = data.value("user_id").toVariant().toLongLong();
        auto readThroughId = data.value("read_through_id").toVariant().toLongLong();
        if (!readThroughId) readThroughId = data.value("message_id").toVariant().toLongLong();
        applyReadState(
            chatId,
            readerId,
            readThroughId,
            unixTime(data.value("read_at").toString()),
            data.value("unread_count").toInt(),
            data.value("state_revision").toVariant().toLongLong());
    } else if (type == u"chat.typing"_q) {
        const auto userId = data.value("user_id").toVariant().toLongLong();
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        if (userId > 0 && userId != client().meId()) {
            if (const auto history = historyForChatId(chatId)) {
                const auto user = _session->data().user(UserId(userId));
                const auto cancel = (data.value("action").toString()
                    == u"cancel"_q);
                _session->data().sendActionManager().registerFor(
                    history,
                    MsgId(),
                    user,
                    cancel
                        ? MTPsendMessageAction(MTP_sendMessageCancelAction())
                        : MTPsendMessageAction(MTP_sendMessageTypingAction()),
                    base::unixtime::now());
            }
        }
	} else if (type == u"chat.presence"_q) {
		applyPresence(data);
    } else if (type == u"chat.history_cleared"_q) {
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        removeHistoryThrough(
            chatId,
            data.value("through_message_id").toVariant().toLongLong(),
            data.value("skipped_pinned_ids").toArray());
        _bottomLoadedChats.erase(chatId);
        reloadChats();
	} else if (handleCommunityEvent(type, data)) {
	} else if (type == u"chat.deleted"_q) {
		const auto chatId = data.value("chat_id").toVariant().toLongLong();
		removeChat(chatId);
		reloadChats();
	} else if (type == u"user.updated"_q || type == u"user.created"_q) {
		if (data.value("id").toVariant().toLongLong() == client().meId()) {
			RememberUser(_session, data);
		}
		ensureUser(data, true);
		_contactsDone = true;
		finishInitialLoadIfReady();
	} else if (type == u"chat.pinned"_q) {
		const auto chatId = data.value("chat_id").toVariant().toLongLong();
		if (const auto history = historyForChatId(chatId)) {
			applyPinnedState(history, chatId, data);
		}
	} else if (type == u"draft.updated"_q || type == u"draft.deleted"_q) {
		const auto chatId = data.value("chat_id").toVariant().toLongLong();
		if (const auto revision = data.value("draft").toObject().value("revision").toVariant().toLongLong(); revision > 0) {
			_draftRevisionByChat[chatId] = revision;
		} else {
			_draftRevisionByChat[chatId] = 0;
		}
	} else if (Scheduled::ApplyEvent(_session, type, data)) {
	} else if (type == u"gap.detected"_q) {
		auto resumeFrom = data.value("next_seq").toVariant().toLongLong();
		if (resumeFrom <= 0) {
			resumeFrom = data.value("resume_from").toVariant().toLongLong();
		}
		resyncAfterGap(resumeFrom, sequence);
		return;
	} else if (type == u"settings.updated"_q) {
		applyDefaultNotifySettingsPayload(
			data.value("notification_defaults").toObject());
	} else if (type == u"chat.updated"_q) {
		applyChatSettingsPatch(data);
	} else if (type == u"chat.settings.updated"_q) {
		applyChatLookPatch(data);
    } else if (type.startsWith(u"chat."_q)) {
        reloadChats();
    }
	if (sequence > _eventSeq) {
		_eventSeq = sequence;
		client().setEventSequence(_eventSeq);
		RememberEventSequence(_session, _eventSeq);
		_lastGapResumeFrom = 0;
	}
}

void NativeBridge::forceLiveUpdatesRestart() {
	_liveUpdates->restartNow();
}

void NativeBridge::resyncAfterGap(qint64 resumeFrom, qint64 observedSeq) {
	if (resumeFrom <= 0) {
		resumeFrom = observedSeq;
	}
	if (resumeFrom <= 0) {
		reloadChats();
		return;
	}
	if (_lastGapResumeFrom == resumeFrom) {
		LOG(("FoxMes: repeated gap at seq %1, resync skipped"
			).arg(resumeFrom));
		return;
	}
	_lastGapResumeFrom = resumeFrom;

	_messageRevisions.clear();
	_appliedReadStates.clear();
	_pinnedSyncedChats.clear();
	_pinnedIds.clear();
	_pinRevisions.clear();

	_eventSeq = resumeFrom - 1;
	client().setEventSequence(_eventSeq);
	RememberEventSequence(_session, _eventSeq);

	reloadChats();
	forceLiveUpdatesRestart();
}

void NativeBridge::onWebSocketMessage(const QString &message) {
	const auto doc = QJsonDocument::fromJson(message.toUtf8());
	if (!doc.isObject()) {
		return;
	}
	handleEvent(doc.object());
}

LiveUpdatesStatus NativeBridge::liveUpdatesStatus() const {
	return _liveUpdates->status();
}

rpl::producer<LiveUpdatesStatus> NativeBridge::liveUpdatesStatusValue() const {
	return _liveUpdates->statusValue();
}

void NativeBridge::restartLiveUpdates() {
	_liveUpdates->restartNow();
}

void NativeBridge::stopLiveUpdates() {
	_liveUpdates->stop();
}

}
