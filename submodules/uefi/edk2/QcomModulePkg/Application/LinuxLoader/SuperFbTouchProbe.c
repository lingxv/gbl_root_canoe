/*
 * Stage 0 touch feasibility probe.
 *
 * The path here was established by the earlier revisions, all of which ran on
 * the real device:
 *
 *   v1  opened a serial engine and called the vendor transfer before the large
 *       stack existed. It died before it could log anything.
 *   v2  only located the vendor protocols, recovered the module load addresses
 *       from the interface pointers and checked them against the reverse
 *       engineered layout. It ran clean and proved the SPI protocol layout.
 *   v3  asked the vendor mapper for the touch index (25 = WRAP4 SE0), called
 *       the vendor open(), read the register state and closed again. open()
 *       succeeded, so power, clock and pin mux for that engine are available.
 *   v4  fed a corrected client configuration to the vendor transfer. The
 *       transfer is the vendor's full duplex path - its own interrupt handler,
 *       its own FIFO bookkeeping - and it took the device down to 900E.
 *
 * So this revision keeps the vendor open(), which is the part that works, and
 * replaces the vendor transfer with the polling sequence the mainline kernel
 * driver uses for FIFO mode: program the word and transaction lengths, push the
 * bytes into the TX FIFO, start the command, then poll the primary interrupt
 * status for M_CMD_DONE and drain the RX FIFO. No interrupt handler of ours, no
 * vendor transfer, no DMA.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "SuperFbTouchProbe.h"
#include "SuperFbGfx.h"

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

/* Protocols published by the vendor modules inside uefi.img. */
#define SFB_SPI_PROTOCOL_GUID \
  { 0x4C7FFD28, 0x6A06, 0x4425, \
    { 0x9E, 0xE2, 0x67, 0x6E, 0xBC, 0x08, 0x96, 0x83 } }

#define SFB_TLMM_PROTOCOL_GUID \
  { 0x4CE41849, 0xF4E7, 0x480E, \
    { 0x9F, 0xA3, 0x00, 0x3A, 0xAA, 0x43, 0x45, 0x15 } }

#define SFB_GPI_PROTOCOL_GUID \
  { 0x569EA0DE, 0xB557, 0x4043, \
    { 0x84, 0xCF, 0x01, 0x10, 0x3F, 0xE5, 0x16, 0xE5 } }

/*
 * Image relative addresses inside the vendor modules, straight out of the
 * disassembly. The runtime address of a module is recovered by subtracting the
 * address of one of these objects from the pointer the firmware handed us.
 */
#define SFB_SPI_IFACE_RVA      0x17138   /* installed protocol interface */
#define SFB_SPI_GUID_RVA       0x17068   /* its GUID */
#define SFB_SPI_OPEN_RVA       0x31EC
#define SFB_SPI_XFER_RVA       0x32A0
#define SFB_SPI_CLOSE_RVA      0x3230
#define SFB_SPI_COUNT_RVA      0x135B9   /* serial engines per wrapper, 3 bytes */
#define SFB_SPI_OFFSET_RVA     0x13880   /* start of each group inside the blob */
#define SFB_SPI_NAMES_RVA      0x13868   /* three name pointers */
#define SFB_SPI_NUMSE_RVA      0x1AA08   /* runtime filled num_se blob */
#define SFB_SPI_FLAG_RVA       0x1AA0E   /* "blob is populated" flag */
#define SFB_SPI_DATA_START     0x17000
#define SFB_SPI_DATA_END       0x1C000
#define SFB_SPI_GROUPS         3

#define SFB_TLMM_IFACE_RVA     0x9160
#define SFB_TLMM_GUID_RVA      0x9098

/* spi19 = spi@1a80000 = QUPv3 WRAP4 SE0 in the kernel device tree. */
#define SFB_TOUCH_SE_BASE      0x01A80000

#define SFB_PROBE_MAX_LINES    22
#define SFB_PROBE_LINE_CHARS   64

/*
 * TLMM extension protocol published by TLMMExDxe: a version followed by five
 * entry points, reverse engineered from that module.
 *
 *   0x1a50  configure one pin        (pin)
 *   0x1a70  configure a pin list     (pin array, count)
 *   0x1adc  read one pin             (pin, out value)
 *   0x1b1c  write one pin            (pin, value)
 *   0x1b40  stub, returns success
 *
 * A pin number is a linear index across the TLMM tiles: the read and write
 * helpers take the byte holding it as pin/8 and the bit inside that byte as
 * pin%8, which is exactly how the kernel pinctrl numbers them.
 */
typedef struct {
  UINT64  Version;
  UINT64  Method[5];
} SFB_TLMM_PROTOCOL;

STATIC CONST UINT64  mTlmmMethodRva[5] = {
  0x1A50, 0x1A70, 0x1ADC, 0x1B1C, 0x1B40
};

/* Module base used to check the five methods are still the ones we expect. */
#define SFB_TLMM_IFACE_RVA_ALT 0x9160

typedef EFI_STATUS (EFIAPI *SFB_TLMM_CONFIGURE) (IN UINT32 Pin);
typedef EFI_STATUS (EFIAPI *SFB_TLMM_CONFIGURE_LIST) (IN UINT32 *Pins,
                                                      IN UINT8 Count);
typedef EFI_STATUS (EFIAPI *SFB_TLMM_READ) (IN UINT32 Pin, OUT UINT8 *Value);
typedef EFI_STATUS (EFIAPI *SFB_TLMM_WRITE) (IN UINT32 Pin, IN UINT8 Value);

/* The OnePlus 15 device tree puts the Synaptics reset on TLMM gpio159. */
#define SFB_TOUCH_RESET_PIN 159


/* SPI protocol interface: version plus open and close. */
typedef struct {
  UINT64  Version;
  UINT64  Open;
  UINT64  Transfer;
  UINT64  Close;
} SFB_SPI_INTERFACE;

/*
 * The probe normally prints nothing: logfs is the output channel. This buffer
 * exists for the read only pin controller check, where a crash can wipe the
 * firmware log and the screen may be the only surviving evidence.
 */
STATIC CHAR16  mScreen[SFB_PROBE_MAX_LINES][SFB_PROBE_LINE_CHARS];
STATIC UINTN   mScreenLines;
STATIC UINTN   mScreenColumn;

STATIC
VOID
SfbProbeNewLine (VOID)
{
  if (mScreenLines + 1 < SFB_PROBE_MAX_LINES) {
    mScreenLines++;
    mScreenColumn = 0;
    mScreen[mScreenLines][0] = L'\0';
  }
}

STATIC
VOID
SfbProbePut (IN CONST CHAR16 *Text)
{
  while (*Text != L'\0' && mScreenColumn + 1 < SFB_PROBE_LINE_CHARS) {
    mScreen[mScreenLines][mScreenColumn++] = *Text++;
  }

  mScreen[mScreenLines][mScreenColumn] = L'\0';
}

STATIC
VOID
SfbProbePutHex (IN UINT64 Value)
{
  CHAR16  Buffer[19];
  UINTN   Index;

  Buffer[0]  = L'0';
  Buffer[1]  = L'x';
  Buffer[18] = L'\0';
  for (Index = 0; Index < 16; Index++) {
    Buffer[17 - Index] = L"0123456789abcdef"[Value & 0xF];
    Value            >>= 4;
  }

  Index = 2;
  while (Index < 17 && Buffer[Index] == L'0') {
    Index++;
  }

  SfbProbePut (L"0x");
  SfbProbePut (&Buffer[Index]);
}

STATIC
VOID
SfbProbePutDec (IN UINT32 Value)
{
  CHAR16  Buffer[11];
  UINTN   Index = 10;

  if (Value == 0) {
    SfbProbePut (L"0");
    return;
  }

  Buffer[10] = L'\0';
  while (Value != 0 && Index > 0) {
    Buffer[--Index] = (CHAR16)(L'0' + (Value % 10));
    Value          /= 10;
  }

  SfbProbePut (&Buffer[Index]);
}

/* Log a byte range to logfs; the screen gets a shortened form on request. */
STATIC
VOID
SfbProbeDump (IN CONST CHAR8 *Tag, IN CONST UINT8 *Data, IN UINTN Length)
{
  UINTN  Index;

  DEBUG ((EFI_D_INFO, "SFB: touch %a len=%u\n", Tag, (UINT32)Length));
  for (Index = 0; Index + 8 <= Length; Index += 8) {
    DEBUG ((EFI_D_INFO,
            "SFB: touch  +0x%x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            (UINT32)Index, Data[Index], Data[Index + 1], Data[Index + 2],
            Data[Index + 3], Data[Index + 4], Data[Index + 5], Data[Index + 6],
            Data[Index + 7]));
  }
}

STATIC
BOOLEAN
SfbProbeGuidAt (IN CONST UINT8 *Base, IN UINTN Offset, IN CONST EFI_GUID *Guid)
{
  return (BOOLEAN)(CompareMem (Base + Offset, Guid, sizeof (EFI_GUID)) == 0);
}

