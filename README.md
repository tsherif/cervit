cervit
======

A minimal, multi-threaded POSIX HTTP 1.1 server for local web development, written in C using only system libraries.

Primarily intended as a learning resource for anyone curious how to build an HTTP server without helper libraries.

Build:

```bash
  $ make
```

Run:

```bash
  $ ./cervit
```

Defaults to using port 5000. Optionally, provide a different port as the first argument:

```bash
  $ ./cervit 3000
```
