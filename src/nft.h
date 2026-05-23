#ifndef NETPARENT_NFT_H
#define NETPARENT_NFT_H

#include <stdbool.h>

/* Validate and normalize a MAC address string.
 * Accepts case-insensitive aa:bb:cc:dd:ee:ff or aa-bb-cc-dd-ee-ff.
 * Writes a normalized lowercase colon-separated form into `out` (>=18 bytes).
 * Returns true on success.
 */
bool np_mac_normalize(const char *in, char out[18]);

/* Ensure the nftables table/set/chain exist. Idempotent.
 * `table` is the full table identifier, e.g. "inet netparent".
 */
int np_nft_ensure(const char *table, const char *set);

/* Add a MAC to the blocked set. Idempotent. */
int np_nft_block(const char *table, const char *set, const char *mac);

/* Remove a MAC from the blocked set. Idempotent. */
int np_nft_unblock(const char *table, const char *set, const char *mac);

/* Return whether a MAC is currently blocked.
 *   1 = blocked, 0 = not blocked, -1 = error.
 */
int np_nft_is_blocked(const char *table, const char *set, const char *mac);

/* List currently blocked MACs.
 * Calls `cb(mac, user)` for each entry. Returns 0 on success.
 */
int np_nft_list(const char *table, const char *set,
                void (*cb)(const char *mac, void *user), void *user);

#endif
