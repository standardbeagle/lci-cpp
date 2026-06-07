# lci-cli

Python installer for [Lightning Code Index (lci)](https://github.com/standardbeagle/lci-cpp)
— sub-millisecond semantic code search for AI assistants.

```sh
uv tool install lci-cli   # or: pipx install lci-cli
lci --version
```

This package is a thin wrapper: on first run it downloads the prebuilt `lci`
binary for your platform from GitHub releases and caches it. No compiler
required.

Update with `uv tool upgrade lci-cli` or the binary's own `lci update`.
