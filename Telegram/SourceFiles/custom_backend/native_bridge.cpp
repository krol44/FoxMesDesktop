#include "custom_backend/native_bridge.h"

#include "custom_backend/native_chat_themes_adapter.h"
#include "custom_backend/native_gifs_adapter.h"
#include "custom_backend/native_wallpaper_adapter.h"

#include "custom_backend/api_client.h"
#include "custom_backend/native_delete_adapter.h"
#include "custom_backend/native_reactions_adapter.h"
#include "custom_backend/native_runtime.h"
#include "custom_backend/native_scheduled_adapter.h"
#include "custom_backend/native_streaming_loader.h"
#include "data/components/scheduled_messages.h"

#include "api/api_common.h"
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
#include <QUuid>

#include <algorithm>
#include <climits>
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

// The request produced no HTTP answer at all: it never left, or the reply
// never came back.
constexpr auto kSendNoStatus = 0;

// The send itself was accepted and only the reconciliation that follows it
// went wrong, so the message exists on the server already.
constexpr auto kSendServerAccepted = 200;

// How many times a send is replayed automatically before it is left to the
// manual retry. Something that fails this often is failing for a reason a
// reconnect does not fix, and a pending entry lives until it succeeds - so
// without a bound it would churn on every reconnect for the rest of the
// session.
constexpr auto kMaxSendReplays = 3;

// A failed send is replayed on reconnect only when the server never answered,
// or answered that it was temporarily unable to: a 4xx verdict reads exactly
// the same on every further attempt, and the entry would be resent on every
// reconnect for the rest of the session.
[[nodiscard]] bool SendMayBeRetried(int status) {
    return !status || (status == 429) || (status >= 500);
}

// A bare link and a labelled one are the same "link" mark in the document
// schema, so the wire type is text_url for both. Telegram splits them: a bare
// url is messageEntityUrl and opens directly, while messageEntityTextUrl means
// the visible text hides a different target and always goes through the
// "Open this link?" confirmation (see UrlRequiresConfirmation). Emitting
// text_url for every link would therefore put that box on every ordinary link
// in the chat, so the covered text is compared with the target and a match is
// reported as a bare url. Scheme and a trailing slash are ignored because the
// web editor's autolink completes "example.com" into "https://example.com/".
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

// Delivery acknowledgements are coalesced over this window so a
// history page does not produce one request per message.
constexpr auto kDeliveredBatchDelayMs = 400;

// How many pinned bodies a pinned-list fetch asks the server to hydrate. The
// full id set always comes back regardless; this only bounds how much of it
// arrives ready to render, the rest is fetched per id by the pinned bar.
constexpr auto kPinnedMessagesLimit = 30;

QString AttachmentMime(const QJsonObject &attachment) {
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
    // Off the file itself when there is one: the content is no longer read
    // into memory for anything but a photo preview, so bytes.size() would
    // report zero for every document and video.
    const auto size = file.path.isEmpty()
        ? qint64(bytes.size())
        : info.size();
    auto result = QJsonObject{
        {"id", QString::number(stableId)},
        {"name", name},
        {"mime", file.mime.trimmed()},
        {"size", QString::number(size)},
    };
    // The same media metadata the send carries. Without it the optimistic
    // bubble is a plain document until the server answers, so a voice message
    // shows up as a file for the length of a round trip and only then turns
    // into a waveform.
    if (!file.kind.isEmpty()) result.insert("kind", file.kind);
    if (file.durationMs > 0) result.insert("duration_ms", file.durationMs);
    if (!file.waveform.isEmpty()) result.insert("waveform", file.waveform);
    if (!file.performer.isEmpty()) result.insert("performer", file.performer);
    if (!file.title.isEmpty()) result.insert("title", file.title);
    if (file.spoiler) result.insert("spoiler", true);
    return result;
}

// Whether the optimistic item will render this upload inline. That is the only
// case whose content has to be in memory - a photo preview needs pixels, while
// a document and a video are drawn from the file on disk.
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

// The media id the optimistic item's attachment got, mirroring the stable id
// AttachmentObjectFromUpload() writes and the offset MediaFromAttachment()
// adds - upload progress has to reach that very object.
qint64 LocalAttachmentMediaId(qint64 localId) {
    const auto stableId = (localId < 0) ? -localId : (localId > 0 ? localId : 1);
    return kAttachmentMediaIdOffset + stableId;
}

// The peer bar settings upstream fills from messages.getPeerSettings, which
// never runs under the bridge. They start out as Unknown, and
// PeerData::hideLinks() answers true for an unknown bar (data_peer.cpp:
// "return !settings || (*settings & PeerBarSetting::ReportSpam)"), which makes
// HistoryItem::translatedTextWithLocalEntities() strip every url, mention and
// hashtag entity out of any message that is not out(). Saved Messages is
// exactly that case - NewMessageFlags() deliberately leaves Outgoing off for
// the self peer - so links there rendered as plain text no matter what the
// entities said, and so did every incoming message in every chat. FoxMes has
// no report-spam bar, so the settings are simply known and empty.
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
    // A reply made through "Reply in Another Chat" points at a message in a
    // different conversation. Rebuilding the id with this history's peer, as
    // this used to do unconditionally, turned it into a reply to whatever
    // shares that number here - which is nothing, so the bubble drew an empty
    // header and the send lost the link.
    const auto peer = replyTo.peer ? replyTo.peer : history->peer->id;
    return FullReplyTo{
        .messageId = FullMsgId(peer, MsgId(int32(replyTo.messageId))),
    };
}

// Fallback side for legacy attachments the server has no dimensions for:
// everything uploaded before the desktop started using the typed image
// endpoint was stored as a plain file, and a plain file carries no width or
// height. The bubble still needs some geometry to lay out.
constexpr auto kUnknownPhotoSide = 320;

// An attachment is rendered inline when its MIME says image and the sender did
// not pick "send as file".
bool AttachmentIsPhoto(const QJsonObject &attachment, bool forceFile) {
    if (forceFile || attachment.value("as_file").toBool()) {
        return false;
    }
    return AttachmentMime(attachment).startsWith(u"image/"_q);
}

// Voice messages and round videos are the two medias upstream marks as
// unlistened - HistoryItem::isUnreadMedia() ignores the flag on anything else.
bool AttachmentPlaysOnce(const QJsonObject &attachment) {
    const auto kind = attachment.value("kind").toString();
    return (kind == u"voice"_q) || (kind == u"video_note"_q);
}

// Registers the PhotoData behind an image attachment.
//
// Bytes are only at hand for our own optimistic send; everything else is
// pointed at its fxl-cdn URL and downloaded by upstream itself - a plain URL
// is a first class download location (PlainUrlLocation -> webFileLoader, see
// storage/file_download.cpp:520), so progress, cancelling and caching are the
// upstream ones and the bridge has no loader of its own.
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
        session->data().processPhoto(photo, thumbs);
        return photo;
    }
    // No local copy: hand upstream the CDN url as the large image location.
    // It cannot come from the MTPDphoto above - photoApplyFields() only
    // applies anything when the large location is valid (data_session.cpp:3742)
    // and an MTProto location needs a dc_id we do not have - so the size and
    // the url are registered straight on the PhotoData.
    const auto url = attachment.value("url").toString().trimmed();
    const auto data = session->data().photo(mediaId);
    // No bytesCount on purpose. For a url location upstream does the same
    // (image_location_factory.cpp:322) because the declared size need not
    // match what the server actually sends, and webFileLoader learns the real
    // one from the response. Declaring it made LoadCloudFile() ask the loader
    // to grow past its own full size and trip
    // Expects(size <= _fullSize) in FileLoader::increaseLoadSize().
    data->updateImages(
        QByteArray(),
        ImageWithLocation(),
        ImageWithLocation(),
        ImageWithLocation{
            .location = ImageLocation(
                url.isEmpty()
                    ? DownloadLocation()
                    : DownloadLocation{ PlainUrlLocation{ url } },
                width,
                height),
        },
        ImageWithLocation(),
        ImageWithLocation(),
        0);
    return photo;
}

// Points a document at where its bytes actually live: the url fxl-cdn serves it
// from, and the separate file that is its preview - a video thumbnail or the
// cover art of a track.
//
// This has to be callable twice, on two different objects, and that is not a
// quirk of the bridge but of how upstream lands a sent message. The optimistic
// item is not replaced by a new one: HistoryItem::applySentMessage() keeps it
// and Session::documentConvert() renames the LOCAL document to the server id,
// copying only what an MTP document carries. A content url and a poster url are
// not among those fields, and when a document with the server id already exists
// - the one MediaFromAttachment() just prepared - the map keeps that one while
// the bubble goes on using the renamed local object. So the document the user
// sees is not the document that was given the url: without this second call it
// has none at all, its automatic download can only build an MTProto loader that
// never finishes under the bridge, the row stays loading for good, and a
// loading document is handed no click handler. That is why a voice message, a
// round video and a track cover only came alive after the history was reloaded.
void ApplyAttachmentSource(
        not_null<DocumentData*> document,
        const QJsonObject &attachment) {
    const auto url = attachment.value("url").toString().trimmed();
    if (!url.isEmpty()) {
        document->setContentUrl(url);
        // The streaming loader needs the same url, and DocumentData hands out
        // no getter for it.
        Streaming::RememberSource(document, url);
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
        ImageWithLocation{
            .location = ImageLocation(
                DownloadLocation{ PlainUrlLocation{ poster } },
                width,
                height),
        },
        ImageWithLocation(),
        false);
}

MTPMessageMedia MediaFromAttachment(
        not_null<Main::Session*> session,
        const QJsonObject &attachment,
        const LocalAttachment &local) {
    const auto &bytes = local.bytes;
    const auto attachmentId = attachment.value("id").toVariant().toLongLong();
    const auto mediaId = kAttachmentMediaIdOffset + attachmentId;
    const auto name = attachment.value("name").toString().isEmpty()
        ? u"attachment"_q
        : attachment.value("name").toString();
    const auto mime = AttachmentMime(attachment);
    const auto size = attachment.value("size").toVariant().toLongLong() > 0
        ? attachment.value("size").toVariant().toLongLong()
        : bytes.size();
    // An image is a photo even before its content arrives - waiting for the
    // bytes would render it as a file row that never turns back into a photo.
    if (AttachmentIsPhoto(attachment, local.forceFile)) {
        using Flag = MTPDmessageMediaPhoto::Flag;
        auto photoFlags = Flag::f_photo | Flag();
        if (attachment.value("spoiler").toBool()) {
            photoFlags |= Flag::f_spoiler;
        }
        return MTP_messageMediaPhoto(
            MTP_flags(photoFlags),
            AttachmentPhoto(session, attachment, bytes),
            MTPint(),
            MTPDocument());
    }

    // files_real keeps real dimensions for videos and images, and the DTO
    // carries them: a document laid out without them used to claim 1x1.
    auto width = attachment.value("width").toInt();
    auto height = attachment.value("height").toInt();
    if (width <= 0 || height <= 0) {
        width = height = kUnknownPhotoSide;
    }
    // kind is what separates media that share a MIME type: a voice message
    // from a music file, a round video note or a GIF-like animation from a
    // plain video. Only the sender knows it, so it travels with the DTO.
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
        // The track title, falling back to the file name for a node that has
        // none - which is every node written before the contract split them.
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
        // The MIME is only a fallback for a node written before kind existed.
        // It must never outrank an explicit "document": the chat stores an
        // .mkv as a plain file, and claiming video here laid the bubble out as
        // a media box the size of kUnknownPhotoSide - a black square with a
        // spinner - for something that is a file row. Same rule as the server
        // converter (api_fox_mes/converter.go).
        || (kind.isEmpty() && mime.startsWith(u"video/"_q))) {
        using Flag = MTPDdocumentAttributeVideo::Flag;
        auto flags = Flag::f_supports_streaming | Flag();
        if (kind == u"video_note"_q) {
            // Square by definition, and streamed like any other video: the
            // flag is what DocumentData::supportsStreaming() reads, and
            // without it a round message can only be played after a download.
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
            // Upstream renders a document as a looping GIF only when it
            // carries the animated attribute.
            attributes.push_back(MTP_documentAttributeAnimated());
        }
    } else if (mime.startsWith(u"image/"_q)) {
        // Reached only for "send as file": the row still wants the geometry
        // so its preview is not laid out as a square.
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
    // Everything that says where the bytes are has to be on the document
    // BEFORE processDocument() applies the fields, because applying them
    // repaints every item already showing this document, and that repaint is
    // what starts the automatic download and decides whether there is a
    // thumbnail to draw. Setting them afterwards was a race the send always
    // lost: the download started on a document with no url, so the only loader
    // that fitted was an MTProto one - which never finishes under the bridge
    // and leaves the document loading for good, and a loading document is
    // handed no click handler at all. The cover had the same race and simply
    // never appeared. Both looked fixed after a restart, because then the
    // document is built from scratch before anything shows it.
    // Session::document(id) only hands back the object, without notifying
    // anyone, so it is safe to prepare it here.
    const auto data = session->data().document(mediaId);
    // While a file of ours is still uploading there is no url yet, but the
    // content is already on disk - point the document at it, exactly as
    // finishLoad() would. setDataAndCache() cannot stand in for this: it fills
    // the active DocumentMedia, and at this moment the item that owns one does
    // not exist yet, so the row would sit at "0 / size" until a restart.
    if (!local.path.isEmpty() && QFileInfo::exists(local.path)) {
        data->setLocation(Core::FileLocation(local.path));
    }
    // The url is what makes the file work at all. DocumentData::save() builds
    // a webFileLoader for a document that has no access hash but does have a
    // content url (data_document.cpp:1268), and that loader is what drives the
    // progress, the cancel button, "save as" and playback. Without it the row
    // sits at "0 / size" forever, because canBeStreamed() additionally wants
    // hasRemoteLocation(), which an fxl-cdn attachment can never have.
    ApplyAttachmentSource(data, attachment);
    // "Save GIF" runs on this very document and the server names a file by its
    // content address. DocumentData stores neither the sha256 nor a readable
    // url, and this is the only place that ever holds both.
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
    // The media-level markers upstream reads directly when deciding how to
    // lay a bubble out, in addition to the document attributes above.
    if (kind == u"video_note"_q) {
        mediaFlags |= Flag::f_round;
    } else if (kind == u"voice"_q) {
        mediaFlags |= Flag::f_voice;
    } else if (kind == u"video"_q || kind == u"animation"_q) {
        mediaFlags |= Flag::f_video;
    }
    return MTP_messageMediaDocument(
        MTP_flags(mediaFlags),
        document,
        MTPVector<MTPDocument>(),
        MTPPhoto(),
        MTPint(),
        MTPint());
}

