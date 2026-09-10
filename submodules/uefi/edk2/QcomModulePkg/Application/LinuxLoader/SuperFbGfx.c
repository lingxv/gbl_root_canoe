/*
 * Minimal graphics backend for the super-fastboot boot menu.
 *
 * Everything is composed in an off-screen surface and handed to the display
 * with a single GOP Blt, which is the only path that reaches the surface the
 * display controller scans.
 *
 * Text comes from the firmware's own font through EFI_HII_FONT_PROTOCOL
 * StringToImage - the same call GraphicsConsoleDxe uses, so it works on
 * firmware whose GetGlyph() cannot resolve the system font. Strings are
 * rendered into a reusable scratch box and copied into the surface with an
 * integer scale, which is what makes the menu readable on a high-density
 * panel. A missing GOP or font keeps the caller on the console text path.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbGfx.h"

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiFont.h>

/* Tallest text row the scratch box accepts, in pixels. */
#define SFB_GFX_SCRATCH_ROWS  256

/*
 * The font draws the glyph cell background in the colour we ask for, so an
 * unmistakable one turns "is this pixel ink?" into an exact comparison.
 */
#define SFB_GFX_BG_RED    0xFF
#define SFB_GFX_BG_GREEN  0x00
#define SFB_GFX_BG_BLUE   0xFF

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL   *mGop;
STATIC EFI_HII_FONT_PROTOCOL          *mFont;
STATIC UINT32                         *mBack;      /* off-screen surface */
STATIC UINT32                         *mFb;        /* == mBack */
STATIC UINTN                          mWidth;
STATIC UINTN                          mHeight;
STATIC UINTN                          mStride;     /* pixels, not bytes */
STATIC BOOLEAN                        mReady;
STATIC BOOLEAN                        mPresentFailed;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *mScratch;    /* text render box */
STATIC UINTN                          mScratchHeight;
STATIC UINTN                          mLineHeight; /* system font row height */

BOOLEAN
SfbGfxReady (VOID)
{
  return mReady;
}

UINTN
SfbGfxWidth (VOID)
{
  return mWidth;
}

UINTN
SfbGfxHeight (VOID)
{
  return mHeight;
}

UINTN
SfbGfxTextHeight (IN UINTN Scale)
{
  return (Scale == 0) ? mLineHeight : mLineHeight * Scale;
}

/* ---- off-screen surface ------------------------------------------------- */

VOID
SfbGfxFillRect (IN UINTN X, IN UINTN Y, IN UINTN W, IN UINTN H, IN UINT32 Color)
{
  UINTN  Row;
  UINTN  Col;

  if (!mReady || mFb == NULL || W == 0 || H == 0) {
    return;
  }
  if (X >= mWidth || Y >= mHeight) {
    return;
  }
  if (X + W > mWidth) {
    W = mWidth - X;
  }
  if (Y + H > mHeight) {
    H = mHeight - Y;
  }

  for (Row = 0; Row < H; Row++) {
    UINT32  *Line = mFb + (Y + Row) * mStride + X;

    for (Col = 0; Col < W; Col++) {
      Line[Col] = Color;
    }
  }
}

VOID
SfbGfxClear (IN UINT32 Color)
{
  SfbGfxFillRect (0, 0, mWidth, mHeight, Color);
}

/*
 * Filled rectangle with rounded corners: the corner rows are inset by the
 * circle equation, everything between them is a full-width span.
 */
VOID
SfbGfxRoundRect (IN UINTN X, IN UINTN Y, IN UINTN W, IN UINTN H,
                 IN UINTN Radius, IN UINT32 Color)
{
  UINTN  Row;
  INTN   Limit;

  if (Radius == 0 || W < 2 * Radius || H < 2 * Radius) {
    SfbGfxFillRect (X, Y, W, H, Color);
    return;
  }

  Limit = (INTN)(Radius * Radius);
  for (Row = 0; Row < H; Row++) {
    INTN   Dy;
    UINTN  Inset = 0;

    if (Row < Radius) {
      Dy = (INTN)(Radius - 1 - Row);
    } else if (Row >= H - Radius) {
      Dy = (INTN)(Row - (H - Radius));
    } else {
      Dy = -1;
    }

    if (Dy >= 0) {
      while (Inset < Radius) {
        INTN  Dx = (INTN)(Radius - 1 - Inset);

        if (Dx * Dx + Dy * Dy <= Limit) {
          break;
        }
        Inset++;
      }
    }

    SfbGfxFillRect (X + Inset, Y + Row, W - 2 * Inset, 1, Color);
  }
}

