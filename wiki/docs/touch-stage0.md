# Touch menu, stage 0: can the firmware reach the panel controller?

The boot menu is driven by the volume and power keys only. The panel is a
Synaptics **S3910** hanging off a QUPv3 GENI SPI serial engine, and no UEFI
driver claims it, so before writing any touch code we first have to answer
three questions:

1. does the shipped firmware expose a SPI engine we may drive?
2. which serial engine index is the touch bus?
3. does a TCM identify command actually come back?

`SuperFbTouchProbe.c` was written to answer all three and logs everything into
logfs. It paints nothing on screen and holds nothing: logfs is the output
channel.

The probe is **not wired into the build**. It is kept as the record of what the
device answered, and it must stay out of `LinuxLoader.inf` and out of
`LinuxLoaderEntry` so that a normal BDS build carries none of it. To run it
again, add the two files to `[Sources]` in `LinuxLoader.inf` and call
`SfbTouchProbe()` from `LinuxLoaderEntry` right after the graphics backend comes
up.

**Status: the host side and the reset line are done and proven on hardware. The
panel still does not answer because its analogue rail is never powered in the
boot flow. Every route to that rail from inside the bootloader has now been
tried and closed off with evidence.** Stage 1 is paused at the power step; see
"Where it stopped" at the end for the one remaining kind of solution.

## What was reverse engineered

The vendor modules were extracted from `uefi.img` and disassembled.

| Protocol | GUID | Published by | Interface |
| --- | --- | --- | --- |
| SPI (QUP/SPI framework) | `4C7FFD28-6A06-4425-9EE2-676EBC089683` | `SPI` FV file | `version(u64)=0x00010000` then `open`, `transfer`, `close` |
| TLMM extension | `4CE41849-F4E7-480E-9FA3-003AAA434515` | `TLMMExDxe` | `version(u64)=1` then five entry points |
| GPI (packet interface DMA) | `569EA0DE-B557-4043-84CF-01103FE516E5` | `GpiDxe` | opaque |

`open` maps a 1-based serial engine index onto a `(qup block, se index)` pair.
The mapping table is built at runtime from a device tree: the module formats
`/soc/%s_%d` with the block names `TOP_QUP`, `SSC`, `HUB` and reads their
`num_se` property. The index that matters was confirmed on hardware:

```
idx 1..5   -> qup 0, se 0..4      (TOP_QUP_0, num_se 5)
idx 6..13  -> qup 1, se 0..7      (8)
idx 14..18 -> qup 2, se 0..4      (5)
idx 19..24 -> qup 3, se 0..5      (6)
idx 25     -> qup 4, se 0         <-- touch, WRAP4 SE0 = spi@1a80000
```

### The vendor `transfer` must not be called

`open(25)` is safe and configures power, clock, pin mux and FIFO mode. The
vendor `transfer` entry (`+0x10`) is not: calling it, even with a configuration
block that passes its own validation, takes the device down to 900E. The probe
therefore drives the serial engine directly.

## Driving the GENI serial engine from the firmware

The register sequence is the one the mainline `spi-geni-qcom` driver uses for
FIFO mode, transcribed into the probe:

* `SE_GENI_TX_FIFOn` (`0x700`) / `SE_GENI_RX_FIFOn` (`0x780`), four bytes per
  FIFO word at eight bits per word
* `SE_SPI_TX_TRANS_LEN` (`0x26c`) / `SE_SPI_RX_TRANS_LEN` (`0x270`) in bytes
  for byte aligned words
* `SE_GENI_M_CMD0` (`0x600`), opcode in bits 27..31, transmit only is `1`, full
  duplex is `7`
* `SE_GENI_M_IRQ_STATUS` (`0x610`), `M_CMD_DONE` is bit 0
* packing must be programmed (`0x260`/`0x264`/`0x284`/`0x288`), then
  `SE_SPI_WORD_LEN` (`0x268`) = bits per word - 4

Three things are easy to get wrong, and all three cost a revision each:

1. **The transmit FIFO has to be kept fed while the command runs.** The
   sequencer drains it and then waits forever without ever raising
   `M_CMD_DONE`; the kernel feeds it from a watermark interrupt, a polling
   implementation has to feed it from the poll loop.
2. **The receive FIFO has to be drained while the command runs.** It is shallow
   (a few words) and a full receive FIFO stops the engine from making progress.
