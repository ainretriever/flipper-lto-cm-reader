#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later

const fs = require("fs");
const path = require("path");

const inputPath = process.argv[2];
if (!inputPath) {
    console.error("Usage: node tools/lto_cm_parse.js <dump.bin> [output-prefix]");
    process.exit(2);
}

const outputPrefix = process.argv[3] || inputPath.replace(/\.[^.]+$/, "");
const data = fs.readFileSync(inputPath);

function hex(offset, length) {
    return data.subarray(offset, offset + length).toString("hex").toUpperCase();
}

function ascii(offset, length) {
    return data
        .subarray(offset, offset + length)
        .toString("ascii")
        .replace(/\0+$/g, "")
        .trimEnd();
}

function uintString(offset, length) {
    let value = 0n;
    for (const byte of data.subarray(offset, offset + length)) {
        value = (value << 8n) | BigInt(byte);
    }
    return value.toString();
}

function u16(offset) {
    return data.readUInt16BE(offset);
}

function u32(offset) {
    return data.readUInt32BE(offset);
}

function printableStrings(offset, length) {
    const result = [];
    let current = "";
    let start = offset;
    for (let i = offset; i < offset + length; i++) {
        const byte = data[i];
        if (byte >= 0x20 && byte <= 0x7e) {
            if (!current) start = i;
            current += String.fromCharCode(byte);
        } else {
            if (current.length >= 4) result.push({offset: start, value: current});
            current = "";
        }
    }
    if (current.length >= 4) result.push({offset: start, value: current});
    return result;
}

const pageNames = {
    0x001: "Cartridge Manufacturer Information",
    0x002: "Media Manufacturer Information",
    0x101: "Initialisation Data",
    0x102: "Tape Write Pass",
    0x103: "Tape Directory",
    0x104: "EOD Information",
    0x105: "Cartridge Status and Tape Alert Flags",
    0x106: "Mechanism Related",
    0x107: "Suspended Append Writes",
    0x108: "Usage Information 0",
    0x109: "Usage Information 1",
    0x10a: "Usage Information 2",
    0x10b: "Usage Information 3",
    0x10c: "LTO-5 Extension 10C (unknown)",
    0x10d: "LTO-5 Extension 10D (unknown)",
    0x10e: "LTO-5 Extension 10E (unknown)",
    0x180: "Vendor Extension 180",
    0x181: "Vendor Extension 181",
    0x182: "Vendor Extension 182",
    0x183: "Vendor Extension 183",
    0x184: "Vendor Extension 184",
    0x200: "Application Specific",
    0xffc: "Padding",
    0xffd: "Defect",
    0xffe: "Empty",
    0xfff: "End of Page Table",
};

function parseTable(start, label) {
    const descriptors = [];
    for (let offset = start; offset + 3 < data.length; offset += 4) {
        const version = data[offset] >> 4;
        const pageId = ((data[offset] & 0x0f) << 8) | data[offset + 1];
        const startAddress = u16(offset + 2);
        descriptors.push({
            descriptorOffset: offset,
            version,
            pageId,
            pageIdHex: pageId.toString(16).toUpperCase().padStart(3, "0"),
            name: pageNames[pageId] || "Unknown",
            startAddress,
            firstBlock: Math.floor(startAddress / 32),
        });
        if (version === 0 && pageId === 0xfff) break;
    }
    return {label, start, descriptors};
}

const protectedTable = parseTable(0x24, "Protected");
const protectedEopt = protectedTable.descriptors.find((d) => d.pageId === 0xfff);
const unprotectedTable = parseTable(protectedEopt.startAddress, "Unprotected");

const realDescriptors = [...protectedTable.descriptors, ...unprotectedTable.descriptors].filter(
    (d) => ![0xffc, 0xffd, 0xffe, 0xfff].includes(d.pageId),
);

function pageBase(pageId) {
    const descriptor = realDescriptors.find((d) => d.pageId === pageId);
    if (!descriptor) return null;
    return descriptor.startAddress;
}

