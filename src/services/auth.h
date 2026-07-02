/* auth.h — PAM password check for the lock screen.
 *
 * Thin wrapper over PAM: verify the current user's password. Kept isolated so
 * the lock screen never touches PAM internals. See docs/03-SERVICES.
 */
#ifndef DC_SERVICES_AUTH_H
#define DC_SERVICES_AUTH_H

#include <stdbool.h>

/* True if `password` authenticates the current user via PAM. Blocking (PAM does
 * its own I/O); called on a submit, not in the render path. `password` is never
 * logged or copied beyond the PAM conversation. */
bool dc_auth_check(const char *password);

#endif /* DC_SERVICES_AUTH_H */
