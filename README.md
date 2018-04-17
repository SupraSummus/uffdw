uffdw
=====

"uffdw" stands for "userfaultfd wrapper". "userfaultfd" is linux kernel API for handling page faults in userspace. With use of it you can do cross-machine process migration or lazily map blobs into address space.

This project is in "playground" state.

API
---

See `include/uffdw.h`.

For example use take a look at tests (eg `test/page_repeat.c`).

See also
--------

* `man 2 userfaultfd`
