# ci-sysroot — link-time sysroot for the webOS build

`tv-sysroot.tar.gz` holds the runtime libraries of LG webOS 2.2 (glibc 2.19,
from an LG 49UF850V) that `webos-build/build.sh` links against. They live on
this orphan branch instead of the source history: they are LG firmware
binaries (glibc itself is LGPL-2.1) and there is no reason to bloat every
checkout with them. Re-create them with the scp commands from
`webos-build/INSTALL.md` ("TV sysroot") if this branch ever disappears.

CI fetches them with:

```sh
git fetch --depth=1 origin ci-sysroot
git show FETCH_HEAD:tv-sysroot.tar.gz | tar -C webos-build/tv-sysroot -xz
```
