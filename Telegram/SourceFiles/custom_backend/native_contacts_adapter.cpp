/*
This file is part of FoxMes Desktop.
*/
#include "custom_backend/native_contacts_adapter.h"

#include "api/api_peer_photo.h"
#include "apiwrap.h"
#include "base/debug_log.h"
#include "base/weak_ptr.h"
#include "custom_backend/api_client.h"
#include "custom_backend/native_bridge.h"
#include "custom_backend/native_mtp_router.h"
#include "custom_backend/native_runtime.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "editor/photo_editor_common.h"
#include "editor/photo_editor_layer_widget.h"
#include "info/profile/info_profile_values.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "settings/settings_common.h"
#include "ui/controls/userpic_button.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_edit_peer_members.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonObject>
#include <QtGui/QImage>

namespace CustomBackend::Contacts {
namespace {

constexpr auto kMaxNameLength = 128;
constexpr auto kMaxNoteLength = 128;
constexpr auto kPhotoJpegQuality = 87;

[[nodiscard]] qint64 UserIdOf(not_null<UserData*> user) {
	return qint64(peerToUser(user->id).bare);
}

void ApplyUser(not_null<Main::Session*> session, const QJsonDocument &doc) {
	if (const auto bridge = BridgeFor(session); bridge && doc.isObject()) {
		bridge->ensureUser(doc.object(), true);
	}
}

void SaveFields(
		not_null<UserData*> user,
		QJsonObject fields,
		Fn<void(bool success)> done) {
	const auto session = &user->session();
	const auto weak = base::make_weak(session);
	ClientFor(session).setUserContact(
		UserIdOf(user),
		std::move(fields),
		[=](QJsonDocument doc, QString error, int status) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			if (!error.isEmpty() || !doc.isObject()) {
				LOG(("FoxMes: PUT contact failed (%1, %2)"
					).arg(status
					).arg(error));
				if (done) done(false);
				return;
			}
			ApplyUser(strong, doc);
			if (done) done(true);
		});
}

void SetBlocked(
		not_null<PeerData*> peer,
		bool blocked,
		Fn<void(bool success)> done) {
	const auto user = peer->asUser();
	if (!user) {
		if (done) done(false);
		return;
	}
	const auto session = &user->session();
	const auto weak = base::make_weak(session);
	ClientFor(session).setUserBlocked(
		UserIdOf(user),
		blocked,
		[=](QJsonDocument doc, QString error, int status) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			if (!error.isEmpty() || !doc.isObject()) {
				LOG(("FoxMes: block %1 failed (%2, %3)"
					).arg(blocked ? u"PUT"_q : u"DELETE"_q
					).arg(status
					).arg(error));
				if (done) done(false);
				return;
			}
			ApplyUser(strong, doc);
			if (done) done(true);
		});
}

[[nodiscard]] QString OriginalName(not_null<UserData*> user) {
	const auto bridge = BridgeFor(&user->session());
	const auto view = bridge
		? bridge->contactView(UserIdOf(user))
		: NativeBridge::ContactView();
	return view.originalName.isEmpty() ? user->name() : view.originalName;
}

[[nodiscard]] bool HasOwnPhoto(not_null<UserData*> user) {
	const auto bridge = BridgeFor(&user->session());
	return bridge && !bridge->contactView(UserIdOf(user)).photoUrl.isEmpty();
}

void AddCover(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user) {
	const auto &st = st::infoEditContactCover;
	const auto cover = box->addRow(
		object_ptr<Ui::FixedHeightWidget>(box, st.height),
		style::margins());
	const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
		cover,
		window,
		user,
		Ui::UserpicButton::Role::OpenPhoto,
		Ui::UserpicButton::Source::PeerPhoto,
		st.photo);
	userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto name = Ui::CreateChild<Ui::FlatLabel>(
		cover,
		Info::Profile::NameValue(user),
		st.name);
	const auto username = user->username();
	const auto status = Ui::CreateChild<Ui::FlatLabel>(
		cover,
		username.isEmpty() ? QString() : (u"@"_q + username),
		st.status);
	status->setAttribute(Qt::WA_TransparentForMouseEvents);
	rpl::combine(
		cover->widthValue(),
		Info::Profile::NameValue(user)
	) | rpl::on_next([=, &st](int width, const QString &) {
		userpic->moveToLeft(st.photoLeft, st.photoTop, width);
		name->resizeToNaturalWidth(width - st.nameLeft - st.rightSkip);
		name->moveToLeft(st.nameLeft, st.nameTop, width);
		status->resizeToNaturalWidth(width - st.statusLeft - st.rightSkip);
		status->moveToLeft(st.statusLeft, st.statusTop, width);
	}, cover->lifetime());
}

