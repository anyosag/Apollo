// Renders Apollo's icons, tray icons and app covers from icons.js and covers.js
// into the repo (or into the folder given as the first argument).
// Run from this folder: npm install, then npm run build.
const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');
const { svg } = require('./icons');
const { cover, NAMES } = require('./covers');

const out = process.argv[2] || path.resolve(__dirname, '../../..');

// Cover labels use Segoe UI Semibold, Windows' counterpart to LuzLunar's SF Pro.
const FONT = process.env.APOLLO_COVER_FONT || 'C:/Windows/Fonts/seguisb.ttf';
if (!fs.existsSync(FONT)) {
  console.error(`Cover font not found: ${FONT} (set APOLLO_COVER_FONT to Segoe UI Semibold)`);
  process.exit(1);
}
const png = (src, w) =>
  new Resvg(src, {
    fitTo: { mode: 'width', value: w },
    font: { loadSystemFonts: false, fontFiles: [FONT], defaultFontFamily: 'Segoe UI' },
  }).render().asPng();
const write = (rel, data) => {
  const p = path.join(out, rel);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, data);
};

// The sizes scripts/icons/convert_and_pack.sh packs: 16-64 in steps of 4,
// 64-256 in steps of 16.
const ICO_SIZES = [...new Set([
  ...Array.from({ length: 13 }, (_, i) => 16 + i * 4),
  ...Array.from({ length: 13 }, (_, i) => 64 + i * 16),
])].sort((a, b) => a - b);

// An .ico of PNG images, like go-png2ico makes.
function ico(pngs) {
  const header = Buffer.alloc(6 + 16 * pngs.length);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(pngs.length, 4);
  let offset = header.length;
  pngs.forEach(([size, data], i) => {
    const e = 6 + i * 16;
    header.writeUInt8(size >= 256 ? 0 : size, e);
    header.writeUInt8(size >= 256 ? 0 : size, e + 1);
    header.writeUInt16LE(1, e + 4);
    header.writeUInt16LE(32, e + 6);
    header.writeUInt32LE(data.length, e + 8);
    header.writeUInt32LE(offset, e + 12);
    offset += data.length;
  });
  return Buffer.concat([header, ...pngs.map(([, d]) => d)]);
}

function icns(entries) {
  const body = Buffer.concat(entries.map(([type, data]) => {
    const h = Buffer.alloc(8);
    h.write(type, 0, 'ascii');
    h.writeUInt32BE(data.length + 8, 4);
    return Buffer.concat([h, data]);
  }));
  const h = Buffer.alloc(8);
  h.write('icns', 0, 'ascii');
  h.writeUInt32BE(body.length + 8, 4);
  return Buffer.concat([h, body]);
}

// Each size gets the layout drawn for it (see LAYOUT in icons.js).
const appLayout = (z) => (z <= 16 ? 'micro' : z <= 24 ? 'tiny' : z <= 48 ? 'small' : 'full');
const statusLayout = (z) => (z <= 16 ? 'micro' : z <= 24 ? 'tiny' : 'small');
const icoFor = (layoutAt, state) => ico(ICO_SIZES.map((z) => [z, png(svg(layoutAt(z), state), z)]));

const IMG = 'src_assets/common/assets/web/public/images';

// App icon: the Windows executable and installer, Linux packages, macOS.
write('apollo.svg', svg('full'));
write('apollo.png', png(svg('full'), 1024));
const appIco = icoFor(appLayout, null);
write('apollo.ico', appIco);
const mac = (z) => png(svg(z <= 32 ? 'small' : 'mac'), z);
write('apollo.icns', icns([
  ['icp4', mac(16)], ['icp5', mac(32)], ['ic11', mac(32)], ['ic12', mac(64)],
  ['ic07', mac(128)], ['ic13', mac(256)], ['ic08', mac(256)], ['ic14', mac(512)],
  ['ic09', mac(512)], ['ic10', mac(1024)],
]));

// Web UI (favicon, navbar logo) and the tray, idle and per state.
write(`${IMG}/apollo.ico`, appIco);
write(`${IMG}/logo-apollo.svg`, svg('full'));
write(`${IMG}/logo-apollo-16.png`, png(svg('micro'), 16));
write(`${IMG}/logo-apollo-45.png`, png(svg('small'), 45));
for (const state of ['playing', 'pausing', 'locked']) {
  write(`${IMG}/apollo-${state}.svg`, svg('tiny', state));
  write(`${IMG}/apollo-${state}.png`, png(svg('tiny', state), 1024));
  write(`${IMG}/apollo-${state}.ico`, icoFor(statusLayout, state));
  write(`${IMG}/apollo-${state}-16.png`, png(svg('micro', state), 16));
  write(`${IMG}/apollo-${state}-45.png`, png(svg('small', state), 45));
}

// Covers for the built-in apps. box.png stays: Moonlight treats a 130x180
// image as "no box art" and draws its own placeholder with the app name.
for (const name of NAMES) write(`src_assets/common/assets/${name}.png`, png(cover(name), 600));

console.log(`Wrote Apollo icons and ${NAMES.length} covers to ${out}`);
