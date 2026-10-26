#include "exchange.h"

/* Grand Exchange panel. Every action is a "::ge ..." command by item id; the
 * server answers with EXCHANGE_STATE (140), EXCHANGE_QUOTE (141) and a "[GE]"
 * chat line. See Autonomous-RSC docs/GRAND_EXCHANGE.md. */

static int ge_fits(int offset, int length, int size) {
    return offset >= 0 && length >= 0 && offset + length <= size;
}

static int ge_valid_item(int item_id) {
    return item_id >= 0 && item_id < game_data.item_count;
}

static const char *ge_item_name(int item_id) {
    if (!ge_valid_item(item_id) || game_data.items[item_id].name == NULL) {
        return "Unknown item";
    }

    return game_data.items[item_id].name;
}

static int ge_tradeable(int item_id) {
    return ge_valid_item(item_id) && item_id != COINS_ID &&
           !game_data.items[item_id].special;
}

static void ge_send(mudclient *mud, const char *command) {
    char buffer[64] = {0};
    snprintf(buffer, sizeof(buffer), "%s", command);
    mudclient_send_command_string(mud, buffer);
}

static void ge_short_amount(int amount, char *dest) {
    format_amount_suffix(amount < 0 ? 0 : amount, 0, 1, 0, dest);
}

static void ge_reset_offer(mudclient *mud) {
    mud->ge_mode = GE_MODE_IDLE;
    mud->ge_selected_slot = -1;
    mud->ge_side = GE_SLOT_EMPTY;
    mud->ge_item_id = -1;
    mud->ge_qty = 0;
    mud->ge_price = 0;
    mud->ge_price_set = 0;
    mud->ge_entry_field = GE_FIELD_NONE;
    mud->ge_search_enter = 0;
    memset(mud->ge_search, '\0', GE_SEARCH_LENGTH + 1);
}

void mudclient_exchange_close(mudclient *mud, int send_close) {
    if (send_close) {
        ge_send(mud, "ge close");
    }

    if (mud->show_dialog_ge && mud->ge_entry_field != GE_FIELD_NONE) {
        mud->show_dialog_offer_x = 0;
        mud->input_digits_final = 0;
    }

    mud->show_dialog_ge = 0;
    ge_reset_offer(mud);
}

/* 140: byte flags, int coins, byte slots, per slot (byte state, short item,
 * int qty, int filled, int price), byte collect, per entry (short item,
 * int amount) */
void mudclient_exchange_read_state(mudclient *mud, int8_t *data, int size) {
    int offset = 1;

    if (!ge_fits(offset, 6, size)) {
        return;
    }

    int flags = get_unsigned_byte(data, offset, size);
    offset += 1;

    int opens = flags & GE_FLAG_OPEN;

    /* without the open flag it is only an update for an open panel */
    if (!opens && !mud->show_dialog_ge) {
        return;
    }

    int coins = get_unsigned_int(data, offset, size);
    offset += 4;

    int slot_count = get_unsigned_byte(data, offset, size);
    offset += 1;

    int states[GE_SLOTS_MAX] = {0};
    int items[GE_SLOTS_MAX] = {0};
    int qtys[GE_SLOTS_MAX] = {0};
    int filleds[GE_SLOTS_MAX] = {0};
    int prices[GE_SLOTS_MAX] = {0};

    for (int i = 0; i < slot_count; i++) {
        if (!ge_fits(offset, 15, size)) {
            return;
        }

        if (i < GE_SLOTS_MAX) {
            states[i] = get_unsigned_byte(data, offset, size);
            items[i] = get_unsigned_short(data, offset + 1, size);
            qtys[i] = get_unsigned_int(data, offset + 3, size);
            filleds[i] = get_unsigned_int(data, offset + 7, size);
            prices[i] = get_unsigned_int(data, offset + 11, size);

            if ((states[i] != GE_SLOT_BUY && states[i] != GE_SLOT_SELL) ||
                !ge_valid_item(items[i])) {
                states[i] = GE_SLOT_EMPTY;
            }

            if (qtys[i] < 0) {
                qtys[i] = 0;
            }

            if (filleds[i] < 0) {
                filleds[i] = 0;
            }

            if (prices[i] < 0) {
                prices[i] = 0;
            }
        }

        offset += 15;
    }

    int collect_count = 0;
    int collect_items[GE_COLLECT_MAX] = {0};
    int collect_amounts[GE_COLLECT_MAX] = {0};

    if (ge_fits(offset, 1, size)) {
        int count = get_unsigned_byte(data, offset, size);
        offset += 1;

        for (int i = 0; i < count && collect_count < GE_COLLECT_MAX; i++) {
            if (!ge_fits(offset, 6, size)) {
                break;
            }

            int item_id = get_unsigned_short(data, offset, size);
            int amount = get_unsigned_int(data, offset + 2, size);
            offset += 6;

            if (!ge_valid_item(item_id) || amount <= 0) {
                continue;
            }

            collect_items[collect_count] = item_id;
            collect_amounts[collect_count] = amount;
            collect_count++;
        }
    }

    mud->ge_flags = flags;
    mud->ge_coins = coins < 0 ? 0 : coins;

    for (int i = 0; i < GE_SLOTS_MAX; i++) {
        int empty = states[i] == GE_SLOT_EMPTY;

        mud->ge_slot_state[i] = states[i];
        mud->ge_slot_item[i] = empty ? -1 : items[i];
        mud->ge_slot_qty[i] = empty ? 0 : qtys[i];
        mud->ge_slot_filled[i] = empty ? 0 : filleds[i];
        mud->ge_slot_price[i] = empty ? 0 : prices[i];
    }

    mud->ge_collect_count = collect_count;

    for (int i = 0; i < GE_COLLECT_MAX; i++) {
        mud->ge_collect_item[i] = i < collect_count ? collect_items[i] : -1;
        mud->ge_collect_amount[i] = i < collect_count ? collect_amounts[i] : 0;
    }

    if (opens && !mud->show_dialog_ge) {
        ge_reset_offer(mud);
        mud->ge_quote_item = -1;
        mud->show_dialog_ge = 1;
        mud->show_option_menu = 0;
    }

    /* the slot the cancel prompt is about was emptied meanwhile */
    if (mud->ge_mode == GE_MODE_CANCEL &&
        (mud->ge_selected_slot < 0 || mud->ge_selected_slot >= GE_SLOTS_MAX ||
         mud->ge_slot_state[mud->ge_selected_slot] == GE_SLOT_EMPTY)) {
        mud->ge_mode = GE_MODE_IDLE;
        mud->ge_selected_slot = -1;
    }
}

