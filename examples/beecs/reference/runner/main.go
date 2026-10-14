// Runs beecs, the reference, for a range of seeds and writes a CSV line
// for each seed and day: the same columns examples/beecs/beecs.ent writes.
//
//	ref_beecs [-params overrides.json] [-from 1] [-to 1] [-timing]
//
// The overrides are beecs's own JSON parameters (only what differs from
// the defaults). With -timing nothing is written; the time a run takes,
// without setting up, goes to stderr.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/mlange-42/ark-tools/app"
	"github.com/mlange-42/ark/ecs"
	"github.com/mlange-42/beecs/globals"
	"github.com/mlange-42/beecs/model"
	"github.com/mlange-42/beecs/params"
)

// report writes the day's line after every other system has run.
type report struct {
	out    *bufio.Writer
	seed   int
	tick   int
	pop    *globals.PopulationStats
	stores *globals.Stores
	aff    *globals.AgeFirstForaging
	stats  *globals.ForagingStats
	period *globals.ForagingPeriod
	energy *params.EnergyContent
}

func (r *report) Initialize(w *ecs.World) {
	r.pop = ecs.GetResource[globals.PopulationStats](w)
	r.stores = ecs.GetResource[globals.Stores](w)
	r.aff = ecs.GetResource[globals.AgeFirstForaging](w)
	r.stats = ecs.GetResource[globals.ForagingStats](w)
	r.period = ecs.GetResource[globals.ForagingPeriod](w)
	r.energy = ecs.GetResource[params.EnergyContent](w)
	r.tick = 0
}

func (r *report) Update(w *ecs.World) {
	if r.out != nil {
		p := r.pop
		fmt.Fprintf(r.out, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%d,%d\n",
			r.seed, r.tick,
			p.WorkerEggs, p.WorkerLarvae, p.WorkerPupae, p.WorkersInHive, p.WorkersForagers,
			p.DroneEggs, p.DroneLarvae, p.DronePupae, p.DronesInHive,
			0.001*r.stores.Honey/r.energy.Honey, r.stores.Pollen,
			r.aff.Aff, len(r.stats.Rounds))
	}
	r.tick++
}

func (r *report) Finalize(w *ecs.World) {}

func main() {
	paramsFile := flag.String("params", "", "JSON file with parameters that differ from the defaults")
	from := flag.Int("from", 1, "first seed")
	to := flag.Int("to", 1, "last seed")
	timing := flag.Bool("timing", false, "write the time each run takes instead of its days")
	flag.Parse()

	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	if !*timing {
		fmt.Fprintln(out, "seed,tick,w_eggs,w_larvae,w_pupae,w_in_hive,foragers,d_eggs,d_larvae,d_pupae,d_in_hive,honey_kg,pollen_g,aff,rounds")
	}
	var a *app.App
	for seed := *from; seed <= *to; seed++ {
		p := params.Default()
		if *paramsFile != "" {
			if err := p.FromJSONFile(*paramsFile); err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
		}
		p.RandomSeed.Seed = seed
		a = model.Default(&p, a)
		r := &report{seed: seed}
		if !*timing {
			r.out = out
		}
		a.AddSystem(r)
		a.Initialize()
		start := time.Now()
		for a.Update() {
		}
		if *timing {
			fmt.Fprintf(os.Stderr, "%d %.6f\n", seed, time.Since(start).Seconds())
		}
		a.Finalize()
	}
}
