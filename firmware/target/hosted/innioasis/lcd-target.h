/*
 * Innioasis Y1 — lcd-target.h
 *
 * fbdev-backed via firmware/target/hosted/lcd-linuxfb.c (shared with hiby/surfans).
 */

#ifndef __LCD_TARGET_H__
#define __LCD_TARGET_H__

extern fb_data *framebuffer; /* defined in lcd-linuxfb.c */
#define LCD_FRAMEBUF_ADDR(col, row) (framebuffer + (row) * LCD_WIDTH + (col))

#endif /* __LCD_TARGET_H__ */