/* 141: short item, int guide, int bid, int ask, int vol24, int limit left */
void mudclient_exchange_read_quote(mudclient *mud, int8_t *data, int size) {
    int offset = 1;

    if (!ge_fits(offset, 22, size)) {
        return;
    }

    int item_id = get_unsigned_short(data, offset, size);

    if (!ge_valid_item(item_id)) {
        return;
    }

    mud->ge_quote_item = item_id;
    mud->ge_quote_guide = get_unsigned_int(data, offset + 2, size);
    mud->ge_quote_bid = get_unsigned_int(data, offset + 6, size);
    mud->ge_quote_ask = get_unsigned_int(data, offset + 10, size);
    mud->ge_quote_vol24 = get_unsigned_int(data, offset + 14, size);
    mud->ge_quote_limit_left = get_unsigned_int(data, offset + 18, size);

    if (mud->ge_quote_guide < 0) {
        mud->ge_quote_guide = 0;
    }

    if (mud->ge_quote_bid < 0) {
        mud->ge_quote_bid = 0;
    }

    if (mud->ge_quote_ask < 0) {
        mud->ge_quote_ask = 0;
    }

    if (mud->ge_quote_vol24 < 0) {
        mud->ge_quote_vol24 = 0;
    }

    if (mud->ge_quote_limit_left < 0) {
        mud->ge_quote_limit_left = 0;
    }

    /* the price box starts at the guide until the player sets it */
    if (mud->show_dialog_ge && mud->ge_mode == GE_MODE_OFFER &&
        mud->ge_item_id == item_id && !mud->ge_price_set &&
        mud->ge_quote_guide > 0) {
        mud->ge_price = mud->ge_quote_guide;
    }
}

int mudclient_exchange_wants_text(mudclient *mud) {
    return mud->show_dialog_ge && mud->ge_mode == GE_MODE_PICK_BUY &&
           !mud->show_dialog_offer_x;
}

void mudclient_exchange_type_char(mudclient *mud, int char_code) {
    size_t length = strlen(mud->ge_search);

    if (length < GE_SEARCH_LENGTH && char_code >= 32 && char_code < 127) {
        mud->ge_search[length] = (char)char_code;
        mud->ge_search[length + 1] = '\0';
    }
}

void mudclient_exchange_backspace(mudclient *mud) {
    size_t length = strlen(mud->ge_search);

    if (length > 0) {
        mud->ge_search[length - 1] = '\0';
    }
}

void mudclient_exchange_enter(mudclient *mud) { mud->ge_search_enter = 1; }

static int ge_contains_ignore_case(const char *haystack, const char *needle) {
    size_t needle_length = strlen(needle);

    if (needle_length == 0) {
        return 0;
    }

    for (const char *start = haystack; *start != '\0'; start++) {
        size_t i = 0;

        while (i < needle_length && start[i] != '\0' &&
               tolower((unsigned char)start[i]) ==
                   tolower((unsigned char)needle[i])) {
            i++;
        }

        if (i == needle_length) {
            return 1;
        }
    }

    return 0;
}

/* up to GE_MATCHES_MAX tradeable items whose name holds the search text;
 * names that start with it come first */
