// The dish and its Pong (game.c) through WebAssembly: what dish_pong.py
// does through ctypes, frame by frame, for the page's worker and for
// Node.js (check.mjs). dish.mjs and dish.wasm: tools/ent build --shared
// --web network.ent bridge.c game.c -o dish.mjs.
export const TICKS = 200, FRAME_TICKS = 4;

// A seed from any text: FNV-1a (64-bit) of its UTF-8, its low 31 bits
// (ent_dish.seed_of).
export function seedOf(text) {
  if (/^[0-9]+$/.test(text)) return Number(text);
  let h = 0xcbf29ce484222325n;
  for (const byte of new TextEncoder().encode(text))
    h = ((h ^ BigInt(byte)) * 0x100000001b3n) & 0xffffffffffffffffn;
  return Number(h & 0x7fffffffn);
}

// layout 1: game.c's pong_layout (the sensory rows on the side of the dish
// they want), for this dish and game (set before making them); decoder 1:
// game.c's pong_decoder, each motor region read against its own background;
// motor 3: game.c's pong_motor, the motor regions columns 2 and 5, next to
// the sensory electrodes.
export function makeGame(M, seed, gain, cond = 0, ego = 0, layout = 0, decoder = 0, motor = 0) {
  M._pong_layout(layout);
  const dish = M._pong_make_dish(BigInt(seed), gain, cond);
  if (!dish) throw new Error("no dish");
  const game = M._pong_create(dish, BigInt(seed), cond, ego);
  M._pong_decoder(game, decoder);
  M._pong_motor(game, motor);
  const spikes = M._malloc(256), stims = M._malloc(256), state = M._malloc(96);
  const rallies = [];
  let seen = 0;
  return {
    rallies,
    // One frame (20 ms): [t, x, y, paddle, rallies, hits, spikes, stims],
    // as dish_pong.py writes it.
    frame() {
      M.HEAPU8.fill(0, spikes, spikes + 256);
      M.HEAPU8.fill(0, stims, stims + 256);
      if (M._pong_run(game, FRAME_TICKS, spikes, stims) < 0) throw new Error("the dish had no room for its spikes");
      M._pong_state(game, state);
      const s = M.HEAPF64.subarray(state / 8, state / 8 + 12);
      while (seen < s[4]) { rallies.push([Math.round(s[0] * 1000) / 1000, s[6]]); seen++; }
      const sp = M.HEAP32.subarray(spikes / 4, spikes / 4 + 64), st = M.HEAP32.subarray(stims / 4, stims / 4 + 64);
      let a = "", b = "";
      for (let c = 0; c < 64; c++) { a += String.fromCharCode(48 + Math.min(sp[c], 74)); b += st[c]; }
      const r = v => Math.round(v * 1000) / 1000;
      return [r(s[0]), r(s[1]), r(s[2]), r(s[3]), s[4], s[5], a, b];
    },
    // Rallies and hits after the first 5 minutes, moves judged and towards the ball.
    summary() {
      M._pong_state(game, state);
      const s = M.HEAPF64.subarray(state / 8, state / 8 + 12);
      return { rallies: s[4], late: s[10] ? s[11] / s[10] : 0, lateRallies: s[10],
               towards: s[8] ? s[9] / s[8] : 0, hitsAll: s[7] };
    },
  };
}
