/*
 * Console UI for the super-fastboot boot menu.
 *
 * Three keys drive everything: volume up and volume down move the cursor, and
 * power confirms.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbMenu.h"
#include "SuperFbGfx.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/ShutdownServices.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextIn.h>

/* Keeps the translation unit legal when the feature is compiled out. */
CONST CHAR8 *gSfbMenuModuleTag = "SuperFbMenu";

#define SFB_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
/* Project credit line shown under the boot-menu title. */
#define SFB_MENU_CREDIT  L"gbl_root_canoe 6.3.5 by 1vivy"
#define SFB_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
#define SFB_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK)

/* ---- graphical menu ----------------------------------------------------- */

#define SFB_CLR_BG       SFB_RGB (0x0A, 0x0E, 0x13)
#define SFB_CLR_BAND     SFB_RGB (0x11, 0x17, 0x20)
#define SFB_CLR_RULE     SFB_RGB (0x2E, 0xC4, 0x8A)
#define SFB_CLR_TEXT     SFB_RGB (0xE9, 0xEF, 0xF5)
#define SFB_CLR_DIM      SFB_RGB (0x8E, 0x9C, 0xAB)
#define SFB_CLR_SEL_BG   SFB_RGB (0x1A, 0x2A, 0x3A)
#define SFB_CLR_SEL_TEXT SFB_RGB (0xFF, 0xFF, 0xFF)
#define SFB_CLR_MARK     SFB_RGB (0x2E, 0xC4, 0x8A)

/* True while the current screen is drawn into the framebuffer. */
STATIC BOOLEAN  mGfxUi;
STATIC UINTN    mGfxPad;
STATIC UINTN    mGfxHeaderH;
STATIC UINTN    mGfxContentTop;
STATIC UINTN    mGfxContentBottom;
STATIC UINTN    mGfxFooterY;
STATIC UINTN    mGfxRowSlot;
STATIC UINTN    mGfxRowBarH;
STATIC UINTN    mGfxRowIndex;
STATIC UINTN    mGfxScaleTitle;
STATIC UINTN    mGfxScaleRow;
STATIC UINTN    mGfxScaleHint;

/* Largest integer scale whose text stays within Target pixels. */
STATIC
UINTN
SfbGfxScaleFor (IN UINTN Target)
{
  UINTN  Cell  = SfbGfxTextHeight (1);
  UINTN  Scale;

  if (Cell == 0) {
    return 1;
  }

  Scale = Target / Cell;
  if (Scale < 1) {
    Scale = 1;
  }
  if (Scale > 6) {
    Scale = 6;
  }
  return Scale;
}

/*
 * Derive the whole layout from the framebuffer size and the font cell, so the
 * menu keeps its proportions on any panel. Returns FALSE when there is no
 * framebuffer to draw into, which sends every caller back to console text.
 */
STATIC
BOOLEAN
SfbGfxLayout (VOID)
{
  UINTN  Height;
  UINTN  SubtitleH;
  UINTN  TitleH;
  UINTN  FooterH;
  UINTN  Slot;

  mGfxUi = FALSE;
  if (!SfbGfxReady ()) {
    return FALSE;
  }

  Height = SfbGfxHeight ();
  if (Height == 0) {
    return FALSE;
  }

  mGfxPad        = Height / 64;
  if (mGfxPad == 0) {
    mGfxPad = 1;
  }
  mGfxScaleTitle = SfbGfxScaleFor (Height / 48);
  mGfxScaleRow   = SfbGfxScaleFor (Height / 64);
  mGfxScaleHint  = SfbGfxScaleFor (Height / 96);

  TitleH    = SfbGfxTextHeight (mGfxScaleTitle);
  SubtitleH = SfbGfxTextHeight (mGfxScaleHint);
  mGfxHeaderH    = TitleH + SubtitleH + 3 * mGfxPad;
  FooterH        = SubtitleH + 2 * mGfxPad;
  mGfxContentTop = mGfxHeaderH;
  mGfxContentBottom = (Height > mGfxHeaderH + FooterH + 4 * mGfxPad)
                        ? (Height - FooterH) : (Height - mGfxPad);
  mGfxFooterY    = Height - mGfxPad - SubtitleH;

  if (mGfxContentBottom <= mGfxContentTop + 4 * mGfxPad) {
    return FALSE;
  }

  Slot = (mGfxContentBottom - mGfxContentTop) / SFB_VISIBLE_ROWS;
  if (Slot < SfbGfxTextHeight (1) + 2) {
    return FALSE;
  }

  mGfxRowSlot = Slot;
  mGfxRowBarH = SfbGfxTextHeight (mGfxScaleRow) + Slot / 5;
  if (mGfxRowBarH > Slot) {
    mGfxRowBarH = Slot;
  }
  mGfxRowIndex = 0;
  mGfxUi = TRUE;
  return TRUE;
}

