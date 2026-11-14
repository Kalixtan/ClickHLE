# Next steps

- Keep filling Platform 1 API and VC2 gaps from traceable evidence. Compare JIT
  results with the interpreter and retain a clear note where behavior is still
  provisional.
- Finish Platform 3 graphics state and OpenGL ES coverage, then test frame
  output across more than Sonic and Mini Golf. Audio and several OS services
  still have temporary stubs.
- Identify the hardware families for Platform IDs 2 and 4 before adding their
  APIs. They currently take the Platform 3 placeholder path.
- Validate music browsing against a real user-provided iTunesDB, including
  artwork and playlist behavior. The drive initializer creates only an empty
  track and playlist database.
- Add a repeatable Windows runtime deployment step and test the GUI on Windows.
  A normal CMake build does not bundle Unicorn, Qt, or SDL runtime DLLs.
- Keep firmware, game media, keys, captures, and reverse-engineering notes out
  of Git. The ignored `docs/` folder is for local working notes.
