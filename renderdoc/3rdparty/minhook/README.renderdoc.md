# MinHook

Upstream: https://github.com/TsudaKageyu/minhook

Version: v1.3.4, commit c3fcafdc10146beb5919319d0683e44e3c30d537.

The include/ and src/ directories are copied without modifications. See LICENSE.txt.

Used by the optional Windows graphics entry-point hooks. The normal IAT-based
hooking path remains the default. Set RENDERDOC_WIN32_INLINE_GRAPHICS_HOOKS=1
in the process environment before launch to enable the compatibility path.
