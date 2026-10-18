// Apollo icon set in LuzLunar's look: geometry and SVG sources.
const f = (n) => (Math.round(n * 10) / 10).toString();
const pol = (cx, cy, r, deg) => {
  const a = (deg * Math.PI) / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
};

// LuzLunar's sparkle (anyosag/LuzLunar, scripts/fork/LuzLunar.icon): four tips joined by
// quadratic curves whose control points sit k*R off both axes, k = 0.16.
function sparkle(cx, cy, R, rot = 0, k = 0.16) {
  const c = k * R * Math.SQRT2;
  let d = '';
  for (let i = 0; i < 4; i++) {
    const tip = pol(cx, cy, R, -90 + rot + i * 90);
    const ctl = pol(cx, cy, c, -45 + rot + i * 90);
    const next = pol(cx, cy, R, rot + i * 90);
    if (i === 0) d += `M${f(tip[0])} ${f(tip[1])}`;
    d += `Q${f(ctl[0])} ${f(ctl[1])} ${f(next[0])} ${f(next[1])}`;
  }
  return d + 'Z';
}

const C = {
  cream: '#F4EBD0', // LuzLunar's moon
  white: '#FFFFFF', // LuzLunar's sparkles
  top: '#34419A', // tile gradient around LuzLunar's #1F296B fill
  bottom: '#18205A',
  ink: '#18205A',
  playing: '#34C759',
  pausing: '#29A8FF', // Apollo's pause blue
  locked: '#FF9F0A',
};

// Layouts, all on a 1024 grid:
// "full": above 48 px, tile inset like an app icon, soft shadow.
// "small": 25-48 px, tile to the edges, wider ring gap.
// "tiny": 17-24 px and the tray SVGs, four rays and a bigger disc.
// "micro": 16 px, tiny with status glyphs on the pixel grid.
// "mac": the .icns, on Apple's 824 px icon grid.
const LAYOUT = {
  full: { inset: 56, radius: 205, disc: 180, gap: 220, rayLong: 410, rayShort: 320, shadow: true },
  small: { inset: 0, radius: 230, disc: 222, gap: 288, rayLong: 500, rayShort: 420, shadow: false },
  tiny: { inset: 0, radius: 230, disc: 268, gap: 336, rayLong: 540, rayShort: 0, shadow: false },
  micro: { inset: 0, radius: 230, disc: 268, gap: 336, rayLong: 540, rayShort: 0, shadow: false, micro: true },
  mac: { inset: 100, radius: 185, disc: 162, gap: 198, rayLong: 370, rayShort: 288, shadow: true },
};

function microGlyph(state) {
  // One 16 px pixel = 64 units; edges land on pixel boundaries.
  if (state === 'playing') return `<path d="M416 352L672 512L416 672Z" fill="${C.ink}"/>`;
  if (state === 'pausing') {
    return `<rect x="384" y="384" width="64" height="256" fill="${C.ink}"/><rect x="576" y="384" width="64" height="256" fill="${C.ink}"/>`;
  }
  if (state === 'locked') {
    return `<path d="M416 512V416A96 96 0 0 1 608 416V512" fill="none" stroke="${C.ink}" stroke-width="64"/><rect x="384" y="512" width="256" height="192" rx="16" fill="${C.ink}"/>`;
  }
  return '';
}

function statusGlyph(state, r) {
  // Drawn in the disc, centered on 512,512, scaled to the disc radius r.
  const s = r / 214;
  if (state === 'playing') {
    // Rounded play triangle, optically centered.
    const cx = 512 + 14 * s, h = 112 * s, w = 104 * s, rr = 14 * s;
    return `<path d="M${f(cx - w + rr)} ${f(512 - h)}L${f(cx + w)} ${f(512 - rr * 0.6)}Q${f(cx + w + rr)} 512 ${f(cx + w)} ${f(512 + rr * 0.6)}L${f(cx - w + rr)} ${f(512 + h)}Q${f(cx - w)} ${f(512 + h + rr * 0.4)} ${f(cx - w)} ${f(512 + h - rr)}L${f(cx - w)} ${f(512 - h + rr)}Q${f(cx - w)} ${f(512 - h - rr * 0.4)} ${f(cx - w + rr)} ${f(512 - h)}Z" fill="${C.ink}"/>`;
  }
  if (state === 'pausing') {
    const bw = 50 * s, bh = 104 * s, gap = 30 * s, rr = 14 * s;
    return [
      `<rect x="${f(512 - gap - bw)}" y="${f(512 - bh)}" width="${f(bw)}" height="${f(2 * bh)}" rx="${f(rr)}" fill="${C.ink}"/>`,
      `<rect x="${f(512 + gap)}" y="${f(512 - bh)}" width="${f(bw)}" height="${f(2 * bh)}" rx="${f(rr)}" fill="${C.ink}"/>`,
    ].join('');
  }
  if (state === 'locked') {
    // Padlock: shackle ring above a rounded body.
    const bw = 104 * s, bh = 76 * s, by = 512 - 10 * s, rr = 18 * s;
    const sw = 26 * s, sr = 58 * s, sy = by;
    const shackle = `<path d="M${f(512 - sr)} ${f(sy)}V${f(sy - 56 * s)}A${f(sr)} ${f(sr)} 0 0 1 ${f(512 + sr)} ${f(sy - 56 * s)}V${f(sy)}" fill="none" stroke="${C.ink}" stroke-width="${f(sw)}" stroke-linecap="round"/>`;
    const body = `<rect x="${f(512 - bw)}" y="${f(by)}" width="${f(2 * bw)}" height="${f(2 * bh)}" rx="${f(rr)}" fill="${C.ink}"/>`;
    return shackle + body;
  }
  return '';
}

// state: null (the logo) | 'playing' | 'pausing' | 'locked'
function svg(layout, state = null) {
  const L = LAYOUT[layout];
  const t = L.inset, w = 1024 - 2 * t;
  const rays = sparkle(512, 512, L.rayLong) + (L.rayShort ? ' ' + sparkle(512, 512, L.rayShort, 45) : '');
  const discFill = state ? C[state] : C.cream;
  const defs = [
    `<linearGradient id="tile" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${C.top}"/><stop offset="1" stop-color="${C.bottom}"/></linearGradient>`,
    `<mask id="corona" maskUnits="userSpaceOnUse" x="0" y="0" width="1024" height="1024"><rect width="1024" height="1024" fill="#fff"/><circle cx="512" cy="512" r="${L.gap}" fill="#000"/></mask>`,
    `<clipPath id="edge"><rect x="${t}" y="${t}" width="${w}" height="${w}" rx="${L.radius}"/></clipPath>`,
  ];
  if (L.shadow) {
    defs.push(`<filter id="lift" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="10" stdDeviation="16" flood-color="#0B0F2E" flood-opacity="0.45"/></filter>`);
  }
  const title = state ? `Apollo (${state})` : 'Apollo';
  return `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024" role="img" aria-label="${title}">
<title>${title}</title>
<defs>${defs.join('')}</defs>
<rect x="${t}" y="${t}" width="${w}" height="${w}" rx="${L.radius}" fill="url(#tile)"/>
<g clip-path="url(#edge)"${L.shadow ? ' filter="url(#lift)"' : ''}>
<path d="${rays}" fill="${C.white}" mask="url(#corona)"/>
<circle cx="512" cy="512" r="${L.disc}" fill="${discFill}"/>
${L.micro ? microGlyph(state) : statusGlyph(state, L.disc)}
</g>
</svg>
`;
}

module.exports = { svg, sparkle, C };
