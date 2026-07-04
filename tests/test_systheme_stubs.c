/* Link-only stubs for tests/test_systheme.c.
 *
 * tests/test_systheme.c exercises only systheme.c's PURE/file-IO helpers
 * (dc_systheme_hex_rgb/hex_argb, dc_systheme_prefers_black_text,
 * dc_systheme_write_owned, dc_systheme_ensure_line[_top]) -- it never calls
 * dc_systheme_apply() or dc_systheme_app_detected(). But systheme.c is one
 * translation unit whose dc_systheme_apply()/apply_gtk()/
 * build_gtk_colors_css() also reference every per-app emitter declared in
 * systheme_apps.h/systheme_browser.h/systheme_editors.h/
 * systheme_launchers.h/systheme_misc.h/systheme_notify.h/systheme_qtkde.h/
 * systheme_term.h/systheme_term2.h, plus dc_config_light_mode() and the
 * dc_theme_current global from theme/theme.h. Linking systheme.o directly
 * (not pulled a symbol at a time out of an archive) requires every one of
 * those to resolve, even though this test binary never actually calls into
 * dc_systheme_apply()'s code path -- same shape as
 * tests/test_text_edit_stubs.c's rationale for text_edit.c.
 *
 * Rather than link ~10 real per-app emitter .c files (each pulling in cJSON/
 * more filesystem surface) just to satisfy the linker, this file provides
 * trivial no-op stand-ins for every symbol systheme.c references but this
 * test never exercises. */
#include "core/config.h"
#include "theme/theme.h"

#include <stddef.h>

/* Zeroed placeholder palette: build_gtk_colors_css() (reachable only from
 * dc_systheme_apply(), never called by this test) reads through this
 * pointer; its contents don't matter since that path is never exercised. */
static const dc_theme g_stub_theme;
const dc_theme *dc_theme_current = &g_stub_theme;

bool dc_config_light_mode(void)
{
    return false;
}

void dc_systheme_apply_alacritty(bool light) { (void)light; }
void dc_systheme_apply_btop(bool light) { (void)light; }
void dc_systheme_apply_cava(bool light) { (void)light; }
void dc_systheme_apply_discord(bool light) { (void)light; }
void dc_systheme_apply_dunst(bool light) { (void)light; }
void dc_systheme_apply_emacs(bool light) { (void)light; }
void dc_systheme_apply_firefox(bool light) { (void)light; }
void dc_systheme_apply_foot(bool light) { (void)light; }
void dc_systheme_apply_fuzzel(bool light) { (void)light; }
void dc_systheme_apply_ghostty(bool light) { (void)light; }
void dc_systheme_apply_gtk2(bool light) { (void)light; }
void dc_systheme_apply_helix(bool light) { (void)light; }
void dc_systheme_apply_kde(bool light) { (void)light; }
void dc_systheme_apply_kitty(bool light) { (void)light; }
void dc_systheme_apply_konsole(bool light) { (void)light; }
void dc_systheme_apply_kvantum(bool light) { (void)light; }
void dc_systheme_apply_mako(bool light) { (void)light; }
void dc_systheme_apply_neovim(bool light) { (void)light; }
void dc_systheme_apply_qt(bool light) { (void)light; }
void dc_systheme_apply_qutebrowser(bool light) { (void)light; }
void dc_systheme_apply_rofi(bool light) { (void)light; }
void dc_systheme_apply_spicetify(bool light) { (void)light; }
void dc_systheme_apply_sublime(bool light) { (void)light; }
void dc_systheme_apply_swaync(bool light) { (void)light; }
void dc_systheme_apply_tofi(bool light) { (void)light; }
void dc_systheme_apply_vim(bool light) { (void)light; }
void dc_systheme_apply_vscode(bool light) { (void)light; }
void dc_systheme_apply_wezterm(bool light) { (void)light; }
void dc_systheme_apply_wofi(bool light) { (void)light; }
void dc_systheme_apply_xresources(bool light) { (void)light; }
void dc_systheme_apply_zathura(bool light) { (void)light; }
void dc_systheme_apply_zed(bool light) { (void)light; }