function pageEnvelope(descriptor) {
    const offset = descriptor.startAddress;
    const header = u16(offset);
    const length = u16(offset + 2);
    return {
        ...descriptor,
        header,
        headerHex: header.toString(16).toUpperCase().padStart(4, "0"),
        length,
        lastAddress: offset + length - 1,
        lastBlock: Math.floor((offset + length - 1) / 32),
        rawHex: hex(offset, length),
        printableStrings: printableStrings(offset, length),
    };
}

const pages = realDescriptors.map(pageEnvelope);

function parseCartridgeManufacturer() {
    const o = pageBase(0x001);
    const cartridgeType = u16(o + 22);
    const cleaning = (cartridgeType & 0x8000) !== 0;
    const cartridgeTypeLow = cartridgeType & 0xff;
    const formatByType = {
        1: "LTO-1",
        2: "LTO-2",
        4: "LTO-3",
        8: "LTO-4",
        16: "LTO-5",
        32: "LTO-6",
        64: "LTO-7",
        128: "LTO-8",
        129: "LTO-9",
    };
    const particleByte = data[o + 42];
    return {
        pageId: hex(o, 2),
        pageLength: u16(o + 2),
        cartridgeManufacturer: ascii(o + 4, 8),
        cartridgeSerialNumber: ascii(o + 12, 10),
        cartridgeType: hex(o + 22, 2),
        format: cleaning ? "Cleaning" : formatByType[cartridgeTypeLow] || "Unknown",
        cleaning,
        worm: ((cartridgeType >> 13) & 1) === 1,
        dateOfManufacture: ascii(o + 24, 8),
        tapeLength: u16(o + 32),
        tapeLengthMetres: u16(o + 32) / 4,
        tapeThickness: u16(o + 34),
        emptyReelInertia: u16(o + 36),
        hubRadius: u16(o + 38),
        fullReelPackRadius: u16(o + 40),
        maximumMediaSpeed: u16(o + 42),
        particleType: (particleByte & 0x0f) !== 0 ? "BaFe" : "MP",
        substrateType: (particleByte & 0xf0) === 0x10 ? "SPALTAN" : "PEN",
        licenseCode: hex(o + 44, 4),
        mediaCode: hex(o + 46, 2),
        manufacturerUseHex: hex(o + 48, 12),
        crc32: hex(o + 60, 4),
    };
}

function parseMediaManufacturer() {
    const o = pageBase(0x002);
    if (o === null) return null;
    return {
        pageId: hex(o, 2),
        pageLength: u16(o + 2),
        mediaManufactureDate: ascii(o + 4, 8),
        mediaVendor: ascii(o + 12, 8),
        servowriterInformation: ascii(o + 20, 32),
        reservedHex: hex(o + 52, 8),
        crc32: hex(o + 60, 4),
    };
}

function parseInitialisation() {
    const o = pageBase(0x101);
    if (o === null) return null;
    return {
        pageId: hex(o, 2),
        pageLength: u16(o + 2),
        initialisingDriveManufacturer: ascii(o + 4, 8),
        driveId: ascii(o + 12, 16),
        formatType: hex(o + 28, 4),
        logicalPosition1: u32(o + 32),
        logicalPosition3: u32(o + 40),
        logicalPosition5: u32(o + 48),
        lto5PayloadHex: hex(o + 28, 32),
        crc32: hex(o + 60, 4),
    };
}

function parseTapeWritePass() {
    const o = pageBase(0x102);
    if (o === null) return null;
    return {
        pageId: hex(o, 2),
        pageLength: u16(o + 2),
        reservedHex: hex(o + 4, 12),
        writePassFields: Array.from({length: 8}, (_, i) => u32(o + 16 + i * 4)),
    };
}

