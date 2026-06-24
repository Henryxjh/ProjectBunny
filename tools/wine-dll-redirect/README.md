# wine-dll-redirect

Small `LD_PRELOAD` helper for testing Wine DLL load redirection without placing
the DLL in the game directory.

Build:

```sh
make -C tools/wine-dll-redirect
```

Example:

```sh
LD_PRELOAD="/run/media/share/SharedProjects/ProjectBunny/tools/wine-dll-redirect/wine_dll_redirect.so" \
WINE_DLL_REDIRECTS="d3d12=/run/media/share/XXMI/ZZZ/d3d12.dll" \
WINE_DLL_REDIRECT_LOG="/tmp/wine-dll-redirect.log" \
WINEDLLOVERRIDES="d3d12=n,b" \
wine Game.exe
```

`WINE_DLL_REDIRECTS` is a semicolon-separated list:

```text
d3d12=/path/to/d3d12.dll;dxgi=/path/to/dxgi.dll
```

The helper redirects the first Unix file open for a matching DLL under Wine's
DLL loading path. It only redirects once per DLL name so a proxy DLL can still
load the real system DLL later.

Set `WINE_DLL_REDIRECT_LOG=/path/to/log` to write debug messages to a file. If
no log file is set, `WINE_DLL_REDIRECT_DEBUG=1` writes debug messages to stderr.

Limitations:

- This is a test shim, not a stable Wine loader extension.
- It intercepts common libc file APIs and selected `syscall()` paths. If Wine
  uses another loader path, it may not trigger.
- Proton launchers may rewrite `LD_PRELOAD`; verify with `/proc/<pid>/environ`.
- Anti-cheat software may still reject API hooking or preload-based changes.