static int ge_find_matches(mudclient *mud, int *matches) {
    int count = 0;

    if (strlen(mud->ge_search) == 0) {
        return 0;
    }

    for (int pass = 0; pass < 2 && count < GE_MATCHES_MAX; pass++) {
        for (int id = 0; id < game_data.item_count && count < GE_MATCHES_MAX;
             id++) {
            if (!ge_tradeable(id) || game_data.items[id].name == NULL) {
                continue;
            }

            const char *name = game_data.items[id].name;
            size_t search_length = strlen(mud->ge_search);

            int prefix = strncasecmp(name, mud->ge_search, search_length) == 0;

            if (pass == 0 ? !prefix
                          : (prefix ||
                             !ge_contains_ignore_case(name, mud->ge_search))) {
                continue;
            }

            int duplicate = 0;

            for (int i = 0; i < count; i++) {
                if (strcmp(ge_item_name(matches[i]), name) == 0) {
                    duplicate = 1;
                    break;
                }
            }

            if (!duplicate) {
                matches[count++] = id;
            }
        }
    }

    return count;
}

static int ge_hover(mudclient *mud, int x, int y, int width, int height) {
    return !mud->show_dialog_offer_x && mud->mouse_x >= x &&
           mud->mouse_x < x + width && mud->mouse_y >= y &&
           mud->mouse_y < y + height;
}

static int ge_clicked(mudclient *mud, int x, int y, int width, int height) {
    if (mud->mouse_button_click != 0 && ge_hover(mud, x, y, width, height)) {
        mud->mouse_button_click = 0;
        return 1;
    }

    return 0;
}

static int ge_button(mudclient *mud, int x, int y, int width, int height,
                     const char *label, int enabled) {
    int hover = enabled && ge_hover(mud, x, y, width, height);

    surface_draw_box_alpha(mud->surface, x, y, width, height,
                           hover ? GREY_B5 : BLACK, hover ? 192 : 128);

    surface_draw_border(mud->surface, x, y, width, height,
                        enabled ? GREY_D0 : GREY_98);

    surface_draw_string_centre(mud->surface, label, x + width / 2,
                               y + height / 2 + 5, FONT_BOLD_12,
                               !enabled ? GREY_98 : (hover ? YELLOW : WHITE));

    return enabled && ge_clicked(mud, x, y, width, height);
}

static void ge_open_number_entry(mudclient *mud, int field) {
    mud->ge_entry_field = field;
    memset(mud->input_digits_current, '\0', INPUT_DIGITS_LENGTH + 1);
    mud->input_digits_final = 0;
    mud->show_dialog_offer_x = 1;
}

static void ge_pick_item(mudclient *mud, int item_id, int side) {
    mud->ge_mode = GE_MODE_OFFER;
    mud->ge_side = side;
    mud->ge_item_id = item_id;
    mud->ge_price_set = 0;
    mud->ge_price = 0;

    if (side == GE_SLOT_SELL) {
        int owned = mudclient_get_inventory_count(mud, item_id);
        mud->ge_qty = owned > 0 ? owned : 1;
    } else {
        mud->ge_qty = 1;
    }

    /* a quote already in hand starts the price box */
    if (mud->ge_quote_item == item_id && mud->ge_quote_guide > 0) {
        mud->ge_price = mud->ge_quote_guide;
    }

    char command[32] = {0};
    snprintf(command, sizeof(command), "ge price %d", item_id);
    ge_send(mud, command);
}

static void ge_format_quote(mudclient *mud, char *dest, size_t dest_length) {
    char guide[16] = {0};
    char bid[16] = {0};
    char ask[16] = {0};
    char vol[16] = {0};

    mudclient_format_number_commas(mud, mud->ge_quote_guide, guide);

    if (mud->ge_quote_bid > 0) {
        mudclient_format_number_commas(mud, mud->ge_quote_bid, bid);
    } else {
        strcpy(bid, "-");
    }

    if (mud->ge_quote_ask > 0) {
        mudclient_format_number_commas(mud, mud->ge_quote_ask, ask);
    } else {
        strcpy(ask, "-");
    }

    mudclient_format_number_commas(mud, mud->ge_quote_vol24, vol);

    snprintf(dest, dest_length, "Guide %s  Bid %s  Ask %s  24h %s", guide, bid,
             ask, vol);
}

