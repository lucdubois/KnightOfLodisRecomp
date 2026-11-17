// kol_shop_buy.h — the magic shop's second screen buys a spell and equips it.
#pragma once

#include <cstdint>

namespace kol {

void shop_buy_install();
// Song 10's lost end of a tie (kol_shop_buy.cpp).
void song_tie_install();
// From the second screen's Yes: `id`, the spell or item under the Buy list's cursor,
// goes in slot `slot` of the unit `record`: a copy owned if there is one, else bought.
void shop_buy_request(std::uint32_t record, int slot, int id, bool is_item);
// The shop's "1 Air Blade for 400 Goth?" is being answered for the player.
bool shop_buy_answering();

}  // namespace kol