VOID
SfbGfxPresent (VOID)
{
  EFI_STATUS  Status;

  if (!mReady || mGop == NULL || mBack == NULL) {
    return;
  }

  Status = mGop->Blt (mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mBack,
                      EfiBltBufferToVideo, 0, 0, 0, 0, mWidth, mHeight, 0);
  if (EFI_ERROR (Status) && !mPresentFailed) {
    mPresentFailed = TRUE;
    DEBUG ((EFI_D_ERROR, "SFB: gfx: Blt failed: %r\n", Status));
  }
}

/* ---- text --------------------------------------------------------------- */

/*
 * Render Text into the scratch box at (0, 0) and report the size the font
 * actually used. Returns an error when there is nothing renderable.
 */
STATIC
EFI_STATUS
SfbGfxRenderText (IN CONST CHAR16 *Text, OUT UINTN *Width, OUT UINTN *Height)
{
  EFI_FONT_DISPLAY_INFO  Info;
  EFI_IMAGE_OUTPUT       Box;
  EFI_IMAGE_OUTPUT       *Blt = &Box;
  EFI_HII_ROW_INFO       *Rows = NULL;
  UINTN                  RowCount = 0;
  EFI_STATUS             Status;

  *Width  = 0;
  *Height = 0;
  if (!mReady || mFont == NULL || mScratch == NULL || Text == NULL ||
      Text[0] == L'\0') {
    return EFI_INVALID_PARAMETER;
  }

  /*
   * A zeroed font request asks for the system font, exactly as
   * GraphicsConsoleDxe does; the colours are ours so the cell background can
   * be recognised as "not ink".
   */
  ZeroMem (&Info, sizeof (Info));
  Info.ForegroundColor.Red      = 0xFF;
  Info.ForegroundColor.Green    = 0xFF;
  Info.ForegroundColor.Blue     = 0xFF;
  Info.BackgroundColor.Red      = SFB_GFX_BG_RED;
  Info.BackgroundColor.Green    = SFB_GFX_BG_GREEN;
  Info.BackgroundColor.Blue     = SFB_GFX_BG_BLUE;
  Info.FontInfoMask             = 0;
  Info.FontInfo.FontSize        = 0;
  Info.FontInfo.FontStyle       = 0;
  Info.FontInfo.FontName[0]     = L'\0';

  Box.Width            = (UINT16)mWidth;
  Box.Height           = (UINT16)mScratchHeight;
  Box.Image.Bitmap     = mScratch;

  Status = mFont->StringToImage (mFont,
                                  EFI_HII_OUT_FLAG_CLIP |
                                    EFI_HII_IGNORE_IF_NO_GLYPH |
                                    EFI_HII_IGNORE_LINE_BREAK,
                                  (EFI_STRING)Text, &Info, &Blt, 0, 0,
                                  &Rows, &RowCount, NULL);
  if (EFI_ERROR (Status)) {
    if (Rows != NULL) {
      FreePool (Rows);
    }
    return Status;
  }

  if (Rows != NULL && RowCount > 0) {
    *Width  = Rows[0].LineWidth;
    *Height = Rows[0].LineHeight;
    FreePool (Rows);
  }

  if (*Width > mWidth) {
    *Width = mWidth;
  }
  if (*Height > mScratchHeight) {
    *Height = mScratchHeight;
  }
  return (*Width == 0 || *Height == 0) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

/*
 * Copy the rendered string from the scratch box into the surface, one glyph
 * pixel turned into a Scale x Scale block. Returns the advance in pixels.
 */
UINTN
SfbGfxText (IN UINTN X, IN UINTN Y, IN CONST CHAR16 *Text,
            IN UINTN Scale, IN UINT32 Color)
{
  UINTN  Width;
  UINTN  Height;
  UINTN  SrcX;
  UINTN  SrcY;

  if (!mReady || mFb == NULL) {
    return 0;
  }
  if (Scale == 0) {
    Scale = 1;
  }
  if (EFI_ERROR (SfbGfxRenderText (Text, &Width, &Height))) {
    return 0;
  }

  for (SrcY = 0; SrcY < Height; SrcY++) {
    UINTN  DstY = Y + SrcY * Scale;
    UINTN  BlockH;

    if (DstY >= mHeight) {
      break;
    }
    BlockH = Scale;
    if (DstY + BlockH > mHeight) {
      BlockH = mHeight - DstY;
    }

    for (SrcX = 0; SrcX < Width; SrcX++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixel =
        &mScratch[SrcY * mWidth + SrcX];
      UINTN  DstX;
      UINTN  BlockW;
      UINTN  Dy;
      UINTN  Dx;

      if ((Pixel->Red == SFB_GFX_BG_RED) &&
          (Pixel->Green == SFB_GFX_BG_GREEN) &&
          (Pixel->Blue == SFB_GFX_BG_BLUE)) {
        continue;
      }

      DstX = X + SrcX * Scale;
      if (DstX >= mWidth) {
        break;
      }
      BlockW = Scale;
      if (DstX + BlockW > mWidth) {
        BlockW = mWidth - DstX;
      }

      for (Dy = 0; Dy < BlockH; Dy++) {
        UINT32  *Line = mFb + (DstY + Dy) * mStride + DstX;

        for (Dx = 0; Dx < BlockW; Dx++) {
          Line[Dx] = Color;
        }
      }
    }
  }

  return Width * Scale + Scale;
}

UINTN
SfbGfxTextWidth (IN CONST CHAR16 *Text, IN UINTN Scale)
{
  UINTN  Width;
  UINTN  Height;

  if (Scale == 0) {
    Scale = 1;
  }
  if (EFI_ERROR (SfbGfxRenderText (Text, &Width, &Height))) {
    return 0;
  }

  return Width * Scale;
}

/* ---- bring-up ----------------------------------------------------------- */

EFI_STATUS
SfbGfxInit (VOID)
{
  EFI_STATUS  Status;
  UINTN       ProbeWidth;
  UINTN       ProbeHeight;

  if (mReady) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                (VOID **)&mGop);
  if (EFI_ERROR (Status) || mGop == NULL || mGop->Mode == NULL ||
      mGop->Mode->Info == NULL || mGop->Mode->FrameBufferBase == 0) {
    DEBUG ((EFI_D_INFO, "SFB: gfx: no GOP (%r); using console text\n", Status));
    mGop = NULL;
    return EFI_NOT_FOUND;
  }

  Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mFont);
  if (EFI_ERROR (Status) || mFont == NULL) {
    DEBUG ((EFI_D_INFO, "SFB: gfx: no HII font (%r); using console text\n",
            Status));
    mGop  = NULL;
    mFont = NULL;
    return EFI_NOT_FOUND;
  }

  mWidth  = mGop->Mode->Info->HorizontalResolution;
  mHeight = mGop->Mode->Info->VerticalResolution;
  mStride = mWidth;
  if (mWidth < 320 || mHeight < 240) {
    DEBUG ((EFI_D_INFO, "SFB: gfx: mode %ux%u too small; text menu\n",
            (UINT32)mWidth, (UINT32)mHeight));
    mGop  = NULL;
    mFont = NULL;
    return EFI_UNSUPPORTED;
  }

  mBack = AllocatePool (mWidth * mHeight * sizeof (UINT32));
  mScratchHeight = SFB_GFX_SCRATCH_ROWS;
  mScratch = AllocatePool (mWidth * mScratchHeight *
                           sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (mBack == NULL || mScratch == NULL) {
    DEBUG ((EFI_D_INFO, "SFB: gfx: out of memory; using console text\n"));
    if (mBack != NULL) {
      FreePool (mBack);
      mBack = NULL;
    }
    if (mScratch != NULL) {
      FreePool (mScratch);
      mScratch = NULL;
    }
    mGop  = NULL;
    mFont = NULL;
    return EFI_OUT_OF_RESOURCES;
  }
  mFb = mBack;

  /* Measure the system font before anything is laid out. */
  mReady = TRUE;
  Status = SfbGfxRenderText (L"M", &ProbeWidth, &ProbeHeight);
  if (EFI_ERROR (Status) || ProbeHeight == 0) {
    DEBUG ((EFI_D_INFO, "SFB: gfx: font metrics unavailable (%r); text menu\n",
            Status));
    mReady = FALSE;
    mGop   = NULL;
    mFont  = NULL;
    mFb    = NULL;
    return EFI_NOT_FOUND;
  }
  mLineHeight = ProbeHeight;

  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  DEBUG ((EFI_D_INFO, "SFB: gfx ready %ux%u line=%u surface=%u KB\n",
          (UINT32)mWidth, (UINT32)mHeight, (UINT32)mLineHeight,
          (UINT32)(mWidth * mHeight * sizeof (UINT32) / 1024)));
  return EFI_SUCCESS;
}
