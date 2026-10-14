#ifndef QUORUM_RUNTIME_SKETCH_TABLES_H
#define QUORUM_RUNTIME_SKETCH_TABLES_H

// A device holds ONE image, so the single-casebook and palette tables share storage: the Uno R4 has 32 KiB of RAM
// and the two together do not fit beside the framework. Both types are trivially destructible.
//
// In a header rather than the .ino because the Arduino build generates prototypes for everything in a .ino that
// looks like a function, and it takes a union's constructor for one.

#include <quorum/runtime/blob.hpp>

union Tables {
    quorum::runtime::Casebook cb;
    quorum::runtime::Palette pal;
    Tables() : cb() {}
    ~Tables() {}
};

#endif
