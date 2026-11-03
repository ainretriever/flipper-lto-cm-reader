# v0.1.1

Fix health reads of uninitialized LTO data cartridges whose factory page table contains no initialisation or usage-history pages.

- Recognize a complete factory-style directory with one valid status page and zero loads.
- Display an uninitialized-cartridge result with manufacture date, zero loads, and usage marked N/A.
- Preserve errors for incomplete reads and inconsistent layouts.
- Add matching offline JSON/Markdown reporting and a synthetic regression fixture.

## Validation

- C parser and JavaScript regression suites passed.
- The C parser was also checked against the supplied physical blank-cartridge dump. The original dump is not included in the repository.
- The fix passed a user hardware test on Momentum mntm-012 (`d3f89dfe`), API 87.1.

## Download

`lto_cm_probe-0.1.1-mntm-012.fap` is built for Momentum mntm-012 (`d3f89dfe`), API 87.1. Copy it to `/apps/NFC/` on the Flipper Zero SD card. A SHA-256 checksum is supplied in `SHA256SUMS`.

Factory layouts from other vendors and LTO generations have not been verified on hardware.