static void ge_draw_slots(mudclient *mud, int x, int y, int can_act,
                          int *hovered_slot) {
    for (int i = 0; i < GE_SLOTS_MAX; i++) {
        int column = i % GE_SLOT_COLUMNS;
        int row = i / GE_SLOT_COLUMNS;
        int slot_x = x + 4 + column * GE_SLOT_WIDTH;
        int slot_y = y + row * GE_SLOT_HEIGHT;
        int box_width = GE_SLOT_WIDTH - 2;
        int box_height = GE_SLOT_HEIGHT - 2;
        int state = mud->ge_slot_state[i];
        int hover = ge_hover(mud, slot_x, slot_y, box_width, box_height);

        if (hover) {
            *hovered_slot = i;
        }

        surface_draw_box_alpha(mud->surface, slot_x, slot_y, box_width,
                               box_height, hover ? GREY_B5 : GREY_D0, 160);

        surface_draw_border(mud->surface, slot_x, slot_y, box_width, box_height,
                            mud->ge_selected_slot == i ? RED : BLACK);

        if (state == GE_SLOT_EMPTY) {
            surface_draw_stringf(mud->surface, slot_x + 3, slot_y + 11,
                                 FONT_REGULAR_11, BLACK, "%d", i + 1);

            surface_draw_string_centre(mud->surface, "Empty",
                                       slot_x + box_width / 2,
                                       slot_y + box_height / 2 + 5,
                                       FONT_BOLD_12, GREY_98);
        } else {
            int item_id = mud->ge_slot_item[i];

            if (ge_valid_item(item_id)) {
                mudclient_draw_item(mud, slot_x + 1, slot_y + 3,
                                    ITEM_GRID_SLOT_WIDTH, ITEM_GRID_SLOT_HEIGHT,
                                    item_id);
            }

            int is_buy = state == GE_SLOT_BUY;

            surface_draw_string(mud->surface, is_buy ? "Buy" : "Sell",
                                slot_x + 51, slot_y + 11, FONT_BOLD_12,
                                is_buy ? GREEN : ORANGE);

            char price[16] = {0};
            ge_short_amount(mud->ge_slot_price[i], price);

            surface_draw_stringf(mud->surface, slot_x + 51, slot_y + 23,
                                 FONT_REGULAR_11, YELLOW, "%sgp", price);

            int qty = mud->ge_slot_qty[i];
            int filled = mud->ge_slot_filled[i];
            int bar_x = slot_x + 50;
            int bar_y = slot_y + 26;
            int bar_width = box_width - 52;
            int bar_height = 10;
            int fill_width =
                qty <= 0 ? 0
                         : (int)(((int64_t)(filled > qty ? qty : filled) *
                                  bar_width) /
                                 qty);

            surface_draw_box(mud->surface, bar_x, bar_y, bar_width, bar_height,
                             BLACK);

            if (fill_width > 0) {
                surface_draw_box(mud->surface, bar_x, bar_y, fill_width,
                                 bar_height, filled >= qty ? GREEN : 0x2a8a2a);
            }

            char formatted_filled[16] = {0};
            char formatted_qty[16] = {0};
            ge_short_amount(filled, formatted_filled);
            ge_short_amount(qty, formatted_qty);

            surface_draw_stringf_centre(mud->surface, bar_x + bar_width / 2,
                                        bar_y + 9, FONT_REGULAR_11, WHITE,
                                        "%s/%s", formatted_filled,
                                        formatted_qty);

            surface_draw_stringf(mud->surface, slot_x + 2, slot_y + 11,
                                 FONT_REGULAR_11, BLACK, "%d", i + 1);
        }

        if (can_act && ge_clicked(mud, slot_x, slot_y, box_width, box_height)) {
            mud->ge_selected_slot = i;
            mud->ge_mode =
                state == GE_SLOT_EMPTY ? GE_MODE_SIDE : GE_MODE_CANCEL;
        }
    }
}

static void ge_draw_collection(mudclient *mud, int x, int y, int width,
                               int can_act) {
    surface_draw_string(mud->surface, "Collection box", x + 4, y + 11,
                        FONT_BOLD_12, WHITE);

    int has_items = mud->ge_collect_count > 0;

    if (ge_button(mud, x + width - 254, y, 70, 14, "Collect",
                  has_items && can_act)) {
        ge_send(mud, "ge collect");
    }

    if (ge_button(mud, x + width - 180, y, 110, 14, "Collect to bank",
                  has_items && can_act)) {
        ge_send(mud, "ge collect bank");
    }

    if (ge_button(mud, x + width - 66, y, 62, 14, "Close", 1)) {
        mudclient_exchange_close(mud, 1);
        return;
    }

    int grid_x = x + (width - GE_COLLECT_COLUMNS * GE_COLLECT_SLOT_WIDTH) / 2;
    int grid_y = y + 16;

    for (int i = 0; i < GE_COLLECT_MAX; i++) {
        int slot_x = grid_x + (i % GE_COLLECT_COLUMNS) * GE_COLLECT_SLOT_WIDTH;
        int slot_y = grid_y + (i / GE_COLLECT_COLUMNS) * GE_COLLECT_SLOT_HEIGHT;

        surface_draw_box_alpha(mud->surface, slot_x, slot_y,
                               GE_COLLECT_SLOT_WIDTH, GE_COLLECT_SLOT_HEIGHT,
                               GREY_D0, 160);

        surface_draw_border(mud->surface, slot_x, slot_y,
                            GE_COLLECT_SLOT_WIDTH, GE_COLLECT_SLOT_HEIGHT,
                            BLACK);

        if (i >= mud->ge_collect_count) {
            continue;
        }

        int item_id = mud->ge_collect_item[i];

        mudclient_draw_item(mud, slot_x + 6, slot_y + 1, 38,
                            GE_COLLECT_SLOT_HEIGHT, item_id);

        char amount[16] = {0};
        mudclient_format_item_amount(mud, mud->ge_collect_amount[i], amount);

        surface_draw_string(mud->surface, amount, slot_x + 2, slot_y + 10,
                            FONT_REGULAR_11, YELLOW);

        if (ge_hover(mud, slot_x, slot_y, GE_COLLECT_SLOT_WIDTH,
                     GE_COLLECT_SLOT_HEIGHT)) {
            char formatted[16] = {0};
            mudclient_format_number_commas(mud, mud->ge_collect_amount[i],
                                           formatted);

            surface_draw_stringf_right(mud->surface, x + width - 4,
                                       grid_y + GE_COLLECT_SLOT_HEIGHT * 2 + 10,
                                       FONT_REGULAR_11, WHITE, "%s x %s",
                                       formatted, ge_item_name(item_id));
        }
    }

    if (!has_items) {
        surface_draw_string_centre(mud->surface, "Nothing waiting",
                                   x + width / 2,
                                   grid_y + GE_COLLECT_SLOT_HEIGHT + 5,
                                   FONT_BOLD_12, GREY_98);
    }
}

