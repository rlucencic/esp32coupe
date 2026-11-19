# esp32coupe

SAM Coupé emulator for the Makerfabs MaTouch 7-inch ESP32-S3 and the M5Stack
Tab5 ESP32-P4. The GitHub repository is [rlucencic/esp32coupe](https://github.com/rlucencic/esp32coupe)
and its default branch is `main`.

## Code and configuration

- Keep the machine core in `components/coupe/` portable. It uses the Z80 from
  the sibling `../esp32emu` checkout. `main/main.c` connects it to ESP-IDF and
  the shared `emu_machine_t` runtime. The host harness links the same core.
- The core models a roughly 6 MHz Z80B, 16 KiB paging, all four video modes,
  the 128-colour palette, the keyboard matrix, and a stereo SAA1099
  tone/noise mixer with the ASIC beeper. Video samples palette, border, and
  mode changes by scanline. Interrupt, contention, and floppy timing are
  approximate. SAA1099 envelopes, cassette, MIDI, mouse, and external RAM
  remain open.
- The firmware setup title uses **SAM Coupe** because the board's LVGL font
  rendered the accented `é` as a missing glyph. The README can use **SAM Coupé**.
- The **Memory** row selects **256 KB** or **512 KB**. The default is 512 KiB.
  The choice is saved in NVS namespace `coupe`, key `memory`. In the 256 KiB
  configuration the upper 16 internal RAM pages read as open bus and ignore
  writes. Video fetches use the same page bounds.

## ROM and disks

- The original MGT v2.4 ROM used for testing is 32 KiB. Put a local copy in
  `roms/coupe.rom` before configuring ESP-IDF to include it in a full flash.
  A valid `coupe/roms/coupe.rom` on the SD card takes precedence. The flash
  partition is `coupe_rom` at `0x810000` in the project's partition table.
  Keep ROMs and test disk images out of Git.
- Place disk images in `coupe/` on the SD card. A mounted image is a table
  of sectors per track (ID C/H/R/N, size, file offset; `floppy.c`), and the
  controller finds a sector by its ID (C against the track register, R
  against the sector register, as the 1772 does). Accepted, read and
  written in place: raw 819,200-byte `.mgt`/`.dsk` (80 cylinders, two heads,
  ten 512-byte sectors, both heads of a cylinder consecutive), raw
  737,280-byte `.cpm`/`.dsk` (Pro-DOS's CP/M format: the same with nine
  sectors; its disks start with a PCW-style spec block `03 81 50 09 02`),
  and EDSK (`EXTENDED CPC DSK File`: each track's own sectors, any IDs and
  sizes, larger sectors streamed in 1 KiB chunks; the image's ST1/ST2 give
  CRC errors and deleted data). Each drive has its own 1772-style
  registers. Drive 1 is at `0xE0–0xE7`, Drive 2 at `0xF0–0xF7`, and bit 2
  selects the side. The setup screen and web page create blank 800 KiB
  (`.mgt`) and 720 KiB (`.cpm`) images. **Write Track** (format) collects a
  revolution's bytes (6,250; $F5 writes A1, $F6 C2, $F7 the CRC), finds the
  sectors (ID mark $FE after a sync, data mark $FB/$F8) and writes them where
  the image's layout allows: a raw image's own geometry, an EDSK track's own
  number and sizes of sectors (IDs updated in its header); anything else is
  a write error. **Read Track** synthesises the track (gaps, syncs, IDs, data,
  CRCs). Checked on the host: SAMDOS 2's "Format New Disk" on a blank 800 KiB
  image ($AA-filled: every byte rewritten), DOS copied on, the new disk
  boots and lists samdos2 with 770 K free; Pro-DOS's FORMAT B: on a blank
  720 KiB image (its spec block `03 81 50 09 ...` written, then `SAVE 2
  B:NEW.COM` listed) and on the Modula-2 EDSK in place (empty, 706 K free,
  same file size). Read Track: no software here uses it. The SAD, SDF and
  TD0 containers remain open. The console's `k <hex>` taps a
  key by its HID usage (`k 42`: F9, boot), `K <hex> 0|1` holds one.
- **Floppy restart regression.** The old drive config omitted `.path`.
  Opening Floppy calls the shared `disks_name()`, which invokes that callback
  while refreshing each drive, including empty ones. Keep
  `.path = coupe_disk_path` wired in `main/main.c`. It returns the mounted
  path until eject, and `NULL` for an empty drive. Controller resets keep
  mounted media and their paths.
- **SAMDOS boot timing.** ROM v2.4 waits in software before reading the first
  sector byte, so the initial DRQ timeout must allow that delay. An abandoned
  partial read must still clear BUSY. Force Interrupt restores Type I
  status so SAMDOS can see the index pulse and spin-up status on a
  mounted drive.
- **Pro-DOS** (the user's links: samcoupe-pro-dos.co.uk `prodosdisk.html`,
  `fdos_download.html`, `cpm_images.html`; they loaded on 2026-11-18 on a
  second try, 88 zips). Of their images, 76 are raw 737,280-byte Pro-DOS
  CP/M disks (60 `.cpm`, 16 `.dsk`), 4 raw 800 KiB boot disks, 4 EDSK
  (Pro-DOS 1.8 and Lite boot disks: three 1 KiB sectors a track numbered
  253-255, sector 1 on cylinder 4 side 0; two FTL Modula-2 disks of nine
  skewed 512-byte sectors over 83 track slots); 3 zips came truncated. Host
  checks: Pro-DOS 1.9 boots, takes its system disk in Drive 1 after Space
  and reaches `A>`; `DIR` lists A:, `DIR B:` the disk in Drive 2 (Zork I's
  `.cpm`, FDOS-001's 32 files, the Modula-2 EDSK's 13); Zork I runs ("open
  mailbox"; on the Tab5 too: the images uploaded through the web API,
  inserted, F9 and Space by the console's `k`, Enter first to leave the
  start-up screen); `SAVE 1 B:TEST.COM` writes to a `.cpm` and to the EDSK, and a
  fresh run lists TEST.COM on both; Pro-DOS 1.8 and Lite boot from their
  EDSK images to their prompts; SAMDOS 2 still boots from its 800 KiB
  image (unchanged). The host harness: `COUPE_DISK2=<image>` mounts Drive
  2, `\d` in `COUPE_KEYS` swaps Drive 1 to `COUPE_SWAP`, `:` types the
  SAM's own colon key (HID 0x32, the ISO key).

## The web page

- `status()` in `main/main.c`: System, ROM (its source, ROM 0/1 paged in or
  out), Memory (sections A-D and their pages, from LMPR/HMPR), Screen (mode
  1-4 with resolution, colours, size and pages, the border's palette entry,
  screen off), Palette (the 16 CLUT entries as swatches of the 128 colours),
  Line interrupt, Sound (the SAA1099's sounding channels: pitch, noise,
  volume L/R; the beeper), Keyboard (the SAM keys' places), Disks; each
  drive's detail is its image's kind. The core reports them through
  `coupe_get_status()` and `coupe_get_sound()`.
- The whole `/api/status` reply must fit esp32emu's `STATUS_SIZE` in `web.c`
  (8 KiB since 2026-11-19, 4 KiB before): rows are HTML inside JSON (quotes
  escaped), so keep markup short (a swatch is
  `<b style="background:#rrggbb">&nbsp;&nbsp;&nbsp;</b>`); the Coupé's reply
  is ~2.7 KiB. A palette row in a 640-byte buffer was cut after four
  swatches.
- Speed: core 1 is busy all the time on the Tab5; 94% speed at BASIC, 86%
  in Pro-DOS's mode 3 (2026-11-18). Open.

## Build and verification

Use ESP-IDF 5.5.6 with `esp32emu` beside this checkout.

```sh
idf.py -B build -DIDF_TARGET=esp32s3 -DSDKCONFIG=build/sdkconfig build
idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-tab5/sdkconfig build
make -C components/coupe/host
COUPE_MEMORY=256 COUPE_KEYS='\nPRINT 6+7\n' \
  components/coupe/host/build/coupe_host roms/coupe.rom 400 basic.ppm
```

- The host defaults to 512 KiB. `COUPE_KEYS` takes `\n` for Enter, `\b` for
  F9, and `\w` for a five-second wait. Pass a writable copy of a disk image
  as the optional final argument to mount it in Drive 1. The original SAMDOS
  2.0 disk runs its `AUTO` demo, so use a scratch copy with that entry
  disabled when scripting BASIC commands after DOS boots.
- Host checks booted ROM v2.4 and SAMDOS 2.0 at both memory sizes, printed a
  value in all four display modes, and saved a BASIC program that loaded
  and ran after the disk image was reopened. Also checked second-drive
  sector persistence and stereo tone output.
- Both ESP-IDF targets built. On the Tab5, a full flash installed ROM v2.4,
  both memory configurations reached their corresponding splash screens,
  Floppy opened from setup, and the selected 256 KiB survived a board
  reset. The Tab5 was then returned to 512 KiB. MaTouch hardware startup,
  game compatibility, and physical keyboard and speaker checks are open.
- Use a full flash when switching from a different emulator so the
  matching partition table and ROM are installed. Use
  `../esp32emu/tools/emu_console.py` for serial status and LCD capture.
  The capture helper's stock PNG converter uses macOS `sips`. On Linux,
  decode its RGB565 screen dump with a local PNG converter.
- The public README uses Tab5 LCD captures for startup and the Floppy page,
  and labelled host captures for BASIC and the SAMDOS directory. Keep
  network names, addresses, and private paths out of public screenshots.

Hardware references are linked from the [README](README.md).
