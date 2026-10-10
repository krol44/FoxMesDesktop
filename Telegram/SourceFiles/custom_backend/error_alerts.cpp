#include "custom_backend/error_alerts.h"
#include "custom_backend/native_runtime.h"

#include "ui/boxes/confirm_box.h"
#include <QtCore/QRegularExpression>
#include <map>

namespace CustomBackend {

bool ErrorAlertsEnabled() {
    return Enabled();
}

QString ErrorDisplayText(const QString &code, int status) {
    static const auto messages = std::map<QString, QString>{
        { u"CALL_BUSY"_q, u"Unable to start the call because a participant is already in another call."_q },
        { u"CALL_PEER_OFFLINE"_q, u"The person you're calling is offline. Please try again later."_q },
        { u"CALL_PEER_REFUSES"_q, u"This person is not accepting calls."_q },
        { u"USER_PRIVACY_RESTRICTED"_q, u"This person is not accepting calls."_q },
        { u"CALL_RELAY_UNAVAILABLE"_q, u"Unable to connect the call. Please try again later."_q },
        { u"GROUPCALL_MEDIA_UNAVAILABLE"_q, u"Unable to connect the call. Please try again later."_q },
        { u"CALL_PEER_UNSUPPORTED"_q, u"The other person's app does not support this call. They may need to update it."_q },
        { u"CALL_VERSION_MISMATCH"_q, u"The other person's app does not support this call. They may need to update it."_q },
        { u"PARTICIPANT_VERSION_OUTDATED"_q, u"The other person's app does not support this call. They may need to update it."_q },
        { u"CALL_PROTOCOL_LAYER_INVALID"_q, u"The other person's app does not support this call. They may need to update it."_q },
        { u"CALL_RESPONSE_INVALID"_q, u"Unable to start the call. Please try again later."_q },
        { u"CALLS_UNAVAILABLE"_q, u"Unable to start the call. Please try again later."_q },
        { u"CALL_CHAT_UNRESOLVED"_q, u"Unable to start the call. Please try again later."_q },
        { u"DH_CONFIG_INVALID"_q, u"Unable to start the call. Please try again later."_q },
        { u"CALL_CONFIG_INVALID"_q, u"Unable to start the call. Please try again later."_q },
        { u"FOXMES_CALL_FAILED"_q, u"Unable to start the call. Please try again later."_q },
        { u"GROUPCALL_FORBIDDEN"_q, u"You do not have permission to join or manage this call."_q },
        { u"GROUPCALL_INVALID"_q, u"This call is no longer available."_q },
        { u"GROUPCALL_NOT_STARTED"_q, u"This call has not started yet."_q },
        { u"GROUPCALL_ALREADY_STARTED"_q, u"This chat already has an active call."_q },
        { u"GROUPCALL_PARTICIPANTS_TOO_MUCH"_q, u"This call has reached its participant limit."_q },
        { u"GROUPCALL_MESSAGES_DISABLED"_q, u"Messages are disabled in this call."_q },
        { u"GROUPCALL_SSRC_DUPLICATE_MUCH"_q, u"Unable to connect to this call. Please try again."_q },
        { u"CONF_WRITE_CHAIN_INVALID"_q, u"Unable to connect to this call. Please try again."_q },
        { u"BLOCK_INVALID"_q, u"Unable to connect to this call. Please try again."_q },
        { u"PARTICIPANT_ID_INVALID"_q, u"This participant is no longer in the call."_q },
        { u"GROUPCALL_TITLE_INVALID"_q, u"The call title is too long."_q },
        { u"FORBIDDEN"_q, u"You do not have permission to perform this action."_q },
        { u"CHAT_ACCESS_DENIED"_q, u"You do not have permission to perform this action."_q },
        { u"CHAT_ADMIN_REQUIRED"_q, u"Only a chat administrator can perform this action."_q },
        { u"CHAT_WRITE_FORBIDDEN"_q, u"You cannot send messages in this chat."_q },
        { u"USER_BANNED_IN_CHANNEL"_q, u"You cannot contact this person or post in this chat."_q },
        { u"USER_BLOCKED"_q, u"You cannot contact this person or post in this chat."_q },
        { u"YOU_BLOCKED_USER"_q, u"Unblock this person before contacting them."_q },
        { u"CHANNEL_PRIVATE"_q, u"This chat is no longer available or you do not have access to it."_q },
        { u"CHANNEL_INVALID"_q, u"This chat is no longer available or you do not have access to it."_q },
        { u"CHAT_ID_INVALID"_q, u"This chat is no longer available or you do not have access to it."_q },
        { u"NOT_FOUND"_q, u"The requested item is no longer available."_q },
        { u"MESSAGE_ID_INVALID"_q, u"The requested item is no longer available."_q },
        { u"MSG_ID_INVALID"_q, u"The requested item is no longer available."_q },
        { u"USERNAME_NOT_OCCUPIED"_q, u"The requested item is no longer available."_q },
        { u"UNAUTHORIZED"_q, u"Your session has ended. Please sign in again."_q },
        { u"AUTH_KEY_UNREGISTERED"_q, u"Your session has ended. Please sign in again."_q },
        { u"SESSION_REVOKED"_q, u"Your session has ended. Please sign in again."_q },
        { u"BAD_REQUEST"_q, u"Unable to complete this action. Please check your input and try again."_q },
        { u"MESSAGE_INVALID"_q, u"Unable to complete this action. Please check your input and try again."_q },
        { u"MESSAGE_EMPTY"_q, u"Enter a message or add an attachment."_q },
        { u"MESSAGE_PINNED"_q, u"Unpin this message before deleting it."_q },
        { u"MESSAGE_EDIT_FORBIDDEN"_q, u"You cannot edit this message."_q },
        { u"MESSAGE_DELETE_FORBIDDEN"_q, u"You do not have permission to delete this message."_q },
        { u"EDIT_TIME_EXPIRED"_q, u"This message can no longer be edited."_q },
        { u"MESSAGE_EDIT_TIME_EXPIRED"_q, u"This message can no longer be edited."_q },
        { u"MESSAGE_EDIT_TIME_EXPIRED_ERROR"_q, u"This message can no longer be edited."_q },
        { u"EPHEMERAL_EXPIRED"_q, u"This media has expired and is no longer available."_q },
        { u"GONE"_q, u"This media has expired and is no longer available."_q },
        { u"PIN_LIMIT"_q, u"This chat has reached its pin limit."_q },
        { u"PINNED_DIALOGS_TOO_MUCH"_q, u"This chat has reached its pin limit."_q },
        { u"REMINDER_LIMIT"_q, u"This chat has reached its scheduled message limit."_q },
        { u"SCHEDULE_TOO_MUCH"_q, u"This chat has reached its scheduled message limit."_q },
        { u"REMINDER_NOT_PENDING"_q, u"This scheduled message is already being sent."_q },
        { u"SCHEDULE_DATE_INVALID"_q, u"Choose a date and time in the future."_q },
        { u"CONFLICT"_q, u"This item has changed. Reload it and try again."_q },
        { u"APPEARANCE_CONFLICT"_q, u"This item has changed. Reload it and try again."_q },
        { u"DRAFT_CONFLICT"_q, u"This item has changed. Reload it and try again."_q },
        { u"PHOTOS_CONFLICT"_q, u"This item has changed. Reload it and try again."_q },
        { u"MESSAGE_REVISION_CONFLICT"_q, u"This item has changed. Reload it and try again."_q },
        { u"REACTION_REVISION_CONFLICT"_q, u"This item has changed. Reload it and try again."_q },
        { u"REMINDER_REVISION_CONFLICT"_q, u"This item has changed. Reload it and try again."_q },
        { u"OPERATION_CONFLICT"_q, u"Unable to confirm this action. Reload the current state and try again."_q },
        { u"OPERATION_RESULT_UNAVAILABLE"_q, u"Unable to confirm this action. Reload the current state and try again."_q },
        { u"OPERATION_IN_PROGRESS"_q, u"This action is still being processed. Please wait."_q },
        { u"REACTION_INVALID"_q, u"This reaction is not available for this message."_q },
        { u"REACTIONS_BUSY"_q, u"Unable to update reactions right now. Please try again."_q },
        { u"RATE_LIMITED"_q, u"Too many attempts. Please wait and try again."_q },
        { u"FLOOD_WAIT"_q, u"Too many attempts. Please wait and try again."_q },
        { u"PAYLOAD_TOO_LARGE"_q, u"This file is too large. Please choose a smaller file."_q },
        { u"PHOTO_FILE_TOO_BIG"_q, u"This file is too large. Please choose a smaller file."_q },
        { u"VIDEO_FILE_TOO_BIG"_q, u"This file is too large. Please choose a smaller file."_q },
        { u"WALLPAPER_NOT_FOUND"_q, u"This theme or wallpaper is no longer available."_q },
        { u"THEME_INVALID"_q, u"This theme or wallpaper is no longer available."_q },
        { u"USERNAME_INVALID"_q, u"Enter a valid username."_q },
        { u"USERNAME_OCCUPIED"_q, u"This username is already taken."_q },
        { u"CHAT_TITLE_EMPTY"_q, u"Enter a chat name."_q },
        { u"CHAT_ABOUT_TOO_LONG"_q, u"The chat description is too long."_q },
        { u"CHAT_CREATE_FORBIDDEN"_q, u"Your account cannot create this chat."_q },
        { u"CHAT_OWNER_LEAVE"_q, u"The chat owner must delete the chat to leave it."_q },
        { u"USERS_TOO_MUCH"_q, u"This chat has reached its member limit."_q },
        { u"USERS_TOO_FEW"_q, u"Select at least one person."_q },
        { u"USER_ID_INVALID"_q, u"This person is not available for this action."_q },
        { u"USER_NOT_PARTICIPANT"_q, u"This person is not available for this action."_q },
        { u"USER_ADMIN_INVALID"_q, u"This person is not available for this action."_q },
        { u"USER_ALREADY_PARTICIPANT"_q, u"This person is already a participant."_q },
        { u"INVITE_HASH_EXPIRED"_q, u"This invitation is no longer valid."_q },
        { u"INVITE_HASH_INVALID"_q, u"This invitation is no longer valid."_q },
        { u"STICKER_CHAT_ONLY"_q, u"This sticker can only be sent in FoxMes."_q },
        { u"MEDIA_NOT_ON_SITE"_q, u"Voice and video messages cannot be sent to the site's comments."_q },
        { u"WELCOME_MESSAGES_TOO_MUCH"_q, u"This chat has reached its welcome message limit."_q },
        { u"STICKER_FAVORITES_TOO_MUCH"_q, u"You have reached the favorite sticker limit."_q },
        { u"NOT_IMPLEMENTED"_q, u"This action is not available."_q },
        { u"FOXMES_MTPROTO_DISABLED"_q, u"This action is not available."_q },
        { u"INTERNAL_ERROR"_q, u"Unable to complete this action. Please try again later."_q },
        { u"INTERNAL_SERVER_ERROR"_q, u"Unable to complete this action. Please try again later."_q },
        { u"FOXMES_INTERNAL"_q, u"Unable to complete this action. Please try again later."_q },
    };
    const auto key = code.trimmed().toUpper();
    if (const auto i = messages.find(key); i != messages.end()) {
        return i->second;
    }
    if (key.startsWith(u"FLOOD_WAIT_"_q) || key.startsWith(u"FLOOD_PREMIUM_WAIT_"_q)) {
        return u"Too many attempts. Please wait and try again."_q;
    }
    if (status == 401) return u"Your session has ended. Please sign in again."_q;
    if (status == 403) return u"You do not have permission to perform this action."_q;
    if (status == 404) return u"The requested item is no longer available."_q;
    if (status == 409) return u"This item has changed. Reload it and try again."_q;
    if (status == 413) return u"This file is too large. Please choose a smaller file."_q;
    if (status == 429) return u"Too many attempts. Please wait and try again."_q;
    if (status >= 500) return u"Unable to complete this action. Please try again later."_q;
    return u"Unable to complete this action. Please try again."_q;
}

QString ErrorAlertText(const QString &text) {
    static const auto token = QRegularExpression(u"^[A-Za-z][A-Za-z0-9]*(?:_[A-Za-z0-9]+)+$"_q);
    auto lines = text.split('\n');
    auto changed = false;
    for (auto &line : lines) {
        const auto trimmed = line.trimmed();
        const auto message = ErrorDisplayText(trimmed);
        if (message != u"Unable to complete this action. Please try again."_q
            || token.match(trimmed).hasMatch()) {
            line = message;
            changed = true;
        }
    }
    return changed ? lines.join('\n') : text;
}

object_ptr<Ui::GenericBox> MakeErrorAlertBox(Ui::ConfirmBoxArgs &&args) {
    if (args.inform && !v::is_null(args.text)) {
        args.text = v::text::take_marked(std::move(args.text)) | rpl::map([](TextWithEntities value) {
            const auto text = ErrorAlertText(value.text);
            if (text != value.text) {
                value.text = text;
                value.entities.clear();
            }
            return value;
        });
    }
    return Box(Ui::ConfirmBox, std::move(args));
}

}
