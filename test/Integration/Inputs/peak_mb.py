# peak_mb.py LIMIT_MB COMMAND...: runs the command (its output dropped) and
# fails if it failed or held more than LIMIT_MB of memory at its most.
import resource, subprocess, sys

limit = int(sys.argv[1])
done = subprocess.run(sys.argv[2:], stdout=subprocess.DEVNULL)
peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
peak_mb = peak // (1 << 20) if sys.platform == "darwin" else peak // 1024
print(f"peak {peak_mb} MB (limit {limit})")
sys.exit(1 if done.returncode or peak_mb > limit else 0)