function parseTapeDirectory() {
    const o = pageBase(0x103);
    if (o === null) return null;
    const length = u16(o + 2);
    const sectionCount = 80;
    return {
        pageId: hex(o, 2),
        pageLength: length,
        fidTapeWritePass: u32(o + 4),
        reservedHex: hex(o + 8, 8),
        wrapSectionCount: sectionCount,
        wrapSections: Array.from({length: sectionCount}, (_, i) => ({
            index: i,
            writePass: u32(o + 16 + i * 32),
            dataSetId: u32(o + 20 + i * 32),
            headOfWrapRecordCount: u32(o + 24 + i * 32),
            endOfWrapRecordCount: u32(o + 28 + i * 32),
            headOfWrapFileMarkCount: u32(o + 32 + i * 32),
            endOfWrapFileMarkCount: u32(o + 36 + i * 32),
            fileMarkMap: u32(o + 40 + i * 32),
            crc32: hex(o + 44 + i * 32, 4),
            rawHex: hex(o + 16 + i * 32, 32),
        })),
    };
}

function parseEod() {
    const o = pageBase(0x104);
    if (o === null) return null;
    const length = u16(o + 2);
    return {
        pageId: hex(o, 2),
        pageLength: length,
        tapeWritePassForLastWrittenEod: u32(o + 4),
        threadCount: u32(o + 8),
        recordCountAtEod: uintString(o + 12, 6),
        fileMarkCountAtEod: uintString(o + 18, 6),
        eodDataSetNumber: u32(o + 24),
        wrapSectionNumberOfEod: u32(o + 28),
        validityOfEod: u16(o + 32),
        firstCqSetNumber: u16(o + 34),
        physicalPositionOfEod: u32(o + 36),
        lto5ExtensionHex: hex(o + 40, length - 44),
        extensionStrings: printableStrings(o + 40, length - 44),
        crc32: hex(o + length - 4, 4),
    };
}

function parseStatus() {
    const o = pageBase(0x105);
    if (o === null) return null;
    const length = u16(o + 2);
    return {
        pageId: hex(o, 2),
        pageLength: length,
        tapeAlertFlagsHex: hex(o + 4, 8),
        threadCount: u32(o + 12),
        lastCleaningPositionQuarterMetres: length >= 28 ? u16(o + 26) : null,
        cartridgeStatus: hex(o + 16, 2),
        encryptedDataPresent: !data.subarray(o + 22, o + 28).every((byte) => byte === 0xff),
        lto5StatusPayloadHex: hex(o + 18, length - 22),
        crc32: hex(o + length - 4, 4),
    };
}

function parseMechanism() {
    const o = pageBase(0x106);
    if (o === null) return null;
    const length = u16(o + 2);
    return {
        pageId: hex(o, 2),
        pageLength: length,
        driveManufacturerIdentity: ascii(o + 4, 8),
        mechanismDataHex: hex(o + 12, length - 16),
        crc32: hex(o + length - 4, 4),
        printableStrings: printableStrings(o + 12, length - 16),
    };
}

function parseSuspendedAppend() {
    const o = pageBase(0x107);
    if (o === null) return null;
    const length = u16(o + 2);
    return {
        pageId: hex(o, 2),
        pageLength: length,
        reservedHex: hex(o + 4, 4),
        slotCount: 22,
        suspendedAppendEntries: Array.from({length: 22}, (_, i) => ({
            index: i,
            dataSetId: u32(o + 8 + i * 8),
            writeTapePass: u32(o + 12 + i * 8),
            rawHex: hex(o + 8 + i * 8, 8),
        })),
        tailHex: hex(o + 184, length - 184),
    };
}

