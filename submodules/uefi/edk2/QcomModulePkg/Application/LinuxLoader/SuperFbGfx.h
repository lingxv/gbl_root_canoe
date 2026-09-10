/*
 * Minimal graphics backend for the super-fastboot boot menu.
 *
 * The platform hands us a single GOP mode at the panel's native geometry
 * (1272x2772, 32bpp, stride == width), so the menu can be drawn 1:1 into the
 * framebuffer. Text comes from the firmware's HII font, scaled by an integer
 * factor so the menu stays readable on a high-density panel; nothing is
 * bundled, and a missing GOP or font simply keeps the caller on the console
 * text path.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_GFX_H__
#define __SUPER_FB_GFX_H__

#include <Uefi.h>

/* 0x00RRGGBB, independent of the framebuffer byte order. */
#define SFB_RGB(Red, Green, Blue) \
  ((UINT32)(((UINT32)(Red) << 16) | ((UINT32)(Green) << 8) | (UINT32)(Blue)))

/*
 * Bring up the framebuffer and the font. Returns an error - and leaves every
 * other call a no-op - when the platform has no usable GOP/HII font, so the
 * caller can fall back to printf-style console output.
 */
EFI_STATUS
SfbGfxInit (VOID);

BOOLEAN
SfbGfxReady (VOID);

UINTN
SfbGfxWidth (VOID);

UINTN
SfbGfxHeight (VOID);

/*
 * Push the off-screen surface to the display. Everything is composed in RAM
 * and handed over with one GOP Blt: only Blt reaches the surface the display
 * controller scans, so the menu never writes video memory directly.
 */
VOID
SfbGfxPresent (VOID);

VOID
SfbGfxClear (IN UINT32 Color);

VOID
SfbGfxFillRect (IN UINTN X, IN UINTN Y, IN UINTN W, IN UINTN H, IN UINT32 Color);

VOID
SfbGfxRoundRect (IN UINTN X, IN UINTN Y, IN UINTN W, IN UINTN H,
                 IN UINTN Radius, IN UINT32 Color);

/* Pixel height of one text cell at the given integer scale. */
UINTN
SfbGfxTextHeight (IN UINTN Scale);

/* Pixel width of Text at the given scale, excluding trailing spacing. */
UINTN
SfbGfxTextWidth (IN CONST CHAR16 *Text, IN UINTN Scale);

/* Draw Text with its top-left corner at (X, Y); returns the advance in pixels. */
UINTN
SfbGfxText (IN UINTN X, IN UINTN Y, IN CONST CHAR16 *Text,
            IN UINTN Scale, IN UINT32 Color);

#endif /* __SUPER_FB_GFX_H__ */