static void ge_draw_slot_detail(mudclient *mud, int slot, int x, int y) {
    if (slot < 0 || slot >= GE_SLOTS_MAX ||
        mud->ge_slot_state[slot] == GE_SLOT_EMPTY) {
        return;
    }

    char filled[16] = {0};
    char qty[16] = {0};
    char price[16] = {0};

    mudclient_format_number_commas(mud, mud->ge_slot_filled[slot], filled);
    mudclient_format_number_commas(mud, mud->ge_slot_qty[slot], qty);
    mudclient_format_number_commas(mud, mud->ge_slot_price[slot], price);

    surface_draw_stringf_centre(
        mud->surface, x, y, FONT_BOLD_12, WHITE, "Slot %d: %s %s, %s/%s at %sgp",
        slot + 1, mud->ge_slot_state[slot] == GE_SLOT_BUY ? "Buy" : "Sell",
        ge_item_name(mud->ge_slot_item[slot]), filled, qty, price);
}

static void ge_draw_idle(mudclient *mud, int x, int y, int width,
                         int hovered_slot) {
    int centre_x = x + width / 2;

    surface_draw_string_centre(
        mud->surface, "Click an empty slot to buy or sell,", centre_x, y + 16,
        FONT_BOLD_12, YELLOW);

    surface_draw_string_centre(mud->surface, "or a slot with an offer to cancel it.",
                               centre_x, y + 30, FONT_BOLD_12, YELLOW);

    ge_draw_slot_detail(mud, hovered_slot, centre_x, y + 52);

    if (ge_valid_item(mud->ge_quote_item)) {
        char quote[96] = {0};
        ge_format_quote(mud, quote, sizeof(quote));

        surface_draw_stringf_centre(mud->surface, centre_x, y + 72,
                                    FONT_BOLD_12, CYAN, "%s: %s",
                                    ge_item_name(mud->ge_quote_item), quote);
    }
}

static void ge_draw_side(mudclient *mud, int x, int y, int width,
                         int can_trade) {
    int centre_x = x + width / 2;

    surface_draw_stringf_centre(mud->surface, centre_x, y + 18, FONT_BOLD_13,
                                YELLOW, "Slot %d is empty. Make an offer:",
                                mud->ge_selected_slot + 1);

    if (ge_button(mud, centre_x - 160, y + 36, 100, 22, "Buy", can_trade)) {
        ge_reset_offer(mud);
        mud->ge_mode = GE_MODE_PICK_BUY;
        mud->ge_side = GE_SLOT_BUY;
    } else if (ge_button(mud, centre_x - 50, y + 36, 100, 22, "Sell",
                         can_trade)) {
        ge_reset_offer(mud);
        mud->ge_mode = GE_MODE_PICK_SELL;
        mud->ge_side = GE_SLOT_SELL;
    } else if (ge_button(mud, centre_x + 60, y + 36, 100, 22, "Back", 1)) {
        ge_reset_offer(mud);
    }
}

static void ge_draw_cancel(mudclient *mud, int x, int y, int width) {
    int centre_x = x + width / 2;
    int slot = mud->ge_selected_slot;

    if (slot < 0 || slot >= GE_SLOTS_MAX ||
        mud->ge_slot_state[slot] == GE_SLOT_EMPTY) {
        ge_reset_offer(mud);
        return;
    }

    ge_draw_slot_detail(mud, slot, centre_x, y + 18);

    surface_draw_string_centre(
        mud->surface, "What is left goes to the collection box.", centre_x,
        y + 32, FONT_REGULAR_11, GREY_D0);

    if (ge_button(mud, centre_x - 115, y + 44, 110, 22, "Cancel offer", 1)) {
        char command[24] = {0};
        snprintf(command, sizeof(command), "ge cancel %d", slot + 1);
        ge_send(mud, command);
        ge_reset_offer(mud);
    } else if (ge_button(mud, centre_x + 5, y + 44, 110, 22, "Back", 1)) {
        ge_reset_offer(mud);
    }
}