function parseUsage(pageId, index) {
    const o = pageBase(pageId);
    if (o === null) return null;
    const length = u16(o + 2);
    return {
        index,
        pageId: hex(o, 2),
        pageLength: length,
        driveManufacturer: ascii(o + 4, 8),
        driveId: ascii(o + 12, 16),
        suspendedAppendWrites: u16(o + 28),
        threadCount: u32(o + 32),
        lifeSetsWritten: uintString(o + 36, 8),
        lifeSetsRead: uintString(o + 44, 8),
        lifeWriteRetries: u32(o + 52),
        lifeReadRetries: u32(o + 56),
        lifeUnrecoveredWrites: u16(o + 60),
        lifeUnrecoveredReads: u16(o + 62),
        lifeSuspendedWrites: u16(o + 64),
        lifeFatalSuspendedWrites: u16(o + 66),
        lifeLp3Passes: u32(o + 68),
        lifeMidpointPasses: u32(o + 72),
        maximumTapeTemperatureC: data[o + 76],
        lifeTapeMetresPulled: u32(o + 80),
        remainingPayloadHex: hex(o + 84, length - 88),
        crc32: hex(o + length - 4, 4),
        printableStrings: printableStrings(o, length),
    };
}

function parseCartridgeContent() {
    const o = pageBase(0x10d);
    if (o === null) return null;
    const content = u16(o + 28);
    return {
        pageId: hex(o, 2),
        pageLength: u16(o + 2),
        driveId: ascii(o + 12, 16),
        cartridgeContent: content,
        partitionedCartridge: ((data[o + 28] >> 3) & 1) === 1,
        driveFirmwareId: ascii(o + 48, 4),
        crc32: hex(o + u16(o + 2) - 4, 4),
    };
}

function parsePartitionEod(pageId, partition) {
    const o = pageBase(pageId);
    if (o === null) return null;
    return {
        partition,
        pageId: hex(o, 2),
        pageLength: u16(o + 2),
        dataSetNumber: u32(o + 24),
        wrapNumber: u32(o + 28),
        validity: u16(o + 32),
        physicalPosition: u32(o + 36),
        crc32: hex(o + u16(o + 2) - 4, 4),
    };
}

function parseApplicationSpecific() {
    const o = pageBase(0x200);
    if (o === null) return null;
    const length = u16(o + 2);
    const signature = ascii(o + 4, 6);
    const attributes = [];
    if (signature === "MAM001" || signature === "MAM002") {
        for (let cursor = 10; cursor + 4 <= length; ) {
            const attributeId = u16(o + cursor);
            const attributeLength = u16(o + cursor + 2) & 0x0fff;
            if (attributeId === 0x0fff || attributeLength === 0) break;
            const valueOffset = o + cursor + 4;
            attributes.push({
                id: attributeId,
                idHex: attributeId.toString(16).toUpperCase().padStart(4, "0"),
                length: attributeLength,
                valueHex: hex(valueOffset, attributeLength),
                valueText: ascii(valueOffset, attributeLength),
            });
            cursor += 4 + attributeLength;
        }
    }
    const findText = (id) => attributes.find((attribute) => attribute.id === id)?.valueText || "";
    return {
        pageId: hex(o, 2),
        pageLength: length,
        signature,
        applicationVendor: findText(0x0800),
        applicationName: findText(0x0801),
        applicationVersion: findText(0x0802),
        barcode: findText(0x0806),
        attributes,
        crc32: hex(o + length - 4, 4),
    };
}

const cmSerial = data.subarray(0, 4);
const cmCheck = cmSerial.reduce((value, byte) => value ^ byte, 0);

