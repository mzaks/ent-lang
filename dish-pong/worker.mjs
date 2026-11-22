// The page's worker: two dishes of one seed, untrained (reward gain 0) and
// trained (gain 1), each with its Pong, frame by frame; posts the frames
// to the page in batches. Messages in: {start: {text, cond, ego, layout, decoder,
// minutes}},
// {speed: 1 | 0 (as fast as it goes)}, {pause: bool}.
import Dish from "./dish.mjs";
import { makeGame, seedOf, TICKS, FRAME_TICKS } from "./pong.mjs";

const FRAME_S = FRAME_TICKS / TICKS;
let run = null, speed = 1, paused = false;

async function start({ text, cond, ego = 0, layout = 0, decoder = 0, minutes }) {
  const seed = seedOf(text);
  const games = [];
  for (const gain of [0, 1]) games.push(makeGame(await Dish(), seed, gain, cond, ego, layout, decoder));
  run = { games, seed, frames: Math.round(minutes * 60 / FRAME_S), done: 0, began: performance.now(), simAtBegin: 0, id: Symbol() };
  postMessage({ type: "started", seed, text });
  loop(run.id);
}

function loop(id) {
  if (!run || run.id !== id) return;
  if (paused) { setTimeout(() => loop(id), 100); return; }
  const now = performance.now();
  // Real time: the frames due by now; fast: what 40 ms of work allows.
  let want;
  if (speed > 0) {
    const due = run.simAtBegin + Math.floor((now - run.began) / 1000 / FRAME_S * speed);
    want = Math.min(due - run.done, 50);
  } else want = Infinity;
  const batch = [[], []];
  const t = performance.now();
  while (run.done < run.frames && batch[0].length < want && performance.now() - t < 40) {
    for (let i = 0; i < 2; i++) batch[i].push(run.games[i].frame());
    run.done++;
  }
  if (batch[0].length)
    postMessage({ type: "frames", batch, rallies: run.games.map(g => g.rallies.slice(-50)),
                  summary: run.games.map(g => g.summary()), done: run.done, of: run.frames });
  if (run.done >= run.frames) { postMessage({ type: "finished" }); return; }
  setTimeout(() => loop(id), speed > 0 ? 15 : 0);
}

onmessage = e => {
  const m = e.data;
  if (m.start) start(m.start).catch(err => postMessage({ type: "error", message: String(err) }));
  if (m.speed !== undefined) {
    if (run) { run.began = performance.now(); run.simAtBegin = run.done; }
    speed = m.speed;
  }
  if (m.pause !== undefined) {
    paused = m.pause;
    if (!paused && run) { run.began = performance.now(); run.simAtBegin = run.done; }
  }
};
