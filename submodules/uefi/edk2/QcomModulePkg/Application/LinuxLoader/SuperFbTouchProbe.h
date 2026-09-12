/*
 * Stage 0 touch feasibility probe for the super-fastboot boot menu.
 *
 * The panel's Synaptics S3910 sits on a QUPv3 GENI SPI serial engine that no
 * UEFI driver claims, so before any touch UI can be written we need to know
 * whether the firmware we already have can reach the controller at all:
 *
 *   - is the vendor SPI protocol (and the pin/GPIO protocols) present, and
 *     does its interface table look like the reverse engineered one?
 *   - which QUP serial engine index opens, if any?
 *   - does a TCM identify command clock out of that engine and answer?
 *
 * Everything the probe learns is written to the boot log (logfs) and rendered
 * on screen, and the normal menu continues afterwards.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SUPER_FB_TOUCH_PROBE_H__
#define __SUPER_FB_TOUCH_PROBE_H__

#include <Uefi.h>

/*
 * Run the probe. Safe to call unconditionally: with no graphics, no SPI
 * protocol or no reachable engine it logs its findings and returns.
 */
VOID
SfbTouchProbe (VOID);

#endif /* __SUPER_FB_TOUCH_PROBE_H__ */