const parsed = {
    source: {
        file: path.resolve(inputPath),
        bytes: data.length,
        blocks: data.length / 32,
    },
    cmManufacturerInformation: {
        serialNumberHex: hex(0, 4),
        serialCheckByteHex: hex(4, 1),
        calculatedSerialCheckByteHex: cmCheck.toString(16).toUpperCase().padStart(2, "0"),
        serialCheckValid: cmCheck === data[4],
        cmSizeKiB: data[5],
        cmTypeHex: hex(6, 2),
        manufacturerInformationHex: hex(8, 24),
        manufacturerInformationStrings: printableStrings(8, 24),
    },
    writeInhibit: {
        lastWriteInhibitedBlock: data[32],
        block1ProtectionFlag: data[33],
        reservedHex: hex(34, 2),
    },
    pageTables: {
        protected: protectedTable,
        unprotected: unprotectedTable,
    },
    pageMap: pages.map(({rawHex, ...page}) => page),
    fields: {
        cartridgeManufacturer: parseCartridgeManufacturer(),
        mediaManufacturer: parseMediaManufacturer(),
        initialisation: parseInitialisation(),
        tapeWritePass: parseTapeWritePass(),
        tapeDirectory: parseTapeDirectory(),
        eod: parseEod(),
        statusAndTapeAlert: parseStatus(),
        mechanismRelated: parseMechanism(),
        suspendedAppendWrites: parseSuspendedAppend(),
        usageInformation: [0x108, 0x109, 0x10a, 0x10b].map(parseUsage).filter(Boolean),
        cartridgeContent: parseCartridgeContent(),
        partitionEod: [parsePartitionEod(0x104, 0), parsePartitionEod(0x10e, 1)].filter(Boolean),
        applicationSpecific: parseApplicationSpecific(),
        unknownExtensions: pages
            .filter((page) => [0x10c, 0x180, 0x181, 0x182, 0x183, 0x184].includes(page.pageId))
            .map((page) => ({
                pageIdHex: page.pageIdHex,
                name: page.name,
                startAddress: page.startAddress,
                firstBlock: page.firstBlock,
                lastBlock: page.lastBlock,
                length: page.length,
                rawHex: page.rawHex,
                printableStrings: page.printableStrings,
            })),
    },
    selectiveReadPlans: {
        identify: {
            blocks: [0],
            description: "CM serial, size and type",
        },
        cartridgeIdentity: {
            blocks: [0, 1, 2, 3, 4, 5, 6, 7],
            description: "CM, cartridge, media and initialising drive identity",
        },
        discoverPages: {
            blocks: [0, 1, 8, 9, 10],
            description: "CM identity plus protected and unprotected page tables",
        },
        eodOnly: {
            blocks: [0, 1, 8, 9, 10, 93, 94, 95],
            description: "Discovery plus EOD page",
        },
        alertsOnly: {
            blocks: [0, 1, 8, 9, 10, 96, 97],
            description: "Discovery plus cartridge status and Tape Alert page",
        },
        usageOnly: {
            blocks: [0, 1, 8, 9, 10, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127],
            description: "Discovery plus four usage-history pages",
        },
        healthSummary: {
            ranges: ["0-1", "8-10", "93-97", "116-137"],
            blockCount: 32,
            description: "EOD, alerts, usage and LTO-5 health extensions",
        },
        fullDump: {
            ranges: ["0-254"],
            blockCount: 255,
            description: "Complete raw CM image",
        },
    },
};