static void ge_draw_pick_sell(mudclient *mud, int x, int y, int width,
                              int can_act) {
    surface_draw_string(mud->surface, "Choose an item from your inventory to sell",
                        x + 6, y + 12, FONT_BOLD_12, YELLOW);

    if (ge_button(mud, x + width - 66, y, 62, 14, "Back", 1)) {
        ge_reset_offer(mud);
        return;
    }

    int columns = 10;
    int slot_width = 39;
    int slot_height = 27;
    int grid_x = x + (width - columns * slot_width) / 2;
    int grid_y = y + 17;

    for (int i = 0; i < INVENTORY_ITEMS_MAX; i++) {
        int slot_x = grid_x + (i % columns) * slot_width;
        int slot_y = grid_y + (i / columns) * slot_height;
        int has_item = i < mud->inventory_items_count;
        int item_id = has_item ? mud->inventory_item_id[i] : -1;
        int tradeable = has_item && ge_tradeable(item_id);
        int hover = has_item && ge_hover(mud, slot_x, slot_y, slot_width,
                                          slot_height);

        surface_draw_box_alpha(mud->surface, slot_x, slot_y, slot_width,
                               slot_height, hover ? GREY_B5 : GREY_D0, 160);

        surface_draw_border(mud->surface, slot_x, slot_y, slot_width,
                            slot_height, BLACK);

        if (!has_item || !ge_valid_item(item_id)) {
            continue;
        }

        mudclient_draw_item(mud, slot_x, slot_y, slot_width, slot_height,
                            item_id);

        if (game_data.items[item_id].stackable == 0) {
            char amount[16] = {0};
            mudclient_format_item_amount(
                mud, mud->inventory_item_stack_count[i], amount);
            surface_draw_string(mud->surface, amount, slot_x + 1, slot_y + 10,
                                FONT_REGULAR_11, YELLOW);
        }

        if (!tradeable) {
            surface_draw_box_alpha(mud->surface, slot_x, slot_y, slot_width,
                                   slot_height, BLACK, 128);
        }

        if (hover) {
            surface_draw_string_right(mud->surface, ge_item_name(item_id),
                                      x + width - 70, y + 12, FONT_BOLD_12,
                                      tradeable ? WHITE : GREY_98);
        }

        if (can_act &&
            ge_clicked(mud, slot_x, slot_y, slot_width, slot_height)) {
            if (tradeable) {
                ge_pick_item(mud, item_id, GE_SLOT_SELL);
                return;
            }

            mudclient_show_message(mud,
                                   "That item cannot be traded on the Exchange.",
                                   MESSAGE_TYPE_GAME);
        }
    }
}

static void ge_draw_pick_buy(mudclient *mud, int x, int y, int width,
                             int can_act) {
    int is_touch = mudclient_is_touch(mud);

    surface_draw_stringf(mud->surface, x + 6, y + 12, FONT_BOLD_12, WHITE,
                         "Search: %s*", mud->ge_search);

    if (is_touch && ge_clicked(mud, x, y, width - 70, 16)) {
        mudclient_trigger_keyboard(mud, mud->ge_search, 0, x + 50, y,
                                   width - 120, 16, FONT_BOLD_12, 0);
    }

    if (ge_button(mud, x + width - 66, y, 62, 14, "Back", 1)) {
        ge_reset_offer(mud);
        return;
    }

    int matches[GE_MATCHES_MAX] = {0};
    int match_count = ge_find_matches(mud, matches);

    if (mud->ge_search_enter) {
        mud->ge_search_enter = 0;

        if (match_count > 0 && can_act) {
            ge_pick_item(mud, matches[0], GE_SLOT_BUY);
            return;
        }
    }

    if (strlen(mud->ge_search) == 0) {
        surface_draw_string_centre(mud->surface,
                                   "Type part of an item name to buy",
                                   x + width / 2, y + 50, FONT_BOLD_12,
                                   YELLOW);
        return;
    }

    if (match_count == 0) {
        surface_draw_string_centre(mud->surface, "No tradeable item matches",
                                   x + width / 2, y + 50, FONT_BOLD_12,
                                   YELLOW);
        return;
    }

    int cell_width = (width - 8) / 2;
    int cell_height = 18;

    for (int i = 0; i < match_count; i++) {
        int cell_x = x + 4 + (i % 2) * cell_width;
        int cell_y = y + 18 + (i / 2) * cell_height;
        int item_id = matches[i];
        int hover = ge_hover(mud, cell_x, cell_y, cell_width - 2,
                             cell_height - 2);

        surface_draw_box_alpha(mud->surface, cell_x, cell_y, cell_width - 2,
                               cell_height - 2, hover ? GREY_B5 : BLACK,
                               hover ? 192 : 96);

        mudclient_draw_item(mud, cell_x + 1, cell_y, 24, cell_height - 1,
                            item_id);

        surface_draw_string(mud->surface, ge_item_name(item_id), cell_x + 28,
                            cell_y + 12, FONT_BOLD_12,
                            hover ? YELLOW : WHITE);

        if (can_act &&
            ge_clicked(mud, cell_x, cell_y, cell_width - 2, cell_height - 2)) {
            ge_pick_item(mud, item_id, GE_SLOT_BUY);
            return;
        }
    }
}

static void ge_confirm(mudclient *mud) {
    int item_id = mud->ge_item_id;
    int is_buy = mud->ge_side == GE_SLOT_BUY;

    if (mud->ge_qty <= 0 || mud->ge_price <= 0) {
        mudclient_show_message(mud, "Set a quantity and a price first.",
                               MESSAGE_TYPE_GAME);
        return;
    }

    if (is_buy) {
        int64_t total = (int64_t)mud->ge_qty * (int64_t)mud->ge_price;

        if (total > mudclient_get_inventory_count(mud, COINS_ID)) {
            mudclient_show_message(mud, "You don't have enough coins.",
                                   MESSAGE_TYPE_GAME);
            return;
        }
    } else if (mud->ge_qty > mudclient_get_inventory_count(mud, item_id)) {
        mudclient_show_message(mud, "You don't have that many to sell.",
                               MESSAGE_TYPE_GAME);
        return;
    }

    char command[64] = {0};
    snprintf(command, sizeof(command), "ge %s %d %d %d",
             is_buy ? "buy" : "sell", item_id, mud->ge_qty, mud->ge_price);
    ge_send(mud, command);
    ge_reset_offer(mud);
}

