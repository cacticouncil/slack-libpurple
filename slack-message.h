#ifndef _PURPLE_SLACK_MESSAGE_H
#define _PURPLE_SLACK_MESSAGE_H

#include <json.h>

#include "slack.h"

void slack_message(SlackAccount *sa, json_value *json);
void slack_user_typing(SlackAccount *sa, json_value *json);
unsigned int slack_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state);

#endif // _PURPLE_SLACK_MESSAGE_H