const usageByRecency = [...parsed.fields.usageInformation].sort(
    (a, b) => b.threadCount - a.threadCount,
);
if (parsed.fields.cartridgeManufacturer.cleaning) {
    const tapeLength = parsed.fields.cartridgeManufacturer.tapeLength;
    const used = parsed.fields.statusAndTapeAlert.lastCleaningPositionQuarterMetres;
    const remaining = tapeLength - 44 - used;
    parsed.derived = {
        cleaning: {
            cleansPerformed: parsed.fields.statusAndTapeAlert.threadCount,
            tapeLengthQuarterMetres: tapeLength,
            usedQuarterMetres: used,
            estimatedRemaining: remaining > 0 ? Math.floor(remaining / 22) : 0,
            expired: remaining <= 0,
        },
    };
} else if (
    unprotectedTable.descriptors.at(-1)?.version === 0 &&
    unprotectedTable.descriptors.at(-1)?.pageId === 0xfff &&
    unprotectedTable.descriptors.every((d) => [0xffc, 0xffe, 0xfff, 0x200, 0x105].includes(d.pageId)) &&
    unprotectedTable.descriptors.filter((d) => d.pageId === 0x105).length === 1 &&
    parsed.fields.statusAndTapeAlert?.threadCount === 0 &&
    parsed.fields.statusAndTapeAlert.pageLength >= 64 &&
    pages.some((p) => p.pageId === 0x105 && (p.header & 0xfff) === 0x105 && p.startAddress + p.length <= data.length)
) {
    parsed.derived = {uninitialized: true, loadCount: 0, usageAvailable: false};
} else if (usageByRecency.length >= 2) {
    const currentUsage = usageByRecency[0];
    const previousUsage = usageByRecency[1];
    const difference = (field) => Number(BigInt(currentUsage[field]) - BigInt(previousUsage[field]));
    const lifeSetsWritten = BigInt(currentUsage.lifeSetsWritten);
    const lifeSetsRead = BigInt(currentUsage.lifeSetsRead);
    const kbPerDataSet = parsed.fields.cartridgeManufacturer.format === "LTO-5" ? 2473n : 0n;
    parsed.derived = {
        loadCount: parsed.fields.statusAndTapeAlert.threadCount,
        currentUsagePage: currentUsage.pageId,
        previousUsagePage: previousUsage.pageId,
        lastLoad: {
            setsWritten: difference("lifeSetsWritten"),
            setsRead: difference("lifeSetsRead"),
            writeRetries: difference("lifeWriteRetries"),
            readRetries: difference("lifeReadRetries"),
            unrecoveredWrites: difference("lifeUnrecoveredWrites"),
            unrecoveredReads: difference("lifeUnrecoveredReads"),
            suspendedWrites: difference("lifeSuspendedWrites"),
            fatalSuspendedWrites: difference("lifeFatalSuspendedWrites"),
            suspendedAppendWrites: difference("suspendedAppendWrites"),
            lp3Passes: difference("lifeLp3Passes"),
            midpointPasses: difference("lifeMidpointPasses"),
        },
        lifetime: {
            setsWritten: currentUsage.lifeSetsWritten,
            setsRead: currentUsage.lifeSetsRead,
            writeRetries: currentUsage.lifeWriteRetries,
            readRetries: currentUsage.lifeReadRetries,
            unrecoveredWrites: currentUsage.lifeUnrecoveredWrites,
            unrecoveredReads: currentUsage.lifeUnrecoveredReads,
            suspendedWrites: currentUsage.lifeSuspendedWrites,
            fatalSuspendedWrites: currentUsage.lifeFatalSuspendedWrites,
            suspendedAppendWrites: currentUsage.suspendedAppendWrites,
            lp3Passes: currentUsage.lifeLp3Passes,
            midpointPasses: currentUsage.lifeMidpointPasses,
            maximumTapeTemperatureC: currentUsage.maximumTapeTemperatureC,
            estimatedBytesWritten: (lifeSetsWritten * kbPerDataSet * 1000n).toString(),
            estimatedBytesRead: (lifeSetsRead * kbPerDataSet * 1000n).toString(),
            fullVolumeEquivalents: Number(lifeSetsWritten + lifeSetsRead) / (80 * 7800),
        },
    };
} else {
    parsed.derived = {};
}

fs.writeFileSync(`${outputPrefix}.json`, JSON.stringify(parsed, null, 2));

const md = [];
md.push("# LTO-CM parsed report", "");
md.push(`- Source: \`${parsed.source.file}\``);
md.push(`- Size: ${parsed.source.bytes} bytes (${parsed.source.blocks} blocks)`);
md.push(`- CM serial: ${parsed.cmManufacturerInformation.serialNumberHex}`);
md.push(`- Serial check: ${parsed.cmManufacturerInformation.serialCheckValid ? "valid" : "INVALID"}`);
md.push(`- CM size: ${parsed.cmManufacturerInformation.cmSizeKiB} KiB`);
md.push(`- CM type: ${parsed.cmManufacturerInformation.cmTypeHex}`, "");

