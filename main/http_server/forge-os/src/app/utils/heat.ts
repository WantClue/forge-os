// Forge glow: cool steel when idle, straw to orange while mining normally,
// ember at the firmware's shutoff (75 °C for ASICs, 105 °C for the VR).
export const ASIC_SHUTOFF_C = 75;
export const VR_SHUTOFF_C = 105;

type Rgb = [number, number, number];

const HEAT_STOPS: Array<[number, Rgb]> = [
  [0.53, [143, 166, 191]], // steel
  [0.73, [242, 196, 109]], // straw
  [0.87, [247, 137, 43]],  // forge orange
  [1.0, [229, 72, 43]],    // ember
];

const rgb = ([r, g, b]: Rgb) => `rgb(${Math.round(r)}, ${Math.round(g)}, ${Math.round(b)})`;

export function heatColor(temp: number, shutoff = ASIC_SHUTOFF_C): string {
  const t = Math.max(0, temp || 0) / shutoff;
  if (t <= HEAT_STOPS[0][0]) return rgb(HEAT_STOPS[0][1]);
  for (let i = 1; i < HEAT_STOPS.length; i++) {
    const [pos, color] = HEAT_STOPS[i];
    if (t <= pos) {
      const [prevPos, prevColor] = HEAT_STOPS[i - 1];
      const k = (t - prevPos) / (pos - prevPos);
      return rgb(prevColor.map((c, j) => c + (color[j] - c) * k) as Rgb);
    }
  }
  return rgb(HEAT_STOPS[HEAT_STOPS.length - 1][1]);
}

// Hottest part relative to its own shutoff, expressed on the ASIC scale, so
// one colour can stand for "how close is anything to tripping".
export function hottestTemp(asicTemps: Array<number | undefined | null>, vrTemp?: number | null): number {
  const asic = Math.max(0, ...asicTemps.map((t) => t || 0));
  const vr = vrTemp && vrTemp > 0 ? (vrTemp / VR_SHUTOFF_C) * ASIC_SHUTOFF_C : 0;
  return Math.max(asic, vr);
}
