#!/usr/bin/env python3
"""Convert host symlinks in a guest root dir into BoxedWine's "<name>.link" files.

BoxedWine's VFS does not follow host symlinks inside a -root directory; it
expects a regular file "<name>.link" whose content is the link target.
Usage: symlinks-to-boxedwine.py /path/to/root
"""
import os
import sys

root = sys.argv[1]
count = 0
for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
    for name in dirnames + filenames:
        path = os.path.join(dirpath, name)
        if os.path.islink(path):
            target = os.readlink(path)
            os.unlink(path)
            with open(path + ".link", "w") as f:
                f.write(target)
            count += 1
print("converted %d symlinks" % count)
