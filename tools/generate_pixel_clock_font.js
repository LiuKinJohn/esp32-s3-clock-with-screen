const fs = require("fs");

if (process.argv.length !== 4) {
    throw new Error("usage: node generate_pixel_clock_font.js <opentype.js> <output.ttf>");
}

const opentype = require(process.argv[2]);
const outputPath = process.argv[3];

const patterns = {
    "0": ["0111110", "1100011", "1100011", "1100011", "1100011", "1100011", "1100011", "1100011", "0111110"],
    "1": ["0011000", "0111000", "0011000", "0011000", "0011000", "0011000", "0011000", "0011000", "1111111"],
    "2": ["0111110", "1100011", "0000011", "0000011", "0001110", "0110000", "1100000", "1100000", "1111111"],
    "3": ["0111110", "1100011", "0000011", "0000011", "0011110", "0000011", "0000011", "1100011", "0111110"],
    "4": ["0001110", "0011110", "0110110", "1100110", "1100110", "1111111", "0000110", "0000110", "0000110"],
    "5": ["1111111", "1100000", "1100000", "1111110", "0000011", "0000011", "0000011", "1100011", "0111110"],
    "6": ["0011110", "0110000", "1100000", "1111110", "1100011", "1100011", "1100011", "1100011", "0111110"],
    "7": ["1111111", "0000011", "0000110", "0000110", "0001100", "0011000", "0011000", "0110000", "0110000"],
    "8": ["0111110", "1100011", "1100011", "1100011", "0111110", "1100011", "1100011", "1100011", "0111110"],
    "9": ["0111110", "1100011", "1100011", "1100011", "0111111", "0000011", "0000011", "0000110", "0111100"],
    ":": ["00", "00", "11", "11", "00", "00", "11", "11", "00"],
};

const cellWidth = 110;
const cellHeight = 100;
const gapX = 15;
const gapY = 15;
const digitAdvance = 900;
const colonAdvance = 275;

function addPixel(path, x, y) {
    path.moveTo(x, y);
    path.lineTo(x + cellWidth, y);
    path.lineTo(x + cellWidth, y + cellHeight);
    path.lineTo(x, y + cellHeight);
    path.close();
}

function createGlyph(character, pattern) {
    const path = new opentype.Path();
    const rows = pattern.length;

    for (let row = 0; row < rows; row += 1) {
        for (let column = 0; column < pattern[row].length; column += 1) {
            if (pattern[row][column] !== "1") continue;
            const x = column * (cellWidth + gapX);
            const y = (rows - row - 1) * (cellHeight + gapY);
            addPixel(path, x, y);
        }
    }

    return new opentype.Glyph({
        name: character === ":" ? "colon" : character,
        unicode: character.codePointAt(0),
        advanceWidth: character === ":" ? colonAdvance : digitAdvance,
        path,
    });
}

const glyphs = [
    new opentype.Glyph({name: ".notdef", advanceWidth: digitAdvance, path: new opentype.Path()}),
    ...Object.entries(patterns).map(([character, pattern]) => createGlyph(character, pattern)),
];

const font = new opentype.Font({
    familyName: "Pixel Clock",
    styleName: "Bold",
    unitsPerEm: 1440,
    ascender: 1020,
    descender: 0,
    glyphs,
});

fs.writeFileSync(outputPath, Buffer.from(font.toArrayBuffer()));
