# Flipper Zero LTO CM Reader

An experimental, read-only Flipper Zero application for reading the cartridge memory (CM) fitted to LTO tape cartridges.

The application can display selected health information without reading the entire CM, or save a complete raw dump to the SD card for offline analysis. It also recognizes universal LTO cleaning cartridges.

## Status

Hardware-tested:

- LTO-5 rewritable data cartridges, including an uninitialized cartridge
- Momentum mntm-012 (`d3f89dfe`), API 87.1 (uninitialized-cartridge fix tested on hardware)
- Universal cleaning cartridges with 32-byte and 64-byte status pages
- Momentum mntm-003 (`186bc96`), API 62.0

Build-tested:

- Flipper Zero development firmware, API 88.0

Implemented but not yet verified with physical media:

- LTO generations 1-4 and 6-9
- WORM cartridges
- LTO-7 Type M cartridges
- Extended block addressing used by larger CM devices

Treat support outside the hardware-tested list as experimental.

## Features

- Read-only LTO CM protocol implementation over the Flipper Zero NFC hardware
- Recognizes uninitialized data cartridges with a complete factory page table and zero load count; displays manufacture date and marks usage as unavailable
- Partial reads for health information
- Estimated cumulative written and read data
- Full-volume-equivalent passes, load count, retries, error counters, pass counters, maximum tape temperature, and manufacture date
- Cleaning count, used tape length, and estimated remaining cleanings for supported universal cleaning cartridges
- Complete raw CM dump to the SD card
- Safe cancellation and bounded communication retries

## Installation

Copy a compatible `lto_cm_probe.fap` to the NFC applications directory on the Flipper Zero SD card. A FAP must be built for the API version used by the installed firmware.

## Use

1. Start **LTO CM Reader** from the NFC applications menu.
2. Place the Flipper firmly over the CM antenna area of the cartridge.
3. Choose **Health information** for a targeted read, or **Full dump to file** for the complete CM.
4. Keep the Flipper and cartridge still until reading finishes.

Raw dumps are saved under:

```text
/ext/apps_data/lto_cm_probe/
```

The filename is based on the four-byte CM serial number.

## Building

Install [uFBT](https://github.com/flipperdevices/flipperzero-ufbt), then run this command in the repository root:

```sh
ufbt
```

The repository can also be copied into a compatible firmware tree under `applications_user/lto_cm_probe` and built with:

```sh
./fbt fap_lto_cm_probe
```

## Offline dump analyzer

The `tools/lto_cm_parse.js` script generates JSON and Markdown reports from a raw CM dump. It requires Node.js:

```sh
node tools/lto_cm_parse.js cartridge.bin
```

## Tests

The repository contains synthetic fixtures for an LTO-5 data cartridge , an uninitialized LTO-5 cartridge, and a universal cleaning cartridge with a 32-byte status page. They contain no data copied from physical media.

Run the pure C parser tests with a host C compiler:

```sh
cc -std=c11 -Wall -Wextra -Werror -o tests/test_parser tests/test_parser.c lto_cm_parser.c
./tests/test_parser
```

Run the offline dump-tool tests with Node.js:

```sh
npm test
```

GitHub Actions runs both test suites and an uFBT build for every push and pull request. Version changes in `application.fam` on `main` also build against Momentum mntm-012 and publish a tagged release with the FAP and SHA-256 checksum.

## Important limitations

- This project does not write to cartridge memory.
- Health values are interpretations of CM counters, not a guarantee that a cartridge is safe or reliable.
- Cleaning-cartridge remaining-use counts are estimates and may differ from drive or vendor policy.
- Antenna coupling is sensitive to cartridge position and movement.
- Raw dumps may contain cartridge serial numbers, manufacturer data, usage history, and drive identifiers. Review and sanitize dumps before publishing them.

## License

This project is licensed under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE).
