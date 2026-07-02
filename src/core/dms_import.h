/* dms_import.h — best-effort import of an existing DankMaterialShell (DMS)
 * install's settings onto dc_config, so a user switching from DMS to dankc
 * gets a matching bar with zero manual config (docs/12-BAR-SPEC.md sec.7,
 * stage S5).
 */
#ifndef DC_CORE_DMS_IMPORT_H
#define DC_CORE_DMS_IMPORT_H

#include <stdbool.h>

#include "core/config.h"

/* Reads ~/.config/DankMaterialShell/settings.json (and, for
 * weatherCoordinates, ~/.local/state/DankMaterialShell/session.json as a
 * fallback) and applies matching fields onto `cfg`. No-op (returns false,
 * `cfg` untouched) if DMS was never installed/configured on this machine or
 * the file fails to parse.
 *
 * Precedence: call this AFTER dc_config's built-in defaults are in place but
 * BEFORE parsing dankc's own ~/.config/dankc/config.json — dankc's config.json
 * always wins for any key it actually sets (dc_config_load()'s get_*()
 * helpers only touch a field when the corresponding JSON key is present, so
 * import-then-overlay is safe). */
bool dc_dms_import(dc_config *cfg);

#endif /* DC_CORE_DMS_IMPORT_H */
