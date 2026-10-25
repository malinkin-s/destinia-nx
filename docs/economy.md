# Economy: Special Shop priced in Gold

The original sells **Coins** (premium currency) for real money through Google Play. This port has no store and no network, so
the Special Shop sells Coins for **Gold** instead, and the items in the Special Shop are still bought with Coins.
No game data is modified on disk: the text is rewritten when the asset is read and the price table is patched in guest
memory after start-up (`economy_patch_asset`, `economy_install`, `economy_step` in `port/src/native.c`).

## What the player sees

* System tab → **[Special Shop]**: the six coin packs keep their original amounts and "% MORE" bonuses; the dollar price column
  now shows Gold (`$99.99` → `200000 G`).
* Choosing a pack spends the Gold and adds the Coins immediately ("You have purchased Coins."). Without enough Gold the same
  popup shows "Not enough Gold." and nothing is spent.
* The death-screen offer "Resurrection Scroll + 800 Coins" costs 2,000 Gold.
* Every sentence that mentioned dollars, charging or the paid "full version" was reworded (see `ECON_TEXT`).

## Numbers

All amounts are in the original tables (`CASH_RATIO`, `CASH_BONUS_RATE`, `CASH_ITEM_PRICE`); only the price column changed.
Original packs were 1,000 coins per US$1 before the volume bonus, so one tunable constant defines everything:

    ECON_GOLD_PER_COIN = 2        gold per coin of the bonus-less base amount

| Pack (coins) | Base coins | Bonus | Gold price | Gold per coin |
|-------------:|-----------:|------:|-----------:|--------------:|
| 1,000        | 1,000      | –     | 2,000      | 2.00 |
| 6,500        | 5,000      | 30 %  | 10,000     | 1.54 |
| 14,000       | 10,000     | 40 %  | 20,000     | 1.43 |
| 45,000       | 30,000     | 50 %  | 60,000     | 1.33 |
| 80,000       | 50,000     | 60 %  | 100,000    | 1.25 |
| 170,000      | 100,000    | 70 %  | 200,000    | 1.18 |
| Revive pack (800 coins + scroll) | | | 2,000 | |

Special Shop items cost 200 – 4,500 coins (`CASH_ITEM_PRICE`), i.e. about 235 – 9,000 Gold depending on the pack used.

## Why 2 Gold per coin

Gold income was read from the game code (`GameCanvas_dropItem`):

* a kill of an ordinary monster drops a gold piece with 40 % chance (another branch of the code uses 50/90 %; which monsters take it was not traced);
* amount = `(5·L + 20) · (80 … 120) / 1000` for a monster of level `L`, i.e. on average `0.5·L + 2`;
* so one kill is worth about `0.2·L + 0.8` Gold on average (L 10: 2.8, L 20: 4.8, L 30: 6.8, L 50: 10.8).

Assuming about 300 kills per hour (an estimate, not measured in play) that is about 1,400 Gold/h at level 20 and 3,200 Gold/h at level 50, before selling loot.
Regular shop gear costs `50 · (minLevel/5 + 1)` Gold (150 – 550).

With 2 Gold per coin:

* the smallest pack (2,000 Gold) is about 1.5 h of play at level 20 – a deliberate premium, not an impulse buy;
* a mid pack (14,000 coins, 20,000 Gold) is a long-term goal, which is what the bonus tiers are for: saving up is rewarded
  with up to 41 % cheaper coins;
* the cheapest item (Resurrection Scroll, 200 coins ≈ 400 Gold) stays affordable, the most expensive (4,500 coins) is a
  several-hour goal.

If gold turns out to be too tight or too loose in play, change `ECON_GOLD_PER_COIN` (and `ECON_REVIVE_GOLD`) in
`port/src/native.c`; nothing else depends on it.

## Implementation notes

* `Comm_buyCash` (the IAP request) is replaced by a hook that only records the purchase; `economy_step()` settles it one frame later
  with the guest's own `getMyGold/setMyGold/getMyCash/setMyCash/MMain_SaveData`, so the game's checksums stay valid.
* The hook tells the call sites apart by return address (`MainMenu_mKeyPressed`: Special Shop, `GameCanvas_mKeyPressed`: revive).
* The "not enough" case reuses the "purchased" popup (the only message popup that is safe to open from the pack list) with the text
  swapped in place and restored when it closes.
* Not covered: the revive offer does nothing when Gold is short (the prompt just stays), and the shop's "My Coins" line does not show Gold.
