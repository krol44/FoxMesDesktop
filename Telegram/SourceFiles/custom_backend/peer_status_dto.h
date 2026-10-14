#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QVariant>
#include <optional>
#include <vector>

namespace CustomBackend::PeerStatuses {

struct StatusAssetDto {
    qint64 id = 0;
    qint64 documentId = 0;
    QString url;
    QString previewUrl;
    QString mimeType;
    static StatusAssetDto Parse(const QJsonObject &value) {
        return {
            value.value("id").toVariant().toLongLong(),
            value.value("document_id").toVariant().toLongLong(),
            value.value("url").toString(),
            value.value("preview_url").toString(),
            value.value("mime_type").toString(),
        };
    }
};

struct EmojiStatusDto {
    std::optional<StatusAssetDto> asset;
    qint64 until = 0;
    qint64 revision = 0;
    static EmojiStatusDto Parse(const QJsonObject &value) {
        auto result = EmojiStatusDto();
        if (value.value("asset").isObject()) result.asset = StatusAssetDto::Parse(value.value("asset").toObject());
        result.until = value.value("until").toVariant().toLongLong();
        result.revision = value.value("revision").toVariant().toLongLong();
        return result;
    }
};

struct PeerDecorationDto {
    qint64 userId = 0;
    qint64 chatId = 0;
    std::optional<bool> verified;
    qint64 verificationRevision = 0;
    std::optional<EmojiStatusDto> status;
    static PeerDecorationDto Parse(const QJsonObject &value) {
        auto result = PeerDecorationDto();
        result.userId = value.value("id").toVariant().toLongLong();
        result.chatId = value.value("chat_id").toVariant().toLongLong();
        if (value.value("verified").isBool()) result.verified = value.value("verified").toBool();
        result.verificationRevision = value.value("verification_revision").toVariant().toLongLong();
        if (value.contains("emoji_status")) result.status = EmojiStatusDto::Parse(value.value("emoji_status").toObject());
        return result;
    }
};

}
