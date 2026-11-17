// fe-export <project-or-scene> <outdir> [--name N] [--scene S] [--player P] [--run [FRAMES]]
// The editor's export (File > Export game) on the PC, without a screen.
#include "export.h"

int main(int argc, char** argv) { return fe::export_cli(argc, argv, 0); }
