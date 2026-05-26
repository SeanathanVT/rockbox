/*
 * Innioasis Y1 — lcd-target.h
 *
 * fbdev-backed via firmware/target/hosted/lcd-linuxfb.c (shared with hiby/surfans).
 */

#ifndef __LCD_TARGET_H__
#define __LCD_TARGET_H__

/* lcd-linuxfb.c provides lcd_update / lcd_update_rect; suppress the generic
 * versions in lcd-memframe.c. */
#define LCD_OPTIMIZED_UPDATE
#define LCD_OPTIMIZED_UPDATE_RECT

extern fb_data *framebuffer; /* defined in lcd-linuxfb.c */
#define LCD_FRAMEBUF_ADDR(col, row) (framebuffer + (row) * LCD_WIDTH + (col))

/* defined in lcd-memframe.c, called by lcd-linuxfb.c's lcd_enable path — every
 * hosted lcd-target.h sharing lcd-memframe.c declares this. */
extern void lcd_set_active(bool active);

#endif /* __LCD_TARGET_H__ */