void AddPhotoButtons(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		rpl::producer<QString> nameValue) {
	const auto inner = box->verticalLayout();
	Ui::AddSkip(inner);

	auto shortName = rpl::duplicate(nameValue);
	const auto setButton = Settings::AddButtonWithIcon(
		inner,
		tr::lng_set_photo_for_user(lt_user, std::move(shortName)),
		st::settingsButtonLight,
		{ &st::menuIconPhotoSet });
	setButton->setClickedCallback([=] {
		Editor::PrepareProfilePhotoFromFile(
			box,
			&window->window(),
			Editor::EditorData{
				.about = tr::lng_profile_set_personal_sure(
					tr::now,
					lt_user,
					tr::bold(user->shortName()),
					tr::marked),
				.confirm = tr::lng_profile_set_photo_button(tr::now),
				.cropType = Editor::EditorData::CropType::Ellipse,
				.keepAspectRatio = true,
			},
			[=](QImage &&image) {
				user->session().api().peerPhoto().upload(
					user,
					Api::PeerPhoto::UserPhoto{ .image = std::move(image) });
			});
	});

	const auto resetWrap = inner->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inner,
			object_ptr<Ui::VerticalLayout>(inner)));
	const auto resetButton = Settings::AddButtonWithIcon(
		resetWrap->entity(),
		tr::lng_profile_photo_reset(),
		st::settingsButtonLight,
		{ &st::menuIconPhotoSet });
	resetWrap->toggleOn(user->session().changes().peerFlagsValue(
		user,
		Data::PeerUpdate::Flag::Photo
	) | rpl::map([=] {
		return HasOwnPhoto(user);
	}) | rpl::distinct_until_changed());
	resetButton->setClickedCallback([=] {
		SaveFields(user, QJsonObject{ { u"photo_file_id"_q, 0 } }, nullptr);
	});

	Ui::AddSkip(inner);
	Ui::AddDividerText(
		inner,
		tr::lng_contact_photo_replace_info(lt_user, std::move(nameValue)));
	Ui::AddSkip(inner);
}

void AddBlockButton(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user) {
	const auto inner = box->verticalLayout();
	auto text = user->session().changes().peerFlagsValue(
		user,
		Data::PeerUpdate::Flag::IsBlocked
	) | rpl::map([=] {
		return user->isBlocked()
			? tr::lng_profile_unblock_user()
			: tr::lng_profile_block_user();
	}) | rpl::flatten_latest();
	const auto button = Settings::AddButtonWithIcon(
		inner,
		std::move(text),
		st::settingsAttentionButton,
		{ nullptr });
	button->setClickedCallback([=] {
		if (user->isBlocked()) {
			Window::PeerMenuUnblockUserWithBotRestart(window->uiShow(), user);
		} else {
			window->show(Box(
				Window::PeerMenuBlockUserBox,
				&window->window(),
				user,
				v::null,
				v::null));
		}
	});
	Ui::AddSkip(inner);
}

} // namespace

void FillEditBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> window,
		not_null<UserData*> user,
		bool focusNote) {
	box->setWidth(st::boxWideWidth);
	box->setTitle(tr::lng_edit_contact_title());

	AddCover(box, window, user);

	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Name"_q),
			user->name()),
		st::addContactFieldMargin);
	field->setMaxLength(kMaxNameLength);

	const auto inner = box->verticalLayout();
	Ui::AddSkip(inner);
	Ui::AddDivider(inner);
	Ui::AddSkip(inner);
	const auto noteField = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_contact_add_notes(),
			user->note().text),
		st::addContactFieldMargin);
	noteField->setMaxLength(kMaxNoteLength);
	Ui::AddSkip(inner);
	Ui::AddDividerText(inner, tr::lng_contact_add_notes_about());

	auto nameValue = rpl::single(
		field->getLastText().trimmed()
	) | rpl::then(field->changes() | rpl::map([=] {
		return field->getLastText().trimmed();
	})) | rpl::map([](const QString &text) {
		return text.isEmpty() ? Ui::kQEllipsis : text;
	});
	AddPhotoButtons(box, window, user, std::move(nameValue));
	AddBlockButton(box, window, user);

	const auto saving = box->lifetime().make_state<bool>(false);
	const auto save = [=] {
		if (*saving) {
			return;
		}
		auto name = TextUtilities::SingleLine(field->getLastText()).trimmed();
		if (name == OriginalName(user)) {
			name = QString();
		}
		const auto current = BridgeFor(&user->session());
		const auto stated = current
			? current->contactView(UserIdOf(user)).name
			: QString();
		auto fields = QJsonObject();
		if (name != stated) {
			fields.insert(u"name"_q, name);
		}
		const auto note = noteField->getLastText().trimmed();
		if (note != user->note().text) {
			fields.insert(u"note"_q, note);
		}
		if (fields.isEmpty()) {
			box->closeBox();
			return;
		}
		*saving = true;
		const auto weak = base::make_weak(box.get());
		SaveFields(user, std::move(fields), [=](bool ok) {
			if (const auto strong = weak.get()) {
				*saving = false;
				if (ok) {
					strong->closeBox();
				} else {
					strong->showToast(u"Could not save the contact."_q);
				}
			}
		});
	};
	field->submits() | rpl::on_next(save, field->lifetime());
	noteField->submits() | rpl::on_next(save, noteField->lifetime());
	box->addButton(tr::lng_box_done(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] {
		if (focusNote) {
			noteField->setFocusFast();
			noteField->setCursorPosition(noteField->getLastText().size());
		} else {
			field->setFocusFast();
		}
	});
}

void Block(not_null<PeerData*> peer) {
	SetBlocked(peer, true, nullptr);
}

void Unblock(not_null<PeerData*> peer, Fn<void(bool success)> done) {
	SetBlocked(peer, false, std::move(done));
}

void UploadPhoto(
		not_null<UserData*> user,
		QImage &&image,
		Fn<void()> done) {
	auto bytes = QByteArray();
	{
		auto buffer = QBuffer(&bytes);
		image.save(&buffer, "JPEG", kPhotoJpegQuality);
	}
	const auto session = &user->session();
	const auto weak = base::make_weak(session);
	ClientFor(session).uploadData(
		u"contact.jpg"_q,
		bytes,
		u"image/jpeg"_q,
		UploadTarget(),
		false,
		[=](QJsonDocument doc, QString error, int status) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto fileId = doc.object().value(u"data"_q).toObject()
				.value(u"id"_q).toVariant().toLongLong();
			if (!error.isEmpty() || fileId <= 0) {
				LOG(("FoxMes: contact photo upload failed (%1, %2)"
					).arg(status
					).arg(error));
				Ui::Toast::Show(u"Could not set the photo."_q);
				return;
			}
			SaveFields(
				user,
				QJsonObject{ { u"photo_file_id"_q, fileId } },
				[=](bool ok) {
					if (ok && done) {
						done();
					}
				});
		},
		{},
		u"contact_photo"_q);
}

bool Intercepts(const MTP::details::SerializedRequest &request) {
	return Mtp::RequestType(request) == mtpc_contacts_updateContactNote;
}

void Intercept(
		not_null<MTP::Instance*> instance,
		mtpRequestId requestId,
		const MTP::details::SerializedRequest &request) {
	const auto answer = Mtp::Answer(instance, requestId);
	const auto session = Mtp::SessionFor(instance);
	auto reader = Mtp::Reader(request);
	const auto input = reader.read<MTPInputUser>();
	const auto note = reader.read<MTPTextWithEntities>();
	const auto userId = input.match([](const MTPDinputUser &data) {
		return UserId(data.vuser_id().v);
	}, [](const auto &) {
		return UserId();
	});
	const auto user = (session && reader.ok() && userId)
		? session->data().userLoaded(userId)
		: nullptr;
	if (!user) {
		answer.fail(u"USER_ID_INVALID"_q);
		return;
	}
	const auto text = note.data().vtext().v;
	SaveFields(user, QJsonObject{
		{ u"note"_q, QString::fromUtf8(text) },
	}, [=](bool ok) {
		if (ok) {
			answer.done(MTP_boolTrue());
		} else {
			answer.fail(u"INTERNAL_SERVER_ERROR"_q, 500);
		}
	});
}

} // namespace CustomBackend::Contacts