/*
 * Look for pointers into the touch serial engine's register window inside the
 * module data. The QUP framework keeps the hardware configuration it built
 * from the device tree somewhere in there, so a hit also tells us that the
 * device tree does describe the engine.
 */
STATIC
UINT32
SfbProbeScanSeBase (IN CONST UINT8 *Base)
{
  UINTN   Offset;
  UINT32  Hits = 0;
  UINT64  Value;
  UINT64  Extra;

  for (Offset = SFB_SPI_DATA_START; Offset + 16 <= SFB_SPI_DATA_END; Offset += 8) {
    CopyMem (&Value, Base + Offset, sizeof (Value));
    if ((Value & ~0x3FFFULL) != (UINT64)SFB_TOUCH_SE_BASE) {
      continue;
    }

    CopyMem (&Extra, Base + Offset + 8, sizeof (Extra));
    DEBUG ((EFI_D_INFO,
            "SFB: touch se-base hit at +0x%x value=0x%lx next=0x%lx\n",
            (UINT32)Offset, Value, Extra));
    Hits++;
    if (Hits >= 8) {
      break;
    }
  }

  return Hits;
}

/*
 * Everything the SPI module is willing to tell us without being called: the
 * protocol interface, the table of serial engines per QUP wrapper, and the
 * index mapping those tables describe.
 */
STATIC
VOID
SfbProbeSpi (IN SFB_SPI_INTERFACE *Spi)
{
  EFI_GUID     SpiGuid = SFB_SPI_PROTOCOL_GUID;
  CONST UINT8  *Base;
  UINT8        Count[SFB_SPI_GROUPS];
  UINT8        Offset[SFB_SPI_GROUPS];
  UINT8        NumSe[16];
  UINT32       Group;
  UINT32       Entry;
  UINT32       Entries;
  UINT32       Se;
  UINT32       Num;
  UINT32       Index;
  UINT32       Hits;
  UINT64       NamePtr;
  UINT64       Want;

  Base = (CONST UINT8 *)Spi - SFB_SPI_IFACE_RVA;

  SfbProbePut (L"SPI base=");
  SfbProbePutHex ((UINT64)(UINTN)Base);
  SfbProbeNewLine ();

  if (!SfbProbeGuidAt (Base, SFB_SPI_GUID_RVA, &SpiGuid)) {
    DEBUG ((EFI_D_ERROR, "SFB: touch spi base check FAILED\n"));
    SfbProbePut (L"guid check FAILED - stop");
    SfbProbeNewLine ();
    return;
  }

  DEBUG ((EFI_D_INFO, "SFB: touch spi guid ok, version=0x%lx\n", Spi->Version));
  DEBUG ((EFI_D_INFO,
          "SFB: touch spi open=%lx (want %lx) xfer=%lx (want %lx) close=%lx\n"
          " (want %lx)\n",
          Spi->Open, (UINT64)(UINTN)(Base + SFB_SPI_OPEN_RVA),
          Spi->Transfer, (UINT64)(UINTN)(Base + SFB_SPI_XFER_RVA),
          Spi->Close, (UINT64)(UINTN)(Base + SFB_SPI_CLOSE_RVA)));

  Want  = (UINT64)(UINTN)(Base + SFB_SPI_OPEN_RVA);
  Num   = (Spi->Open == Want) ? 1 : 0;
  Want  = (UINT64)(UINTN)(Base + SFB_SPI_XFER_RVA);
  Num  += (Spi->Transfer == Want) ? 1 : 0;
  Want  = (UINT64)(UINTN)(Base + SFB_SPI_CLOSE_RVA);
  Num  += (Spi->Close == Want) ? 1 : 0;

  SfbProbePut (L"ver=");
  SfbProbePutHex (Spi->Version);
  SfbProbePut (L" fnmatch=");
  SfbProbePutDec (Num);
  SfbProbePut (L"/3");
  SfbProbeNewLine ();

  CopyMem (Count, Base + SFB_SPI_COUNT_RVA, sizeof (Count));
  CopyMem (Offset, Base + SFB_SPI_OFFSET_RVA, sizeof (Offset));
  CopyMem (NumSe, Base + SFB_SPI_NUMSE_RVA, sizeof (NumSe));

  DEBUG ((EFI_D_INFO,
          "SFB: touch count=%u,%u,%u offset=%u,%u,%u flag=%u\n",
          Count[0], Count[1], Count[2], Offset[0], Offset[1], Offset[2],
          *(Base + SFB_SPI_FLAG_RVA)));
  SfbProbeDump ("numse", NumSe, 16);

  SfbProbePut (L"count ");
  SfbProbePutDec (Count[0]);
  SfbProbePut (L",");
  SfbProbePutDec (Count[1]);
  SfbProbePut (L",");
  SfbProbePutDec (Count[2]);
  SfbProbePut (L" flag=");
  SfbProbePutDec (*(Base + SFB_SPI_FLAG_RVA));
  SfbProbeNewLine ();

  SfbProbePut (L"numse");
  for (Index = 0; Index < 8; Index++) {
    SfbProbePut (L" ");
    SfbProbePutDec (NumSe[Index]);
  }

  SfbProbeNewLine ();

  for (Group = 0; Group < SFB_SPI_GROUPS; Group++) {
    CopyMem (&NamePtr, Base + SFB_SPI_NAMES_RVA + (Group * 8), sizeof (NamePtr));
    DEBUG ((EFI_D_INFO, "SFB: touch group %u name rva=0x%x count=%u offset=%u\n",
            Group, (UINT32)(NamePtr - (UINT64)(UINTN)Base), Count[Group],
            Offset[Group]));
  }

  /* Reproduce the index mapping the module performs on open(). */
  Index = 0;
  for (Group = 0; Group < SFB_SPI_GROUPS; Group++) {
    Entries = (Count[Group] > 1) ? Count[Group] : 1;
    for (Entry = 0; Entry < Entries; Entry++) {
      Num = NumSe[Offset[Group] + Entry];
      DEBUG ((EFI_D_INFO,
              "SFB: touch map blk=%u inst=%u num_se=%u -> index %u..%u\n",
              Group, Entry, Num, Index + 1, Index + Num));

      SfbProbePut (L"b");
      SfbProbePutDec (Group);
      SfbProbePut (L" i");
      SfbProbePutDec (Entry);
      SfbProbePut (L" n");
      SfbProbePutDec (Num);
      SfbProbePut (L" idx ");
      SfbProbePutDec (Index + 1);
      SfbProbePut (L"..");
      SfbProbePutDec (Index + Num);
      SfbProbeNewLine ();

      for (Se = 0; Se < Num; Se++) {
        Index++;
      }
    }
  }

  Hits = SfbProbeScanSeBase (Base);
  SfbProbePut (L"se-base 0x1a80000 hits=");
  SfbProbePutDec (Hits);
  SfbProbePut (L" total_idx=");
  SfbProbePutDec (Index);
  SfbProbeNewLine ();
}

/*
 * Fingerprints of the vendor entry points this file is allowed to call, taken
 * from the disassembly of uefi.img. A mismatch means the running firmware is
 * not the one that was reverse engineered, and nothing gets called.
 */
STATIC CONST UINT8  mFpMapper[16] = {
  0xfd, 0x7b, 0xbd, 0xa9, 0xf6, 0x57, 0x01, 0xa9,
  0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x03, 0x00, 0x91
};
STATIC CONST UINT8  mFpOpen[16] = {
  0xfd, 0x7b, 0xbe, 0xa9, 0xf3, 0x0b, 0x00, 0xf9,
  0xfd, 0x03, 0x00, 0x91, 0xf3, 0x03, 0x01, 0xaa
};
STATIC CONST UINT8  mFpClose[16] = {
  0xfd, 0x7b, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9,
  0xfd, 0x03, 0x00, 0x91, 0xf4, 0x03, 0x00, 0xaa
};

/* Index of the vendor module helper that maps an engine index to its wrapper. */
#define SFB_SPI_MAPPER_RVA     0x8E94
#define SFB_TOUCH_INDEX        25
#define SFB_TOUCH_WRAPPER      4
#define SFB_TOUCH_ENGINE       0
#define SFB_TOUCH_CLK_HZ       19000000

typedef BOOLEAN (EFIAPI *SFB_QUP_MAPPER) (IN UINT32 Index, OUT UINT32 *Wrapper,
                                          OUT UINT32 *Engine);
typedef EFI_STATUS (EFIAPI *SFB_SPI_OPEN) (IN UINT32 Index, OUT VOID **Handle);
typedef EFI_STATUS (EFIAPI *SFB_SPI_CLOSE) (IN VOID *Handle);

STATIC BOOLEAN  mMapValidated;