/* Top of the bar for the row that SfbDrawRow is about to draw. */
STATIC
UINTN
SfbGfxRowTop (VOID)
{
  return mGfxContentTop + mGfxRowIndex * mGfxRowSlot +
         (mGfxRowSlot - mGfxRowBarH) / 2;
}

/*
 * Widest line the menu may use, leaving the side margins alone, plus the
 * helpers that keep every string inside it.
 */
STATIC
UINTN
SfbGfxTextMaxWidth (VOID)
{
  return (SfbGfxWidth () > 2 * mGfxPad) ? (SfbGfxWidth () - 2 * mGfxPad)
                                        : SfbGfxWidth ();
}

/*
 * Largest integer scale in [1, Preferred] whose line still fits MaxWidth.
 * Keeping the text inside the panel matters more than keeping it large: the
 * boot menu carries paths and volume labels that would otherwise run off the
 * right edge.
 */
STATIC
UINTN
SfbGfxTextScale (IN CONST CHAR16 *Text, IN UINTN MaxWidth, IN UINTN Preferred)
{
  UINTN  Unit;

  if (Text == NULL || Text[0] == L'\0') {
    return 1;
  }
  if (Preferred == 0) {
    Preferred = 1;
  }

  Unit = SfbGfxTextWidth (Text, 1);
  if (Unit == 0) {
    return Preferred;
  }
  if (Unit * Preferred <= MaxWidth) {
    return Preferred;
  }

  Preferred = (MaxWidth > Unit) ? (MaxWidth / Unit) : 1;
  return (Preferred == 0) ? 1 : Preferred;
}

/* Draw Text at the given position, shrinking it until it fits MaxWidth. */
STATIC
UINTN
SfbGfxTextFit (IN UINTN X, IN UINTN Y, IN CONST CHAR16 *Text,
               IN UINTN MaxWidth, IN UINTN Preferred, IN UINT32 Color)
{
  return SfbGfxText (X, Y, Text, SfbGfxTextScale (Text, MaxWidth, Preferred),
                     Color);
}

/* Draw Text centred across the width, shrinking it when it would not fit. */
VOID
SfbGfxTextCentred (IN UINTN Y, IN CONST CHAR16 *Text, IN UINTN Scale,
                   IN UINT32 Color)
{
  UINTN  MaxWidth = SfbGfxTextMaxWidth ();
  UINTN  Use      = SfbGfxTextScale (Text, MaxWidth, Scale);
  UINTN  Width    = SfbGfxTextWidth (Text, Use);
  UINTN  X        = (SfbGfxWidth () > Width) ? (SfbGfxWidth () - Width) / 2 : 0;

  SfbGfxText (X, Y, Text, Use, Color);
}

SFB_KEY
SfbWaitForKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      WaitList[2];
  UINTN          WaitCount;
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  SFB_KEY        Result = SfbKeyTimeout;

  if (TimeoutMs != 0) {
    Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
    if (EFI_ERROR (Status)) {
      TimerEvent = NULL;
    } else {
      /* Boot services timers count in 100ns units. */
      Status = gBS->SetTimer (TimerEvent, TimerRelative,
                              (UINT64)TimeoutMs * 10000);
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (TimerEvent);
        TimerEvent = NULL;
      }
    }
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitCount = 1;
  if (TimerEvent != NULL) {
    WaitList[1] = TimerEvent;
    WaitCount = 2;
  }

  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == 1) {
      break;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      continue;
    }

    /*
     * On the handset the Qualcomm keypad driver reports the volume keys as
     * SCAN_UP and SCAN_DOWN, and power arrives as a carriage return.
     *
     * Anything left over counts as confirm: on a three-key handset there is
     * nothing else it can be, so the menu stays usable even if a platform
     * reports power differently from what is expected here.
     */
    if (Key.ScanCode == SCAN_UP) {
      Result = SfbKeyUp;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Result = SfbKeyDown;
    } else {
      DEBUG ((EFI_D_VERBOSE, "SFB: confirm key scan=0x%x char=0x%x\n",
              Key.ScanCode, Key.UnicodeChar));
      Result = SfbKeySelect;
    }
    break;
  }

  if (TimerEvent != NULL) {
    gBS->CloseEvent (TimerEvent);
  }

  return Result;
}

