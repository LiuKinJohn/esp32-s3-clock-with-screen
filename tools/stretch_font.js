const fs = require("fs");

if (process.argv.length !== 6) {
    throw new Error("usage: node stretch_font.js <opentype.js> <input.ttf> <output.ttf> <scale-x>");
}

const opentype = require(process.argv[2]);
const inputPath = process.argv[3];
const outputPath = process.argv[4];
const scaleX = Number(process.argv[5]);

if (!Number.isFinite(scaleX) || scaleX <= 0) {
    throw new Error("scale-x must be a positive number");
}

const font = opentype.loadSync(inputPath);

for (let index = 0; index < font.glyphs.length; index += 1) {
    const glyph = font.glyphs.get(index);

    for (const command of glyph.path.commands) {
        if (typeof command.x === "number") command.x *= scaleX;
        if (typeof command.x1 === "number") command.x1 *= scaleX;
        if (typeof command.x2 === "number") command.x2 *= scaleX;
    }

    if (typeof glyph.advanceWidth === "number") glyph.advanceWidth *= scaleX;
    if (typeof glyph.leftSideBearing === "number") glyph.leftSideBearing *= scaleX;
    if (typeof glyph.xMin === "number") glyph.xMin *= scaleX;
    if (typeof glyph.xMax === "number") glyph.xMax *= scaleX;
}

fs.writeFileSync(outputPath, Buffer.from(font.toArrayBuffer()));
