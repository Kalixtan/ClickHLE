# Third-party software

ClickHLE does not bundle its dependencies. Unicorn Engine is required at runtime and is loaded from the host system (`unicorn.dll` on Windows or a `libunicorn` shared library on macOS and Linux). SDL3 and OpenGL provide optional video and audio support; Qt 6 Widgets provides the optional desktop interface.

Each dependency is distributed under its own license. Check the terms for the version and package you install, especially if redistributing a build that includes or links optional libraries.