/* ---- drawing ------------------------------------------------------------ */

STATIC CONST CHAR16*
SfbGetFileName (IN CONST CHAR16 *Path)
{
  CONST CHAR16 *FileName = Path;
  while (*Path != L'\0') {
    if (*Path == L'\\') FileName = Path + 1;
    Path++;
  }
  return FileName;
}

STATIC BOOLEAN
SfbStrCaseEqual (IN CONST CHAR16 *Str1, IN CONST CHAR16 *Str2)
{
  while (*Str1 && *Str2) {
    CHAR16 c1 = (*Str1 >= L'a' && *Str1 <= L'z') ? *Str1 - 0x20 : *Str1;
    CHAR16 c2 = (*Str2 >= L'a' && *Str2 <= L'z') ? *Str2 - 0x20 : *Str2;
    if (c1 != c2) return FALSE;
    Str1++;
    Str2++;
  }
  return *Str1 == L'\0' && *Str2 == L'\0';
}

VOID
SfbBeginScreen (IN CONST CHAR16 *Title, IN CONST CHAR16 *Subtitle)
{
  if (SfbGfxLayout ()) {
    SfbGfxClear (SFB_CLR_BG);
    SfbGfxFillRect (0, 0, SfbGfxWidth (), mGfxHeaderH, SFB_CLR_BAND);
    SfbGfxFillRect (0, mGfxHeaderH - mGfxPad / 4, SfbGfxWidth (),
                    mGfxPad / 4 + 1, SFB_CLR_RULE);
    SfbGfxTextFit (mGfxPad, mGfxPad, Title, SfbGfxTextMaxWidth (),
                   mGfxScaleTitle, SFB_CLR_TEXT);
    if (Subtitle != NULL) {
      SfbGfxTextFit (mGfxPad,
                     mGfxPad + SfbGfxTextHeight (mGfxScaleTitle) + mGfxPad / 2,
                     Subtitle, SfbGfxTextMaxWidth (), mGfxScaleHint,
                     SFB_CLR_DIM);
    }
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"%s\r\n", Title);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  if (Subtitle != NULL) {
    Print (L"%s\r\n", Subtitle);
  }
  Print (L"\r\n");
}

VOID
SfbEndScreen (IN CONST CHAR16 *Footer)
{
  if (mGfxUi) {
    if (Footer != NULL) {
      /* Centred so it reads as a hint line rather than as another entry. */
      SfbGfxTextCentred (mGfxFooterY, Footer, mGfxScaleHint, SFB_CLR_DIM);
    }
    SfbGfxPresent ();
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n%s\r\n", Footer);
}

VOID
SfbDrawRow (IN BOOLEAN Selected, IN CONST CHAR16 *Marker, IN CONST CHAR16 *Text)
{
  UINTN  Top;
  UINTN  TextY;
  UINTN  X;

  if (mGfxUi) {
    Top   = SfbGfxRowTop ();
    TextY = Top + (mGfxRowBarH - SfbGfxTextHeight (mGfxScaleRow)) / 2;
    X     = mGfxPad;

    if (Selected) {
      SfbGfxRoundRect (mGfxPad / 2, Top, SfbGfxWidth () - mGfxPad,
                       mGfxRowBarH, mGfxRowBarH / 2, SFB_CLR_SEL_BG);
      SfbGfxFillRect (mGfxPad / 2, Top + mGfxRowBarH / 4, mGfxPad / 5 + 3,
                      mGfxRowBarH / 2, SFB_CLR_MARK);
    }

    if (Marker != NULL && Marker[0] != L' ' && Marker[0] != L'\0') {
      X += SfbGfxTextFit (X, TextY, Marker, SfbGfxTextMaxWidth (),
                          mGfxScaleRow, SFB_CLR_MARK) + mGfxPad / 3;
    } else {
      X += SfbGfxTextWidth (L"  ", mGfxScaleRow);
    }

    if (Text != NULL) {
      UINTN  Room = (SfbGfxWidth () > X + mGfxPad)
                      ? (SfbGfxWidth () - X - mGfxPad) : 0;

      SfbGfxTextFit (X, TextY, Text, Room, mGfxScaleRow,
                     Selected ? SFB_CLR_SEL_TEXT : SFB_CLR_TEXT);
    }

    mGfxRowIndex++;
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut,
                             Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL);
  Print (L"%s %s %s", Selected ? L">" : L" ", Marker, Text);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n");
}
STATIC
CONST CHAR16 *
SfbBootModeLabel (IN SFB_BOOT_MODE Mode)
{
  switch (Mode) {
  case SfbBootModeHonestUnlocked:
    return L"Mode 0 - Honest unlocked";
  case SfbBootModeAblFakeLocked:
    return L"Mode 1 - ABL fake locked";
  case SfbBootModeKmProfile:
    return L"Mode 2 - KM/SPSS profile spoof";
  default:
    return L"Mode 1 - ABL fake locked";
  }
}


/*
 * First row of the visible window, keeping the cursor inside it. Lists longer
 * than the window scroll rather than overflow the console.
 */
UINTN
SfbWindowStart (IN UINTN Cursor, IN UINTN Count, IN UINTN Rows)
{
  if (Count <= Rows) {
    return 0;
  }
  if (Cursor < Rows / 2) {
    return 0;
  }
  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) {
    return Count - Rows;
  }

  return Cursor - Rows / 2;
}

