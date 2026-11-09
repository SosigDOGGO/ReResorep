// The hero's Tint demo. Generates an icy crack texture, then applies the same three blends as
// the app's Tint dialog (manager/tint.cpp): Recolor, Colorize and Multiply.
(() => {
  const before = document.getElementById("before");
  const after = document.getElementById("after");
  const hue = document.getElementById("hue");
  const strength = document.getElementById("strength");
  const help = document.getElementById("mode-help");
  if (!before || !after) return;

  const W = before.width, H = before.height;
  const bctx = before.getContext("2d");
  const actx = after.getContext("2d");

  const HELP = {
    recolor: "Takes the colour's hue and keeps each pixel's saturation and brightness, so whites and shading stay.",
    colorize: "Puts the colour's hue and saturation on every pixel, grey and white parts included.",
    multiply: "Multiplies the texture by the colour, like tinted glass.",
  };

  // Seeded random, so the texture is the same on every visit.
  let seed = 7;
  const rand = () => ((seed = (seed * 16807) % 2147483647) - 1) / 2147483646;

  // Voronoi cells with bright crack lines along their edges, tiled so the pattern wraps.
  const cells = Array.from({ length: 46 }, () => ({ x: rand() * W, y: rand() * H, v: 0.16 + rand() * 0.42 }));
  const base = bctx.createImageData(W, H);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      let d1 = Infinity, d2 = Infinity, v = 0;
      for (const c of cells) {
        let dx = Math.abs(x - c.x), dy = Math.abs(y - c.y);
        if (dx > W / 2) dx = W - dx;
        if (dy > H / 2) dy = H - dy;
        const d = dx * dx + dy * dy;
        if (d < d1) { d2 = d1; d1 = d; v = c.v; } else if (d < d2) d2 = d;
      }
      const edge = Math.sqrt(d2) - Math.sqrt(d1);            // distance to the nearest crack
      const crack = Math.max(0, 1 - edge / 1.8);
      const glow = Math.max(0, 1 - edge / 9) * 0.35;
      const grain = (rand() - 0.5) * 0.06;
      const val = Math.min(1, v + glow * 0.5 + crack * 0.6 + grain + (y / H) * 0.08);
      const sat = 0.62 * (1 - crack * 0.85);
      const [r, g, b] = hsvToRgb(204 + (v - 0.35) * 30, sat, val);
      const i = (y * W + x) * 4;
      base.data[i] = r; base.data[i + 1] = g; base.data[i + 2] = b; base.data[i + 3] = 255;
    }
  }
  bctx.putImageData(base, 0, 0);
  const out = actx.createImageData(W, H);

  function render() {
    const mode = document.querySelector('input[name="mode"]:checked').value;
    const h = +hue.value, k = +strength.value / 100;
    const [tr, tg, tb] = hsvToRgb(h, 0.85, 0.95);           // the picked colour
    const src = base.data, dst = out.data;
    for (let i = 0; i < src.length; i += 4) {
      const r = src[i], g = src[i + 1], b = src[i + 2];
      let nr, ng, nb;
      if (mode === "recolor") {
        const [, s, v] = rgbToHsv(r, g, b);
        [nr, ng, nb] = hsvToRgb(h, s, v);
      } else if (mode === "colorize") {
        const l = (Math.max(r, g, b) + Math.min(r, g, b)) / 510;
        [nr, ng, nb] = hslToRgb(h, 0.85, l);
      } else {
        nr = (r * tr) / 255; ng = (g * tg) / 255; nb = (b * tb) / 255;
      }
      dst[i] = r + (nr - r) * k;
      dst[i + 1] = g + (ng - g) * k;
      dst[i + 2] = b + (nb - b) * k;
      dst[i + 3] = 255;
    }
    actx.putImageData(out, 0, 0);
    help.textContent = HELP[mode];
  }

  let introFrame;
  hue.addEventListener("input", () => {
    cancelAnimationFrame(introFrame);
    render();
  });
  strength.addEventListener("input", render);
  document.querySelectorAll('input[name="mode"]').forEach((el) => el.addEventListener("change", render));

  // One moment of motion on load: sweep the colour from the original blue to red, so it's clear
  // the panel is live. Skipped when reduced motion is asked for.
  const target = +hue.value;
  if (!matchMedia("(prefers-reduced-motion: reduce)").matches) {
    const from = 204, span = (target - from + 360) % 360, start = performance.now(), dur = 1100;
    const step = (t) => {
      const p = Math.min(1, (t - start) / dur);
      const e = 1 - Math.pow(1 - p, 3);
      hue.value = Math.round(from + span * e) % 360;
      render();
      if (p < 1) introFrame = requestAnimationFrame(step);
    };
    introFrame = requestAnimationFrame(step);
  } else {
    render();
  }

  function rgbToHsv(r, g, b) {
    const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
    let h = 0;
    if (d) {
      if (max === r) h = ((g - b) / d) % 6;
      else if (max === g) h = (b - r) / d + 2;
      else h = (r - g) / d + 4;
      h *= 60;
      if (h < 0) h += 360;
    }
    return [h, max ? d / max : 0, max / 255];
  }

  function hsvToRgb(h, s, v) {
    h = ((h % 360) + 360) % 360;
    const c = v * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m = v - c;
    const [r, g, b] = h < 60 ? [c, x, 0] : h < 120 ? [x, c, 0] : h < 180 ? [0, c, x]
      : h < 240 ? [0, x, c] : h < 300 ? [x, 0, c] : [c, 0, x];
    return [(r + m) * 255, (g + m) * 255, (b + m) * 255];
  }

  function hslToRgb(h, s, l) {
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const v = l + c / 2;
    return hsvToRgb(h, v ? (2 * (v - l)) / v : 0, v);
  }
})();
