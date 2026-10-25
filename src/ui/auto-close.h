#ifndef _H_AUTO_CLOSE
#define _H_AUTO_CLOSE

#include "../mudclient.h"

#define AUTO_CLOSE_WILDERNESS_WARNING 0
#define AUTO_CLOSE_WELCOME 1
#define AUTO_CLOSE_SERVER_MESSAGE 2

/* followed bots never click these boxes, so they close themselves. the timer
 * restarts on mouse movement, clicks and new content so a person reading is
 * never cut off. */
int mudclient_auto_close_expired(mudclient *mud, int slot, int ms,
                                 const char *content);

#endif
