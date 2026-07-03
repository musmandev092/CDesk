#include "theme/theme.h"

#include <string.h>

/* A named built-in palette, with both mode variants. The table below is
 * generated from DMS's StockThemes.js (see stock_themes.inc /
 * scripts/gen_stock_themes.py). */
typedef struct {
    const char *id;
    const char *name;
    dc_theme dark;
    dc_theme light;
} dc_stock_theme;

#include "stock_themes.inc"

#define DC_STOCK_COUNT ((int)(sizeof(dc_stock_themes) / sizeof(dc_stock_themes[0])))
#define DC_DEFAULT_THEME "green"

/* Active palette (a copy so the pointer target stays stable across switches). */
static dc_theme active_theme;
const dc_theme *dc_theme_current = &active_theme;

/* Whether dc_theme_set() resolves to the light variant. Config drives this via
 * dc_theme_set_light() before (re)applying the theme. */
static bool light_mode;

void dc_theme_set_light(bool light)
{
    light_mode = light;
}

bool dc_theme_is_light(void)
{
    return light_mode;
}

static const dc_stock_theme *find_stock(const char *id)
{
    for (int i = 0; i < DC_STOCK_COUNT; i++)
        if (strcmp(dc_stock_themes[i].id, id) == 0)
            return &dc_stock_themes[i];
    return NULL;
}

void dc_theme_init(void)
{
    dc_theme_set(DC_DEFAULT_THEME);
}

bool dc_theme_set(const char *id)
{
    const dc_stock_theme *stock = id ? find_stock(id) : NULL;
    if (!stock)
        stock = find_stock(DC_DEFAULT_THEME);
    if (!stock)
        return false;
    active_theme = light_mode ? stock->light : stock->dark;
    return id && strcmp(stock->id, id) == 0;
}

int dc_theme_count(void)
{
    return DC_STOCK_COUNT;
}

const char *dc_theme_id_at(int index)
{
    if (index < 0 || index >= DC_STOCK_COUNT)
        return NULL;
    return dc_stock_themes[index].id;
}

const char *dc_theme_name_at(int index)
{
    if (index < 0 || index >= DC_STOCK_COUNT)
        return NULL;
    return dc_stock_themes[index].name;
}

dc_color dc_theme_primary_at(int index)
{
    if (index < 0 || index >= DC_STOCK_COUNT) {
        dc_color none = {0, 0, 0, 255};
        return none;
    }
    /* Swatch reflects the mode currently in use so the settings picker matches
     * the live surfaces. */
    return light_mode ? dc_stock_themes[index].light.primary
                      : dc_stock_themes[index].dark.primary;
}

void dc_theme_set_custom(const dc_theme *theme)
{
    if (theme)
        active_theme = *theme;
}