3. **Long transfers must be split.** A single 64-byte full duplex command stalls,
   so the probe issues 16-byte chunks, each with its own command.

The watermark registers (`0x80c`/`0x810`/`0x814`) are configured by the vendor
`open` and must be left alone; zeroing the ready-for-receive level also makes the
engine start a command and then wait forever.

Two safety properties are kept in the code: a poll deadline per transfer, so a
stalled engine is cancelled rather than hung on, and the whole probe is wrapped
behind a window check plus entry point fingerprints before the vendor `open` is
called at all.

## TCM over SPI

Taken from the Synaptics driver in this tree (`synaptics_tcm_spi.c` and
`synaptics_tcm.c`):

* write: plain transmit of `{cmd, len_lo, len_hi, payload...}`
* read: full duplex with the transmit side filled with `0xff`
* report header is `{marker = 0xa5, code, len_lo, len_hi}` after one acknowledge
  byte, so `REPORT_IDENTIFY = 0x10` answers an identify
* identify command is `0x02` in normal mode, `0x07` in HBP mode

## Touch wiring (from the kernel device tree)

| Item | Value |
| --- | --- |
| Controller | Synaptics S3910, `compatible = "synaptics-s3910"` |
| Bus | `spi19` = `spi@1a80000` (QUPv3 WRAP4 SE0), chip select 0 |
| SPI mode | mode 0, 8 bits/word, 19 MHz |
| Interrupt | TLMM GPIO 158, level low, held until the report is read |
| Reset | TLMM GPIO 159, active low (power-on delays 200/10/80 ms) |
| Rails | `vreg_l4b_1p8` (vdd), `touch_avdd_2v8` (avdd, switched by PMH0110_D GPIO 9) |

GPIO 159 is referenced exactly once in the whole device tree: as this reset. No
`firmware-name` is present, so the controller runs its own application firmware
and needs no download before it can report touches.

## Measured behaviour

| Revision | Change | Result |
| --- | --- | --- |
| v1 | open + vendor transfer, before the large stack existed | dead before logging |
| v2 | read only: protocols, module bases, layout checks | clean, layout verified on device |
| v3 | vendor `open(25)`, register reads, vendor `close` | clean, `open` succeeded |
| v4 | vendor `transfer` with a passing configuration block | **900E** |
| v5 | direct GENI FIFO, receive only | clean, 64 bytes returned, all zero |
| v6 | transmit, but watermarks were zeroed by the probe | stall, `status=0x41` |
| v7..v9 | chunking, receive drain, progress counters | clean, but `rxpeak = 0` |
| v11 | TLMMExDxe write, pin 159 | **900E** |
| v14 | direct TLMM write of the whole ctl register | **900E** |
| v17 | read only TLMM pass, then a single io bit write | clean, **reset released (io 0x0 -> 0x2)** |
| v19, v20 | PMIC GPIO protocol, 3 then 4 arguments | **900E** (see the crash log below) |
| v21 | PMIC code removed; safe baseline | current image |

The transmit side is proven: the FIFO drains completely (`txleft = 0`,
`txwc = 0`), so data really is shifted out. The receive side never sees a byte
(`rxpeak = 0`) and `SE_GENI_IOS` bit 0 stays low, which is what an unpowered or
held-in-reset controller looks like.

## Power-up and reset sequence (from the shipped driver)

The touch driver that actually runs on this device is the vendor one: `adb shell
cat /proc/modules` lists `oplus_bsp_tp_hbp_syna_s3910` and
`oplus_bsp_tp_tcm_S3910`, which is `syna_tcm2.c` / `syna_tcm2_spi.c` in this
tree. `synaptics_tcm.c` is a second, newer driver that also matches the
compatible string; both were read and they agree on the physical sequence.

`synaptics_tcm_parse_dt()` (in `syna_tcm2_spi.c`) hard codes the panel side,
and none of it is overridable from the device tree:

```c
rst->reset_on_state  = 0;      /* reset is asserted by driving the line LOW  */
rst->reset_active_ms = 10;
rst->reset_delay_ms  = 80;
pwr->power_on_state  = 1;
pwr->power_on_delay_ms = 200;
```

`synaptics_tcm_power_on()` then runs, in this order:

