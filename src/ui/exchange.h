#ifndef _H_EXCHANGE
#define _H_EXCHANGE

#include "../mudclient.h"

/* Grand Exchange panel (ARSC). Contract: Autonomous-RSC docs/GRAND_EXCHANGE.md.
 * Server -> client: 140 EXCHANGE_STATE, 141 EXCHANGE_QUOTE, 142 EXCHANGE_CLOSE.
 * Client -> server: "::ge ..." command strings only (CLIENT_COMMAND). */

#define GE_WIDTH 408
#define GE_SLOT_COLUMNS 4
#define GE_SLOT_WIDTH 100
#define GE_SLOT_HEIGHT 40
#define GE_COLLECT_COLUMNS 8
#define GE_COLLECT_SLOT_WIDTH 49
#define GE_COLLECT_SLOT_HEIGHT 26
#define GE_MODE_HEIGHT 100
#define GE_MATCHES_MAX 8

#define GE_FLAG_OPEN 1
#define GE_FLAG_BARRED 2
#define GE_FLAG_IN_RANGE 4

#define GE_SLOT_EMPTY 0
#define GE_SLOT_BUY 1
#define GE_SLOT_SELL 2

typedef enum {
    GE_MODE_IDLE = 0,
    GE_MODE_SIDE = 1,      /* empty slot clicked: Buy or Sell */
    GE_MODE_CANCEL = 2,    /* full slot clicked: Cancel */
    GE_MODE_PICK_SELL = 3, /* choose an inventory item */
    GE_MODE_PICK_BUY = 4,  /* type part of a name */
    GE_MODE_OFFER = 5      /* quantity, price, confirm */
} GeMode;

typedef enum {
    GE_FIELD_NONE = 0,
    GE_FIELD_QTY = 1,
    GE_FIELD_PRICE = 2
} GeField;

void mudclient_draw_exchange(mudclient *mud);
void mudclient_exchange_read_state(mudclient *mud, int8_t *data, int size);
void mudclient_exchange_read_quote(mudclient *mud, int8_t *data, int size);
void mudclient_exchange_close(mudclient *mud, int send_close);
int mudclient_exchange_wants_text(mudclient *mud);
void mudclient_exchange_type_char(mudclient *mud, int char_code);
void mudclient_exchange_backspace(mudclient *mud);
void mudclient_exchange_enter(mudclient *mud);

#endif
