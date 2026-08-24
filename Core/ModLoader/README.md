# Vendored mod-loader headers

`fable_mod_api.h` and `fable_frontend.h` are **copies** from
[fable-mod-loader](https://github.com/jeremypotter321/fable-mod-loader), not a submodule.

They are copied rather than referenced because that is the whole point of them: they are a
plain **C** ABI, and this project cannot share a build with the loader. Core links a
prebuilt MSVC static library (SLikeNet), so it is MSVC-only; the loader cross-compiles with
mingw so it can be built and tested from macOS. Two compilers cannot agree on a C++ ABI --
name mangling, exceptions and object layout all differ -- but they agree on C structs and
function pointers.

Both structs carry their own `size` and an `abi_version`, so a mismatch between this copy
and the loader in the game folder is detected rather than guessed at: the loader refuses a
mod whose `abi_version` differs, and `FABLE_HOST_HAS` / `FABLE_FRONTEND_HAS` test for
fields a copy may be too old to know about.

**When updating:** copy both files whole from the loader repo. Do not edit them here.