VOID
SfbMoveCursor (IN OUT UINTN *Cursor, IN UINTN Count, IN SFB_KEY Key)
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }

  if (Key == SfbKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == SfbKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

/* Centred title plus optional hint line, on the graphical screen. */
STATIC
VOID
SfbGfxBanner (IN CONST CHAR16 *Title, IN CONST CHAR16 *Hint)
{
  UINTN  TitleH = SfbGfxTextHeight (mGfxScaleTitle);
  UINTN  Y      = (SfbGfxHeight () > TitleH) ? (SfbGfxHeight () - TitleH) / 2 : 0;

  SfbGfxClear (SFB_CLR_BG);
  if (Title != NULL) {
    SfbGfxTextCentred (Y, Title, mGfxScaleTitle, SFB_CLR_TEXT);
  }
  if (Hint != NULL) {
    SfbGfxTextCentred (Y + TitleH + mGfxPad, Hint, mGfxScaleHint, SFB_CLR_DIM);
  }
  SfbGfxPresent ();
}

/* Report a failure and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (IN CONST CHAR16 *What, IN EFI_STATUS Status)
{
  if (SfbGfxLayout ()) {
    CHAR16  Detail[64];
    UINTN   Y = SfbGfxHeight () * 2 / 5;

    SfbGfxClear (SFB_CLR_BG);
    SfbGfxTextCentred (Y, What, mGfxScaleTitle, SFB_CLR_TEXT);
    UnicodeSPrint (Detail, sizeof (Detail), L"Status: %r", Status);
    SfbGfxTextCentred (Y + SfbGfxTextHeight (mGfxScaleTitle) + mGfxPad, Detail,
                       mGfxScaleHint, SFB_CLR_DIM);
    SfbGfxTextCentred (SfbGfxHeight () * 3 / 4, L"Press power to continue.",
                       mGfxScaleHint, SFB_CLR_DIM);
    SfbWaitForKey (0);
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n%s: %r\r\n", What, Status);
  Print (L"Press power to continue.\r\n");
  SfbWaitForKey (0);
}

/*
 * Hand the screen over to fastboot. The menu is the last thing that draws
 * before control leaves for the fastboot loop, which prints nothing of its own
 * until a host connects, so without this the user would be staring at a boot
 * menu that no longer responds to anything.
 */
VOID
SfbShowFastbootMode (VOID)
{
  if (SfbGfxLayout ()) {
    SfbGfxBanner (L"FASTBOOT MODE",
                  L"Connect USB and use fastboot on the host");
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"FASTBOOT MODE\r\n");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbShowBootingScreen (IN CONST CHAR16 *Name,
                      IN CONST CHAR16 *FilePath,
                      IN BOOLEAN       ClearScreen)
{
  if (SfbGfxLayout ()) {
    CONST CHAR16  *Label = (Name != NULL && Name[0] != L'\0') ? Name : L"...";
    CONST CHAR16  *FileName = (FilePath != NULL) ? SfbGetFileName (FilePath)
                                                 : NULL;
    CHAR16        Line[SFB_DESC_CHARS + 24];

    /*
     * An unattended default boot must not blank whatever is already on screen
     * (typically the boot splash); only the menu path clears and announces.
     */
    if (!ClearScreen) {
      return;
    }

    if (FileName == NULL || !SfbStrCaseEqual (FileName, L"boot.efi")) {
      UnicodeSPrint (Line, sizeof (Line), L"Booting %s", Label);
      SfbGfxBanner (Line, SFB_MENU_CREDIT);
    } else {
      SfbGfxBanner (L"Starting...", SFB_MENU_CREDIT);
    }
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.
   */
  if (ClearScreen) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  if (FilePath != NULL) {
    CONST CHAR16 *FileName = SfbGetFileName (FilePath);
    if (!SfbStrCaseEqual (FileName, L"boot.efi")) {
      Print (L"Booting %s\r\n", (Name != NULL && Name[0] != L'\0') ? Name : L"...");
    }
  } else {
    Print (L"Booting %s\r\n", (Name != NULL && Name[0] != L'\0') ? Name : L"...");
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Announce a power action (Power Off / Restart) and leave the message on
 * screen while the reset takes effect. Neither action returns, so the screen is
 * the last thing the user sees.
 */
VOID
SfbShowActionScreen (IN CONST CHAR16 *Text)
{
  if (SfbGfxLayout ()) {
    SfbGfxBanner (Text, NULL);
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"%s\r\n", Text);

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Seconds to hold on the "Entering Boot Menu" screen before the menu starts
 * taking input. Long enough that a volume key held from power-on has been
 * released, so it does not immediately move the menu cursor.
 */
#define SFB_ENTER_MENU_DELAY_S  3

VOID
SfbShowEnteringMenu (VOID)
{
  if (SfbGfxLayout ()) {
    SfbGfxBanner (L"Entering Boot Menu", SFB_MENU_CREDIT);
  } else {
    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
    gST->ConOut->ClearScreen (gST->ConOut);
    gST->ConOut->EnableCursor (gST->ConOut, FALSE);

    Print (L"Entering Boot Menu\r\n");

    gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  }

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/* ---- boot menu ---------------------------------------------------------- */

/* Seconds the root menu waits for input before booting the default entry. */
#define SFB_AUTO_BOOT_SECONDS  10

STATIC
VOID
SfbDrawMenu (IN CONST SFB_MENU_STATE *Menu,
             IN UINTN                Cursor,
             IN CONST CHAR16         *Title,
             IN UINT32               Countdown)
{
  UINTN  Start;
  UINTN  Index;
  UINTN  Last;

  SfbBeginScreen (Title, SFB_MENU_CREDIT);

  if (Menu->Count == 0) {
    SfbDrawRow (FALSE, L" ", L"No boot entries found.");
  }

  Start = SfbWindowStart (Cursor, Menu->Count, SFB_VISIBLE_ROWS);
  Last = Start + SFB_VISIBLE_ROWS;
  if (Last > Menu->Count) {
    Last = Menu->Count;
  }

  for (Index = Start; Index < Last; Index++) {
    CONST SFB_BOOT_ENTRY  *Entry = &Menu->Entry[Index];
    CONST CHAR16          *Marker = (Index == Menu->DefaultIndex) ? L"*" : L" ";

    /* Mode is the first root row; render from the authoritative snapshot so
     * a rebuild can never display a stale persisted label. */
    if (Entry->Kind == SfbEntryMode) {
      CHAR16  Text[SFB_DESC_CHARS + 40];

      UnicodeSPrint (Text, sizeof (Text), L"Boot Mode: %s",
                     SfbBootModeLabel (Menu->Mode));
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Text);
    } else if (Entry->Kind == SfbEntrySubmenu) {
      CHAR16  Text[SFB_DESC_CHARS + 4];

      UnicodeSPrint (Text, sizeof (Text), L"%s >", Entry->Desc);
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Text);
    } else {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), Marker, Entry->Desc);
    }
  }

  if (Last < Menu->Count) {
    CHAR16  More[40];

    UnicodeSPrint (More, sizeof (More), L"... %u more",
                   (UINT32)(Menu->Count - Last));
    SfbDrawRow (FALSE, L" ", More);
  }

  if (Countdown > 0) {
    CHAR16  Footer[80];

    UnicodeSPrint (Footer, sizeof (Footer),
                   L"Auto boot in %u s   Vol Up/Down: move   Power: select",
                   (UINT32)Countdown);
    SfbEndScreen (Footer);
  } else {
    SfbEndScreen (L"Vol Up/Down: move   Power: select");
  }
}
/*
 * Select and persist the preferred mode. The caller owns the authoritative
 * CurrentMode value; it changes only after the raw tail record was written.
 */
STATIC
VOID
SfbRunModeMenu (IN OUT SFB_BOOT_MODE *CurrentMode)
{
  STATIC CONST CHAR16 *Rows[] = {
    L"Mode 0 - Honest unlocked",
    L"Mode 1 - ABL fake locked",
    L"Mode 2 - KM/SPSS profile spoof",
    L"Back"
  };
  UINTN  Cursor = 0;
  UINTN  Index;

  if (CurrentMode == NULL) {
    return;
  }

  while (TRUE) {
    SFB_KEY  Key;

    SfbBeginScreen (L"Boot Mode", L"Choose the preferred ABL policy.");
    for (Index = 0; Index < ARRAY_SIZE (Rows); Index++) {
      SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Rows[Index]);
    }
    SfbEndScreen (L"Vol Up/Down: move   Power: select");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, ARRAY_SIZE (Rows), Key);
      continue;
    }

    if (Cursor == ARRAY_SIZE (Rows) - 1) {
      return;
    }

    {
      EFI_STATUS Status;

      Status = SfbCommitModeSelection (CurrentMode,
                                       (SFB_BOOT_MODE)Cursor);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Could not save boot mode", Status);
        continue;
      }
    }
    return;
  }
}


