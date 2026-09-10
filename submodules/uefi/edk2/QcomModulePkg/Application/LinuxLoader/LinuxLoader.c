/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *
 *  Copyright (c) 2022 - 2025 Qualcomm Innovation Center, Inc. All rights
 *  reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted (subject to the limitations in the
 *  disclaimer below) provided that the following conditions are met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials provided
 *        with the distribution.
 *
 *      * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *        contributors may be used to endorse or promote products derived
 *        from this software without specific prior written permission.
 *
 *  NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 *  GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 *  HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 *  DAMAGE.
 */

#include "AutoGen.h"
#include "LinuxLoaderLib.h"
#include <FastbootLib/FastbootMain.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/ShutdownServices.h>
#include <Library/StackCanary.h>
#include "Library/ThreadStack.h"
#include <Protocol/EFICardInfo.h>
#include <Protocol/SimpleTextIn.h>
#include "SuperFbMenu.h"
#include "SuperFbGfx.h"

/*
 * OPPO/OnePlus "Phoenix" boot watchdog (PhoenixDxe in the platform UEFI FV).
 *
 * PhoenixDxe arms a private one-shot timer during DXE - 60 s on a normal boot -
 * and resets the device if the boot has not reached Android when it fires. That
 * timer is an event of its own, not the UEFI watchdog timer architectural
 * protocol, so the gBS->SetWatchdogTimer() disarm the fastboot path performs
 * cannot cancel it. Sitting in the boot menu or in the on-device fastboot
 * screen for longer than the timeout therefore reboots the device.
 *
 * The driver also installs a protocol whose third method cancels the timer
 * (SetTimer(TimerCancel) + CloseEvent). Calling it keeps the menu alive for as
 * long as the user needs. Boot modes the handler treats as "not normal" (the
 * official fastboot) are ignored by Phoenix anyway, which is why only the menu
 * and the superfastboot fastboot screen were affected.
 *
 * The layout below was recovered from the shipped PhoenixDxe binary: interface
 * table at image offset 0x9158, DisableTimer at +0x18 (code at 0x1c88).
 */
#define PHOENIX_PROTOCOL_GUID \
  { 0x7D2A39F3, 0x0F8C, 0x47A0, { 0x9B, 0x51, 0xF2, 0x69, 0xB4, 0xBA, 0xF9, 0x93 } }

typedef struct {
  UINT64                     Version;                   /* +0x00 */
  VOID                       *Reserved0;                /* +0x08 */
  VOID                       *Reserved1;                /* +0x10 */
  EFI_STATUS (EFIAPI         *DisableTimer) (VOID);     /* +0x18 */
  VOID                       *Reserved2;                /* +0x20 */
} PHOENIX_PROTOCOL;

/*
 * Cancel the Phoenix boot watchdog when the platform provides it. A missing
 * protocol is normal on devices without Phoenix, so log it at INFO level only.
 */
STATIC
VOID
SfbDisablePhoenixWatchdog (VOID)
{
  EFI_GUID          PhoenixGuid = PHOENIX_PROTOCOL_GUID;
  PHOENIX_PROTOCOL  *Phoenix = NULL;
  EFI_STATUS        Status;

  Status = gBS->LocateProtocol (&PhoenixGuid, NULL, (VOID **)&Phoenix);
  if (EFI_ERROR (Status) || Phoenix == NULL || Phoenix->DisableTimer == NULL) {
    DEBUG ((EFI_D_INFO, "SFB: phoenix watchdog not present: %r\n", Status));
    return;
  }

  Status = Phoenix->DisableTimer ();
  DEBUG ((EFI_D_INFO, "SFB: phoenix watchdog disable -> %r\n", Status));
}

#define MAX_APP_STR_LEN 64
#define MAX_NUM_FS 10
#define DEFAULT_STACK_CHK_GUARD 0xc0c0c0c0

/**
  Linux Loader Application EntryPoint

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

 **/