static void ge_draw_offer(mudclient *mud, int x, int y, int width,
                          int can_trade) {
    int item_id = mud->ge_item_id;
    int is_buy = mud->ge_side == GE_SLOT_BUY;

    if (!ge_valid_item(item_id)) {
        ge_reset_offer(mud);
        return;
    }

    mudclient_draw_item(mud, x + 6, y + 2, ITEM_GRID_SLOT_WIDTH,
                        ITEM_GRID_SLOT_HEIGHT, item_id);

    surface_draw_stringf(mud->surface, x + 60, y + 14, FONT_BOLD_13,
                         is_buy ? GREEN : ORANGE, "%s: %s",
                         is_buy ? "Buy" : "Sell", ge_item_name(item_id));

    if (mud->ge_quote_item == item_id) {
        char quote[96] = {0};
        ge_format_quote(mud, quote, sizeof(quote));

        surface_draw_string(mud->surface, quote, x + 60, y + 28, FONT_BOLD_12,
                            CYAN);

        if (is_buy) {
            char limit[16] = {0};
            mudclient_format_number_commas(mud, mud->ge_quote_limit_left,
                                           limit);
            surface_draw_stringf_right(mud->surface, x + width - 6, y + 14,
                                       FONT_REGULAR_11, GREY_D0,
                                       "Buy limit left: %s", limit);
        }
    } else {
        surface_draw_string(mud->surface, "Asking the Exchange for a price...",
                            x + 60, y + 28, FONT_BOLD_12, GREY_D0);
    }

    char formatted[16] = {0};

    /* quantity */
    mudclient_format_number_commas(mud, mud->ge_qty, formatted);
    surface_draw_stringf(mud->surface, x + 6, y + 50, FONT_BOLD_12, WHITE,
                         "Quantity: %s", formatted);

    if (ge_button(mud, x + 170, y + 39, 50, 14, "Set", 1)) {
        ge_open_number_entry(mud, GE_FIELD_QTY);
    }

    if (!is_buy) {
        int owned = mudclient_get_inventory_count(mud, item_id);

        if (ge_button(mud, x + 224, y + 39, 50, 14, "All", owned > 0)) {
            mud->ge_qty = owned;
        }
    }

    /* price */
    mudclient_format_number_commas(mud, mud->ge_price, formatted);
    surface_draw_stringf(mud->surface, x + 6, y + 68, FONT_BOLD_12, WHITE,
                         "Price each: %sgp", formatted);

    if (ge_button(mud, x + 170, y + 57, 50, 14, "Set", 1)) {
        ge_open_number_entry(mud, GE_FIELD_PRICE);
    }

    int has_guide = mud->ge_quote_item == item_id && mud->ge_quote_guide > 0;

    if (ge_button(mud, x + 224, y + 57, 50, 14, "Guide", has_guide)) {
        mud->ge_price = mud->ge_quote_guide;
        mud->ge_price_set = 1;
    }

    if (ge_button(mud, x + 278, y + 57, 50, 14, "-5%", mud->ge_price > 1)) {
        int price = (int)(((int64_t)mud->ge_price * 95) / 100);

        if (price >= mud->ge_price) {
            price = mud->ge_price - 1;
        }

        mud->ge_price = price < 1 ? 1 : price;
        mud->ge_price_set = 1;
    }

    if (ge_button(mud, x + 332, y + 57, 50, 14, "+5%", mud->ge_price > 0)) {
        int64_t price = ((int64_t)mud->ge_price * 105) / 100;

        if (price <= mud->ge_price) {
            price = (int64_t)mud->ge_price + 1;
        }

        mud->ge_price = price > INT32_MAX ? INT32_MAX : (int)price;
        mud->ge_price_set = 1;
    }

    /* total and confirm */
    int64_t total = (int64_t)mud->ge_qty * (int64_t)mud->ge_price;
    char formatted_total[24] = {0};

    if (total > INT32_MAX) {
        snprintf(formatted_total, sizeof(formatted_total), "%lld",
                 (long long)total);
    } else {
        mudclient_format_number_commas(mud, (int)total, formatted_total);
    }

    surface_draw_stringf(mud->surface, x + 6, y + 88, FONT_BOLD_12, YELLOW,
                         "Total: %sgp", formatted_total);

    if (ge_button(mud, x + width - 152, y + 77, 72, 16, "Confirm",
                  can_trade && mud->ge_qty > 0 && mud->ge_price > 0)) {
        ge_confirm(mud);
    } else if (ge_button(mud, x + width - 76, y + 77, 72, 16, "Back", 1)) {
        ge_reset_offer(mud);
    }
}

