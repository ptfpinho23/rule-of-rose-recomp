#pragma once
#include <QString>

// What kind of graphics hardware the game's GPU renderer would get here.
// The launcher uses it only to choose defaults for a fresh install and to
// tell the player; saved choices are never changed by it.
struct GpuInfo {
    enum Kind {
        None,         // no Vulkan driver the game's GPU renderer can use
        Cpu,          // only a Vulkan implementation that runs on the processor
        Integrated,   // graphics built into the processor
        Discrete,     // a separate graphics card
    };
    Kind kind = None;
    QString name;     // the device's own name; empty for None

    static const char *kindName(Kind k);
    static bool parseKind(const QString &s, Kind *out);
};

// Runs in a child process (main() calls it for "--probe-gpu" before anything
// else): asks the Vulkan loader which device the game would pick, prints one
// line "GPU <kind> <name>" on stdout and returns. A driver that crashes or
// hangs then takes only that child with it.
int gpuProbeChildMain();

// Parse the child's output; anything unexpected reads as None.
GpuInfo parseGpuProbeOutput(const QByteArray &out);
