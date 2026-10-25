/* What treecode 1.4 (NEMO's src/nbody/evolve/hackcode/treecode1) takes
   from NEMO's stdinc.h, to build it by itself (bench/treecode/run.py). */
#ifndef SHIM_STDINC_H
#define SHIM_STDINC_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef double real;
typedef char *string;
typedef FILE *stream;
typedef int bool;
#define TRUE 1
#define FALSE 0
#define local static
#define PI 3.14159265358979323846

void *allocate(int bytes);
void error(string format, ...);
double cputime(void);
stream stropen(string name, string mode);
void strclose(stream str);
bool strnull(string s);
bool scanopt(string options, string word);
string getargv0(void);
string getversion(void);
#define streq(a, b) (strcmp((a), (b)) == 0)
#endif
