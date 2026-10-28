#ifndef OSD_H
#define OSD_H

#include "spi.h"
#include "u8g2.h"

#define OSD_INVISIBLE  0
#define OSD_VISIBLE    (!OSD_INVISIBLE)

extern u8g2_t u8g2;

void osd_init(void);
void osd_enable(char);
int osd_is_visible(void);
// show only the bottom 16 lines, near the bottom of the screen (e.g. volume bar).
// Doesn't count as visible; cores without strip support show the whole OSD.
void osd_strip(int on);

#endif // OSD_H
