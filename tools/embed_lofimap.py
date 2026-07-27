"""PlatformIO pre-build hook: embed assets/lofimap.bin into the firmware.

`board_build.embed_files` only works under the ESP-IDF framework, not the
precompiled Arduino one used here, so we replicate it: objcopy turns the binary
into a linkable object exposing _binary_lofimap_bin_start/_end (see lofimap.cpp),
and we add that object to the link. Rebuilt only when the blob changes.
"""
Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
import os
import subprocess

blob = os.path.join(env["PROJECT_DIR"], "assets", "lofimap.bin")
build_dir = env.subst("$BUILD_DIR")
obj = os.path.join(build_dir, "lofimap_bin.o")

# Resolve the Xtensa objcopy from the installed toolchain package ($CC isn't the
# cross-compiler yet this early in the build).
toolchain = env.PioPlatform().get_package_dir("toolchain-xtensa-esp-elf")
objcopy = os.path.join(toolchain, "bin", "xtensa-esp-elf-objcopy")

if not os.path.isfile(blob):
    print("[embed] WARNING: %s missing — lo-fi map will be disabled" % blob)
else:
    if not os.path.exists(obj) or os.path.getmtime(blob) > os.path.getmtime(obj):
        os.makedirs(build_dir, exist_ok=True)
        # Run from the blob's directory so the symbol name is derived from the
        # bare filename: _binary_lofimap_bin_start / _end.
        # objcopy defaults binary blobs to a .data section, which the linker
        # places in DRAM — 292 KB would blow the ~200 KB RAM budget. Rename it to
        # .rodata.* so it's collected into the flash segment (read-only) instead.
        subprocess.check_call(
            [objcopy, "-I", "binary", "-O", "elf32-xtensa-le", "-B", "xtensa",
             "--rename-section",
             ".data=.rodata.embedded,alloc,load,readonly,data,contents",
             os.path.basename(blob), obj],
            cwd=os.path.dirname(blob),
        )
        print("[embed] built %s from lofimap.bin" % obj)
    env.Append(LINKFLAGS=[obj])
