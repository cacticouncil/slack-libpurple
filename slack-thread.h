#ifndef _PURPLE_SLACK_THREAD_H
#define _PURPLE_SLACK_THREAD_H

#include "slack.h"

/**
 * Appends a formatted ts timestamp to str.
 *
 * @param str String to append to. Will be modified.
 * @param ts Timestamp for format and append.
 */
void slack_append_formatted_thread_timestamp(GString *str, const char *ts);
void slack_thread_post_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr, const char *msg);
void slack_thread_get_replies(SlackAccount *sa, SlackObject *obj, const char *timestr);

#endif // _PURPLE_SLACK_THREAD_H
