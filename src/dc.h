/* dc.h — shared DankC definitions. */
#ifndef DC_DC_H
#define DC_DC_H

#define DC_VERSION "0.0.1"

#define DC_UNUSED(x) ((void)(x))
#define DC_MIN(a, b) ((a) < (b) ? (a) : (b))
#define DC_MAX(a, b) ((a) > (b) ? (a) : (b))
#define DC_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif /* DC_DC_H */
