#!/usr/bin/env python3
# echo-ar — archive wrapper (CMAKE_AR / CMAKE_RANLIB) that mirrors static-archive
# creation into the echo-face/ shadow (docs/06 §7). Static archives don't go
# through the compiler, so without this the face objects (.echoface) never get
# archived, and a downstream face link would wrongly pull the NATIVE archive
# (whose objects reference host-runtime symbols the guest world lacks).
#
#   ar <op> libX.a a.o b.o   -> libX.a   AND   echo-face/libX.a from a.o.echoface ...
#   ranlib libX.a            -> ranlib both
#
# SPDX-License-Identifier: MIT
import os
import subprocess
import sys

REAL_AR = os.environ.get("ECHO_REAL_AR", "ar")
REAL_RANLIB = os.environ.get("ECHO_REAL_RANLIB", "ranlib")
SHADOW = "echo-face"
OBJ_SUFFIX = ".echoface"


def face_path(p):
    d = os.path.dirname(p) or "."
    fd = os.path.join(d, SHADOW)
    os.makedirs(fd, exist_ok=True)
    return os.path.join(fd, os.path.basename(p))


def main():
    args = sys.argv[1:]

    # ranlib mode: a single archive argument.
    if len(args) == 1 and args[0].endswith(".a"):
        subprocess.run([REAL_RANLIB, args[0]], check=True)
        fa = face_path(args[0])
        if os.path.exists(fa):
            subprocess.run([REAL_RANLIB, fa], check=True)
        return

    # ar mode: <op> <archive.a> <objects...>
    subprocess.run([REAL_AR] + args, check=True)
    if len(args) >= 2 and args[1].endswith(".a"):
        op, archive, objs = args[0], args[1], args[2:]
        fobjs = [o + OBJ_SUFFIX for o in objs
                 if o.endswith(".o") and os.path.exists(o + OBJ_SUFFIX)]
        if fobjs:
            fa = face_path(archive)
            if os.path.exists(fa):
                os.remove(fa)
            subprocess.run([REAL_AR, op, fa] + fobjs, check=True)


if __name__ == "__main__":
    main()