/*
 * Run a submenu defined by the ENTRIES file at EntriesPath on Volume. The file
 * is parsed exactly like the root BOOTENTRIES, and may itself contain further
 * '%' submenu rows; Depth bounds the nesting so a chain of files that points at
 * one another cannot recurse without limit. The submenu state is heap-allocated
 * (a single SFB_MENU_STATE is ~17 KB) so deep nesting stays off the call stack.
 *
 * Returns when the user picks the trailing "Back" row, or when the file could
 * not be built at all; the caller then redraws its own menu.
 */
STATIC
VOID
SfbRunSubMenu (IN EFI_HANDLE    Volume,
               IN CONST CHAR16  *EntriesPath,
               IN CONST CHAR16  *Title,
               IN UINTN         Depth,
               IN SFB_BOOT_MODE Mode)
{
  SFB_MENU_STATE  *Menu = NULL;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  Menu = AllocateZeroPool (sizeof (*Menu));
  if (Menu == NULL) {
    return;
  }
  Menu->DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (Menu);
      Status = SfbBuildSubMenu (Menu, Volume, EntriesPath, Mode);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (Title, Status);
        break;
      }
      Cursor = 0;
      Rebuild = FALSE;
    }

    /* Submenus are always interactive: no countdown. */
    SfbDrawMenu (Menu, Cursor, Title, 0);

    /* Same input model as the root menu: volume keys move, power confirms. */
    Key = SfbWaitForKey (0);

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu->Count, Key);
      continue;
    }

    if (Menu->Count == 0) {
      continue;
    }

    Chosen = Cursor;
    switch (Menu->Entry[Chosen].Kind) {
    case SfbEntryBack:
      goto done;

    case SfbEntrySubmenu:
      if (Depth >= SFB_MAX_SUBMENU_DEPTH) {
        SfbReportStatus (L"Submenu too deep", EFI_BUFFER_TOO_SMALL);
      } else {
        SfbRunSubMenu (Menu->Entry[Chosen].Volume,
                       Menu->Entry[Chosen].Path,
                       Menu->Entry[Chosen].Desc,
                       Depth + 1,
                       Mode);
      }
      /* Media may have changed while the child menu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu->Entry[Chosen], TRUE, TRUE, Mode);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      Rebuild = TRUE;
      break;
    }
  }

done:
  SfbFreeMenu (Menu);
  FreePool (Menu);
}

BOOLEAN
SfbRunBootMenu (IN SFB_BOOT_MODE InitialMode)
{
  SFB_MENU_STATE  Menu;
  SFB_BOOT_MODE   CurrentMode = InitialMode;
  UINTN           Cursor = 0;
  BOOLEAN         Rebuild = TRUE;
  BOOLEAN         AutoBooted = FALSE;
  BOOLEAN         Draw = TRUE;
  UINT32          Countdown = SFB_AUTO_BOOT_SECONDS;
  SFB_KEY         Key;
  EFI_STATUS      Status;

  if (CurrentMode > SfbBootModeKmProfile) {
    CurrentMode = SfbBootModeAblFakeLocked;
  }

  ZeroMem (&Menu, sizeof (Menu));
  Menu.DefaultIndex = SFB_NO_INDEX;

  while (TRUE) {
    UINTN  Chosen;

    if (Rebuild) {
      SfbFreeMenu (&Menu);
      SfbBuildMenu (&Menu, CurrentMode);
      Cursor = (Menu.DefaultIndex != SFB_NO_INDEX &&
                Menu.DefaultIndex < Menu.Count) ? Menu.DefaultIndex : 0;
      Rebuild = FALSE;
      Draw    = TRUE;
    }

    if (Draw) {
      SfbDrawMenu (&Menu, Cursor, L"Boot Menu", Countdown);
    }
    Draw = TRUE;

    /*
     * Count down while the user decides. Any key restarts the countdown, so
     * browsing never boots out from under the user, and an untouched handset
     * still boots its default entry when the countdown runs out.
     */
    Key = SfbWaitForKey ((Countdown > 0) ? 1000 : 0);

    if (Key == SfbKeyTimeout) {
      if (Countdown > 0) {
        Countdown--;
      }
      /* Console text cannot be repainted cheaply: only the framebuffer menu
       * redraws once a second. */
      Draw = SfbGfxReady ();

      if ((Countdown == 0) && !AutoBooted && (Menu.Count > 0)) {
        UINTN  Idle = (Menu.DefaultIndex != SFB_NO_INDEX &&
                       Menu.DefaultIndex < Menu.Count) ? Menu.DefaultIndex
                                                       : Cursor;

        AutoBooted = TRUE;
        DEBUG ((EFI_D_INFO, "SFB: auto-boot '%s' after %u s idle\n",
                Menu.Entry[Idle].Desc, (UINT32)SFB_AUTO_BOOT_SECONDS));
        Status = SfbLaunchEntry (&Menu.Entry[Idle], FALSE, TRUE, CurrentMode);
        if (EFI_ERROR (Status)) {
          SfbReportStatus (L"Boot failed", Status);
        }
        Rebuild = TRUE;
      }
      continue;
    }

    /*
     * A key press cancels auto-boot for the rest of the session, including
     * after submenus: once the user is driving, the menu waits for them.
     */
    Countdown  = 0;
    AutoBooted = TRUE;

    if (Key == SfbKeyUp || Key == SfbKeyDown) {
      SfbMoveCursor (&Cursor, Menu.Count, Key);
      continue;
    }

    Chosen = Cursor;

    if (Menu.Count == 0) {
      continue;
    }

    switch (Menu.Entry[Chosen].Kind) {
    case SfbEntryFastboot:
      SfbFreeMenu (&Menu);
      return TRUE;

    case SfbEntryMode:
      SfbRunModeMenu (&CurrentMode);
      Rebuild = TRUE;
      break;

    case SfbEntrySelector:
      SfbRunFileBrowser (CurrentMode);
      /* The browser may have added a custom entry. */
      Rebuild = TRUE;
      break;

    case SfbEntrySubmenu:
      SfbRunSubMenu (Menu.Entry[Chosen].Volume,
                     Menu.Entry[Chosen].Path,
                     Menu.Entry[Chosen].Desc,
                     1,
                     CurrentMode);
      /* Media may have changed while the submenu was open. */
      Rebuild = TRUE;
      break;

    case SfbEntryBack:
      /* Only submenus carry a Back row; the root menu never adds one. */
      Rebuild = TRUE;
      break;

    case SfbEntryPowerOff:
      SfbShowActionScreen (L"Powering off...");
      ShutdownDevice ();
      break;

    case SfbEntryRestart:
      SfbShowActionScreen (L"Restarting...");
      RebootDevice (NORMAL_MODE);
      break;

    case SfbEntryEfiFile:
    default:
      Status = SfbLaunchEntry (&Menu.Entry[Chosen], FALSE, TRUE, CurrentMode);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }
      /* Media or variables may have changed while the image ran. */
      Rebuild = TRUE;
      break;
    }
  }
}
