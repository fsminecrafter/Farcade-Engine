// Human readable .mof summary (used by mofconv --info and the tests).
#pragma once
#include <stdio.h>
#include "mof/mof.h"

void mof_print_info(const mof_file* f, FILE* out);
