// Apollo's built-in app covers (600x800 box art) in LuzLunar's look:
// the icon's indigo night sky, LuzLunar sparkles, cream line glyphs.
const { sparkle, C } = require('./icons');

const CREAM = C.cream;
const SW = 24; // glyph stroke weight

// The same quiet constellation on every cover.
const SKY = [
  [96, 118, 16, 0.55], [500, 92, 22, 0.7], [540, 238, 10, 0.45], [64, 392, 9, 0.35],
  [534, 470, 13, 0.4], [112, 668, 12, 0.35], [476, 706, 19, 0.5], [292, 752, 7, 0.3],
];
const DOTS = [[188, 64, 0.35], [402, 168, 0.3], [40, 236, 0.3], [572, 610, 0.3], [222, 716, 0.25], [586, 352, 0.25]];

const sky = () => [
  ...SKY.map(([x, y, r, o]) => `<path d="${sparkle(x, y, r)}" fill="#fff" fill-opacity="${o}"/>`),
  ...DOTS.map(([x, y, o]) => `<circle cx="${x}" cy="${y}" r="2.5" fill="#fff" fill-opacity="${o}"/>`),
].join('\n');

const lineW = (w) => `fill="none" stroke="${CREAM}" stroke-width="${w}" stroke-linecap="round" stroke-linejoin="round"`;
const line = lineW(SW);
const wash = `fill="${CREAM}" fill-opacity="0.12"`;

const monitor = (screen = '') => `
<rect x="170" y="215" width="260" height="170" rx="26" ${wash}/>
${screen}
<rect x="170" y="215" width="260" height="170" rx="26" ${line}/>
<path d="M282 397H318V430H282Z" fill="${CREAM}"/>
<rect x="222" y="426" width="156" height="26" rx="13" fill="${CREAM}"/>`;

const GLYPHS = {
  desktop: monitor(),
  'desktop-alt': monitor(`
<g fill="${CREAM}" fill-opacity="0.9">
<rect x="204" y="247" width="122" height="66" rx="8"/>
<rect x="340" y="247" width="56" height="106" rx="8"/>
<rect x="204" y="325" width="55" height="28" rx="8"/>
<rect x="271" y="325" width="55" height="28" rx="8"/>
</g>`),
  virtual_desktop: monitor(`
<path d="${sparkle(292, 306, 58)}" fill="#fff"/>
<path d="${sparkle(352, 258, 20)}" fill="#fff"/>`),
  steam: `
<path d="M222 236H378C428 236 452 262 463 312L482 400C492 452 450 474 418 442L384 404H216L182 442C150 474 108 452 118 400L137 312C148 262 172 236 222 236Z" fill="${CREAM}"/>
<g fill="${C.ink}">
<rect x="198" y="301" width="62" height="20" rx="6"/>
<rect x="219" y="280" width="20" height="62" rx="6"/>
<circle cx="378" cy="286" r="11"/><circle cx="378" cy="336" r="11"/>
<circle cx="353" cy="311" r="11"/><circle cx="403" cy="311" r="11"/>
</g>
<path d="${sparkle(300, 296, 14)}" fill="${C.ink}"/>`,
  input_only: `
<rect x="236" y="226" width="128" height="200" rx="64" ${wash}/>
<rect x="236" y="226" width="128" height="200" rx="64" ${line}/>
<path d="M300 228V300" ${line}/>
<rect x="290" y="252" width="20" height="38" rx="10" fill="${CREAM}"/>
<path d="M384 214A44 44 0 0 1 410 252" ${lineW(18)}/>
<path d="M394 172A90 90 0 0 1 452 248" ${lineW(18)}/>`,
  terminate: `
<circle cx="300" cy="330" r="118" ${wash}/>
<circle cx="300" cy="330" r="118" ${line}/>
<path d="M256 286L344 374M344 286L256 374" ${lineW(28)}/>`,
  template: '',
};

const LABELS = {
  desktop: 'Desktop',
  'desktop-alt': 'Desktop',
  virtual_desktop: 'Virtual Desktop',
  steam: 'Steam',
  input_only: 'Remote Input',
  terminate: 'Terminate',
  template: '',
};

function cover(name) {
  const label = LABELS[name];
  return `<svg xmlns="http://www.w3.org/2000/svg" width="600" height="800" viewBox="0 0 600 800">
<defs>
<linearGradient id="night" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${C.top}"/><stop offset="1" stop-color="${C.bottom}"/></linearGradient>
<radialGradient id="glow" cx="300" cy="330" r="270" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#6170D6" stop-opacity="0.38"/><stop offset="1" stop-color="#6170D6" stop-opacity="0"/></radialGradient>
</defs>
<rect width="600" height="800" fill="url(#night)"/>
<rect width="600" height="800" fill="url(#glow)"/>
${sky()}
${GLYPHS[name]}
${label ? `<text x="300" y="548" text-anchor="middle" font-family="Segoe UI" font-weight="600" font-size="52" letter-spacing="0.5" fill="#fff">${label}</text>` : ''}
</svg>
`;
}

module.exports = { cover, NAMES: Object.keys(GLYPHS) };
