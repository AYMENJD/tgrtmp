# Installation

tgrtmp builds from source on install, so the compiler and OpenSSL of your
machine are all it needs. Python 3.10+ on Linux, Windows, and macOS.

```bash
pip install --upgrade tgrtmp
```

## Prerequisites

A C++17 compiler, CMake, and the OpenSSL development
headers. The runtime library alone is not enough; the headers are
a separate package on every system:

| System | Install |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential libssl-dev` |
| Fedora / RHEL | `sudo dnf install gcc-c++ openssl-devel` |
| Alpine | `apk add build-base openssl-dev` |
| macOS | `xcode-select --install` and `brew install openssl@3` |
| Windows | Visual Studio Build Tools and OpenSSL, see below |

If a header is missing, the build stops with a message naming the exact
package for your system.

## Windows

Install Visual Studio Build Tools with the C++ workload, then hand the build
an OpenSSL through vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg --depth 1
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install openssl:x64-windows
```

Then install with that OpenSSL on the path:

```powershell
$env:CMAKE_PREFIX_PATH = "C:\vcpkg\installed\x64-windows"
pip install --upgrade tgrtmp
```

## Verify

```python
>>> import tgrtmp
>>> tgrtmp.__version__
'X.Y.Z'
```