void mudclient_draw_exchange(mudclient *mud) {
    /* a number typed into the offer-x box */
    if (mud->show_dialog_offer_x && mud->ge_entry_field != GE_FIELD_NONE &&
        mud->input_digits_final > 0) {
        if (mud->ge_entry_field == GE_FIELD_QTY) {
            mud->ge_qty = mud->input_digits_final;
        } else if (mud->ge_entry_field == GE_FIELD_PRICE) {
            mud->ge_price = mud->input_digits_final;
            mud->ge_price_set = 1;
        }

        mud->ge_entry_field = GE_FIELD_NONE;
        mud->show_dialog_offer_x = 0;
        mud->input_digits_final = 0;
    }

    if (!mud->show_dialog_offer_x) {
        mud->ge_entry_field = GE_FIELD_NONE;
    }

    int width = GE_WIDTH;
    int slots_height = (GE_SLOTS_MAX / GE_SLOT_COLUMNS) * GE_SLOT_HEIGHT;
    int collect_height = 16 + 2 * GE_COLLECT_SLOT_HEIGHT + 14;
    int height = 12 + 17 + slots_height + collect_height + GE_MODE_HEIGHT + 4;

    int x = (mud->surface->width / 2) - (width / 2);
    int y = (mud->surface->height / 2) - (height / 2) - 6;

    if (x < 0) {
        x = 0;
    }

    if (y < 0) {
        y = 0;
    }

    int barred = mud->ge_flags & GE_FLAG_BARRED;
    int in_range = mud->ge_flags & GE_FLAG_IN_RANGE;
    int can_act = !mud->show_dialog_offer_x;
    int can_trade = can_act && !barred;

    /* title bar */
    surface_draw_box(mud->surface, x, y, width, 12, TITLE_BAR_COLOUR);

    surface_draw_box_alpha(mud->surface, x, y + 12, width, height - 12, GREY_98,
                           160);

    surface_draw_string(mud->surface, "Grand Exchange", x + 1, y + 10,
                        FONT_BOLD_12, WHITE);

    int close_hover = ge_hover(mud, x + width - 88, y, 88, 12);

    surface_draw_string_right(mud->surface, "Close window", x + width - 2,
                              y + 10, FONT_BOLD_12,
                              close_hover ? RED : WHITE);

    if (close_hover && ge_clicked(mud, x + width - 88, y, 88, 12)) {
        mudclient_exchange_close(mud, 1);
        return;
    }

    /* status line */
    char coins[16] = {0};
    mudclient_format_number_commas(
        mud, mudclient_get_inventory_count(mud, COINS_ID), coins);

    surface_draw_stringf(mud->surface, x + 4, y + 25, FONT_BOLD_12, YELLOW,
                         "Your money: %sgp", coins);

    if (barred) {
        surface_draw_string_right(mud->surface,
                                  "The Exchange does not deal with you",
                                  x + width - 4, y + 25, FONT_BOLD_12, RED);
    } else if (!in_range) {
        surface_draw_string_right(mud->surface, "No clerk in range",
                                  x + width - 4, y + 25, FONT_BOLD_12, ORANGE);
    } else {
        surface_draw_string_right(mud->surface, "Clerk in range", x + width - 4,
                                  y + 25, FONT_BOLD_12, GREEN);
    }

    /* slots */
    int slots_y = y + 30;
    int hovered_slot = -1;

    ge_draw_slots(mud, x, slots_y, can_act, &hovered_slot);

    /* collection box */
    int collect_y = slots_y + slots_height + 2;

    surface_draw_line_horizontal(mud->surface, x + 4, collect_y - 1, width - 8,
                                 BLACK);

    ge_draw_collection(mud, x, collect_y + 2, width, can_act);

    if (!mud->show_dialog_ge) {
        return;
    }

    /* the lower area follows the mode */
    int mode_y = collect_y + collect_height;

    surface_draw_line_horizontal(mud->surface, x + 4, mode_y - 1, width - 8,
                                 BLACK);

    switch (mud->ge_mode) {
    case GE_MODE_SIDE:
        ge_draw_side(mud, x, mode_y, width, can_trade);
        break;
    case GE_MODE_CANCEL:
        ge_draw_cancel(mud, x, mode_y, width);
        break;
    case GE_MODE_PICK_SELL:
        ge_draw_pick_sell(mud, x, mode_y, width, can_trade);
        break;
    case GE_MODE_PICK_BUY:
        ge_draw_pick_buy(mud, x, mode_y, width, can_trade);
        break;
    case GE_MODE_OFFER:
        ge_draw_offer(mud, x, mode_y, width, can_trade);
        break;
    default:
        ge_draw_idle(mud, x, mode_y, width, hovered_slot);
        break;
    }

    if (mud->show_dialog_offer_x) {
        mudclient_draw_offer_x(mud);
        mudclient_handle_offer_x_input(mud);
    }

    /* clicks inside the panel go nowhere else */
    if (mud->mouse_button_click != 0 && mud->mouse_x >= x &&
        mud->mouse_x < x + width && mud->mouse_y >= y &&
        mud->mouse_y < y + height) {
        mud->mouse_button_click = 0;
    }
}