md.push("## Cartridge identity", "");
for (const [key, value] of Object.entries(parsed.fields.cartridgeManufacturer)) {
    md.push(`- ${key}: ${Array.isArray(value) ? value.join(", ") : value}`);
}
md.push("", "## Media and initialisation", "");
if (parsed.fields.mediaManufacturer) {
    for (const [key, value] of Object.entries(parsed.fields.mediaManufacturer)) {
        md.push(`- media.${key}: ${value}`);
    }
}
if (parsed.fields.initialisation) {
    for (const [key, value] of Object.entries(parsed.fields.initialisation)) {
        md.push(`- initialisation.${key}: ${Array.isArray(value) ? value.join(", ") : value}`);
    }
}

md.push("", "## Page map", "");
md.push("| ID | Version | Start | Blocks | Bytes | Name |");
md.push("|---:|---:|---:|---:|---:|---|");
for (const page of parsed.pageMap) {
    md.push(
        `| 0x${page.pageIdHex} | ${page.version} | 0x${page.startAddress
            .toString(16)
            .toUpperCase()
            .padStart(4, "0")} | ${page.firstBlock}-${page.lastBlock} | ${page.length} | ${page.name} |`,
    );
}

function addObjectSection(title, object) {
    if (!object) return;
    md.push("", `## ${title}`, "");
    for (const [key, value] of Object.entries(object)) {
        if (typeof value === "object") continue;
        md.push(`- ${key}: ${value}`);
    }
}

addObjectSection("Tape write pass", parsed.fields.tapeWritePass);
addObjectSection("EOD", parsed.fields.eod);
addObjectSection("Status and Tape Alert", parsed.fields.statusAndTapeAlert);
addObjectSection("Cartridge content", parsed.fields.cartridgeContent);
addObjectSection("Application specific", parsed.fields.applicationSpecific);
if (parsed.derived.uninitialized) {
    addObjectSection("Uninitialized cartridge", parsed.derived);
} else if (parsed.derived.cleaning) {
    addObjectSection("Derived cleaning summary", parsed.derived.cleaning);
} else if (parsed.derived.lifetime) {
    md.push("", "## Derived LTO-5 summary", "");
    md.push(`- loadCount: ${parsed.derived.loadCount}`);
    md.push(`- currentUsagePage: ${parsed.derived.currentUsagePage}`);
    md.push(`- previousUsagePage: ${parsed.derived.previousUsagePage}`);
    for (const [key, value] of Object.entries(parsed.derived.lastLoad)) {
        md.push(`- lastLoad.${key}: ${value}`);
    }
    addObjectSection("Derived LTO-5 health summary", parsed.derived.lifetime);
}

md.push("", "## Usage information", "");
for (const usage of parsed.fields.usageInformation) {
    md.push(`### Slot ${usage.index}`, "");
    for (const [key, value] of Object.entries(usage)) {
        if (typeof value === "object") continue;
        md.push(`- ${key}: ${value}`);
    }
    md.push("");
}

md.push("## Unknown/extended pages", "");
for (const page of parsed.fields.unknownExtensions) {
    md.push(
        `- 0x${page.pageIdHex}: blocks ${page.firstBlock}-${page.lastBlock}, ${page.length} bytes, strings: ${
            page.printableStrings.map((item) => JSON.stringify(item.value)).join(", ") || "(none)"
        }`,
    );
}

md.push("", "## Selective read plans", "");
for (const [name, plan] of Object.entries(parsed.selectiveReadPlans)) {
    const blocks = plan.blocks ? plan.blocks.join(", ") : plan.ranges.join(", ");
    md.push(`- ${name}: ${blocks} - ${plan.description}`);
}

fs.writeFileSync(`${outputPrefix}.md`, `${md.join("\n")}\n`);
console.log(`${outputPrefix}.json`);
console.log(`${outputPrefix}.md`);
