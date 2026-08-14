# Simulation LTLf for VS Code

A dependency-free VS Code language extension for this repository's `.spec` DSL. It provides syntax highlighting, comment toggling, bracket matching, automatic closing pairs, and live parser/type-check diagnostics from the authoritative C++ implementation compiled to WebAssembly.

## Try it from this checkout

Open `editors/vscode-ltlf` in VS Code and press `F5`, then open any file in `scenarios/*.spec` in the Extension Development Host.

To create an installable package, run:

```sh
npx @vscode/vsce package
```

from this directory, then install the resulting `.vsix` with **Extensions: Install from VSIX...**.

This checkout already includes `simulation-ltlf-0.1.0.vsix`, which can also be installed from a shell with:

```sh
code --install-extension simulation-ltlf-0.1.0.vsix
```

## Type-checking environment

By default, type checking considers routing module IDs 1 through 24 and tower module 255 available. Set `ltlf.moduleIds` in VS Code settings to the IDs from the selected layout for exact module-existence diagnostics.

## Rebuilding the validator

After activating an Emscripten SDK, run:

```sh
./scripts/build-wasm.sh
```

The wrapper compiles `src/ltl/lexer.cpp`, `src/ltl/parser.cpp`, and `src/ltl/checker.cpp` directly. No parser or type rules are duplicated in the extension.
