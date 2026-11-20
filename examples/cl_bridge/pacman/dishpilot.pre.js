// dishpilot on a web page: before the game's main runs, the dish's own
// module (dish.mjs and dish.wasm, next to the page: examples/cl_bridge/web/
// build.sh) is loaded as Module.dish, and the URL's ?seed= (any text,
// hashed as ent_dish.seed_of does: FNV-1a of its UTF-8, its low 31 bits),
// ?stim=0 and ?ghosts= are put where dishpilot.c reads DISH_SEED,
// DISH_STIM and DISH_GHOSTS.
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
  var query = new URLSearchParams(location.search);
  var text = query.get("seed");
  if (text !== null) {
    var seed = text;
    if (!/^[0-9]+$/.test(text)) {
      var h = 0xcbf29ce484222325n;
      for (var b of new TextEncoder().encode(text))
        h = ((h ^ BigInt(b)) * 0x100000001b3n) & 0xffffffffffffffffn;
      seed = String(h & 0x7fffffffn);
    }
    ENV.DISH_SEED = seed;
  }
  if (query.get("stim") !== null) ENV.DISH_STIM = query.get("stim");
  if (query.get("ghosts") !== null) ENV.DISH_GHOSTS = query.get("ghosts");
  addRunDependency("dish");
  import(new URL("dish.mjs", document.baseURI).href)
    .then(function (made) { return made.default(); })
    .then(function (dish) { Module.dish = dish; removeRunDependency("dish"); });
});
