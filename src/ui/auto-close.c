#include "auto-close.h"

enum { LAST, AT, X, Y, HASH };

int mudclient_auto_close_expired(mudclient *mud, int slot, int ms,
                                 const char *content) {
    int *state = mud->auto_close_state[slot];
    int now = get_ticks();
    int hash = 0;

    for (; content != NULL && *content != '\0'; content++) {
        hash = hash * 31 + (unsigned char)*content;
    }

    if (now - state[LAST] > 1000 || state[HASH] != hash ||
        state[X] != mud->mouse_x || state[Y] != mud->mouse_y ||
        mud->mouse_button_click != 0) {
        state[AT] = now;
        state[X] = mud->mouse_x;
        state[Y] = mud->mouse_y;
        state[HASH] = hash;
    }

    state[LAST] = now;

    return now - state[AT] >= ms;
}