// Link preview objects live in their own id range so they can never collide
// with an attachment's media id: both end up in the same Data::Session maps.
constexpr auto kWebPageMediaIdOffset = qint64(2000000000000000LL);
constexpr auto kWebPageMediaIdRange = qint64(1000000000000000LL);

// A stable id for a preview, derived from what it previews. The server has no
// id of its own for a web page, and upstream needs one that survives a reload:
// WebPageData is keyed by it, so a fresh id on every history page would build a
// new object for the same card each time.
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

// Registers the PhotoData behind a preview image. Same shape as
// AttachmentPhoto(): no bytes at hand, so the image is a plain url location
// and upstream downloads it itself.
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
    // Same reason as AttachmentPhoto(): photoApplyFields() only applies a
    // location it considers valid, and an MTProto one needs a dc_id we do not
    // have, so the url is registered straight on the PhotoData.
    const auto data = session->data().photo(mediaId);
    data->updateImages(
        QByteArray(),
        ImageWithLocation(),
        ImageWithLocation(),
        ImageWithLocation{
            .location = ImageLocation(
                DownloadLocation{ PlainUrlLocation{ url } },
                width,
                height),
        },
        ImageWithLocation(),
        ImageWithLocation(),
        0);
    return photo;
}

// Builds the media of a link preview card - the messageMediaWebPage of
// MTProto. A card whose page is still being read comes through as
// webPagePending, exactly like the pending state the Telegram server sends
// before it has finished fetching; the full card then arrives as an ordinary
// message update.
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
        // The server is still reading the page. No media at all rather than a
        // webPagePending: upstream draws a pending card as 0x0 anyway, but
        // registering one makes ApiWrap::requestWebPageDelayed() schedule a
        // messages.getMessages that can never complete under the bridge. The
        // real card arrives as an ordinary message update instead.
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
        // Nothing to draw. An empty card is worse than none: upstream would
        // still lay out the frame around it.
        return std::nullopt;
    }
    // Shrunk picture by default: the page type is passed through as the server
    // read it, so upstream's computeDefaultSmallMedia() decides the size the
    // same way it does for a Telegram card. It shrinks any page carrying a
    // site name, a title and a description - every ordinary article - and
    // leaves a photo or video page large. The default is left to that function
    // rather than forced with force_small_media on the message, because the
    // composer's settings box reads computeDefaultSmallMedia() too: a flag set
    // on the message alone would leave the box offering "Shrink photo" for a
    // card that was already going out small.
    using Flag = MTPDwebPage::Flag;
    auto flags = MTPDwebPage::Flags();
    if (!type.isEmpty()) flags |= Flag::f_type;
    if (!siteName.isEmpty()) flags |= Flag::f_site_name;
    if (!title.isEmpty()) flags |= Flag::f_title;
    if (!description.isEmpty()) flags |= Flag::f_description;
    if (!imageUrl.isEmpty()) {
        flags |= Flag::f_photo;
        // What makes "Shrink photo" / "Enlarge photo" appear in the preview
        // settings at all: the box only offers the choice for a card whose
        // media can be shown large (see setupLinkActions in
        // history_view_draft_options.cpp). Telegram sets it server-side for
        // any card with a picture, so a card with one gets it here too.
        flags |= Flag::f_has_large_media;
    }
    using MediaFlag = MTPDmessageMediaWebPage::Flag;
    auto mediaFlags = MTPDmessageMediaWebPage::Flags();
    const auto large = webPage.value("large").toBool();
    const auto small = webPage.value("small").toBool();
    // The sender touched the preview settings, so the layout is their choice
    // and not a default upstream may recompute.
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

Data::AllowedReactions ParseAllowedReactions(const QJsonObject &chat) {
    Data::AllowedReactions result;
    result.type = Data::AllowedReactionsType::Some;
    // Server contract: catalog /reactions reports {max_selected}. A per-chat
    // override wins if the DTO ever carries one; never hardcode a client
    // limit that may diverge from the server.
    const auto chatMax = chat.value("max_selected").toInt();
    result.maxCount = chatMax > 0
        ? chatMax
        : Reactions::MaxSelectedReactions();
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

// externalPeer is the chat the parent lives in when that is not the chat the
// message itself is in - the server sends the whole reply_to object only then.
// The snapshot it carries is not decoration: the reader of a cross-chat reply
// is usually not a participant of the parent's chat and can never fetch it, so
// the author name and the quoted text are the only material their reply header
// can be drawn from. Upstream renders exactly that from reply_from plus
// quote_text once the resolve attempt has come back empty.
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
        return MTP_messageReplyHeader(
            MTP_flags(Flag::f_reply_to_msg_id),
            MTP_int(int(replyId)),
            MTPPeer(),
            MTPMessageFwdHeader(),
            MTPMessageMedia(),
            MTPint(),
            MTP_string(QString()),
            MTPVector<MTPMessageEntity>(),
            MTPint(),
            MTPint(),
            MTP_bytes(QByteArray()));
    }
    const auto authorId = external.value("author_id").toVariant().toLongLong();
    const auto authorName = external.value("author_name").toString();
    const auto quote = external.value("text").toString();
    auto flags = Flag::f_reply_to_msg_id | Flag::f_reply_to_peer_id | Flag();
    if (!authorName.isEmpty() || authorId > 0) {
        flags |= Flag::f_reply_from;
    }
    if (!quote.isEmpty()) {
        flags |= Flag::f_quote_text;
    }
    using FwdFlag = MTPDmessageFwdHeader::Flag;
    auto fwdFlags = MTPDmessageFwdHeader::Flags();
    if (authorId > 0) {
        fwdFlags |= FwdFlag::f_from_id;
    }
    if (!authorName.isEmpty()) {
        fwdFlags |= FwdFlag::f_from_name;
    }
    return MTP_messageReplyHeader(
        MTP_flags(flags),
        MTP_int(int(replyId)),
        peerToMTP(externalPeer),
        MTP_messageFwdHeader(
            MTP_flags(fwdFlags),
            (authorId > 0
                ? MTP_peerUser(MTP_long(authorId))
                : MTPPeer()),
            MTP_string(authorName),
            MTP_int(0), // date
            MTPint(), // channel_post
            MTP_string(QString()), // post_author
            MTPPeer(), // saved_from_peer
            MTPint(), // saved_from_msg_id
            MTPPeer(), // saved_from_id
            MTP_string(QString()), // saved_from_name
            MTPint(), // saved_date
            MTP_string(QString())), // psa_type
        MTPMessageMedia(),
        MTPint(),
        MTP_string(quote),
        MTPVector<MTPMessageEntity>(),
        MTPint(),
        MTPint(),
        MTP_bytes(QByteArray()));
}

} // namespace

