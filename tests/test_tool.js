// SPDX-License-Identifier: GPL-3.0-or-later

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const {spawnSync} = require("child_process");

const repositoryRoot = path.resolve(__dirname, "..");
const tool = path.join(repositoryRoot, "tools", "lto_cm_parse.js");
const temporaryDirectory = fs.mkdtempSync(path.join(os.tmpdir(), "lto-cm-test-"));

function parseFixture(name) {
    const input = path.join(repositoryRoot, "tests", "fixtures", name);
    const output = path.join(temporaryDirectory, path.parse(name).name);
    const result = spawnSync(process.execPath, [tool, input, output], {encoding: "utf8"});
    assert.strictEqual(result.status, 0, result.stderr || result.stdout);
    return JSON.parse(fs.readFileSync(`${output}.json`, "utf8"));
}

try {
    const lto5 = parseFixture("lto5_health.bin");
    assert.strictEqual(lto5.cmManufacturerInformation.serialCheckValid, true);
    assert.strictEqual(lto5.fields.cartridgeManufacturer.format, "LTO-5");
    assert.strictEqual(lto5.fields.cartridgeManufacturer.cleaning, false);
    assert.strictEqual(lto5.fields.cartridgeManufacturer.dateOfManufacture, "20240102");
    assert.strictEqual(lto5.derived.loadCount, 42);
    assert.strictEqual(lto5.derived.currentUsagePage, "110B");
    assert.strictEqual(lto5.derived.lifetime.setsWritten, "1000");
    assert.strictEqual(lto5.derived.lifetime.setsRead, "2000");
    assert.strictEqual(lto5.derived.lifetime.writeRetries, 11);
    assert.strictEqual(lto5.derived.lifetime.readRetries, 12);
    assert.strictEqual(lto5.derived.lifetime.maximumTapeTemperatureC, 42);
    assert.strictEqual(lto5.derived.lifetime.estimatedBytesWritten, "2473000000");
    assert.strictEqual(lto5.derived.lifetime.estimatedBytesRead, "4946000000");

    const factory = parseFixture("lto5_uninitialized.bin");
    assert.deepStrictEqual(factory.derived, {uninitialized: true, loadCount: 0, usageAvailable: false});
    assert.strictEqual(factory.fields.initialisation, null);
    assert.deepStrictEqual(factory.fields.usageInformation, []);

    const cleaning = parseFixture("cleaning_32byte.bin");
    assert.strictEqual(cleaning.cmManufacturerInformation.serialCheckValid, true);
    assert.strictEqual(cleaning.fields.cartridgeManufacturer.format, "Cleaning");
    assert.strictEqual(cleaning.fields.cartridgeManufacturer.cleaning, true);
    assert.strictEqual(cleaning.fields.cartridgeManufacturer.dateOfManufacture, "20240203");
    assert.strictEqual(cleaning.fields.statusAndTapeAlert.pageLength, 32);
    assert.deepStrictEqual(cleaning.derived.cleaning, {
        cleansPerformed: 10,
        tapeLengthQuarterMetres: 1200,
        usedQuarterMetres: 220,
        estimatedRemaining: 42,
        expired: false,
    });

    console.log("All JavaScript dump-tool regression tests passed");
} finally {
    fs.rmSync(temporaryDirectory, {recursive: true, force: true});
}
