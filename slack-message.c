#include <debug.h>

#include "slack-json.h"
#include "slack-rtm.h"
#include "slack-api.h"
#include "slack-user.h"
#include "slack-channel.h"
#include "slack-message.h"

static gchar *slack_message_to_html(SlackAccount *sa, gchar *s, const char *subtype, PurpleMessageFlags *flags) {
	g_return_val_if_fail(s, NULL);

	size_t l = strlen(s);
	char *end = &s[l];
	GString *html = g_string_sized_new(l);

	if (!g_strcmp0(subtype, "me_message"))
		g_string_append(html, "/me ");
	else if (subtype)
		*flags |= PURPLE_MESSAGE_SYSTEM;
	*flags |= PURPLE_MESSAGE_NO_LINKIFY;

	while (s < end) {
		char c = *s++;
		if (c == '\n') {
			g_string_append(html, "<BR>");
			continue;
		}
		if (c != '<') {
			g_string_append_c(html, c);
			continue;
		}

		/* found a <tag> */
		char *r = strchr(s, '>');
		if (!r)
			/* should really be error */
			r = end;
		else
			*r = 0;
		char *b = memchr(s, '|', r-s);
		if (b) {
			*b = 0;
			b++;
		}
		switch (*s) {
			case '#':
				s++;
				g_string_append_c(html, '#');
				if (!b) {
					SlackChannel *chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, s);
					if (chan)
						b = chan->name;
				}
				g_string_append(html, b ?: s);
				break;
			case '@':
				s++;
				g_string_append_c(html, '@');
				if (!strcmp(s, sa->self))
					*flags |= PURPLE_MESSAGE_NICK;
				if (!b) {
					SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, s);
					if (user)
						b = user->name;
				}
				g_string_append(html, b ?: s);
				break;
			case '!':
				s++;
				if (!strcmp(s, "channel") || !strcmp(s, "group") || !strcmp(s, "here") || !strcmp(s, "everyone")) {
					*flags |= PURPLE_MESSAGE_NOTIFY;
					g_string_append_c(html, '@');
					g_string_append(html, b ?: s);
				} else {
					g_string_append(html, "&lt;");
					g_string_append(html, b ?: s);
					g_string_append(html, "&gt;");
				}
				break;
			default:
				/* URL */
				g_string_append(html, "<A HREF=\"");
				g_string_append(html, s); /* XXX embedded quotes? */
				g_string_append(html, "\">");
				g_string_append(html, b ?: s);
				g_string_append(html, "</A>");
		}
		s = r+1;
	}

	return g_string_free(html, FALSE);
}

void slack_message(SlackAccount *sa, json_value *json) {
	const char *user_id    = json_get_prop_strptr(json, "user");
	const char *channel_id = json_get_prop_strptr(json, "channel");
	const char *subtype    = json_get_prop_strptr(json, "subtype");

	time_t mt = slack_parse_time(json_get_prop(json, "ts"));

	PurpleMessageFlags flags = PURPLE_MESSAGE_RECV;
	if (json_get_prop_boolean(json, "hidden", FALSE))
		flags |= PURPLE_MESSAGE_INVISIBLE;

	char *html = slack_message_to_html(sa, json_get_prop_strptr(json, "text"), subtype, &flags);

	SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);
	SlackChannel *chan;
	if (user && slack_object_id_is(user->im, channel_id)) {
		/* IM */
		serv_got_im(sa->gc, user->name, html, flags, mt);
	} else if ((chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, channel_id))) {
		/* Channel */
		if (!chan->cid) {
			if (!purple_account_get_bool(sa->account, "open_chat", FALSE))
				return;
			slack_chat_open(sa, chan);
		}

		PurpleConvChat *conv;
		if (subtype && (conv = slack_channel_get_conversation(sa, chan))) {
			if (!strcmp(subtype, "channel_topic") ||
					!strcmp(subtype, "group_topic"))
				purple_conv_chat_set_topic(conv, user ? user->name : user_id, json_get_prop_strptr(json, "topic"));
		}

		serv_got_chat_in(sa->gc, chan->cid, user ? user->name : user_id ?: "", flags, html, mt);
	} else {
		purple_debug_warning("slack", "Unhandled message: %s@%s: %s\n", user_id, channel_id, html);
	}
}

void slack_user_typing(SlackAccount *sa, json_value *json) {
	const char *user_id    = json_get_prop_strptr(json, "user");
	const char *channel_id = json_get_prop_strptr(json, "channel");

	SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);
	SlackChannel *chan;
	if (user && slack_object_id_is(user->im, channel_id)) {
		/* IM */
		serv_got_typing(sa->gc, user->name, 3, PURPLE_TYPING);
	} else if ((chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, channel_id))) {
		/* Channel */
		/* libpurple does not support chat typing indicators */
	} else {
		purple_debug_warning("slack", "Unhandled typing: %s@%s\n", user_id, channel_id);
	}
}

unsigned int slack_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state) {
	SlackAccount *sa = gc->proto_data;

	if (state != PURPLE_TYPING)
		return 0;

	SlackUser *user = g_hash_table_lookup(sa->user_names, who);
	if (!user || !*user->im)
		return 0;

	GString *channel = append_json_string(g_string_new(NULL), user->im);
	slack_rtm_send(sa, NULL, NULL, "typing", "channel", channel->str, NULL);
	g_string_free(channel, TRUE);

	return 3;
}

static void get_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackObject *obj = data;
	json_value *list = json_get_prop_type(json, "messages", array);

	if (!list || error) {
		purple_debug_error("slack", "Error loading channel history: %s\n", error ?: "missing");
		g_object_unref(obj);
		return;
	}

	/* what order are these in? */
	for (unsigned i = list->u.array.length; i; i --) {
		json_value *msg = list->u.array.values[i-1];
		if (g_strcmp0(json_get_prop_strptr(msg, "type"), "message"))
			continue;
	}

	g_object_unref(obj);
}

void slack_get_history(SlackAccount *sa, SlackObject *obj, const char *since, unsigned count) {
	const char *call = NULL, *id = NULL;
	SlackChannel *chan;
	SlackUser *user;
	if ((chan = SLACK_CHANNEL(obj))) {
		switch (chan->type) {
			case SLACK_CHANNEL_MEMBER:
				call = "channels.history";
				break;
			case SLACK_CHANNEL_GROUP:
				call = "groups.history";
				break;
			case SLACK_CHANNEL_MPIM:
				call = "mpim.history";
				break;
			default:
				break;
		}
		id = chan->object.id;
	} else if ((user = SLACK_USER(obj))) {
		if (*user->im) {
			call = "im.history";
			id = user->im;
		}
	}

	if (!call)
		return;

	char count_buf[6] = "";
	snprintf(count_buf, 5, "%u", count);
	slack_api_call(sa, get_history_cb, g_object_ref(obj), call, "channel", id, "oldest", since ?: "0", "count", count_buf, NULL);
}