STATIC
BOOLEAN
SfbProbeCodeAt (IN CONST UINT8 *Base, IN UINTN Rva, IN CONST UINT8 *Want)
{
  return (BOOLEAN)(CompareMem (Base + Rva, Want, 16) == 0);
}
/*
 * Ask the vendor mapper which wrapper and engine an index selects. This is the
 * first thing open() does and it touches no hardware, so it is the cheapest way
 * to confirm the mapping we derived from the ABL device tree.
 */
STATIC
VOID
SfbProbeMapper (IN CONST UINT8 *Base)
{
  SFB_QUP_MAPPER  Map;
  UINT32          Index;
  UINT32          Wrapper;
  UINT32          Engine;
  UINT32          Found = 0;
  UINT8           NumSe[16];

  mMapValidated = FALSE;

  if (!SfbProbeCodeAt (Base, SFB_SPI_MAPPER_RVA, mFpMapper)) {
    DEBUG ((EFI_D_ERROR, "SFB: touch mapper fingerprint mismatch\n"));
    SfbProbePut (L"mapper fingerprint MISMATCH");
    SfbProbeNewLine ();
    return;
  }

  Map = (SFB_QUP_MAPPER)(UINTN)(Base + SFB_SPI_MAPPER_RVA);

  for (Index = 1; Index <= 31; Index++) {
    Wrapper = 0xFFFFFFFF;
    Engine  = 0xFFFFFFFF;

    if (!Map (Index, &Wrapper, &Engine)) {
      DEBUG ((EFI_D_INFO, "SFB: touch map idx=%u none\n", Index));
      continue;
    }

    Found++;
    DEBUG ((EFI_D_INFO, "SFB: touch map idx=%u qup=0x%x se=%u\n",
            Index, Wrapper, Engine));

    if (Index == SFB_TOUCH_INDEX) {
      SfbProbePut (L"map25 qup=");
      SfbProbePutDec (Wrapper);
      SfbProbePut (L" se=");
      SfbProbePutDec (Engine);
      mMapValidated = (BOOLEAN)((Wrapper == SFB_TOUCH_WRAPPER) &&
                                (Engine == SFB_TOUCH_ENGINE));
      SfbProbePut (mMapValidated ? L" MATCH" : L" MISMATCH");
      SfbProbeNewLine ();
    }
  }

  CopyMem (NumSe, Base + SFB_SPI_NUMSE_RVA, sizeof (NumSe));
  SfbProbeDump ("numse-after-mapper", NumSe, 16);

  DEBUG ((EFI_D_INFO, "SFB: touch mapper found %u engines\n", Found));
  SfbProbePut (L"mapper engines=");
  SfbProbePutDec (Found);
  SfbProbePut (L"/29");
  SfbProbeNewLine ();
}
/*
 * GENI serial engine register map, the same one the mainline spi-geni-qcom
 * driver uses. The open() call above has already powered the engine, muxed its
 * pins, configured the clock and put it in FIFO mode, so a transfer here is
 * only a matter of driving the sequencer and moving bytes through the FIFOs.
 */
#define SFB_GENI_M_IRQ_STATUS       0x610
#define SFB_GENI_M_IRQ_EN           0x614
#define SFB_GENI_M_IRQ_CLEAR        0x618
#define SFB_GENI_TX_FIFO            0x700
#define SFB_GENI_RX_FIFO            0x780
#define SFB_GENI_TX_FIFO_STATUS     0x800
#define SFB_GENI_RX_FIFO_STATUS     0x804
#define SFB_GENI_TX_WATERMARK       0x80c
#define SFB_GENI_RX_WATERMARK       0x810
#define SFB_GENI_RX_RFR_WATERMARK   0x814
#define SFB_GENI_BYTE_GRAN          0x254
#define SFB_GENI_TX_PACKING_CFG0    0x260
#define SFB_GENI_TX_PACKING_CFG1    0x264
#define SFB_GENI_RX_PACKING_CFG0    0x284
#define SFB_GENI_RX_PACKING_CFG1    0x288
#define SFB_GENI_DMA_MODE_EN        0x258
#define SFB_GENI_IF_DISABLE_RO      0x64
#define SFB_SPI_WORD_LEN            0x268
#define SFB_SPI_TX_TRANS_LEN        0x26c
#define SFB_SPI_RX_TRANS_LEN        0x270
#define SFB_GENI_M_CMD0             0x600

/* GENI_M_CMD0 fields: opcode 27..31, parameters 0..26. */
#define SFB_M_CMD_OPCODE_SHIFT      27
/*
 * SPI opcodes, taken from the kernel spi-geni-qcom driver: a write is a
 * transmit only command, while a Synaptics TCM read is full duplex because the
 * protocol answer is clocked in while 0xFF dummy bytes go out.
 */
#define SFB_SPI_TX_ONLY             0x1
#define SFB_SPI_TX_RX               0x7

/* SE_GENI_M_IRQ_STATUS fields. */
#define SFB_M_CMD_DONE              (1U << 0)
#define SFB_M_CMD_OVERRUN           (1U << 1)
#define SFB_M_ILLEGAL_CMD           (1U << 2)
#define SFB_M_CMD_FAILURE           (1U << 3)
#define SFB_M_CMD_CANCEL            (1U << 4)
#define SFB_M_CMD_ABORT             (1U << 5)
#define SFB_M_RX_FIFO_RD_ERR        (1U << 24)
#define SFB_M_RX_FIFO_WR_ERR        (1U << 25)
#define SFB_M_TX_FIFO_RD_ERR        (1U << 28)
#define SFB_M_TX_FIFO_WR_ERR        (1U << 29)
#define SFB_M_CMD_ERR_MASK \
  (SFB_M_CMD_OVERRUN | SFB_M_ILLEGAL_CMD | SFB_M_CMD_FAILURE | \
   SFB_M_RX_FIFO_RD_ERR | SFB_M_RX_FIFO_WR_ERR | SFB_M_TX_FIFO_RD_ERR | \
   SFB_M_TX_FIFO_WR_ERR)

/* Sequence the engine accepts while a command is still running. */
#define SFB_M_CMD_CANCEL_SEQ        0x2
#define SFB_M_CMD_ABORT_SEQ         0x1

/* GENI_TX_FIFO_STATUS fields. */
#define SFB_TX_FIFO_WC_MASK         0x0FFFFFFF

/* GENI_RX_FIFO_STATUS fields. */
#define SFB_RX_FIFO_WC_MASK         0x01FFFFFF
#define SFB_RX_LAST                 (1U << 31)
#define SFB_RX_LAST_VALID_MASK      0x70000000
#define SFB_RX_LAST_VALID_SHIFT     28

#define SFB_FIFO_WORD_BYTES         4
#define SFB_SPI_BPW                 8
#define SFB_FIFO_POLL_NS            (10 * 1000 * 1000)

STATIC
UINT32
SfbGeniRead (
  IN UINTN  Address
  )
{
  return *(volatile UINT32 *)Address;
}

STATIC
VOID
SfbGeniWrite (
  IN UINTN  Address,
  IN UINT32 Value
  )
{
  *(volatile UINT32 *)Address = Value;
}

/*
 * Bytes per FIFO word for byte aligned transfers. The kernel picks 4 (the FIFO
 * width) when eight bit words pack evenly, and one otherwise.
 */
STATIC
UINT32
SfbGeniBytesPerFifoWord (
  VOID
  )
{
  return (32 % SFB_SPI_BPW) ? 1 : (32 / 8);
}

/*
 * Byte lane assignment inside a FIFO word. This is geni_se_config_packing()
 * with bits_per_word = 8, pack_words = 4 and MSB first, which is what the
 * kernel programs before every FIFO transfer.
 */
STATIC
VOID
SfbGeniConfigPacking (
  IN UINTN  SeBase
  )
{
  UINT32  Config[4];
  UINT32  Index;

  for (Index = 0; Index < 4; Index++) {
    Config[Index]  = (Index * 8) << 5;
    Config[Index] |= 1U << 4;
    Config[Index] |= 7U << 1;
  }

  Config[3] |= 1U;

  SfbGeniWrite (SeBase + SFB_GENI_TX_PACKING_CFG0,
                Config[0] | (Config[1] << 10));
  SfbGeniWrite (SeBase + SFB_GENI_TX_PACKING_CFG1,
                Config[2] | (Config[3] << 10));
  SfbGeniWrite (SeBase + SFB_GENI_RX_PACKING_CFG0,
                Config[0] | (Config[1] << 10));
  SfbGeniWrite (SeBase + SFB_GENI_RX_PACKING_CFG1,
                Config[2] | (Config[3] << 10));

  if (4 != 0) {
    SfbGeniWrite (SeBase + SFB_GENI_BYTE_GRAN, SFB_SPI_BPW / 16);
  }

  /* Word length is stored as bits per word minus the minimum of four. */
  SfbGeniWrite (SeBase + SFB_SPI_WORD_LEN, SFB_SPI_BPW - 4);
}

