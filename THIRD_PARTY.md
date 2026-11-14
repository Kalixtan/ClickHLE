# Third-party software

ClickHLE does not bundle its dependencies. Unicorn Engine is required at runtime and is loaded from the host system (`unicorn.dll` on Windows or a `libunicorn` shared library on macOS and Linux). SDL3 provides optional window, input, and audio support; Qt 6 Widgets provides the optional desktop interface. LLVM 16 provides the optional VC2 JIT compiler.

Each dependency is distributed under its own license. Check the terms for the version and package you install, especially if redistributing a build that includes or links optional libraries.

The native VC2 core in `src/video/platform1/vc2/` includes code derived from the
ProkBox scalar, vector, and peripheral models in `prokbox/prokbox5g`, revision
`fe22071c2706ee8e14e9e51f8eb8df9c73f1a360`.
Those derived files retain GPL-2.0-or-later notices; see
`src/video/platform1/vc2/COPYING`.
They also include locally tested experimental instruction forms. The reference's hypotheses
remain hypotheses in this port. Firmware and game assets are not included.

ClickHLE's original MIT-licensed code remains MIT. A combined executable containing the GPL
VC2 port is distributed under GPL-2.0-or-later terms, with the original MIT notices retained.
