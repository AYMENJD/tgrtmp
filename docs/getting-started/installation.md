# Installation

tgrtmp ships prebuilt wheels:

```bash
pip install --upgrade tgrtmp
```

Python 3.10+ on Linux, Windows, and macOS.

## From source

You need a C++17 compiler, CMake, nanobind, and OpenSSL.

```bash
pip install .
```

## Verify

```python
>>> import tgrtmp
>>> tgrtmp.__version__
'X.Y.Z'
```