```c
enable power gpio / regulator;            /* vdd = vreg_l4b_1p8, avdd = touch_avdd_2v8 */
msleep(200);                              /* power_on_delay_ms  */
gpio_set_value(reset_gpio, 0);            /* ASSERT: low        */
msleep(10);                               /* reset_active_ms    */
gpio_set_value(reset_gpio, 1);            /* RELEASE: high      */
msleep(80);                               /* reset_delay_ms     */
/* only now may the bus be used */
```

So the panel is held in reset by a **low** line, is released by taking it
**high**, and needs 200 ms of power before the pulse and 80 ms after it. The
probe must leave the line high, not low: an earlier revision drove it high and
kept it there while calling that "released" based on the opposite polarity from
the newer driver, which is the state that keeps the controller silent.

The identify command follows from the same place: `use_hbp_mode` is set for
`synaptics-s3910`, so it is **`0x07`**, not `0x02`.

Two further parameters are parsed but never used anywhere in the driver, so they
are not required: `synaptics,spi-byte-delay-us` and `synaptics,spi-block-delay-us`
(both default to 0, which selects one full duplex transfer with the transmit side
filled with `0xff` - exactly what the probe does).

The reset line itself is a TLMM gpio, as on other Qualcomm designs
(`synaptics,reset-gpio = <&tlmm 38 0x0>` on lito-cdp), so driving it does need
the pin controller - the open question below is only how to reach that block.

### The pin controller is reachable after all: only the data register may be written

The kernel layout is correct, and the read only probe proved it on hardware:

```
SFB: tlmm159 ctl=0x1 io=0x0
SFB: tlmm158 ctl=0x1 tlmm48 ctl=0x1801
```

`base = 0x0F100000`, `pin * 0x1000`, `ctl` at +0x00, `io` at +0x04. The window is
mapped and readable, and `ctl` reads `0x1`, that is plain gpio with output
disabled and pull down - which is fine, because the vendor driver only ever
changes the data register.

**Writing the whole control register is what crashed the platform**, twice, in
an earlier revision. Writing one bit of the data register does not:

```
write io(base + 159*0x1000 + 0x04) bit 1 = 0   /* assert, active low */
stall 10 ms
write io(base + 159*0x1000 + 0x04) bit 1 = 1   /* release */
stall 80 ms
```

That exact sequence ran on hardware and was observed to work:

```
SFB: tlmm159 io before=0x0
SFB: tlmm159 io after=0x2 released=1
```

with the platform stable afterwards. So **the reset half of stage 1 is solved and
verified**; only the power half is missing.

### Power is the other half, and every route to it was tried

