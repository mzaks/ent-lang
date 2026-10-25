/* What treecode takes from NEMO's getparam.h (bench/treecode/shim):
   `name=value` arguments over the defaults of `defv`. */
#ifndef SHIM_GETPARAM_H
#define SHIM_GETPARAM_H
#include "stdinc.h"
#define ARGPARAM 1
string getparam(string name);
int getiparam(string name);
double getdparam(string name);
bool getbparam(string name);
int getparamstat(string name);
#endif
