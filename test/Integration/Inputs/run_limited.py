# Runs a program with its memory (address space) limited to the first
# argument, in MB, so that a test whose program would take much more
# fails by itself instead of taking the machine's memory (where the
# system enforces it: Linux does, macOS does not).
import os
import resource
import sys

limit = int(sys.argv[1]) * 1024 * 1024
try:
    resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
except (ValueError, OSError):
    pass
os.execv(sys.argv[2], sys.argv[2:])