// The peer is kept only when it differs from the chat being written to: that
// is exactly the "Reply in Another Chat" case, and carrying it for an ordinary
// reply would make every local echo look external.
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
    // FoxMes has no subscription tier: every premium-gated feature is simply
    // available. Main::Session::premium() answers true under the bridge for
    // the imperative gates; this flag is the reactive half, because
    // Data::AmPremiumValue() and premiumPossibleValue() watch the self user's
    // Premium flag rather than calling premium(), so without it every rpl
    // driven premium UI would still render locked.
    _session->user()->addFlags(UserDataFlag::Premium);
    const auto me = CurrentUser(_session);
    if (!me.isEmpty()) {
        // Same DTO shape and same code path as any other peer, so self
        // picks up name, loaded status and avatar consistently instead of
        // duplicating the field parsing (and missing the avatar) here.
        ensureUser(me, false);
    }
    // Before loadCachedChats(): defaults never arrive over MTProto under the
    // bridge, and an unknown default makes every unmuted peer read as muted.
    // The cached value is applied synchronously so the setting does not blink
    // between startup and the server response.
    {
        const auto cached = LoadDefaultNotifyCache(_session);
        _defaultNotifyRevision = cached.value(
            "settings_revision").toVariant().toLongLong();
        applyDefaultNotifySettings(
            cached.value("mute_until").toVariant().toLongLong(),
            cached.value("sound_none").toBool());
    }
    loadDefaultNotifySettings();
    // The background every chat without its own picture uses. Local storage
    // already holds whatever was applied last, so this only catches up with a
    // choice made on another device.
    Wallpapers::RequestDefault(_session);
    // The chat theme catalog is compiled in, so building it once at startup
    // costs nothing and means the picker opens filled and a chat that already
    // has a theme resolves it on the first paint instead of after a refresh.
    ChatThemes::Request(_session);
    loadContacts();
    refreshReactionsCatalog();
	loadCachedChats();
	reloadChats();
	_eventSeq = std::max<qint64>(0, client().eventSequence());
	_liveUpdates = std::make_unique<LiveUpdatesConnection>(
		this,
		[=] { return client().websocketRequest(_eventSeq); },
		[=] {
			reloadChats();
			updatePresence();
			// The connection is back: anything still waiting to be sent gets
			// another attempt. MTProto resends queued messages on reconnect
			// and the desktop expects the same, otherwise a message typed
			// while offline sits on the clock forever. The server is
			// idempotent per client_nonce, so a retry either creates the
			// message or returns the one that already exists.
			resendPendingSends();
		},
		[=](QString message) { onWebSocketMessage(message); });
	for (const auto &controller : _session->windows()) {
		trackWindow(controller);
	}
	if (!client().accessToken().isEmpty()) {
		_liveUpdates->start();
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
	// Qt::ISODate handles the Z suffix, numeric offsets and fractional
	// seconds; an unparsable value is 0, never "now" - a missing date must
	// not float a chat to the top of the list.
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
    // Formatting travels as entities with UTF-16 offsets, exactly like
    // MTProto, so it maps onto the native types one to one. Unknown types are
    // skipped rather than guessed: the document may carry web-only formatting
    // that Telegram has no equivalent for, and dropping the entity is always
    // better than shifting the ones after it.
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
            // Not produced by the document schema, but a sender that already
            // knows the link is bare says so directly instead of making the
            // reader rediscover it.
            result.push_back(MTP_messageEntityUrl(from, count));
        } else if (type == u"mention"_q) {
            result.push_back(MTP_messageEntityMention(from, count));
        } else if (type == u"hashtag"_q) {
            result.push_back(MTP_messageEntityHashtag(from, count));
        } else if (type == u"email"_q) {
            result.push_back(MTP_messageEntityEmail(from, count));
        } else if (type == u"custom_emoji"_q) {
            // A sticker of this product: the range is the alt, and the entity
            // is what turns it from that literal text into the animation. The
            // id is minted from the alt, never taken from the payload - the
            // catalog and the history are drawn from the same registry, and a
            // server-side id would disagree with it whenever the catalog
            // refresh had not landed yet.
            const auto alt = ((offset >= 0)
                && (length > 0)
                && (offset + length <= text.size()))
                ? text.mid(offset, length)
                : QString();
            if (alt.isEmpty()) continue;
            result.push_back(MTP_messageEntityCustomEmoji(
                from,
                count,
                MTP_long(Reactions::RegisterDocumentId(alt))));
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
        const Data::WebPageDraft &webPage) {
    // Which link the composer's preview settings belong to. The chosen url is
    // matched first; with none chosen the settings land on the first link,
    // because that is the one the server would have built the card from.
    const auto tuned = webPage.removed
        || webPage.invert
        || webPage.forceLargeMedia
        || webPage.forceSmallMedia;
    auto previewAssigned = false;
    // Mirror of renderMessageEntities for the outgoing direction. Only the
    // types the server stores as marks are sent; anything else would be
    // dropped by the document format anyway.
    auto result = QJsonArray();
    for (const auto &entity : entities) {
        auto type = QString();
        auto data = QString();
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
        case EntityType::CustomEmoji:
            // The alt is already the text under the entity, and the server
            // resolves the asset from it against its own catalog. The address
            // is sent anyway so a client without a catalog has something to
            // render; the server never trusts it.
            type = u"custom_emoji"_q;
            data = Reactions::AssetUrlFor(
                Reactions::EmojiFor(
                    Data::ParseCustomEmojiData(entity.data())));
            break;
        case EntityType::CustomUrl:
            type = u"text_url"_q;
            data = entity.data();
            break;
        case EntityType::Url:
            // A bare url the composer detected. The document schema has one
            // link mark for both kinds, so it travels as text_url pointing at
            // itself; renderMessageEntities recognises that shape and turns it
            // back into a bare url on the way in.
            type = u"text_url"_q;
            // The mark stores a target, and the target of a bare url is the
            // text itself - completed to an absolute one, because a link mark
            // with "example.com" in href is a relative link on the web side.
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
        // At most one link carries the settings: the message holds one media,
        // so it holds one card, and the same url can appear in the text twice.
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

HistoryItem *NativeBridge::createPendingTextMessage(
        History *history,
        const QString &text,
        const EntitiesInText &entities,
        ReplyTarget replyTo,
        MsgId localMessageId) {
    if (!history) {
        return nullptr;
    }
    auto flags = NewMessageFlags(history->peer);
    flags |= MessageFlag::HasFromId;
    if (replyTo) {
        flags |= MessageFlag::HasReplyInfo;
    }
    return history->addNewLocalMessage({
        .id = localMessageId,
        .flags = flags,
        .from = _session->userPeerId(),
        .replyTo = ReplyToFromServerId(history, replyTo),
        .date = base::unixtime::now(),
    }, TextWithEntities{ text, entities }, MTP_messageMediaEmpty()).get();
}

HistoryItem *NativeBridge::createPendingFileMessage(
        History *history,
        const UploadSpec &file,
        const TextWithEntities &caption,
        ReplyTarget replyTo,
        const LocalAttachment &local,
        uint64 groupedId,
        std::shared_ptr<Data::DocumentMedia> &keepMedia) {
    if (!history) {
        return nullptr;
    }
    auto flags = NewMessageFlags(history->peer);
    flags |= MessageFlag::HasFromId;
    if (replyTo) {
        flags |= MessageFlag::HasReplyInfo;
    }
    const auto localId = _session->data().nextLocalMessageId();
    const auto attachment = AttachmentObjectFromUpload(
        localId.bare,
        file,
        local.bytes);
    // Content that exists only in memory - a recorded voice message or round
    // video, a picture pasted from the clipboard - reaches its document through
    // an active media view, and DocumentData::setDataAndCache() below silently
    // drops the bytes when there is none. The view has to exist before the item
    // is added to history, because adding it starts an automatic download: with
    // no bytes, no file and no url the only loader that fits is an MTProto one,
    // which never finishes under the bridge and leaves the document loading for
    // good - and a loading document is handed no click handler at all, so the
    // bubble went silent until the history was reloaded.
    if (!local.bytes.isEmpty()
        && local.path.isEmpty()
        && !AttachmentIsPhoto(attachment, local.forceFile)) {
        keepMedia = _session->data().document(
            LocalAttachmentMediaId(localId.bare))->createMediaView();
    }
    return history->addNewLocalMessage({
        .id = localId,
        .flags = flags,
        .from = _session->userPeerId(),
        .replyTo = ReplyToFromServerId(history, replyTo),
        .date = base::unixtime::now(),
        .groupedId = groupedId,
    }, caption, MediaFromAttachment(_session, attachment, local)).get();
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
    // The request goes out immediately after this, so the send counts as in
    // flight from here until it fails or lands.
    request.inFlight = true;
    _pendingSendNonceToLocalId[request.clientNonce] = item->id.bare;
    _pendingSends[item->id.bare] = std::move(request);
}

void NativeBridge::failPendingSend(qint64 localId, int status) {
    // Local message ids are big negative MsgIds (StartClientMsgId) and do NOT
    // fit int32 - never filter them by the server-side id range here.
    if (!localId) {
        return;
    }
    const auto i = _pendingSends.find(localId);
    if (i == _pendingSends.end()) {
        return;
    }
    if (i->second.cancelledAfterCommit) {
        // The commit the user could no longer stop did not go through after
        // all: nothing was created, so there is nothing to delete server-side
        // and nothing to show - upstream destroyed the local item on cancel.
        // The entry has to go too, or the reconnect would send back exactly
        // what the user cancelled.
        finishPendingDraftSave(localId, base::unixtime::now());
        clearPendingSend(localId);
        return;
    }
    if (!SendMayBeRetried(status)) {
        // A verdict, not an accident: the server refused this send and will
        // refuse it identically next time, so nothing was created there and
        // the manual retry upstream offers on a failed bubble cannot help.
        // Leaving the message would show the user something that does not
        // exist and never will - the toast already carried the reason. The
        // entry goes before the item so it cannot resurrect the bubble.
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
    // The request is over and nothing is in flight any more: cancelling is
    // free again, and resendPendingSends() may replay this send once the
    // connection is back.
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
    // A replay puts the count back right after it re-sends: here the send is
    // simply over, one way or another.
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
    // Upstream treats a peer as usable only once it is marked loaded:
    // peerLoaded() returns null for anything else, and Data::Session refuses
    // contact updates for it ("userIsContactChanged() called for a not loaded
    // user"). MTProto sets this while parsing MTPUser, so the bridge has to do
    // the same after filling the DTO in - otherwise every lookup that goes
    // through peerLoaded() silently fails.
    if (!data->isLoaded()) {
        data->setLoadedStatus(PeerData::LoadedStatus::Normal);
    }
    // Pinning in a private dialog is gated on this flag, which upstream only
    // ever sets from userFull.can_pin_message. Without it
    // PeerData::amRestricted(PinMessages) answers Explicit(), canPinMessages()
    // is false for every FoxMes peer including Saved Messages, and the Pin
    // entry is silently absent from both context menus. FoxMes has no
    // per-peer pin permission: the server accepts a pin from any participant.
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
		data->setUserpic(
			AvatarPhotoId(revision),
			ImageLocation(
				DownloadLocation{ PlainUrlLocation{ avatarUrl } },
				160,
				160),
			false);
	}
	_session->changes().peerUpdated(
		data,
		Data::PeerUpdate::Flag::Photo);
}

void NativeBridge::loadContacts() {
	_contactsDone = true;
	finishInitialLoadIfReady();
}

void NativeBridge::refreshReactionsCatalog() {
    const auto weak = QPointer<NativeBridge>(this);
    client().reactionsCatalog([weak](QJsonDocument doc, QString, int) {
        if (!weak || !doc.isObject()) {
            return;
        }
        auto values = QStringList();
        auto assetUrls = QStringList();
        auto categories = QStringList();
        const auto object = doc.object();
        const auto array = object.value("available_reactions").toArray();
        for (const auto &entry : array) {
            const auto reaction = entry.toObject();
            const auto emoji = reaction.value("emoji").toString();
            if (emoji.isEmpty() || values.contains(emoji)) {
                continue;
            }
            values.push_back(emoji);
            assetUrls.push_back(reaction.value("asset_url").toString());
            categories.push_back(reaction.value("category").toString());
        }
        if (!values.isEmpty()) {
            Reactions::SetAvailableCatalog(values, assetUrls, categories);
            const auto maxSelected = object.value("max_selected").toInt();
            if (maxSelected > 0) {
                Reactions::SetMaxSelectedReactions(maxSelected);
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

PeerData *NativeBridge::peerForChat(const QJsonObject &chat) {
    const auto chatId = chat.value("id").toVariant().toLongLong();
    if (chatId <= 0) return nullptr;

    const auto type = chat.value("type").toString();
    if (type == u"encrypted"_q) {
        return nullptr;
    }
    if (CustomBackend::DisableWhile
        && type != u"saved"_q
        && type != u"direct"_q) {
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
        if (type == u"channel"_q) {
            const auto channel = _session->data().channel(ChannelId(chatId));
            channel->setName(chat.value("title").toString(), QString());
            channel->setMembersCount(members.size());
            channel->date = unixTime(chat.value("created_at").toString());
            peer = channel;
        } else {
            const auto group = _session->data().chat(ChatId(chatId));
            group->setName(chat.value("title").toString());
            group->count = members.size();
            group->date = unixTime(chat.value("created_at").toString());
            peer = group;
        }
    }

    if (peer) {
        applyChatConfig(peer, chat);
        _chatByPeer[peer->id.value] = chatId;
        _peerByChat[chatId] = peer->id.value;
    }
    return peer;
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
    const auto allowed = ParseAllowedReactions(chat);
    applyNotificationSettings(
        peer,
        chatId,
        chat.value("notification_settings").toObject());
    // Absent means the chat follows the per-user default, which is a state of
    // its own and not "no payload": ApplyForPeer clears the chat's own picture
    // for it, so a reset made on another device lands here too.
    Wallpapers::ApplyForPeer(peer, chat.value("wallpaper").toObject());
    // An absent emoticon is "no theme", which is a state of its own: a theme
    // cleared on another device has to clear here too.
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
    } else if (const auto channel = peer->asChannel()) {
        auto flags = ChannelDataFlags(ChannelDataFlag::Megagroup);
        if (chat.value("owner_id").toVariant().toLongLong() == client().meId()) {
            flags |= ChannelDataFlag::Creator;
        }
        channel->setFlags(flags);
        channel->setAdminRights(channel->amCreator()
            ? ChatAdminRight::DeleteMessages
                | ChatAdminRight::EditMessages
                | ChatAdminRight::BanUsers
                | ChatAdminRight::InviteByLinkOrAdd
                | ChatAdminRight::PinMessages
                | ChatAdminRight::PostMessages
            : ChatAdminRights());
        channel->setRestrictions(ChatRestrictionsInfo(
            canSend ? ChatRestrictions() : Data::AllSendRestrictions(),
            0));
        channel->setDefaultRestrictions(ChatRestrictions());
        channel->setAllowedReactions(allowed);
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
    // f_other_sound is always set. PeerNotifySettings::change() rewrites the
    // whole value from this payload, so leaving the sound out cleared the
    // per-peer sound on every round-trip: the user switched a chat to silent,
    // the server echoed the settings back, and the sound was on again.
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

// Canonical parse of a server notification_settings object: clamp, record the
// snapshot the next local save reuses, then push it into the native model.
// Both the chat-list snapshot and the chat.updated patch go through here so
// the clamp and the show_previews baseline cannot drift apart.
void NativeBridge::applyNotificationSettings(
        PeerData *peer,
        qint64 chatId,
        const QJsonObject &notifications) {
    if (!peer || chatId <= 0) {
        return;
    }
    // Revision guard. While this path went through reloadChats() the order of
    // events did not matter - the reload refetched authoritative state either
    // way. Applying the patch directly makes it matter: a replayed or
    // reordered chat.updated would roll the settings back. Zero means the
    // payload carries no revision at all (the chat-list snapshot is one of
    // those), so it still applies but never lowers the watermark; an equal
    // revision is idempotent.
    const auto revision = static_cast<qint64>(
        notifications.value("settings_revision").toVariant().toLongLong());
    const auto known = _notificationByChat.find(chatId);
    const auto knownRevision = (known != _notificationByChat.end())
        ? known->second.revision
        : qint64(0);
    if (revision > 0 && revision < knownRevision) {
        return;
    }
    auto muteUntil = static_cast<qint64>(
        notifications.value("mute_until").toVariant().toLongLong());
    const auto showPreviews = notifications.value("show_previews").toBool(true);
    const auto soundNone = notifications.value("sound_none").toBool(false);
    // Native TimeId is int32: never silently narrow a larger server value.
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

// Publishes the per-user defaults into the native model. The mute_until flag
// is set even for zero on purpose: PeerNotifySettings::change() treats a
// payload with no flags as "known, but empty", muteUntil() stays nullopt, and
// NotifySettings::isMuted() then falls through to its final "return true" -
// every unmuted chat would read as muted and every new message would park in
// the notification manager's _settingWaiters forever.
void NativeBridge::applyDefaultNotifySettings(
        qint64 muteUntil,
        bool soundNone) {
    // Native TimeId is int32: never silently narrow a larger server value.
    if (muteUntil > INT32_MAX) {
        muteUntil = INT32_MAX;
    } else if (muteUntil < 0) {
        muteUntil = 0;
    }
    _defaultNotifyMuteUntil = muteUntil;
    _defaultNotifySoundNone = soundNone;
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
    auto &notify = _session->data().notifySettings();
    // All three types: groups and channels are hidden from the settings UI,
    // but isMuted() resolves their peers through the same default fallback.
    notify.apply(Data::DefaultNotify::User, settings);
    notify.apply(Data::DefaultNotify::Group, settings);
    notify.apply(Data::DefaultNotify::Broadcast, settings);
}

// Adopts a canonical defaults payload (GET response, PUT response or
// settings.updated event). An older revision is a replayed or reordered
// event and is ignored whole, exactly like a stale reaction payload.
void NativeBridge::applyDefaultNotifySettingsPayload(
        const QJsonObject &settings) {
    if (settings.isEmpty()) {
        return;
    }
    const auto revision = settings.value("settings_revision").toVariant().toLongLong();
    if (revision < _defaultNotifyRevision) {
        return;
    }
    _defaultNotifyRevision = revision;
    applyDefaultNotifySettings(
        settings.value("mute_until").toVariant().toLongLong(),
        settings.value("sound_none").toBool());
    SaveDefaultNotifyCache(_session, settings);
}

void NativeBridge::loadDefaultNotifySettings() {
    const auto weak = QPointer<NativeBridge>(this);
    client().defaultNotificationSettings([weak](QJsonDocument doc, QString error, int) {
        if (!weak || !error.isEmpty() || !doc.isObject()) {
            // Keep the cached seed: it is a better guess than "unmuted".
            return;
        }
        weak->applyDefaultNotifySettingsPayload(doc.object());
    });
}

void NativeBridge::reloadChats() {
    const auto weak = QPointer<NativeBridge>(this);
    client().chatsLight([weak](QJsonDocument doc, QString error, int status) {
        if (!weak) return;
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
            return;
        }
        weak->applyChats(doc);
        SaveChatsCache(weak->_session, doc.toJson(QJsonDocument::Compact));
        weak->_session->data().chatsListDone(nullptr);
        weak->_session->data().sendHistoryChangeNotifications();
    });
}

void NativeBridge::applyChats(const QJsonDocument &doc) {
    if (!doc.isArray()) return;
    // The response is the canonical snapshot of per-user chat settings:
    // the pinned-order map is rebuilt from scratch on every application.
    _pinnedRanks.clear();
    for (const auto &entry : doc.array()) {
        if (!entry.isObject()) continue;
        const auto chat = entry.toObject();
        const auto chatId = chat.value("id").toVariant().toLongLong();
        const auto peer = peerForChat(chat);
        if (!peer) continue;
        const auto history = _session->data().history(peer);

        // Settings ride along with the same DTO: restore archive, unread
        // mark and pin so a cold start (cached snapshot) or a fallback
        // reload reproduces the server state.
        //
        // The snapshot is authoritative for the folder, exactly like upstream
        // History::applyDialogFields(). Guarding this on !folderKnown() made
        // the archive a write-once value: loadCachedChats() marks the folder
        // known from the cache before the network answer lands, so a chat
        // unarchived elsewhere - another client, or the server's auto-unarchive
        // on a new message - stayed in the archive for the whole session, and
        // every resyncAfterGap() reload was ignored too. setFolderPointer()
        // early-returns when nothing changes, and the pin it drops on a real
        // move is restored by the batched rebuildPinnedOrder() below.
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

        // Draft snapshot: seed the revision the next save/delete guards with.
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
                // The real message page was never fetched through the bridge,
                // so the bottom edge is not real yet. A fresh History starts
                // with loadedAtBottom()==true, and applying the last_message
                // with NewMessageType::Last would put it into blocks, making
                // HistoryWidget::isReadyFor() report the history as ready -
                // the chat then opens with a single last message and no page
                // request at all. Keep the tail registration-only (chat list
                // preview) so opening the chat loads the actual page.
                history->setNotLoadedAtBottom();
            }
            // The chat list tail must never enter the history blocks. With
            // NewMessageType::Last upstream runs addNewItem() with
            // unread == false, and that marks an already open history as no
            // longer loaded at bottom (history.cpp:835) - the chat then shows
            // the "scroll down" button and every message arriving over the
            // WebSocket stops being appended.
            //
            // Upstream has the right shape for this: a dialogs response only
            // registers its messages and points the dialog at its top one, so
            // do the same.
            const auto lastId = last.value("id").toVariant().toLongLong();
            applyMessage(history, last, false, NewMessageType::Existing);
            if (lastId > 0 && lastId <= INT32_MAX) {
                history->applyDialogTopMessage(MsgId(int32(lastId)));
            }
        }
        history->updateChatListExistence();
    }
    // One batched order restoration after the whole array is parsed.
    rebuildPinnedOrder();
	updatePresence();
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
	) | rpl::on_next([weak](bool) {
		if (weak) {
			weak->updatePresence();
		}
	}, controller->lifetime());
	controller->lifetime().add([weak, controller] {
		if (weak) {
			weak->_trackedWindows.erase(controller);
			weak->updatePresence();
		}
	});
	updatePresence();
}

void NativeBridge::updatePresence() {
	auto chatId = qint64(0);
	for (const auto &controller : _session->windows()) {
		if (!controller->widget()->isActive()) {
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
    // Instant cold-start render from the light snapshot saved by the last
    // successful reloadChats(); the network refresh replaces it right after.
    const auto json = LoadChatsCache(_session);
    if (json.isEmpty()) return;
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return;
    applyChats(doc);
    _session->data().sendHistoryChangeNotifications();
}

void NativeBridge::finishInitialLoadIfReady() {
    // Keep the dialogs-list "Loading..." state until both contacts and the
    // first chats response are done: contactsLoaded()=true with an empty
    // list would render the "no chats" empty state instead of loading.
    if (_contactsDone
        && _chatsDone
        && !_session->data().contactsLoaded().current()) {
        _session->data().contactsLoaded() = true;
    }
}

qint64 NativeBridge::chatIdFor(History *history) const {
    if (!history) return 0;
    const auto i = _chatByPeer.find(history->peer->id.value);
    return (i == _chatByPeer.end()) ? 0 : i->second;
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
    // Look the history up by peer id directly. Going through peerLoaded()
    // made every event depend on the peer's loaded status, so a user the
    // bridge had created but not yet marked loaded made incoming messages,
    // read receipts, typing, presence and deletions all no-op in silence.
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
		// Resolve waiters with an invalid id so pending sends fail visibly
		// instead of hanging in the "sending" state forever.
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
    if (senderId <= 0) return std::nullopt;

    using Flag = MTPDmessage::Flag;
    auto mtpFlags = Flag::f_from_id | Flag();
    if (senderId == client().meId()) {
        mtpFlags |= Flag::f_out;
    }
    const auto reactionsArray = message.value("reactions").toArray();
    if (!reactionsArray.isEmpty()) {
        mtpFlags |= Flag::f_reactions;
    }
    const auto replyId = message.value("reply_to_id").toVariant().toLongLong();
    // A cross-chat reply names the chat its parent lives in; an ordinary one
    // does not, and then the parent is here.
    const auto replyPeerId = peerForChatId(message.value("reply_to")
        .toObject()
        .value("chat_id")
        .toVariant()
        .toLongLong());
    const auto replyHeader = ReplyHeaderFrom(message, replyPeerId);
    if (replyId > 0) {
        mtpFlags |= Flag::f_reply_to;
        const auto replyPeer = replyPeerId
            ? _session->data().peerLoaded(replyPeerId)
            : history->peer.get();
        if (replyId <= INT32_MAX
            && replyPeer
            && !_session->data().message(replyPeer, MsgId(int32(replyId)))) {
            // Fetching the parent is what makes upstream settle the header:
            // found, it draws the real reply; refused - which is what the
            // recipient of a cross-chat reply gets - the resolve comes back
            // empty and the snapshot above is drawn instead.
            requestMessageData(replyPeer, MsgId(int32(replyId)), {});
        }
    }
    const auto editedAt = message.value("edited_at").toString();
    if (!editedAt.isEmpty()) {
        mtpFlags |= Flag::f_edit_date;
    }
    // silent is "sent without sound". It is a flag-only field, so the flag is
    // the whole value: FlagsFromMTP turns it into MessageFlag::Silent, which
    // is what mutes the notification and draws the crossed-out bell on a
    // scheduled item.
    if (message.value("silent").toBool()) {
        mtpFlags |= Flag::f_silent;
    }
    // from_scheduled says the message was composed earlier and delivered by
    // the reminder queue. It is what makes the client tell its own author that
    // a scheduled message went out: HistoryItem::showNotification() returns
    // isFromScheduled() for an outgoing message and for anything in Saved
    // Messages, so without this flag a fired reminder is silent - which in
    // Saved Messages means no notification ever. The same flag also lets
    // History::newItemAdded() count it as unread.
    if (message.value("from_scheduled").toBool()) {
        mtpFlags |= Flag::f_from_scheduled;
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
        if (authorId > 0) {
            fwdFlags |= FwdFlag::f_from_id;
            from = MTP_peerUser(MTP_long(authorId));
            // forwarded_from carries only id/name (see legacyForwardedFrom on
            // the server) - never avatar_url/avatar_id. Routing it through
            // ensureUser() for a peer that is already loaded would read that
            // missing avatar field as "no avatar" and blank out whatever the
            // canonical DTO (self at construction, ensureUser() elsewhere)
            // already set. Only use this stub to give a brand-new peer a
            // name to render before the real DTO arrives.
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
            MTPint(),
            MTPstring(),
            MTPPeer(),
            MTPint(),
            MTPPeer(),
            MTPstring(),
            MTPint(),
            MTPstring());
        mtpFlags |= Flag::f_fwd_from;
    }

    // An album is several messages sharing a grouped_id: upstream builds the
    // grid from it, so without the flag every item would render as its own
    // separate bubble.
    const auto groupedId = message
        .value("grouped_id").toVariant().toLongLong();
    if (groupedId != 0) {
        mtpFlags |= Flag::f_grouped_id;
    }

    const auto attachments = message.value("attachments").toArray();
    if (attachments.size() > 1) {
        // A native message holds exactly one media, so an album is a group of
        // messages sharing a grouped_id. A single message with several
        // attachments can only come from an fxl-web send made before that
        // rule, and all but the first attachment cannot be shown at all.
        LOG(("FoxMes: message %1 in chat %2 has %3 attachments, only the "
            "first one is rendered"
            ).arg(messageId).arg(chatId).arg(attachments.size()));
    }
    const auto attachment = attachments.isEmpty()
        ? QJsonObject()
        : attachments.at(0).toObject();
    // A native message holds exactly one media, so the link preview card is
    // only built when there is no attachment to occupy that slot - which is
    // also how the server decides whether to attach one.
    const auto webPage = message.value("web_page").toObject();
    auto media = attachment.isEmpty()
        ? MediaFromWebPage(_session, webPage)
        : std::optional<MTPMessageMedia>(
            MediaFromAttachment(_session, attachment, local));
    if (media && webPage.value("above").toBool()) {
        // "Move up" in the preview settings. Upstream calls it invert_media
        // and keeps it on the message, not on the media.
        mtpFlags |= Flag::f_invert_media;
    }
    if (media) {
        // media is an optional field of MTPDmessage: without f_media it reads
        // back as absent no matter what was passed in. HistoryItem then never
        // calls setMedia() and the item ends up with no media at all, and
        // applySentMessage() drops the media the optimistic local item had -
        // the sent attachment vanishes from the bubble and a reloaded message
        // renders as the empty-text placeholder.
        mtpFlags |= Flag::f_media;
    }
    // media_unread is what draws the dot next to a voice message that has not
    // been listened to yet, and what fills its whole waveform instead of the
    // played part (HistoryItem::isUnreadMedia, HistoryView::Document::draw).
    // Without it every arriving voice bubble looked like an already played
    // one. fxl-api has no per-message "content read" state - only the
    // chat-wide read boundary the unread counter is drawn from - so that
    // boundary is what the flag follows: anything the recipient has not read
    // past has not been listened to either. Playing the message clears the
    // flag locally (ApiWrap::markContentsRead), and opening the chat moves the
    // boundary, which is what keeps it clear across a history reload.
    if (!(mtpFlags & Flag::f_out)
        && AttachmentPlaysOnce(attachment)
        && (messageId > history->inboxReadTillId().bare)) {
        mtpFlags |= Flag::f_media_unread;
    }
    // entities is optional too, with the same trap as media: without the flag
    // MTPDmessage reads the field back as absent no matter what was passed in
    // (HistoryItem takes it through ventities().value_or_empty()), so every
    // link, bold, code, spoiler and blockquote the server sent is dropped
    // before the item ever sees it.
    auto entities = renderMessageEntities(message);
    if (!entities.v.isEmpty()) {
        mtpFlags |= Flag::f_entities;
    }
    // A reminder is dated by when it will be delivered: that is the date the
    // scheduled list sorts and shows. A when-online one carries no time at
    // all, and upstream already has a marker for exactly that case - the
    // sentinel it checks in HasScheduledDate and draws as "when online".
    const auto createdAt = reminder
        ? Scheduled::DeliveryDate(message)
        : unixTime(message.value("created_at").toString());
    auto mtp = MTP_message(
        MTP_flags(mtpFlags),
        MTP_int(int(messageId)),
        MTP_peerUser(MTP_long(senderId)),
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
        MTPint(),
        MTPint(),
        MTPMessageReplies(),
        MTP_int(editedAt.isEmpty() ? 0 : unixTime(editedAt)),
        MTPstring(),
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
        MTPRichMessage());
    if (!reminder) {
        // Reminder ids live in their own space and would collide here: the
        // queue numbers its rows independently of chat_messages.
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
        // A message the chat list already registered has no view yet, and a
        // known id alone must not stop it from reaching the blocks - otherwise
        // whichever of the two arrives first (the chat list tail or the
        // WebSocket event) silently wins and the bubble never appears.
        // addNewMessage() is built for this: createItem() reuses the existing
        // item and detaches its view (history.cpp:573).
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
            applyMessageReactions(existing, message);
            _seenMessages.emplace(SeenKey(chatId, messageId));
            return existing;
        }
        if (replaceExisting) {
            // An edit must keep the same HistoryItem and view. Destroying it
            // runs the native deletion animation and a concurrent REST/live
            // update can then leave the replacement detached from the block.
            auto prepared = prepareMessage(history, message);
            if (!prepared) {
                return existing;
            }
            existing->applyEdition(HistoryMessageEdition(
                _session,
                prepared->mtp.c_message()));
            applyMessageReactions(existing, message);
            _seenMessages.emplace(SeenKey(chatId, messageId));
            return existing;
        }
    } else if (!replaceExisting && !pendingLocal) {
        if (!_seenMessages.emplace(SeenKey(chatId, messageId)).second) return nullptr;
    }

    // The sent message replaces the local item, and the server DTO has no
    // bytes: rebuilding its media from the DTO alone would drop the preview a
    // just-sent photo already showed. Reuse what the pending send read from
    // disk, including its "send as file" choice.
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
        // The document the item is left holding is the renamed local one, not
        // the one prepared from the DTO - see ApplyAttachmentSource() for why -
        // so it has to be pointed at the file the server now serves.
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
        item = history->addNewMessage(
            id,
            prepared->mtp,
            MessageFlags(),
            type);
    }
    applyMessageReactions(item, message);
    history->setChatListTimeId(unixTime(message.value("created_at").toString()));
    history->updateChatListExistence();
    _seenMessages.emplace(SeenKey(chatId, messageId));
    // The message is attached and visible now, which is exactly what delivery
    // means. Own messages are delivered by definition and are never reported.
    if (item
        && message.value("sender_id").toVariant().toLongLong()
            != client().meId()) {
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
        // Coalesce a burst - a history page or a replay flood - into one
        // request per chat instead of one per message.
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
        // Delivery is a first-touch marker: the server ignores repeats, so a
        // failed batch is simply dropped instead of being retried forever.
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
		applyMessageReactions(existing, message);
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
	applyMessageReactions(item, message);
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

void NativeBridge::applyMessageReactions(
        HistoryItem *item,
        const QJsonObject &message) {
    if (!item) {
        return;
    }
    // Remember the canonical reaction revision so the next guarded replace
    // sends a fresh expected_revision. A payload older than what is already
    // known is dropped whole: a stale revision must not roll the reaction
    // set back, and adopting it would make the next replace conflict.
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
            // A message that was never reacted to needs no entry: the
            // default state already carries revision 0.
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
        // Without a History there is nothing to remove from. This is not
        // normal for a confirmed deletion, so say it out loud instead of
        // leaving the bubble on screen with no trace of why.
        LOG(("FoxMes: cannot remove message %1, chat %2 has no history"
            ).arg(messageId).arg(chatId));
        return;
    }
    // Seen-state must go even when the item is not in memory, otherwise the
    // id stays suppressed and a re-created message would never be shown.
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
        HistoryLoaded done) {
    const auto chatId = chatIdFor(history);
    if (!chatId) {
        if (history) {
            const auto user = history->peer->asUser();
            if (user) {
                const auto weak = QPointer<NativeBridge>(this);
				ensureChat(history, [weak, history, aroundId, direction, done = std::move(done)](qint64 chatId) {
					if (!weak || !history) return;
					if (chatId <= 0) {
						// Chat resolution failed - finish instead of retry-looping.
						if (done) done();
						return;
					}
					weak->loadHistory(history, aroundId, direction, std::move(done));
                });
                return;
            }
        }
        reloadChats();
        if (done) done();
        return;
    }
    if (!history->folderKnown()) history->clearFolder();
	// Any first entry into a chat syncs the pinned list, not only the one
	// that lands on the tail page: opening by jump-to-message used to leave
	// the pinned bar empty until the chat was reopened at the bottom.
	syncPinnedMessages(history, chatId);
	loadHistoryPage(history, aroundId, direction, std::move(done));
}

void NativeBridge::loadHistoryPage(
		History *history,
		qint64 aroundId,
		Data::LoadDirection direction,
		HistoryLoaded done) {
	const auto chatId = chatIdFor(history);
	if (!chatId) {
		if (done) done();
		return;
	}
	auto requestDirection = direction;
	auto requestAroundId = aroundId;
	// "Show the end" is not a real anchor. Only an Around request can mean the
	// fresh tail; for Before/After the same value only means "the edge I have
	// is unknown", and those must fall back to the pagination cursor.
	const auto wantsTail = (aroundId == ShowAtTheEndMsgId.bare)
		|| (aroundId >= (ServerMaxMsgId.bare - 1));
	const auto openAtEnd = wantsTail
		&& (requestDirection == Data::LoadDirection::Around);
	if (wantsTail) {
		requestAroundId = 0;
	}
	if (openAtEnd) {
		// The freshest page is requested with no anchor at all. It must never
		// take the "load older" cursor: doing so turned every jump-to-bottom
		// into one more page of older messages, so the newest messages were
		// never fetched, the bottom edge was never reached, and live messages
		// could only bump the unread badge. With the top already loaded it was
		// worse still - the request returned without asking the server at all,
		// which is why the scroll-down button did nothing.
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
		// An identical page request is already in flight and will apply
		// the slice; queue this completion so every waiter is reported
		// exactly once, like upstream pending-request slots do.
		it->second.push_back(std::move(done));
		return;
	}
	_loadingChats.emplace(loadingKey, std::vector<HistoryLoaded>{});

	const auto weak = QPointer<NativeBridge>(this);
	client().messages(chatId, requestAroundId, 80, requestDirection, [=, done = std::move(done)](QJsonDocument doc, QString error, int) mutable {
		// Single completion point for every outcome - success, HTTP error,
		// destroyed bridge: coalesced waiters and our own done each run
		// exactly once.
		auto completed = false;
		auto completeAll = [&]() mutable {
			if (std::exchange(completed, true)) return;
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

		// Convert entries to MTP messages first. Entries matched against a
		// pending optimistic send go through applyMessage(): their local
		// item already has a block view and just needs a real id.
		auto older = QVector<MTPMessage>();
		auto newer = QVector<MTPMessage>();
		auto applied = base::flat_map<MsgId, QJsonObject>();
		for (const auto &entry : response.value("items").toArray()) {
			if (!entry.isObject()) continue;
			const auto message = entry.toObject();
			const auto messageId = message.value("id").toVariant().toLongLong();
			if (messageId <= 0) continue;
			if (messageId > INT32_MAX) {
				// MsgId is 32-bit upstream, so such a message cannot be shown
				// at all. Say so instead of dropping it silently: a whole page
				// going missing is otherwise indistinguishable from an empty
				// history.
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
			// The server returns ascending ids, while addOlderSlice /
			// addNewerSlice expect descending order (like MTProto does).
			// A fresh tail page (openAtEnd) carries no anchor: all of its
			// items belong to the older slice relative to the bottom edge.
			const auto belongsToOlder = openAtEnd
				|| (id.bare <= MsgId(requestAroundId));
			(belongsToOlder ? older : newer).push_back(
				std::move(prepared->mtp));
			applied.emplace(id, message);
			weak->_seenMessages.emplace(SeenKey(chatId, messageId));
		}
		// An empty addOlderSlice()/addNewerSlice() marks the corresponding
		// edge as loaded, so empty slices are applied only when that direction
		// is really exhausted. Marking an edge from a mid-history page kills
		// pagination in that direction and, for the bottom edge, turns live
		// WS appends into badge-only updates.
		//
		// The server always reports each direction separately. A response
		// without the directional fields is a broken server, not an older
		// one: guessing an edge from a single flag is what used to mark the
		// wrong side as loaded and kill pagination.
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
			// Mark the bottom edge before inserting the items: like upstream
			// getReadyFor(ShowAtTheEnd) before the first load, this makes
			// addCreatedOlderSlice treat the page as the visible tail.
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

		// Reactions and the sparse list (used by RPL viewers) need the
		// items that actually made it into the history blocks.
		std::vector<MsgId> messageIds;
		messageIds.reserve(applied.size());
		MsgId firstId = 0;
		MsgId lastId = 0;
		for (const auto &[id, message] : applied) {
			const auto item = weak->_session->data().message(history->peer, id);
			if (!item) continue;
			weak->applyMessageReactions(item, message);
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
					// The newest page arrived with nothing after it:
					// the history bottom is reached.
					range.till = ServerMaxMsgId;
				}
				break;
			case Data::LoadDirection::Around:
				if (requestAroundId > 0) {
					range.from = qMin(range.from, MsgId(requestAroundId));
					range.till = qMax(range.till, MsgId(requestAroundId));
				}
				if (!hasMoreAfter) {
					// The around page proved nothing newer exists: the bottom
					// edge is reached.
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
			// An empty chat or an empty final page: mark both edges as
			// loaded so the view does not keep requesting the same page.
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
    auto result = SendOptions{ .silent = options.silent };
    if (options.scheduled == Api::kScheduledUntilOnlineTimestamp) {
        // Upstream has no separate flag for "send when online": it marks the
        // case with a sentinel timestamp, and the server takes a flag. The
        // sentinel must never travel as a date - it is year 2038, not a time
        // anybody picked.
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
    // Tags are how the composer stores formatting; entities are what both the
    // server and every other client speak. PrepareForSending then adds the
    // ones nobody typed - bare urls, mentions, hashtags - exactly as upstream
    // does before handing a message to MTProto: nothing downstream ever looks
    // for a link in plain text, so a url without an entity here stays plain
    // text on every client forever.
    auto prepared = TextWithEntities{
        message.textWithTags.text,
        TextUtilities::ConvertTextTagsToEntities(message.textWithTags.tags),
    };
    TextUtilities::PrepareForSending(
        prepared,
        Ui::ItemTextOptions(message.action.history, _session->user()).flags);
    const auto options = SendOptionsFrom(message.action.options);
    if (options.scheduled()) {
        // A scheduled send goes to the reminder queue, not to the chat: it is
        // the same message with a delivery time, and the chat learns about it
        // only when it is actually sent.
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
                    // A newer save is in charge; a stale callback must never
                    // issue another mutation (the old second setDraft(empty)
                    // is gone).
                    return;
                }
                if (error.isEmpty() && doc.isObject()) {
                    const auto revision = doc.object().value("draft").toObject().value("revision").toVariant().toLongLong();
                    if (revision > 0) {
                        weak->_draftRevisionByChat[chatId] = revision;
                    }
                    history->draftSavedToCloud(topicRootId, monoforumPeerId);
                } else if (status == 409 && doc.isObject()) {
                    // Lost race: adopt the authoritative server draft per the
                    // agreed conflict policy and learn its revision.
                    const auto draft = doc.object().value("current").toObject();
                    const auto revision = draft.value("revision").toVariant().toLongLong();
                    if (revision > 0) {
                        weak->_draftRevisionByChat[chatId] = revision;
                    }
                    const auto draftText = draft.value("text").toString();
                    const auto replyTo = MsgId(draft.value("reply_to_id").toVariant().toLongLong());
                    // The parent of a draft made through "Reply in Another
                    // Chat" lives elsewhere, and the server names that chat.
                    // Forcing this history's peer here turned the adopted
                    // draft into a reply to a message id that does not exist
                    // in this conversation.
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
    // Trimming through TextWithEntities, not QString::trimmed(): dropping
    // leading whitespace shifts every offset after it, so trimming the text
    // alone would slide the formatting off the words it belongs to.
    auto prepared = TextWithEntities{ text, entities };
    TextUtilities::Trim(prepared);
    const auto trimmed = prepared.text;
    const auto trimmedEntities = prepared.entities;
    if (!history || trimmed.isEmpty()) return;
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
        // Conditional cleanup target: the draft version observed while
        // composing. A retried nonce must not delete a newer draft.
        const auto clearDraftRevision = weak->_draftRevisionByChat[chatId];
        // A text send has no upload phase: it is committing from the moment
        // the request leaves, so cancelling can only be handled server-side.
        if (const auto i = weak->_pendingSends.find(localId)
            ; i != weak->_pendingSends.end()) {
            i->second.committing = true;
        }
        const auto outgoing = entitiesToJson(trimmedEntities, trimmed, webPage);
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
                // The server removed exactly this draft version inside its
               // own transaction; no extra clearing mutation is sent.
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
    // Trimmed through TextWithEntities so the formatting keeps pointing at the
    // same words after leading whitespace is dropped.
    auto prepared = TextWithEntities{ text, entities };
    TextUtilities::Trim(prepared);
    if (!history || prepared.text.isEmpty()) {
        return;
    }
    if (clearDraft) {
        // The composer emptied itself, so the saved draft has to go with it.
        // The reminder endpoint has no draft cleanup of its own - the draft
        // belongs to the chat, and nothing was sent to that chat yet.
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

// Applies the answer of a reminder creation. A when-online reminder whose
// recipient is already in the chat is delivered on the spot, and the server
// answers with the message itself - the same shape a live send returns, so it
// takes the same path here.
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
    // An answer only adds: it says nothing about the rest of the queue, and
    // treating it as a page would drop every reminder it does not mention.
    Scheduled::Apply(&_session->scheduledMessages(), history, list, false);
}

// Clears the server-side draft of a chat outright. The conditional cleanup a
// send carries does not apply here: nothing was sent to the chat, so there is
// no message whose transaction could take the draft down with it.
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

// Removes the optimistic bubbles of a send that is not going into the chat
// after all. Their pending records are already settled by the caller; what is
// left is the local items, and leaving them behind would show a message in the
// conversation that nobody sent there yet.
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
    // Trimmed through TextWithEntities so the caption's formatting keeps
    // pointing at the same words after leading whitespace is dropped.
    auto trimmedCaption = caption;
    TextUtilities::Trim(trimmedCaption);
    // One attachment is one message, so a multi-file send is an album: a group
    // of messages sharing a grouped_id. The group has to exist before the
    // first byte goes out, because a HistoryItem can only be put into a group
    // while it is created - the sent message coming back cannot move it there
    // later. The value is purely local, exactly like the album id upstream
    // makes for its own sends; the server assigns its own grouped_id and the
    // optimistic elements keep theirs.
    const auto groupedId = (files.size() > 1)
        ? base::RandomValue<uint64>()
        : uint64(0);
    auto localIds = std::vector<qint64>();
    auto nonces = std::vector<QString>();
    localIds.reserve(files.size());
    nonces.reserve(files.size());
    for (auto index = 0; index != int(files.size()); ++index) {
        const auto &file = files[index];
        // A photo preview needs the content, and then it is read once and
        // kept: the same bytes render the local item now and the sent message
        // when it comes back, so the preview never blinks out. A file on disk
        // is never read into memory at all - otherwise a multi-gigabyte send
        // would cost its own size in RAM before a single byte reached the
        // network. Content that is already in memory and nowhere else - a
        // recorded voice message or round video, a picture pasted from the
        // clipboard - is kept for free, and it is the only copy the optimistic
        // bubble can be built from.
        auto local = LocalAttachment{
            .bytes = (UploadIsPhoto(file) || !file.content.isEmpty())
                ? LoadUploadBytes(file)
                : QByteArray(),
            .path = file.path,
            .forceFile = file.forceFile,
        };
        // The caption of an album rides on its first message, as in upstream.
        auto keepMedia = std::shared_ptr<Data::DocumentMedia>();
        const auto item = createPendingFileMessage(
            history,
            file,
            index ? TextWithEntities() : trimmedCaption,
            replyTo,
            local,
            groupedId,
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
        // Kept so a replay after reconnect can put the album back together
        // instead of sending each attachment as an album of its own.
        _pendingSends[item->id.bare].groupedId = groupedId;
        // Nothing else holds the view that carries those bytes, and letting it
        // die would put the document back where it started.
        _pendingSends[item->id.bare].localMedia = std::move(keepMedia);
        localIds.push_back(item->id.bare);
        nonces.push_back(std::move(nonce));
    }
    _session->data().sendHistoryChangeNotifications();
    const auto weak = QPointer<NativeBridge>(this);
    const auto forceFile = files.front().forceFile;
    ensureChat(history, [weak, history, files = std::move(files), trimmedCaption, replyTo, localIds, nonces, forceFile, options](qint64 chatId) mutable {
        if (!weak || !history) return;
        // A send that will never succeed has to say why. Upstream only marks
        // the bubble failed, which is right for a dropped connection but tells
        // the user nothing when the server refused the file outright - and the
        // reason (a size limit, an unsupported container) is exactly what
        // decides whether sending it differently would help.
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
        if (chatId <= 0) {
            LOG(("NativeBridge: file send has no chat id"));
            failAll(kSendNoStatus);
            return;
        }
        auto index = std::make_shared<size_t>(0);
        auto ids = std::make_shared<QList<qint64>>();
        // A video upload answers with the poster fxl-api generated for it. The
        // link exists nowhere else, so it has to travel with the send.
        auto posters = std::make_shared<QMap<qint64, QString>>();
        // Media metadata is known before the upload and keyed by the id the
        // upload hands back, so it is collected as each file completes.
        auto meta = std::make_shared<QMap<qint64, AttachmentMeta>>();
        auto next = std::make_shared<std::function<void()>>();
        *next = [weak, history, chatId, files = std::move(files), trimmedCaption, replyTo, localIds, nonces, forceFile, options, failAll, index, ids, posters, meta, next]() mutable {
            if (!weak || !history) return;
            if (*index >= files.size()) {
                // Every attachment is uploaded: from here on the send is
                // committing and can no longer be cancelled for free. The
                // whole album is one POST, so that is true for all of its
                // elements at once.
                for (const auto localId : localIds) {
                    if (const auto i = weak->_pendingSends.find(localId)
                        ; i != weak->_pendingSends.end()) {
                        i->second.committing = true;
                        i->second.cancelUpload = nullptr;
                    }
                }
                // One attachment is one message: a native message holds
                // exactly one media, so several files become an album - a
                // group of messages sharing a grouped_id - instead of one
                // message whose extra attachments nobody could render.
                auto items = QList<ApiClient::AlbumItem>();
                items.reserve(ids->size());
                for (auto i = 0; i != ids->size(); ++i) {
                    const auto attachmentId = (*ids)[i];
                    items.push_back(ApiClient::AlbumItem{
                        // Every element keeps the nonce of its own optimistic
                        // element, so the response replaces the right bubble
                        // and a retry stays idempotent per element.
                        .clientNonce = nonces[i],
                        .attachmentId = attachmentId,
                        .meta = meta->value(attachmentId),
                    });
                }
                const auto outgoing = entitiesToJson(
                    trimmedCaption.entities,
                    trimmedCaption.text);
                if (options.scheduled()) {
                    // The attachments are uploaded and the queue takes over
                    // from here. The optimistic bubbles were only ever the
                    // progress display of that upload - the message is not
                    // going into the chat yet, so they go away and the
                    // reminders take their place in the scheduled list, which
                    // is where the composer already navigated.
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
                            // The optimistic element is found by nonce. When
                            // there is none - the user cancelled it, or a
                            // retry produced more messages than bubbles - the
                            // message has to enter the blocks as a new one:
                            // NewMessageType::Existing only registers an item
                            // in Data::Session and never attaches it.
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
                        // Anything the response never mentioned never reached
                        // the server: leave those bubbles in the failed state
                        // instead of pretending they were sent.
                        failAll(kSendServerAccepted);
                        weak->_session->data().sendHistoryChangeNotifications();
                    });
                return;
            }
            const auto fileIndex = (*index)++;
            const auto file = files[fileIndex];
            // Progress belongs to the bubble of this very attachment.
            const auto localId = localIds[fileIndex];
            const auto isPhoto = UploadIsPhoto(file);
            const auto uploaded = [weak, next, ids, posters, meta, file, chatId, failAll](QJsonDocument doc, QString error, int status) {
                if (!weak) return;
                if (!error.isEmpty() || !doc.isObject()) {
                    LOG(("NativeBridge: attachment upload failed (%1, %2)"
                        ).arg(status).arg(error));
                    failAll(status, error);
                    return;
                }
                // Canonical fxl-api response: {ok, data:{id,url,sha256,...}}.
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
                // Cover art is a file of its own: the server has no way to pull
                // it out of the tag, and the attachment can only point at it by
                // url. It rides the same per-attachment poster map a video
                // preview uses, so nothing downstream needs a second concept.
                // A failure here is not fatal - the track plays without a
                // cover, and losing the whole send over artwork would be worse.
                weak->client().uploadData(
                    u"cover.jpg"_q,
                    file.cover,
                    u"image/jpeg"_q,
                    chatId,
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
            // Upstream draws "21.5 / 66.7 MB" off UploadState; without it the
            // bubble only has an indeterminate spinner to show.
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
            // Per file, not per album: a mixed group can hold a picture the
            // user chose to send as a file next to a track that must keep its
            // audio type, and the album-level flag only describes the send.
            if (!file.path.isEmpty()) {
                cancel = weak->client().uploadFile(
                    file.path,
                    file.mime,
                    chatId,
                    file.forceFile,
                    uploaded,
                    onProgress,
                    file.kind);
            } else if (!file.content.isEmpty()) {
                cancel = weak->client().uploadData(
                    file.displayName.isEmpty() ? u"upload.bin"_q : file.displayName,
                    file.content,
                    file.mime,
                    chatId,
                    file.forceFile,
                    uploaded,
                    onProgress,
                    file.kind);
            } else {
                LOG(("NativeBridge: attachment has neither path nor content"));
                failAll(kSendNoStatus);
                return;
            }
            // Cancelling any element of an album stops the one transfer that
            // is in flight - there is a single sequential upload chain and a
            // single commit behind the whole group.
            for (const auto pendingId : localIds) {
                if (const auto i = weak->_pendingSends.find(pendingId)
                    ; i != weak->_pendingSends.end()) {
                    i->second.cancelUpload = cancel;
                }
            }
        };
        (*next)();
    });
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
        // The final POST is already in flight, so the server will create the
        // message whatever happens here. Upstream is destroying the local item
        // right now, so the pending entry has to survive to reconcile the
        // response - and, because the user asked to cancel, the created
        // message is deleted server-side as soon as its id is known.
        i->second.cancelledAfterCommit = true;
        i->second.cancelUpload = nullptr;
        return;
    }
    // Aborting makes the reply finish with an error, and that path already
    // knows how to tear the pending send down - but the entry goes first so it
    // cannot resurrect the bubble upstream is about to destroy.
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
    // Cancel means the message must not exist anywhere, so undo the commit the
    // user could no longer stop.
    deleteMessages(history, { int32_t(messageId) }, true);
    return true;
}

bool NativeBridge::pendingSendReplayable(
        const PendingSendRequest &request) const {
    // Only a send that is over is replayed. inFlight covers the whole request
    // chain, the commit included, so an upload still running through a
    // reconnect is left alone instead of being started a second time next to
    // itself. A send the user cancelled after its commit is never replayed
    // either: failPendingSend() drops such an entry, and if one survives some
    // other way it must not come back as a message nobody expects.
    if (request.inFlight
        || request.cancelledAfterCommit
        || !request.history) {
        return false;
    }
    const auto replays = _sendReplays.find(request.clientNonce);
    if (replays != _sendReplays.end() && replays->second >= kMaxSendReplays) {
        return false;
    }
    // Attachments are uploaded again from scratch, so a file whose bytes are
    // gone - a temporary the composer already cleaned up, a removed original -
    // cannot be replayed. That send keeps its failed bubble and its manual
    // retry instead of silently sending an empty attachment.
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
    // Local ids grow with every created item, so ascending order is the order
    // the attachments were picked in: it puts an album back together with its
    // caption on the first element, exactly as the first attempt had it.
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
        // One album is one send. Anything else - a text, a single file - is a
        // group of one.
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
        // Still on the clock or already marked failed - anything else is a
        // bubble the user is no longer waiting for.
        if (!item || (!item->isSending() && !item->hasFailed())) {
            return;
        }
        items.push_back(item);
        nonces.push_back(i->second.clientNonce);
        for (const auto &file : i->second.files) {
            files.push_back(file);
        }
    }
    // clearPendingSend() drops the counter along with the entry, so the spent
    // attempts are carried over by hand onto the entries the resend creates.
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
    // The same nonces as the first attempt: the server is idempotent per
    // (user, chat, nonce), so an attempt that did land after all comes back as
    // the stored message instead of being sent twice.
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
        // Manual retry keeps the original nonce as well: the server returns
        // the stored message if the first attempt did land after all.
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
        // The same nonce as the first attempt, exactly like a text retry: the
        // upload produces new attachment ids, so only the nonce can stop a
        // send whose reply was lost from landing twice.
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
    // Same trimming rule as a send: through TextWithEntities, so the offsets
    // stay attached to their words.
    auto edited = text;
    TextUtilities::Trim(edited);
    if (!item || edited.text.isEmpty()) {
        if (done) done(u"MESSAGE_EMPTY"_q);
        return;
    }
    const auto history = item->history().get();
    const auto messageId = item->id.bare;
    if (!_pendingEdits.emplace(messageId).second) {
        if (done) done(u"MESSAGE_EDIT_PENDING"_q);
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
    // revoke mirrors the upstream UI flag and is accepted for hook
    // compatibility only: FoxMes deletion is always global.
    Q_UNUSED(revoke);
    auto finishOnce = std::make_shared<bool>(false);
    const auto finish = [done = std::move(done), finishOnce] {
        // Completion must run exactly once on any outcome: Histories waits
        // for it to close the RequestType::Delete request slot.
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
    // Upstream destroys the items before the request goes out, and the desktop
    // is built around that: deletion looks instant. Do the same instead of
    // waiting for the round trip, then reconcile - the response tells which
    // ids the server refused (pinned or rejected), and those are brought back
    // by reloading the page they belong to.
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
            // The items are already gone locally; the response only decides
            // what has to come back.
            for (const auto messageId : readIds("deleted_ids")) {
                weak->_pendingDeletes.erase(messageId);
                acknowledged = true;
            }
            const auto skippedPinned = readIds("skipped_pinned_ids");
            const auto refused = skippedPinned + readIds("rejected_ids");
            if (!skippedPinned.isEmpty() && history) {
                // The bubble is about to come back on screen. Without a word
                // the delete looks like it silently failed, so say what the
                // server actually refused and what to do about it.
                ShowSettingsToast(
                    weak->_session,
                    history->peer,
                    u"Unpin the message before deleting it"_q);
            }
            for (const auto messageId : refused) {
                weak->_pendingDeletes.erase(messageId);
                // The server kept it, so the optimistic removal was wrong.
                // Drop the seen-state and pull the page again: the message
                // comes back exactly as the server still has it.
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
            // Nothing was confirmed deleted, so the bubbles stay on screen.
            // Without this the UI silently ignores the request and looks
            // broken while the server may well have deleted the messages.
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
            // Ambiguous timeout or transient failure: ask the journal for the
            // stored result instead of guessing.
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
            weak->_session->data().deleteConversationLocally(history->peer);
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
        // A state that arrives without read_at keeps the timestamp already
        // known for this reader: the watermark moved, the read moment did not
        // become unknown.
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
            // inboxRead also clears native notifications through the applied
            // watermark, keeping notification state aligned with the server.
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

    // Saved Messages has no peer reader, but its own outgoing messages are
    // terminally read and need the native outbox watermark as well.
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
    // SeenKey() reuses MessageKey for read states, so messageId holds the
    // reader id here, not a message id.
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
    // The upstream state machine is released after the next HTTP outcome;
    // durable retries continue independently from this completion.
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
    // Upstream ApiWrap::toggleHistoryArchived samples the pin state before
    // the request: moving a history between folders makes
    // History::setFolderPointer() drop the pin silently, so by the time the
    // response lands isPinnedDialog() no longer tells whether the pinned
    // order was disturbed.
    const auto wasPinned = history->isPinnedDialog(FilterId());
    const auto weak = QPointer<NativeBridge>(this);
    client().setChatArchived(chatId, archived, [weak, history, archived, wasPinned, done = std::move(done)](QJsonDocument, QString error, int) mutable {
        if (!weak || !history || !error.isEmpty()) {
            if (weak && history && !error.isEmpty()) {
                // Failure: the UI queue is still released via done() below,
                // but local state stays untouched - the canonical reload
                // reconciles the stale optimistic archive state.
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
            // Same tail as upstream: the folder move already unpinned the
            // history mid-mutation, so subscribers need one more signal once
            // the entry has settled in its new chat list.
            weak->_session->data().notifyPinnedDialogsOrderUpdated();
        }
    });
}

// Toggle chat-list pin through fxl-api. The optimistic local mutation was
// already made by the upstream caller before this hook; on failure it is
// reverted and canonical state is reloaded.
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
                // Revert the optimistic mutation and reconcile with the
                // canonical server order (ranks included).
                weak->_session->data().setChatPinned(history, FilterId(), !pinned);
                weak->reloadChats();
                ShowSettingsToast(
                    weak->_session,
                    history->peer,
                    u"Failed to update pin: "_q + error);
                finish(false);
                return;
            }
            // The canonical rank arrives via the replayable chat.updated
            // event (or the next reload); the optimistic append matches the
            // server policy of placing a new pin at the end.
            finish(true);
        });
    });
}

// Persists the full ordered pinned list after a drag-reorder. The submitted
// list is authoritative on the server: missing chats get unpinned there.
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

// Per-user "marked as unread" dialog flag. Also fires automatically when a
// chat is opened (mark=false), so errors are reconciled quietly.
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

// Restores the native pinned order from _pinnedRanks: unpin locally pinned
// chats missing from the canonical set, then apply pins ascending by rank -
// upstream setChatPinned appends to the end of the list, so rank iteration
// reproduces the server order. Single notification at the end.
void NativeBridge::rebuildPinnedOrder() {
    auto ordered = std::vector<std::pair<qint64, qint64>>(); // (rank, chatId)
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

// Applies a per-user settings patch carried by chat.updated. Unknown chats
// and payloads without applicable fields fall back to a full reload.
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
        // Sampled before the folder move for the same reason as in
        // setChatArchived(): setFolderPointer() drops the pin on its way out
        // of the old chat list, so the order has to be re-announced.
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
            // Pinned without a rank: cannot place it reliably.
            needsReload = true;
        }
    }
    if (data.contains(u"notification_settings"_q)) {
        // Apply the settings instead of reloading the whole chat list for
        // them. This used to be the only path that carried mute state, so the
        // handler fell back to reloadChats() whenever nothing else in the
        // patch applied - which made every mute toggle refetch every chat.
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
	// Native TimeId is int32: clamp instead of silently narrowing.
	if (muteUntil > INT32_MAX) {
		muteUntil = INT32_MAX;
	} else if (muteUntil < 0) {
		muteUntil = 0;
	}
	// The native per-peer sound is a separate value from mute, so it has to
	// travel with the save: without it the switch lived only until the server
	// echoed the settings back, and "disable sound" did nothing at all.
	const auto sound = peer->notify().sound();
	const auto soundNone = sound && sound->none;
	// show_previews is not exposed by the native model: send the last
	// server-accepted value instead of a hardcoded true, so a multi-device
	// "previews off" survives local mute changes.
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
				// The server rejected the change: roll the UI back to the
				// last server-accepted snapshot.
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
			// Success: adopt the canonical response as the new baseline,
			// carrying the revision it was stored under. Writing a bare
			// snapshot here would reset the watermark to zero and re-open the
			// door to the stale event this save just outran.
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
    // Groups and channels are outside the current product scope: their UI is
    // hidden, and the server serves the "user" scope only. Dropping them here
    // keeps that decision in the bridge instead of the upstream queue.
    if (type != Data::DefaultNotify::User) {
        return;
    }
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
    const auto revertMute = _defaultNotifyMuteUntil;
    const auto revertSound = _defaultNotifySoundNone;
    client().setDefaultNotificationSettings(muteUntil, soundNone, QString(),
        [weak, revertMute, revertSound](QJsonDocument doc, QString error, int) {
            if (!weak) {
                return;
            }
            if (!error.isEmpty() || !doc.isObject()) {
                // The server rejected the change: roll the UI back to the
                // last server-accepted snapshot instead of leaving a value
                // that silently disappears on the next restart.
                weak->applyDefaultNotifySettings(revertMute, revertSound);
                return;
            }
            weak->applyDefaultNotifySettingsPayload(doc.object());
        });
}

void NativeBridge::react(HistoryItem *item, const QString &emoji) {
    if (!item || emoji.isEmpty() || item->id.bare <= 0) return;
    const auto history = item->history().get();
    const auto weak = QPointer<NativeBridge>(this);
    client().react(item->id.bare, emoji, [weak, history](QJsonDocument doc, QString error, int) {
        if (!weak || !history || !error.isEmpty() || !doc.isObject()) return;
        const auto data = doc.object();
        const auto messageId = data.value("id").toVariant().toLongLong();
        if (messageId > 0 && messageId <= INT32_MAX) {
            if (const auto existing = weak->_session->data().message(
                    history->peer,
                    MsgId(int32(messageId)))) {
                weak->applyMessageReactions(existing, data);
            } else {
                weak->applyMessage(history, data, false);
            }
        }
        weak->_session->data().sendHistoryChangeNotifications();
    });
}

void NativeBridge::setReactions(HistoryItem *item, const QStringList &emojis) {
    if (!item || item->id.bare <= 0) return;
    const auto history = item->history().get();
    const auto messageId = item->id.bare;
    auto &state = _reactionReplace[messageId];
    // Coalesce: whatever the click storm produces, only the final desired
    // set is sent once the in-flight request completes.
    state.desired = emojis;
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
                // Revisions only grow: an older payload never lowers the
                // guard for the next replace.
                if (const auto revision = data.value("reaction_revision").toVariant().toLongLong(); revision > state.revision) {
                    state.revision = revision;
                }
                if (const auto existing = weak->_session->data().message(
                        history->peer,
                        MsgId(int32(messageId)))) {
                    weak->applyMessageReactions(existing, data);
                }
                weak->_session->data().sendHistoryChangeNotifications();
            };
            if (!error.isEmpty() || !doc.isObject()) {
                if (status == 409 && doc.isObject()) {
                    // Lost race: adopt the authoritative server state (the
                    // agreed conflict policy) instead of rolling back.
                    applyCanonical(doc.object().value("current").toObject());
                } else {
                    // Nothing was stored. Upstream applied the toggle
                    // optimistically, so without this the bubble keeps
                    // reactions the server never accepted, and the divergence
                    // survives until the next canonical payload for that
                    // message arrives.
                    LOG(("FoxMes: reaction replace failed for message %1: "
                        "%2 (status %3)"
                        ).arg(messageId).arg(error).arg(status));
                    weak->reloadMessageReactions(history, messageId);
                }
            } else {
                applyCanonical(doc.object());
            }
            // A newer desired set accumulated while this request was in
            // flight: dispatch it against the freshly learned revision. A
            // plain failure keeps the set for the next user click instead of
            // retrying in a loop.
            if (status == 409 || (error.isEmpty() && doc.isObject())) {
                if (!weak->_reactionReplace.contains(messageId)
                        || weak->_reactionReplace[messageId].desired != sent) {
                    weak->dispatchReactionReplace(history, messageId);
                }
            }
        });
}

// Re-reads one message and applies its canonical reactions. Used to undo an
// optimistic reaction set the server refused: the response of the failed
// mutation carries no state to adopt.
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
            weak->applyMessageReactions(existing, doc.object());
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
            // Server contract: {items:[...], next_before, has_more, total}.
            // A bare array is the legacy single-page shape (has_more=false).
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
                // A server that does not report the total must not shrink the
                // result to this page: upstream stops paging the moment the
                // loaded count reaches it.
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
        // A chat that does not exist on the server yet has no messages to
        // find, and asking without the filter would search every other chat.
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
        if (!error.isEmpty() || !doc.isObject()) {
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

// Forward re-uses the backend basis of fxl-api/chat: POST
// /chats/{id}/messages/forward creates target messages from forwarded_from_*
// references. Returned messages are applied into the target history.
void NativeBridge::forwardMessages(
        History *source,
        History *target,
        const std::vector<int32_t> &ids,
        std::function<void(QString error)> done) {
    if (!source || !target || ids.empty()) {
        if (done) done(u"nothing to forward"_q);
        return;
    }
    const auto weak = QPointer<NativeBridge>(this);
    const auto completion = std::make_shared<std::function<void(QString)>>(std::move(done));
    ensureChat(source, [weak, source, target, ids, completion](qint64 sourceChatId) mutable {
        if (!weak || !source || !target) {
            if (*completion) (*completion)(u"forward cancelled"_q);
            return;
        }
        if (sourceChatId <= 0) {
            if (*completion) (*completion)(u"source chat unavailable"_q);
            return;
        }
        weak->ensureChat(target, [weak, target, ids, sourceChatId, completion](qint64 targetChatId) mutable {
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
            *request = [weak, target, targetChatId, sourceChatId, messageIds, operationId, completion, attempt, request] {
                if (!weak || !target) {
                    *request = {};
                    if (*completion) (*completion)(u"forward cancelled"_q);
                    return;
                }
                ++*attempt;
                weak->client().forwardMessages(
                    targetChatId,
                    sourceChatId,
                    messageIds,
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
    });
}

// Pin/unpin are single mutations on a shared list: the server answers with the
// whole authoritative state, so the reply is applied directly and no local
// guess is needed. operation_id keeps a retried click idempotent.
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

// Applies the authoritative pinned state of a chat. The payload always carries
// the complete id set, so the local list is replaced rather than patched:
// upstream drives the pinned bar off the SharedMedia::Pinned slice, and only a
// complete slice with an exact count makes the "N pinned" counter and the
// next/previous navigation correct.
void NativeBridge::applyPinnedState(
        History *history,
        qint64 chatId,
        const QJsonObject &state) {
    if (!history || chatId <= 0) {
        return;
    }
    // Revisions are monotonic per chat. A payload older than what we already
    // applied describes a list we have left behind; applying it would revert
    // the pinned bar and guarantee a conflict on the next mutation.
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

    // Bodies that travelled with the list are applied first so the bar can
    // render from a native item instead of re-fetching every pinned message
    // one by one through requestMessageData().
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
            // The item may not be loaded at all, and adding a slice never
            // drops ids it does not mention, so the stale entry has to be
            // removed from the shared media index explicitly.
            storage.remove(Storage::SharedMediaRemoveOne(
                peer->id,
                MsgId(0), // topicRootId
                PeerId(0), // monoforumPeerId
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
        MsgId(0), // topicRootId
        PeerId(0), // monoforumPeerId
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

// Fetches the pinned list of a chat once per session. Called from every entry
// into history loading, so opening a chat by jumping to a message syncs the
// pinned bar just like opening it at the tail does.
void NativeBridge::syncPinnedMessages(History *history, qint64 chatId) {
    if (!history || chatId <= 0) return;
    if (!_pinnedSyncedChats.emplace(chatId).second) return;
    const auto weak = QPointer<NativeBridge>(this);
    client().pinnedMessages(chatId, kPinnedMessagesLimit,
        [weak, history, chatId](QJsonDocument doc, QString error, int) {
            if (!weak) return;
            if (!history || !error.isEmpty() || !doc.isObject()) {
                // Allow a later open to retry: a failed fetch must not leave
                // the chat permanently without its pinned bar.
                weak->_pinnedSyncedChats.erase(chatId);
                return;
            }
            weak->applyPinnedState(history, chatId, doc.object());
        });
}

// Entry point for the ApiWrap::requestSharedMedia hook: upstream asks for the
// pinned slice when its local one is insufficient. Under the bridge there is
// no MTProto search to answer with, so the request is served from the pinned
// list instead of being left hanging as a leaked mtpRequestId.
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
		// Events were missed between _eventSeq and this one. Recover from the
		// snapshot and replay from just before the event we can see, so the
		// reconnect never asks for the same window again.
		resyncAfterGap(sequence, sequence);
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
                    applyMessageReactions(item, data);
                    _session->data().sendHistoryChangeNotifications();
                }
            }
        }
    } else if (type == u"message.deleted"_q) {
        removeMessage(
            data.value("chat_id").toVariant().toLongLong(),
            data.value("id").toVariant().toLongLong());
	} else if (type == u"message.delivered"_q) {
		// Native Telegram Desktop has no separate delivered check. The durable
		// event is consumed for sequence continuity, but does not change UI.
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
        // Handle the ephemeral event (seq=0, not stored in fox_mes_events)
        // before the generic chat.* fallback. Otherwise every typing event
        // triggers reloadChats(). The server already skips the sender; this
        // check is an additional safeguard.
        const auto userId = data.value("user_id").toVariant().toLongLong();
        const auto chatId = data.value("chat_id").toVariant().toLongLong();
        if (userId > 0 && userId != client().meId()) {
            if (const auto history = historyForChatId(chatId)) {
                const auto user = _session->data().user(UserId(userId));
                // The web publishes typing as start/update/stop; the server
                // forwards a stop as action=cancel. It maps to MTProto's
                // sendMessageCancelAction, which upstream applies as an
                // immediate clear. Treating it as one more typing ping kept
                // the indicator alive for its whole expiry window - and a
                // stop is exactly what the composer fires after sending, so
                // "typing" stayed on screen after the message had arrived.
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
	} else if (type == u"chat.deleted"_q) {
		const auto chatId = data.value("chat_id").toVariant().toLongLong();
		if (const auto history = historyForChatId(chatId)) {
			_session->data().deleteConversationLocally(history->peer);
		}
		reloadChats();
	} else if (type == u"user.updated"_q || type == u"user.created"_q) {
		ensureUser(data, true);
		_contactsDone = true;
		finishInitialLoadIfReady();
	} else if (type == u"chat.pinned"_q) {
		const auto chatId = data.value("chat_id").toVariant().toLongLong();
		if (const auto history = historyForChatId(chatId)) {
			applyPinnedState(history, chatId, data);
		}
	} else if (type == u"draft.updated"_q || type == u"draft.deleted"_q) {
		// Replayable per-chat draft events keep the local revision in sync so
		// the next save/send guards against the actual server state.
		const auto chatId = data.value("chat_id").toVariant().toLongLong();
		if (const auto revision = data.value("draft").toObject().value("revision").toVariant().toLongLong(); revision > 0) {
			_draftRevisionByChat[chatId] = revision;
		} else {
			_draftRevisionByChat[chatId] = 0;
		}
	} else if (Scheduled::ApplyEvent(_session, type, data)) {
		// The scheduled queue is private to its author, so a reminder event
		// only ever reaches the client that created it.
	} else if (type == u"gap.detected"_q) {
		// next_seq is the authoritative recovery point: everything below it is
		// gone from the replay window, so the snapshot has to stand in for it.
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
		// Per-user settings patches (pin / unread mark / archive) are applied
		// directly; unknown chats and non-settings payloads fall back to a
		// full canonical reload inside applyChatSettingsPatch.
		applyChatSettingsPatch(data);
    } else if (type.startsWith(u"chat."_q)) {
        reloadChats();
    }
	if (sequence > _eventSeq) {
		_eventSeq = sequence;
		client().setEventSequence(_eventSeq);
		RememberEventSequence(_session, _eventSeq);
		// Replay is moving again, so a future gap at this point is a real one
		// and must not be mistaken for the repeat we just guarded against.
		_lastGapResumeFrom = 0;
	}
}

void NativeBridge::forceLiveUpdatesRestart() {
	_liveUpdates->restartNow();
}

void NativeBridge::resyncAfterGap(qint64 resumeFrom, qint64 observedSeq) {
	// The replay window no longer covers what we missed. Rebuild state from
	// the snapshot and move the replay point forward before reconnecting:
	// reconnecting on the old _eventSeq makes the server report the very same
	// gap again, which is how this used to turn into a reconnect loop.
	if (resumeFrom <= 0) {
		resumeFrom = observedSeq;
	}
	if (resumeFrom <= 0) {
		// Nothing to anchor on: reload state and let the connection retry
		// from wherever it is, but do not spin on a bogus sequence.
		reloadChats();
		return;
	}
	if (_lastGapResumeFrom == resumeFrom) {
		// The same recovery point twice means resyncing did not move us
		// forward. Reconnecting again would loop, so stop here and keep the
		// session on the snapshot we already applied.
		LOG(("FoxMes: repeated gap at seq %1, resync skipped"
			).arg(resumeFrom));
		return;
	}
	_lastGapResumeFrom = resumeFrom;

	// Drop caches that describe per-message state we can no longer trust:
	// revisions and read watermarks are re-seeded by the snapshot below.
	_messageRevisions.clear();
	_appliedReadStates.clear();
	// The pinned list is revision-guarded, so a stale local revision would
	// make every post-gap chat.pinned look older than what we hold and be
	// dropped. Forget it and let the next chat open refetch.
	_pinnedSyncedChats.clear();
	_pinnedIds.clear();
	_pinRevisions.clear();

	// Replay must continue from just before the first event we can still see.
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

} // namespace CustomBackend
