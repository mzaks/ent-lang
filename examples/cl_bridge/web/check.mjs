// Node.js: plays a seed as dish_pong.py does and prints the frames' JSON,
// to compare with dish_pong.py's (record=0) byte for byte.
//   node check.mjs dish.mjs minutes seed-text gain [ego [layout [decoder]]]
import { makeGame, seedOf, TICKS, FRAME_TICKS } from "./pong.mjs";
import { pathToFileURL } from "node:url";
import path from "node:path";
const [, , module, minutes, text, gain, ego = "0", layout = "0", decoder = "0"] = process.argv;
const Dish = (await import(pathToFileURL(path.resolve(module)).href)).default;
const M = await Dish();
const g = makeGame(M, seedOf(text), Number(gain), 0, Number(ego), Number(layout), Number(decoder));
const frames = [];
const t = performance.now();
for (let f = 0; f < Math.round(Number(minutes) * 60 * TICKS / FRAME_TICKS); f++) frames.push(g.frame());
console.error(`${((performance.now() - t) / 1000).toFixed(1)} s for ${minutes} minutes`);
console.log(JSON.stringify({ frames, rallies: g.rallies }));