/*
 * Put the engine back on a known footing. Polling never needs an interrupt and
 * this firmware installs no handler for this engine, so mask the primary and
 * secondary interrupt sources and drop whatever is pending before the first
 * command is started.
 *
 * The watermarks the open() call configured are deliberately left alone. They
 * are what lets the receive side advance; zeroing the ready-for-receive level
 * makes the engine start a command and then wait for a condition that never
 * comes, which is exactly what the previous revision did.
 */
STATIC
VOID
SfbGeniQuiesce (
  IN UINTN  SeBase
  )
{
  SfbGeniWrite (SeBase + SFB_GENI_M_IRQ_EN, 0);
  SfbGeniWrite (SeBase + SFB_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  SfbGeniWrite (SeBase + 0x644, 0);
  SfbGeniWrite (SeBase + 0x648, 0xFFFFFFFF);
}

/*
 * One FIFO mode transfer: transact_len bytes are clocked out of Tx when it is
 * not NULL, and the same number of bytes is collected into Rx when that is not
 * NULL. The engine holds the chip select for the whole command.
 */
#define SFB_GENI_CHUNK 16

/* Words the transmit FIFO can hold; used to stop feeding before it overflows. */
#define SFB_TX_FIFO_DEPTH 16

STATIC
VOID
SfbGeniFillTx (
  IN     UINTN        SeBase,
  IN     CONST UINT8  *Tx,
  IN     UINT32       TransactLen,
  IN OUT UINT32       *Sent,
  IN OUT UINT32       *Remaining,
  IN     UINT32       PerWord
  )
{
  UINT32  Value;
  UINT32  Index;

  while ((*Remaining != 0) &&
         ((SfbGeniRead (SeBase + SFB_GENI_TX_FIFO_STATUS) & SFB_TX_FIFO_WC_MASK) <
          SFB_TX_FIFO_DEPTH))
  {
    Value = 0;
    for (Index = 0; (Index < PerWord) && (*Remaining != 0); Index++) {
      Value |= (UINT32)Tx[*Sent] << (8 * Index);
      (*Sent)++;
      (*Remaining)--;
    }

    SfbGeniWrite (SeBase + SFB_GENI_TX_FIFO, Value);
  }
}

STATIC
EFI_STATUS
SfbGeniTransferOnce (
  IN  UINTN  SeBase,
  IN  UINT32 Command,
  IN  CONST UINT8 *Tx,
  IN  UINT32 TransactLen,
  OUT UINT8  *Rx
  )
{
  UINT32      PerWord;
  UINT32      Remaining;
  UINT32      Received;
  UINT32      Sent;
  UINT32      RxPeak;
  UINT32      Irq;
  UINT32      Status;
  UINT32      Count;
  UINT32      Available;
  UINT32      Take;
  UINT32      Vector;
  UINT32      Index;
  UINT32      Value;
  UINT8       Bytes[4];
  UINT64      Elapsed;
  BOOLEAN     Started;

  if ((Tx == NULL) && (Rx == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  PerWord   = SfbGeniBytesPerFifoWord ();
  Remaining = 0;
  Received  = 0;
  Value     = 0;
  Sent      = 0;

  SfbGeniWrite (SeBase + SFB_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);

  if (Tx != NULL) {
    SfbGeniWrite (SeBase + SFB_SPI_TX_TRANS_LEN, TransactLen);
    Remaining = TransactLen;
  }

  if (Rx != NULL) {
    SfbGeniWrite (SeBase + SFB_SPI_RX_TRANS_LEN, TransactLen);
  }

  DEBUG ((EFI_D_INFO,
          "SFB: touch pre cmd=0x%x len=%u mcmd=0x%x mctrl=0x%x demux=0x%x\n"
          " xcfg=0x%x ios=0x%x\n",
          Command, TransactLen, SfbGeniRead (SeBase + SFB_GENI_M_CMD0),
          SfbGeniRead (SeBase + 0x604), SfbGeniRead (SeBase + 0x250),
          SfbGeniRead (SeBase + 0x25c), SfbGeniRead (SeBase + 0x908)));

  /*
   * Preload the transmit FIFO before the command starts. This is only the
   * first helping: the sequencer keeps pulling words as it shifts them out, so
   * the rest is fed from inside the poll loop below. A command that is not kept
   * supplied stops mid transfer and never reports completion.
   */
  SfbGeniFillTx (SeBase, Tx, TransactLen, &Sent, &Remaining, PerWord);

  Value = Command << SFB_M_CMD_OPCODE_SHIFT;
  if (SfbGeniRead (SeBase + SFB_GENI_M_CMD0) == Value) {
    /*
     * The previous revision timed out with this very command still staged, and
     * a command that never started is a strong hint that an interrupt is still
     * latched, so clear everything before retrying the exact same write.
     */
    SfbGeniWrite (SeBase + SFB_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  }

  SfbGeniWrite (SeBase + SFB_GENI_M_CMD0, Value);

  Elapsed = 0;
  Started = FALSE;
  RxPeak  = 0;
  for (;;) {
    Status = SfbGeniRead (SeBase + 0x40);
    Irq    = SfbGeniRead (SeBase + SFB_GENI_M_IRQ_STATUS);
    Count  = SfbGeniRead (SeBase + SFB_GENI_RX_FIFO_STATUS);

    /* Top the transmit FIFO up while the command is running. */
    if (Remaining != 0) {
      SfbGeniFillTx (SeBase, Tx, TransactLen, &Sent, &Remaining, PerWord);
    }

    /*
     * Move whatever the panel has already sent out of the receive FIFO. A
     * receive that is left to fill up stops the engine from making progress,
     * and the count is also the cheapest proof that the clock is really
     * running on the wire.
     */
    if ((Count & SFB_RX_FIFO_WC_MASK) != 0) {
      if ((Count & SFB_RX_FIFO_WC_MASK) > RxPeak) {
        RxPeak = Count & SFB_RX_FIFO_WC_MASK;
      }

      if (Rx != NULL) {
        Available = (Count & SFB_RX_FIFO_WC_MASK) * PerWord;
        if (Available > (TransactLen - Received)) {
          Available = TransactLen - Received;
        }

        while (Available != 0) {
          Take  = (Available > PerWord) ? PerWord : Available;
          Value = SfbGeniRead (SeBase + SFB_GENI_RX_FIFO);
          Bytes[0] = (UINT8)(Value & 0xFF);
          Bytes[1] = (UINT8)((Value >> 8) & 0xFF);
          Bytes[2] = (UINT8)((Value >> 16) & 0xFF);
          Bytes[3] = (UINT8)((Value >> 24) & 0xFF);
          for (Index = 0; Index < Take; Index++) {
            Rx[Received + Index] = Bytes[Index];
          }

          Received += Take;
          Available -= Take;
        }
      } else {
        Value = SfbGeniRead (SeBase + SFB_GENI_RX_FIFO);
      }
    }

    if (Irq & SFB_M_CMD_ERR_MASK) {
      DEBUG ((EFI_D_ERROR, "SFB: touch fifo irq=0x%x cmd=0x%x\n", Irq, Value));
      SfbGeniWrite (SeBase + SFB_GENI_M_CMD0, SFB_M_CMD_ABORT_SEQ);
      return EFI_DEVICE_ERROR;
    }

    if ((Irq & SFB_M_CMD_DONE) != 0) {
      break;
    }

    if (Started && (0 == (Status & 1))) {
      /*
       * The engine went idle without reporting completion; the command was
       * never accepted, so there is nothing to wait for.
       */
      DEBUG ((EFI_D_ERROR, "SFB: touch fifo idle no-done irq=0x%x cmd=0x%x\n",
              Irq, Value));
      SfbGeniWrite (SeBase + SFB_GENI_M_CMD0, SFB_M_CMD_ABORT_SEQ);
      return EFI_DEVICE_ERROR;
    }

    if ((Status & 1) != 0) {
      Started = TRUE;
    }

    if (Elapsed > SFB_FIFO_POLL_NS) {
      DEBUG ((EFI_D_ERROR,
              "SFB: touch fifo timeout irq=0x%x status=0x%x cmd=0x%x rxstat=0x%x\n"
              " txleft=%u txwc=0x%x gplen=0x%x rxpeak=%u rxlen=0x%x txlen=0x%x\n",
              Irq, Status, Value, Count, Remaining,
              SfbGeniRead (SeBase + SFB_GENI_TX_FIFO_STATUS) & SFB_TX_FIFO_WC_MASK,
              SfbGeniRead (SeBase + 0x910), RxPeak,
              SfbGeniRead (SeBase + SFB_SPI_RX_TRANS_LEN),
              SfbGeniRead (SeBase + SFB_SPI_TX_TRANS_LEN)));
      SfbGeniWrite (SeBase + SFB_GENI_M_CMD0, SFB_M_CMD_CANCEL_SEQ);
      return EFI_TIMEOUT;
    }

    gBS->Stall (20);
    Elapsed += 20 * 1000;
  }

  /*
   * The command is done, so the entire response has been clocked in and is
   * sitting in the receive FIFO. This is where the kernel handler drains it
   * too: M_CMD_DONE means every byte was transferred.
   */
  if (Rx != NULL) {
    while (Received < TransactLen) {
      Count = SfbGeniRead (SeBase + SFB_GENI_RX_FIFO_STATUS);
      if ((Count & SFB_RX_FIFO_WC_MASK) == 0) {
        break;
      }

      Available = (Count & SFB_RX_FIFO_WC_MASK) * PerWord;
      if (Count & SFB_RX_LAST) {
        Vector = (Count & SFB_RX_LAST_VALID_MASK) >> SFB_RX_LAST_VALID_SHIFT;
        if ((Vector != 0) && (Vector < 4) && (Available >= PerWord)) {
          Available -= (PerWord - Vector);
        }
      }

      if (Available > (TransactLen - Received)) {
        Available = TransactLen - Received;
      }

      while (Available != 0) {
        Take  = (Available > PerWord) ? PerWord : Available;
        Value = SfbGeniRead (SeBase + SFB_GENI_RX_FIFO);
        Bytes[0] = (UINT8)(Value & 0xFF);
        Bytes[1] = (UINT8)((Value >> 8) & 0xFF);
        Bytes[2] = (UINT8)((Value >> 16) & 0xFF);
        Bytes[3] = (UINT8)((Value >> 24) & 0xFF);
        for (Index = 0; Index < Take; Index++) {
          Rx[Received + Index] = Bytes[Index];
        }

        Received += Take;
        Available -= Take;
      }
    }
  }

  /*
   * Drain anything left over. A count that is never re-read would be handed to
   * the next command as if the TCM had answered it.
   */
  for (Index = 0; Index < 64; Index++) {
    Count = SfbGeniRead (SeBase + SFB_GENI_RX_FIFO_STATUS);
    if ((Count & SFB_RX_FIFO_WC_MASK) == 0) {
      break;
    }

    Value = SfbGeniRead (SeBase + SFB_GENI_RX_FIFO);
    (VOID)Value;
  }

  SfbGeniWrite (SeBase + SFB_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);

  DEBUG ((EFI_D_INFO, "SFB: touch fifo done cmd=0x%x rx=%u\n",
          Command, Received));

  return EFI_SUCCESS;
}

/*
 * Synaptics TCM over SPI, straight from the kernel synaptics_tcm driver.
 *
 * A command is three bytes on the wire - the command itself followed by a
 * little endian payload length - and nothing is clocked back:
 *
 *     write_cmd (cmd, NULL, 0)  ->  { cmd, 0x00, 0x00 }
 *
 * A read sends 0xFF dummy bytes and collects the answer in the same transfer:
 * an acknowledge byte, then a four byte header { 0xA5, code, length[2] }, then
 * the payload. The touch controller only answers once it is out of reset and
 * powered, so an all zero answer here means the reset and power work is not
 * done yet, not that the host side is broken.
 */
#define SFB_TCM_CMD_IDENTIFY       0x02
#define SFB_TCM_CMD_IDENTIFY_HBP   0x07
#define SFB_TCM_MARKER             0xA5
#define SFB_TCM_REPORT_IDENTIFY    0x10
#define SFB_TCM_REPORT_HBP_FRAME   0x23
#define SFB_TCM_READ_LEN           64

/*
 * Break lengths above the chunk size into bounded full duplex steps. A single
 * long command makes the receive FIFO fill while the transmit side is still
 * running, and once it does the engine stops making progress. Fewer bytes per
 * command keeps both FIFOs comfortably inside their depth.
 */
STATIC
EFI_STATUS
SfbGeniTransfer (
  IN  UINTN  SeBase,
  IN  UINT32 Command,
  IN  CONST UINT8 *Tx,
  IN  UINT32 TransactLen,
  OUT UINT8  *Rx
  )
{
  UINT32      Chunk;
  UINT32      Offset;
  EFI_STATUS  Status;

  Chunk = (TransactLen > SFB_GENI_CHUNK) ? SFB_GENI_CHUNK : TransactLen;

  DEBUG ((EFI_D_INFO, "SFB: touch fifo start cmd=0x%x len=%u chunk=%u\n",
          Command, TransactLen, Chunk));

  for (Offset = 0; Offset < TransactLen; Offset += Chunk) {
    Status = SfbGeniTransferOnce (
               SeBase,
               Command,
               (Tx != NULL) ? (Tx + Offset) : NULL,
               Chunk,
               (Rx != NULL) ? (Rx + Offset) : NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB: touch fifo chunk at %u -> %r\n",
              Offset, Status));
      return Status;
    }

  }

  return EFI_SUCCESS;
}

/* Send one TCM command: command byte plus a zero payload length. */
STATIC
EFI_STATUS
SfbProbeSend (
  IN UINTN  SeBase,
  IN UINT8  Command
  )
{
  UINT8  Frame[3];

  Frame[0] = Command;
  Frame[1] = 0x00;
  Frame[2] = 0x00;

  return SfbGeniTransfer (SeBase, SFB_SPI_TX_ONLY, Frame, sizeof (Frame), NULL);
}

/* Clock 0xFF dummy bytes out while collecting whatever the TCM answers. */
STATIC
EFI_STATUS
SfbProbeRead (
  IN  UINTN  SeBase,
  IN  UINT32 TransactLen,
  OUT UINT8  *Rx
  )
{
  UINT8  Frame[SFB_TCM_READ_LEN];

  if (TransactLen > sizeof (Frame)) {
    return EFI_INVALID_PARAMETER;
  }

  /* 0xFF is the idle byte the kernel driver clocks out on reads. */
  SetMem (Frame, sizeof (Frame), 0xFF);

  return SfbGeniTransfer (SeBase, SFB_SPI_TX_RX, Frame, TransactLen, Rx);
}

/*
 * Report whether the TCM answers at all. The kernel is not sure whether this
 * part speaks plain TCM (0x02) or the HBP variant (0x07), so both are tried and
 * the reply is decoded when the marker shows up anywhere in the report - what
 * precedes the header is an acknowledge, not part of it.
 */
STATIC
BOOLEAN
SfbProbeQuery (
  IN UINTN  SeBase
  )
{
  CONST UINT8  Commands[2] = { SFB_TCM_CMD_IDENTIFY_HBP, SFB_TCM_CMD_IDENTIFY };
  UINT8        Rx[SFB_TCM_READ_LEN];
  UINT32       Attempt;
  UINT32       Index;
  UINT32       Offset;
  UINT32       NonZero;
  EFI_STATUS   Status;
  BOOLEAN      Found = FALSE;

  for (Attempt = 0; (Attempt < 2) && !Found; Attempt++) {
    Status = SfbProbeSend (SeBase, Commands[Attempt]);
    DEBUG ((EFI_D_INFO, "SFB: touch send cmd=0x%02x -> %r\n",
            Commands[Attempt], Status));

    SfbProbePut (L"tx ");
    SfbProbePutHex (Commands[Attempt]);
    SfbProbePut (EFI_ERROR (Status) ? L" FAIL" : L" OK");
    SfbProbeNewLine ();

    if (EFI_ERROR (Status)) {
      continue;
    }

    gBS->Stall (3000);

    SetMem (Rx, sizeof (Rx), 0);
    Status = SfbProbeRead (SeBase, SFB_TCM_READ_LEN, Rx);
    DEBUG ((EFI_D_INFO, "SFB: touch read cmd=0x%02x -> %r\n",
            Commands[Attempt], Status));

    NonZero = 0;
    Offset  = SFB_TCM_READ_LEN;
    for (Index = 0; Index < SFB_TCM_READ_LEN; Index++) {
      if (Rx[Index] != 0) {
        NonZero++;
        if ((Rx[Index] == SFB_TCM_MARKER) && (Offset == SFB_TCM_READ_LEN)) {
          Offset = Index;
        }
      }
    }

    SfbProbeDump ("read", Rx, 32);

    SfbProbePut (L"rx nz=");
    SfbProbePutDec (NonZero);
    SfbProbePut (L" a5@");
    SfbProbePutDec (Offset);
    SfbProbeNewLine ();

    if (Offset + 3 < SFB_TCM_READ_LEN) {
      DEBUG ((EFI_D_INFO, "SFB: touch MARKER code=0x%02x len=%u at=%u\n",
              Rx[Offset + 1],
              (UINT32)Rx[Offset + 2] | ((UINT32)Rx[Offset + 3] << 8), Offset));
      SfbProbePut (L"code=");
      SfbProbePutHex (Rx[Offset + 1]);
      SfbProbePut (L" len=");
      SfbProbePutDec ((UINT32)Rx[Offset + 2] | ((UINT32)Rx[Offset + 3] << 8));

      if ((Rx[Offset + 1] == SFB_TCM_REPORT_IDENTIFY) ||
          (Rx[Offset + 1] == SFB_TCM_REPORT_HBP_FRAME)) {
        Found = TRUE;
        SfbProbePut (L" TCM OK");
      }

      SfbProbeNewLine ();
    }

    gBS->Stall (3000);
  }

  if (!Found) {
    SfbProbePut (L"no TCM answer (reset/power?)");
    SfbProbeNewLine ();
  }

  return Found;
}

STATIC
BOOLEAN
SfbProbeSePass (
  IN UINTN   SeBase,
  IN BOOLEAN Initial
  );

/*
 * Open the touch engine and run one pass over it. This is the vendor call that
 * powers the engine, muxes its pins through the pin controller, configures its
 * clock and puts it in FIFO mode. The panel is not addressed in the initial
 * pass, because it is still held in reset until the pin work is done.
 */
STATIC
VOID
SfbProbeSeSetup (IN SFB_SPI_INTERFACE *Spi, IN CONST UINT8 *Base)
{
  SFB_SPI_OPEN   Open;
  SFB_SPI_CLOSE  Close;
  EFI_STATUS     Status;
  VOID           *Handle = NULL;
  UINTN          SeBase;

  if (!mMapValidated ||
      !SfbProbeCodeAt (Base, SFB_SPI_OPEN_RVA, mFpOpen))
  {
    DEBUG ((EFI_D_ERROR, "SFB: touch setup skipped\n"));
    return;
  }

  Open  = (SFB_SPI_OPEN)(UINTN)Spi->Open;
  Close = (SFB_SPI_CLOSE)(UINTN)Spi->Close;

  Status = Open (SFB_TOUCH_INDEX, &Handle);
  DEBUG ((EFI_D_INFO, "SFB: touch setup open idx=%u -> %r handle=%p\n",
          SFB_TOUCH_INDEX, Status, Handle));

  if (EFI_ERROR (Status) || Handle == NULL) {
    return;
  }

  SeBase = SFB_TOUCH_SE_BASE;
  DEBUG ((EFI_D_INFO, "SFB: touch se regs rev=0x%x iface=0x%x dmamode=0x%x\n",
          SfbGeniRead (SeBase + 0x68), SfbGeniRead (SeBase + SFB_GENI_IF_DISABLE_RO),
          SfbGeniRead (SeBase + SFB_GENI_DMA_MODE_EN)));

  if (!SfbProbeSePass (SeBase, TRUE)) {
    DEBUG ((EFI_D_ERROR, "SFB: touch setup pass failed\n"));
  }

  /*
   * Ask the panel who it is *before* the reset line is touched. If the rails
   * survived a warm reboot from Android and the controller was never put back
   * into reset, this is the attempt that answers, and it also separates "the
   * panel has no power" from "the panel needs a reset pulse" without changing
   * anything.
   */
  DEBUG ((EFI_D_INFO, "SFB: touch early identify\n"));
  SfbProbeQuery (SeBase);

  if (SfbProbeCodeAt (Base, SFB_SPI_CLOSE_RVA, mFpClose)) {
    Status = Close (Handle);
    DEBUG ((EFI_D_INFO, "SFB: touch setup close -> %r\n", Status));
  }
}

/*
 * The second pass, run once the panel has been taken out of reset: open the
 * engine again and ask the controller who it is.
 */
STATIC
VOID
SfbProbeOpenTry (IN SFB_SPI_INTERFACE *Spi, IN CONST UINT8 *Base)
{
  SFB_SPI_OPEN   Open;
  SFB_SPI_CLOSE  Close;
  EFI_STATUS     Status;
  VOID           *Handle = NULL;
  UINTN          SeBase;

  if (!mMapValidated ||
      !SfbProbeCodeAt (Base, SFB_SPI_OPEN_RVA, mFpOpen))
  {
    DEBUG ((EFI_D_ERROR, "SFB: touch open skipped\n"));
    return;
  }

  Open  = (SFB_SPI_OPEN)(UINTN)Spi->Open;
  Close = (SFB_SPI_CLOSE)(UINTN)Spi->Close;

  Status = Open (SFB_TOUCH_INDEX, &Handle);
  DEBUG ((EFI_D_INFO, "SFB: touch open idx=%u -> %r handle=%p\n",
          SFB_TOUCH_INDEX, Status, Handle));

  if (EFI_ERROR (Status) || Handle == NULL) {
    return;
  }

  SeBase = SFB_TOUCH_SE_BASE;
  if (!SfbProbeSePass (SeBase, FALSE)) {
    DEBUG ((EFI_D_ERROR, "SFB: touch second pass failed\n"));
  }

  if (SfbProbeCodeAt (Base, SFB_SPI_CLOSE_RVA, mFpClose)) {
    Status = Close (Handle);
    DEBUG ((EFI_D_INFO, "SFB: touch close -> %r\n", Status));
  }
}

/*
 * One pass over the serial engine: check the window, put it in a known state,
 * then either just report the line state (first pass) or run the Synaptics
 * identify (second pass, after reset has been released).
 *
 * @param Initial  TRUE for the pass that runs before the panel is reset, which
 *                 must not try to talk to it.
 *
 * @return TRUE when the register window is readable and the pass completed.
 */
STATIC
BOOLEAN
SfbProbeSePass (
  IN UINTN   SeBase,
  IN BOOLEAN Initial
  )
{
  UINT8   Rx[SFB_TCM_READ_LEN];
  UINT32  Current;
  EFI_STATUS Status;

  Current = SfbGeniRead (SeBase + 0x68);
  if ((Current == 0) || (Current == 0xFFFFFFFF)) {
    DEBUG ((EFI_D_ERROR, "SFB: touch se window unreadable (0x%x)\n", Current));
    return FALSE;
  }

  SfbProbePut (L"SE rev=");
  SfbProbePutHex (Current);
  SfbProbePut (L" ifdis=");
  SfbProbePutHex (SfbGeniRead (SeBase + SFB_GENI_IF_DISABLE_RO));
  SfbProbePut (L" dma=");
  SfbProbePutHex (SfbGeniRead (SeBase + SFB_GENI_DMA_MODE_EN));
  SfbProbeNewLine ();

  SfbGeniQuiesce (SeBase);
  SfbGeniConfigPacking (SeBase);

  /*
   * SE_GENI_IOS exposes the serial lines themselves: bit 0 is the data input.
   * The kernel device tree puts MISO on TLMM gpio48 with the QUP4 SE0 function,
   * so a line that never moves off zero while the engine is running hints at the
   * pin mux rather than at the transfer code.
   */
  Current = SfbGeniRead (SeBase + 0x908);
  DEBUG ((EFI_D_INFO,
          "SFB: touch se status=0x%x irqen=0x%x irq=0x%x rxfifo=0x%x ios=0x%x\n",
          SfbGeniRead (SeBase + 0x40),
          SfbGeniRead (SeBase + SFB_GENI_M_IRQ_EN),
          SfbGeniRead (SeBase + SFB_GENI_M_IRQ_STATUS),
          SfbGeniRead (SeBase + SFB_GENI_RX_FIFO_STATUS), Current));

  SfbProbePut (L"ios=");
  SfbProbePutHex (Current);
  SfbProbeNewLine ();

  if (Initial) {
    /*
     * Nothing is asked of the panel yet: it is still held in reset, so the
     * only useful reading here is whether the engine sees the line idle.
     */
    Current = SfbGeniRead (SeBase + 0x908);
    DEBUG ((EFI_D_INFO, "SFB: touch ios before reset=0x%x\n", Current));
  } else {
    SetMem (Rx, sizeof (Rx), 0);
    Status = SfbProbeRead (SeBase, SFB_TCM_READ_LEN, Rx);
    DEBUG ((EFI_D_INFO, "SFB: touch passive -> %r\n", Status));
    SfbProbeDump ("passive", Rx, 8);

    SfbProbeQuery (SeBase);
  }

  return TRUE;
}




STATIC
VOID
SfbProbeShowScreen (VOID);

STATIC
EFI_STATUS
SfbPmicRead (
  IN  UINT64  Iface,
  IN  UINT32  Addr,
  OUT UINT8   *Value
  );

typedef EFI_STATUS (EFIAPI *SFB_SPMI_XFER) (IN UINT64 Iface, IN UINT32 Bus,
                                            IN UINT32 Sid, IN UINT32 Cmd,
                                            IN UINT32 Address, OUT UINT8 *Buf,
                                            IN UINT32 Len, IN UINT32 Flag);

/*
 * The SPMI protocol, published by the SPMI module. Its interface lives at
 * .data+0x8098 of that module and carries two entry points that were read out
 * of the disassembly: +0x08 reads a register into a buffer, +0x10 writes one.
 */
#define SFB_SPMI_PROTOCOL_GUID \
  { 0xFA5F306B, 0xF47D, 0x4AC4, \
    { 0xA4, 0x7D, 0x88, 0x2F, 0x82, 0x04, 0xEC, 0x30 } }

#define SFB_SPMI_READ_OFFSET   0x28      /* method used by PmicDxe's reader */
#define SFB_PMIC_GPIO_SID      3         /* pmh0110 die D, bus 0           */
#define SFB_PMIC_GPIO_TYPE     0x8804
#define SFB_PMIC_GPIO_SUBTYPE  0x8805
/*
 * pmic gpio register addresses. The gpio controller node sits at 0x8800 and
 * every gpio owns 0x100 bytes of it with the same layout, straight out of the
 * kernel's pinctrl-spmi-gpio driver:
 *
 *   MODE_CTL +0x40   direction in bits 6..4, 1 = digital output
 *   EN_CTL   +0x46   bit 7 enables the gpio
 *
 * so gpio 9 is 0x8800 + 9*0x100 = 0x8900, MODE_CTL 0x8940, EN_CTL 0x8946.
 * An earlier revision added a 0x1000 block offset here, which is not part of
 * this layout: it wrote 0x800 past the target register and took the platform
 * down. The out of range probe at 0x9800 answered EFI_INVALID_PARAMETER, which
 * is what a bad address looks like; 0x9140 was inside the block and therefore
 * looked plausible while being wrong.
 */
#define SFB_PMIC_GPIO9_BASE    0x9100
#define SFB_PMIC_GPIO9_MODE    (SFB_PMIC_GPIO9_BASE + 0x40)
#define SFB_PMIC_GPIO9_OUTSRC  (SFB_PMIC_GPIO9_BASE + 0x44)
#define SFB_PMIC_GPIO9_EN      (SFB_PMIC_GPIO9_BASE + 0x46)
#define SFB_PMIC_GPIO9_ANA     (SFB_PMIC_GPIO9_BASE + 0x4A)

/* PMIC_GPIO_MODE_DIGITAL_OUTPUT, and function 1 in the source selector. */
#define SFB_PMIC_GPIO_OUTPUT   0x01U
#define SFB_PMIC_GPIO_SRC_FUNC 0x01U

typedef struct SfbSpmiXferS SFB_SPMI_XFER_T;

STATIC
VOID
SfbProbeSpmi (VOID)
{
  EFI_GUID    Guid = SFB_SPMI_PROTOCOL_GUID;
  UINT64      Iface;
  EFI_STATUS  Status;
  UINT8       Type;
  UINT8       Subtype;

  SetMem (mScreen, sizeof (mScreen), 0);
  mScreenLines  = 0;
  mScreenColumn = 0;

  SfbProbePut (L"SPMI identity (read only)");
  SfbProbeNewLine ();

  Iface  = 0;
  Status = gBS->LocateProtocol (&Guid, NULL, (VOID **)&Iface);
  if (EFI_ERROR (Status) || (Iface == 0)) {
    SfbProbePut (L"spmi protocol MISSING");
    SfbProbeNewLine ();
    SfbProbeShowScreen ();
    return;
  }

  /*
   * Two reads only, both of them proven safe on hardware and both of them
   * matching the kernel's own view of this slave: the gpio block answers 0x10
   * (PMIC_GPIO_TYPE) at 0x8804 and 0x15 (the low/mid voltage gpio subtype) at
   * 0x8805.
   *
   * A wider scan of the gpio 9 block at 0x9100 crashed the platform, and so did
   * every attempt to write a pmic register, so this is deliberately the whole
   * of the pmic side now: two reads that are known to be harmless. See
   * wiki/docs/touch-stage0.md.
   */
  Type    = 0;
  Subtype = 0;
  SfbPmicRead (Iface, 0x8804, &Type);
  SfbPmicRead (Iface, 0x8805, &Subtype);

  DEBUG ((EFI_D_INFO, "SFB: pmic block type=0x%x subtype=0x%x\n", Type, Subtype));

  SfbProbePut (L"type=");
  SfbProbePutHex (Type);
  SfbProbePut (L" sub=");
  SfbProbePutHex (Subtype);
  SfbProbeNewLine ();
  SfbProbeShowScreen ();
}

/*
 * PMIC register access through the SPMI protocol interface.
 *
 * The interface carries two separate entries, and the disassembly of the module
 * shows they differ only in the operation code passed down to the shared core:
 *
 *   +0x08  read   (base, bus, sid, addr, buf, len, out)   op 1
 *   +0x10  write  (base, bus, sid, addr, buf, len)        op 0
 *
 * There is no command argument. An earlier revision passed one, which shifted
 * every following argument by one slot, so its "write" was really a read of
 * address 2 and wrote nothing at all: the target register came back unchanged.
 *
 * The read entry is proven on hardware - the gpio block of pmh0110 slave 3
 * answers 0x10 (PMIC_GPIO_TYPE) at 0x8804 and 0x15 (the MV gpio subtype) at
 * 0x8805.
 */
STATIC
EFI_STATUS
SfbPmicRead (
  IN  UINT64  Iface,
  IN  UINT32  Addr,
  OUT UINT8   *Value
  )
{
  UINT64  Fn;
  UINT64  Out;

  Fn  = *(UINT64 *)(UINTN)(Iface + 0x08);
  Out = 0;

  return ((EFI_STATUS (EFIAPI *)(IN UINT64 Base, IN UINT32 Bus, IN UINT32 Sid,
                                 IN UINT32 Addr, OUT UINT8 *Buf, IN UINT32 Len,
                                 OUT UINT64 *Out))(UINTN)Fn)
           (Iface, 0, SFB_PMIC_GPIO_SID, Addr, Value, 1, &Out);
}


/*
 * Bring up the panel's analogue rail and release its reset.
 *
 * touch_avdd_2v8 is a regulator-fixed switched by pmh0110 die D gpio 9 (device
 * tree), and the pmic gpio block on that slave answers its identity registers,
 * so the address is settled. Register layout comes from the kernel's
 * pinctrl-spmi-gpio driver:
 *
 *   block = 0x8800 + 0x1000 + gpio * 0x100        -> gpio 9 at 0x9100
 *   +0x40 MODE_CTL   direction in bits 6..4, 1 = digital output
 *   +0x46 EN_CTL     bit 7 enables the gpio
 *
 * The rail is active high, so enabling the output drives it high, which is what
 * switches avdd on. Everything is read back before and after, and the panel
 * itself answers on SPI if the rail really came up.
 */
STATIC
VOID
SfbProbePowerOn (IN SFB_SPMI_XFER_T *Unused)
{
  (VOID)Unused;

  /*
   * Dormant on purpose, and this is a documented decision rather than a
   * leftover. Four attempts to bring the panel's analogue rail up through the
   * pmic took the platform to 900E every time, and every one of those crashes
   * wiped logfs, so the attempts were also undiagnosable.
   *
   * The kernel driver shows why the guesses failed: this pad is a low/mid
   * voltage gpio whose enable needs the voltage source, the pull and the
   * output buffer tuple programmed together, not just a direction bit. None of
   * that can be derived from a read only probe, and the hardware does not
   * tolerate being experimented on.
   *
   * Everything else in this file stays: the serial engine work, the verified
   * reset write on the pin controller, and the read only probes. See
   * wiki/docs/touch-stage0.md.
   */
  DEBUG ((EFI_D_INFO, "SFB: pmic power-on disabled (see touch-stage0.md)\n"));
  return;
}

/*
 * Drive the touch reset line, and nothing else.
 *
 * The read only pass established what this relies on: the pin controller window
 * is mapped and readable (pin 159 ctl=0x1 io=0x0, pin 158 ctl=0x1, pin 48
 * ctl=0x1801), and pin 159 already reads as plain gpio, so its function field
 * needs no change. An earlier plan to rewrite the whole control register was
 * unnecessary and is exactly what is not done here.
 *
 * The driver that actually runs on this device (syna_tcm2_spi.c, loaded as
 * oplus_bsp_tp_hbp_syna_s3910) resets the panel like this:
 *
 *   gpio_set_value(reset, 0);  msleep(10);   -- assert, the line is active low
 *   gpio_set_value(reset, 1);  msleep(80);   -- release
 *
 * so only the data register is written, bit 1 low and then bit 1 high, with
 * every other bit preserved. The control register is left as the boot firmware
 * left it.
 */
#define SFB_TLMM_BASE           0x0F100000
#define SFB_TLMM_STRIDE         0x1000
#define SFB_TLMM_IO(Pin)        (SFB_TLMM_BASE + ((Pin) * SFB_TLMM_STRIDE) + 0x4)
#define SFB_TLMM_OUT_BIT        0x2U
#define SFB_TOUCH_RESET_PIN     159

STATIC
VOID
SfbProbeTlmmReset (IN SFB_TLMM_PROTOCOL *Tlmm)
{
  UINT32  Io;
  UINT32  After;

  (VOID)Tlmm;

  SfbProbePut (L"reset159");
  SfbProbeNewLine ();

  Io = SfbGeniRead (SFB_TLMM_IO (SFB_TOUCH_RESET_PIN));
  DEBUG ((EFI_D_INFO, "SFB: tlmm159 io before=0x%x\n", Io));

  /* Assert: one bit cleared, the rest of the register preserved. */
  SfbGeniWrite (SFB_TLMM_IO (SFB_TOUCH_RESET_PIN), Io & ~SFB_TLMM_OUT_BIT);
  gBS->Stall (10 * 1000);

  /* Release: the panel comes out of reset here. */
  SfbGeniWrite (SFB_TLMM_IO (SFB_TOUCH_RESET_PIN), Io | SFB_TLMM_OUT_BIT);
  gBS->Stall (80 * 1000);

  After = SfbGeniRead (SFB_TLMM_IO (SFB_TOUCH_RESET_PIN));
  DEBUG ((EFI_D_INFO, "SFB: tlmm159 io after=0x%x released=%u\n",
          After, (UINT32)((After & SFB_TLMM_OUT_BIT) != 0)));

  SfbProbePut (L" io=");
  SfbProbePutHex (After);
  SfbProbeNewLine ();
}

/* Paint whatever the diagnostic collected and leave it up to be read. */
STATIC
VOID
SfbProbeShowScreen (VOID)
{
  /*
   * Deliberately empty. The probe reports through logfs and the screen belongs
   * to the boot menu: painting a diagnostic over it and holding it there both
   * hid the menu and cost seconds on every boot.
   */
  return;
}

/*
 * Read only look at the pin controller window, kept as the evidence step: a
 * crash wipes the firmware log, so the values also go on screen.
 */
STATIC
VOID
SfbProbeTlmmRead (VOID)
{
  UINT32  Ctl159;
  UINT32  Io159;
  UINT32  Ctl158;
  UINT32  Ctl48;
  UINT32  Mux159;
  UINT32  Oe159;
  UINT32  Drv159;
  UINT32  Pull159;

  SetMem (mScreen, sizeof (mScreen), 0);
  mScreenLines  = 0;
  mScreenColumn = 0;

  SfbProbePut (L"TLMM read only");
  SfbProbeNewLine ();

  Ctl159 = SfbGeniRead (SFB_TLMM_BASE + (159 * SFB_TLMM_STRIDE));
  Io159  = SfbGeniRead (SFB_TLMM_IO (159));
  Ctl158 = SfbGeniRead (SFB_TLMM_BASE + (158 * SFB_TLMM_STRIDE));
  Ctl48  = SfbGeniRead (SFB_TLMM_BASE + (48 * SFB_TLMM_STRIDE));

  DEBUG ((EFI_D_INFO, "SFB: tlmm159 ctl=0x%x io=0x%x\n", Ctl159, Io159));
  DEBUG ((EFI_D_INFO, "SFB: tlmm158 ctl=0x%x tlmm48 ctl=0x%x\n", Ctl158, Ctl48));

  SfbProbePut (L"159 ctl=");
  SfbProbePutHex (Ctl159);
  SfbProbePut (L" io=");
  SfbProbePutHex (Io159);
  SfbProbeNewLine ();

  SfbProbePut (L"158 ctl=");
  SfbProbePutHex (Ctl158);
  SfbProbePut (L" 48 ctl=");
  SfbProbePutHex (Ctl48);
  SfbProbeNewLine ();

  Mux159  = (Ctl159 >> 2) & 7;
  Oe159   = (Ctl159 >> 9) & 1;
  Drv159  = (Ctl159 >> 6) & 7;
  Pull159 = Ctl159 & 3;

  SfbProbePut (L"159 mux=");
  SfbProbePutDec (Mux159);
  SfbProbePut (L" oe=");
  SfbProbePutDec (Oe159);
  SfbProbePut (L" drv=");
  SfbProbePutDec (Drv159);
  SfbProbePut (L" pull=");
  SfbProbePutDec (Pull159);
  SfbProbeNewLine ();

  SfbProbeShowScreen ();
}

STATIC
VOID
SfbProbeTlmm (IN SFB_TLMM_PROTOCOL *Tlmm)
{
  EFI_GUID     TlmmGuid = SFB_TLMM_PROTOCOL_GUID;
  CONST UINT8  *Base;
  UINT32       Index;
  UINT32       Matches = 0;

  Base = (CONST UINT8 *)Tlmm - SFB_TLMM_IFACE_RVA;

  if (!SfbProbeGuidAt (Base, SFB_TLMM_GUID_RVA, &TlmmGuid)) {
    DEBUG ((EFI_D_ERROR, "SFB: touch tlmm base check FAILED\n"));
    SfbProbePut (L"TLMM guid check FAILED");
    SfbProbeNewLine ();
    return;
  }

  for (Index = 0; Index < 5; Index++) {
    if (Tlmm->Method[Index] ==
        (UINT64)(UINTN)(Base + mTlmmMethodRva[Index])) {
      Matches++;
    }

    DEBUG ((EFI_D_INFO, "SFB: touch tlmm m%u=%lx (want %lx)\n", Index,
            Tlmm->Method[Index],
            (UINT64)(UINTN)(Base + mTlmmMethodRva[Index])));
  }

  SfbProbePut (L"TLMM base=");
  SfbProbePutHex ((UINT64)(UINTN)Base);
  SfbProbePut (L" ver=");
  SfbProbePutHex (Tlmm->Version);
  SfbProbeNewLine ();
  SfbProbePut (L"TLMM fnmatch=");
  SfbProbePutDec (Matches);
  SfbProbePut (L"/5");
  SfbProbeNewLine ();
}

VOID
SfbTouchProbe (VOID)
{
  EFI_GUID            SpiGuid  = SFB_SPI_PROTOCOL_GUID;
  EFI_GUID            TlmmGuid = SFB_TLMM_PROTOCOL_GUID;
  EFI_GUID            GpiGuid  = SFB_GPI_PROTOCOL_GUID;
  SFB_SPI_INTERFACE   *Spi     = NULL;
  CONST UINT8         *SpiBase = NULL;
  SFB_TLMM_PROTOCOL   *Tlmm    = NULL;
  VOID                *Gpi     = NULL;
  EFI_STATUS          Status;

  SetMem (mScreen, sizeof (mScreen), 0);
  mScreenLines  = 0;
  mScreenColumn = 0;

  SfbProbePut (L"SFB TOUCH PROBE v35 read-only");
  SfbProbeNewLine ();

  Status = gBS->LocateProtocol (&SpiGuid, NULL, (VOID **)&Spi);
  DEBUG ((EFI_D_INFO, "SFB: touch SPI protocol %r ptr=%p\n", Status, Spi));
  SfbProbePut (L"SPI proto ");
  SfbProbePut (EFI_ERROR (Status) || Spi == NULL ? L"MISSING" : L"ok");
  SfbProbeNewLine ();

  Status = gBS->LocateProtocol (&TlmmGuid, NULL, (VOID **)&Tlmm);
  DEBUG ((EFI_D_INFO, "SFB: touch TLMM protocol %r ptr=%p\n", Status, Tlmm));
  SfbProbePut (L"TLMM proto ");
  SfbProbePut (EFI_ERROR (Status) || Tlmm == NULL ? L"MISSING" : L"ok");
  SfbProbeNewLine ();

  Status = gBS->LocateProtocol (&GpiGuid, NULL, (VOID **)&Gpi);
  DEBUG ((EFI_D_INFO, "SFB: touch GPI protocol %r ptr=%p\n", Status, Gpi));
  SfbProbePut (L"GPI proto ");
  SfbProbePut (EFI_ERROR (Status) || Gpi == NULL ? L"MISSING" : L"ok");
  SfbProbeNewLine ();
  SfbProbeNewLine ();

  if (Spi != NULL) {
    SpiBase = (CONST UINT8 *)Spi - SFB_SPI_IFACE_RVA;
    SfbProbeSpi (Spi);
  } else {
    SfbProbePut (L"SPI protocol absent");
    SfbProbeNewLine ();
  }

  SfbProbeNewLine ();

  if (Tlmm != NULL) {
    SfbProbeTlmm (Tlmm);
  }

  /*
   * Release the panel from reset before the engine is brought up. This is the
   * whole point of the TLMM work: without it the controller never drives MISO
   * and every transfer on this bus waits forever for a reply.
   */
  if (SpiBase != NULL) {
    SfbProbeMapper (SpiBase);

    /*
     * Two passes. The first one only proves the serial engine window is live,
     * because the memory mapped pin controller is brought in by the vendor
     * open() call - the same call that muxes this engine's pins - and that has
     * to happen before the reset line may be touched. The second pass runs
     * after reset is released and is the one that talks to the panel.
     */
    SfbProbeSeSetup (Spi, SpiBase);
    SfbProbeTlmmRead ();
    SfbProbeSpmi ();
    SfbProbePowerOn (NULL);
    SfbProbeTlmmReset (Tlmm);
    SfbProbeOpenTry (Spi, SpiBase);
  }

  DEBUG ((EFI_D_INFO, "SFB: touch probe done\n"));
}