EFI_STATUS EFIAPI  __attribute__ ( (no_sanitize ("safe-stack")))
LinuxLoaderEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status;

   /* Update stack check guard with random value for better security */
  /* SilentMode Boot */
  /* MultiSlot Boot */
  /* Flashless Boot */
  EFI_MEM_CARDINFO_PROTOCOL *CardInfo = NULL;
  /* set ROT, BootState and VBH only once per boot*/

  /* RED = entry point reached */

  DEBUG ((EFI_D_INFO, "Loader Build Info: %a %a\n", __DATE__, __TIME__));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoader Load Address to debug ABL: 0x%llx\n",
         (UINTN)LinuxLoaderEntry & (~ (0xFFF))));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoaderEntry Address: 0x%llx\n",
         (UINTN)LinuxLoaderEntry));

  /*
   * The boot menu and the on-device fastboot screen can sit idle for minutes;
   * Phoenix's boot watchdog resets the device once its timeout expires, so drop
   * it before any interactive screen is shown.
   */
  SfbDisablePhoenixWatchdog ();

  /*
   * Bring up the graphical menu. When the platform has no usable GOP or font
   * this is a no-op and every screen keeps its console text rendering.
   */
  SfbGfxInit ();

  Status = InitThreadUnsafeStack ();

  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Unable to Allocate memory for Unsafe Stack: %r\n",
            Status));
    goto stack_guard_update_default;
  }


  /* Check if memory card is present; goto flashless if not */
  Status = gBS->LocateProtocol (&gEfiMemCardInfoProtocolGuid, NULL,
                                  (VOID **)&CardInfo);

  Status = EnumeratePartitions ();

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LinuxLoader: Could not enumerate partitions: %r\n",
            Status));
    /* Leave the partition table alone; it was never populated. */
  } else {
    UpdatePartitionEntries ();
  }

  {
    SFB_BOOT_MODE  Mode = SfbBootModeAblFakeLocked;
    BOOLEAN        ModeDefaulted = TRUE;

    /*
     * Now bring up the embedded FAT/USB stack so both the default entry and the
     * menu can see every FAT32 volume, including one on a USB drive.
     */
    Status = SfbStartFatStack ();
    if (EFI_ERROR (Status)) {
      /* Not fatal: the menu still offers fastboot and the program selector. */
      DEBUG ((EFI_D_ERROR, "Unable to start the FAT stack: %r\n", Status));
    }

    /*
     * With the filesystem drivers up, mount logfs so the earlier boot-chain
     * BDS log flush has somewhere to land; the chainloaded ABL inherits the
     * mounted volume.
     */
    SfbMountLogfs ();

    Status = SfbStoreReadMode (&Mode, &ModeDefaulted);
    if (EFI_ERROR (Status)) {
      Mode = SfbBootModeAblFakeLocked;
      ModeDefaulted = TRUE;
      DEBUG ((EFI_D_ERROR, "SFB: preferred mode unavailable: %r\n", Status));
    } else if (ModeDefaulted) {
      DEBUG ((EFI_D_INFO, "SFB: preferred mode defaulted to Mode 1\n"));
    }
    DEBUG ((EFI_D_INFO,
            "SFB: MARK mode-current mode=%u defaulted=%u\n",
            (UINT32)Mode, (UINT32)ModeDefaulted));

    /*
     * The menu is shown on every boot with nothing in front of it: it counts
     * down for a few seconds and then boots the saved default entry itself
     * when no key is pressed. Anything already queued is dropped so a key
     * still held from power-on cannot act on the menu the moment it appears.
     * It only returns TRUE when the user picked fastboot.
     */
    gST->ConIn->Reset (gST->ConIn, FALSE);
    if (!SfbRunBootMenu (Mode)) {
      Status = EFI_SUCCESS;
      goto stack_guard_update_default;
    }

    SfbShowFastbootMode ();
    DEBUG ((EFI_D_INFO, "Boot menu requested fastboot\n"));
  }

#ifdef AUTO_VIRT_ABL
  DEBUG ((EFI_D_INFO, "Rebooting the device.\n"));
  RebootDevice (NORMAL_MODE);
#endif
  DEBUG ((EFI_D_INFO, "Launching fastboot\n"));
  Status = FastbootInitialize ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Launch Fastboot App: %d\n", Status));
    goto stack_guard_update_default;
  }

stack_guard_update_default:
  /*Update stack check guard with defualt value then return*/
  __stack_chk_guard = DEFAULT_STACK_CHK_GUARD;

  DeInitThreadUnsafeStack ();

  return Status;
}
