# CrossPad Manifesto

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

### Write once, run everywhere
CrossPad runs the same core logic on ESP32-S3 hardware and desktop simulators. Platform differences are hidden behind thin abstraction layers — not thick frameworks. Business logic should never know what chip it's running on.

### Platform repos are thin, shared repos are thick
Cross-platform is a first-class goal. Platform-specific repositories (platform-idf, crosspad-pc) should contain only what cannot be shared — hardware drivers, HAL bindings, build system glue. All business logic belongs in crosspad-core; all UI components belong in crosspad-gui. If you're writing code that could work on another platform, it doesn't belong in a platform repo.

### Hardware is software you can touch
We design hardware and software as one system. The pad grid, the encoder, the LED strip — they are first-class citizens with well-defined interfaces. If you can simulate it on a PC, you can ship it on a board.

### Small team, big surface area
We are a small team. This is a feature, not a limitation. Every architectural decision is made to reduce maintenance burden: shared init sequences, unified event buses, portable abstractions. Code that exists in two places will eventually exist in one.

### Open by default
Firmware, PC tools and documentation are written to be read, changed and shared. Not because it's trendy, but because a music controller you can't modify isn't yours. We want people to fork, adapt, and build things we never imagined.

### Community-driven, not committee-driven
We welcome contributions, but we ship fast. A clear architecture and good documentation lower the barrier to entry. You shouldn't need to read the entire codebase to add an app or write a new platform driver.

### Documentation is not an afterthought
If it's not documented, it doesn't exist. API references, architecture guides, build instructions — they ship with the code, not after it.