`touch_avdd_2v8` is not a PMIC regulator but a regulator-fixed whose enable is
`<&pmh0110_d_e0_gpios 9 GPIO_ACTIVE_HIGH>` (device tree comment: "Touch AVDD is
GPIO-switched on PMH0110_D GPIO9"). It is SPMI, not the pin controller, and
nothing in the boot flow turns it on.

What the firmware does contain, all confirmed offline:

* **The touch rails are already declared as an ABL power domain**:

  ```
  /sw/prm/prm-pam/touch-screen          pam-name="touch_screen", 3 modes
    touch-screen-ldo4b-e0    pam-supply -> L4B_E0    (vdd,  vreg_l4b_1p8)
    touch-screen-ldo14b-e0   pam-supply -> L14B_E0   (avdd, vreg_l14b_3p2)
  ```

  with `qcom,pm-sid = 1`, `qcom,pm-bid = 0` on both resources.
* The AVDD switch is `pmh0110_d_e0` = **SPMI slave 3 on bus 0**
  (`pmh0110-kaanapali.dtsi`: `pmh0110_d_e0: pmic@3`, `gpio@8800`, 14 gpios),
  so the target register block is `gpio@8800 + 9*0x100`.
* SPMI GPIO register layout, from the kernel `pinctrl-spmi-gpio.c`:
  `MODE_CTL +0x40` (dir bits 6..4), `DIG_OUT_CTL +0x45` (bit 7 master enable),
  `EN_CTL +0x46` (bit 7 enable).
* The **PMIC GPIO protocol** exists and is locatable by GUID
  `22D38D3D-E8B6-4F8F-9C26-BCEB07D6CB68`, with entry points at +0x08, +0x10,
  +0x50, +0x58, +0x70, +0x80 taking `(pmic, gpio, value, out)`. The first
  argument is a PMIC **type id** in 0..27, not an SPMI address.
* The **SPMI protocol** is also public, GUID `FA5F306B-F47D-4AC4-A47D-882F8204EC30`,
  and `PmicDxe`'s internal read wrapper at `0xebdc` takes `(bus, addr, buf)`.
* None of these is usable from `LinuxLoader`, and that is now proven rather
  than assumed:

  - the PAM enable functions are internal statics needing a module base;
  - **calling the PMIC GPIO protocol from this phase crashes the platform even
    with a correct argument list** (two attempts, the second with the output
    buffer pointer filled in properly);
  - the **SPMI protocol is reachable and does not crash**, and its interface
    table reads back as a real, internally consistent vtable (five consecutive
    relocated code pointers, matching the module's `.data` at `0x3ec50`,
    `0x3ecd0`, `0x3ed40`, `0x3ede8`, `0x3ee60`), with the read method at
    `+0x28` exactly where `PmicDxe`'s own read wrapper forwards to;
  - and yet **every read returns Success with the buffer untouched**. A sweep
    of both buses, slaves 0..15 and three register offsets - 96 reads, all of
    them safe - produced nothing but zeros.

  So the protocol object exists, the call path completes, and the transfer
  itself does not happen: `PmicDxe`'s SPMI channel is established during DXE
  driver initialisation, which has already finished by the time LinuxLoader
  runs. There is no argument left to fix.

## Crash log, and what each one eliminated

| Revision | Attempt | Outcome |
| --- | --- | --- |
| v4 | vendor SPI `transfer` with a config block that passed its own validation | 900E |
| v11 | `TLMMExDxe` write at pin 159 (that module is an I2C expander driver) | 900E |
| v14 | direct write of the whole pin controller `ctl` register | 900E |
| v19 | PMIC GPIO protocol, `Enable(id, gpio, value)` with three arguments | 900E |
| v20 | same call with the fourth (output buffer) argument supplied | 900E |

The safe revisions are the ones that only ever wrote the pin controller's data
register (v17) or nothing at all. Every crash left logfs holding either Android
boot output or FAT12 leftovers, so **a crash is undiagnosable from the firmware
side**; the only evidence is the return to Android and the absence of probe
lines.

## The three probes that settled it

| Probe | Result |
| --- | --- |
| `PmicGpioProtocol` (GUID `22D38D3D-…`), `Enable(id, gpio, value, out)` | **900E**, twice, even with a valid output buffer |
| `SPMIProtocol` (GUID `FA5F306B-…`), 4 fixed reads then a 96-read sweep | Success every time, **no crash, no data** |
| interface table dump of the SPMI protocol | a genuine vtable; `+0x28` confirmed as the read entry |

The middle row is the important one: it is the only hardware access in this
whole investigation that was both dead and harmless, which is what makes the
conclusion safe to draw. A protocol that answers Success without transferring
anything is not a protocol whose parameters are wrong; it is one whose backing
channel is not up yet.

## The power attempt inside the bootloader, and why it was abandoned

The SPMI protocol **is** reachable from this phase and it works. That was
established the hard way, and the details are worth recording because they are
not obvious from the module listing:

* the protocol is published by the `SPMI` module, not by `PmicDxe`, although
  `PmicDxe` is what drives it at runtime;
* its interface is at `.data+0x8098` of that module and holds two entry points:

  ```
  +0x08  read   (base, bus, sid, addr, buf, len, out)   operation 1
  +0x10  write  (base, bus, sid, addr, buf, len)        operation 0
  ```

  there is **no command argument** - an early revision passed one, which shifted
  every following argument by a slot, so its writes were really reads of address
  2 and changed nothing;
* reads were proven on hardware: slave 3 answers `0x10` (PMIC_GPIO_TYPE) at
  `0x8804` and `0x15` (the low/mid voltage gpio subtype) at `0x8805`, and those
  same two bytes appear in the kernel's own view of the device under
  `/sys/kernel/debug/regmap/0-03/registers`;
* that kernel view also settles the layout: **every gpio is its own SPMI
  peripheral block**, `0x100` apart, and the device tree node `gpio@8800` names
  only the first of them. Gpio 9 therefore lives at `0x9100`, its `MODE_CTL` at
  `0x9140`, `DIG_OUT_SOURCE_CTL` at `0x9144` and `EN_CTL` at `0x9146`.

**Writes still crash the platform**, and four attempts to find a working
sequence all ended the same way, each one taking the device to 900E and leaving
no log behind:

| Attempt | What was written | Outcome |
| --- | --- | --- |
| v19, v20 | PMIC gpio protocol `Enable`, three then four arguments | 900E |
| v27 | "write" through the read entry with a command argument | no effect, reads unchanged |
| v28 | `0x9140`/`0x9146` through the real write entry | 900E |
| v30 | `0x8940`, which is gpio 1's block, not gpio 9's | 900E |
| v31 | `0x9140` `0x9144` `0x9146`, the corrected block | 900E |

The likely reason is visible in the kernel driver: switching this rail needs
more than a direction and an enable. It also programs the voltage source
(`DIG_VIN_CTL`), the pull (`DIG_PULL_CTL`) and the output buffer type and drive
strength (`DIG_OUT_CTL`), and the observed registers agree - gpio 9 currently
reads `0x9141=0x01`, `0x9142=0x04` while `0x9145` is still zero, so the kernel
itself has not finished a full output configuration on this pad. Setting a bare
direction bit on a pad that switches a 2.8 V analogue rail, without knowing the
rest of that tuple, is not something a read only probe can validate - and the
hardware clearly does not tolerate the guesses.

So: the bootloader has no supported way to bring this rail up, and blind writes
to the pmic are off the table. Crashes also wipe logfs every time, which means
any further attempt would be undiagnosable as well as unsafe.

### The pmic side is closed: even reads stop being safe

A final read only attempt settled it. The plan was to dump the gpio 9 block at
`0x9100` and compare it against the kernel's own view of the same block, which
is known from android:

```
0x9104 = 0x10 type      0x9105 = 0x15 subtype     0x9108 = 0x80
0x9141 = 0x01 source    0x9142 = 0x04 pull
0x9144 = 0x01 output source (bit 7 clear, so it drives high)
0x9146 = 0x80 enabled
```

That scan **crashed the platform**, and like every crash before it, logfs came
back empty, so it is not even known which address did it. Reads at `0x8804` and
`0x8805` are harmless - they have run several times and match the kernel exactly
- so the difference is the wider sweep, not the protocol setup.

That is the end of the line for reaching this rail from the bootloader, and the
reasoning is worth stating plainly rather than leaving as a list of failures:

* writes crash; five attempts, every value and every address variant tried,
  including the values the kernel itself uses;
* a wider read crashes as well, so it is not possible to survey the block and
  narrow the value down by observation;
* every crash wipes logfs, so each attempt is blind in both directions - no way
  to confirm what was written, no way to see where it stopped;
* and the one place that would have answered everything, the kernel's own
  register view, is only reachable from android, i.e. exactly when the
  bootloader is no longer running.

There is therefore no experiment left that is both safe and informative. The
probe keeps the two reads that are known to be harmless and nothing else.

## What would unblock stage 1

Only the power half remains, and inside the bootloader it has no route left: the
two protocols that could have driven it are respectively fatal and non
functional from this phase, and the PAM layer is internal. What is left is
information the firmware does not contain, or a change outside the bootloader:

* the **PMIC type id** for PMH0110 together with evidence that the PMIC GPIO
  protocol is meant to be callable after DXE, or
* a documented way to ask ABL's PRM/PAM layer to bring up the `touch-screen`
  domain, or
* the raw SPMI sequence for `pmh0110@3` `gpio@8800` gpio 9 **plus** a phase in
  which the SPMI channel is actually live - the kernel layout above is already
  authoritative for the register offsets, so the offsets are not the problem, or
* a way to keep the rail alive across the reboot into the bootloader. A warm
  reboot from Android does **not** preserve it; that was measured directly. This
  is the only avenue that does not require the bootloader to initialise a PMIC
  channel, which is why it is the one worth trying next.

Until then the probe stays in its current shape: it drives the serial engine,
performs the one verified reset write, reads the pin controller, and leaves the
power alone.

## Running the probe

```
fastboot boot BDS_touch18.bootimg
```

Nothing is drawn; the boot continues into the menu. Read the log with:

```
adb shell su -c "dd if=/dev/block/by-name/logfs of=/data/local/tmp/logfs.bin bs=1M"
adb pull /data/local/tmp/logfs.bin
tr '\0' '\n' < logfs.bin | grep 'SFB: touch'
```

Note that a crash can leave logfs holding the Android boot log instead of the
probe output, in which case the boot log itself is the only evidence.
