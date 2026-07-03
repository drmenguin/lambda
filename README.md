# Lambda
[![CI](https://github.com/drmenguin/lambda/actions/workflows/ci.yml/badge.svg)](https://github.com/drmenguin/lambda/actions/workflows/ci.yml)
[![License: GPL v3.0](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](LICENSE)
[![AUR](https://img.shields.io/aur/version/lambda?label=AUR)](https://aur.archlinux.org/packages/lambda)

`lambda` is a small lambda calculus beta-reduction playground for the terminal.
It includes an interactive ncurses interface and a plain command-line reducer.

![Demo of lambda reducing terms in the terminal](images/demo.gif)

## What It Does
Lambda expressions are parsed, printed with Unicode lambda notation, and reduced
using normal-order beta reduction.

Examples:

```sh
lambda-cli '(\x.x) y'
lambda --define 'I=\x.x' --eval 'I y'
lambda
```

The interactive interface lets you save definitions:

```text
I = \x.x
K = \x y.x
:defs
:free I
```

When a displayed term is alpha-equivalent to saved definitions, `lambda` shows
the matching names beside it:

```sh
lambda -d 'I=\x.x' -d 'J=\y.y' '\z.z'
```

```text
  λz.z    [I, J]
```

## Syntax
```text
\x.x        abstraction
x y z       application, left associative: (x y) z
xx          lowercase letters split into variables: x x
x1 x2       subscript-style variables, displayed as x₁ x₂
KI, Ki      uppercase-starting names stay as one identifier
```

Both backslash and the UTF-8 lambda character are accepted as lambdas.

## Installation
### Arch Linux
Use the AUR package:

```sh
yay -S lambda
```

Or build the package from this repository:

```sh
makepkg -si
```

### Ubuntu and Debian
On a fresh Ubuntu or Debian system, run:

```sh
sudo apt update
sudo apt install build-essential libncurses-dev groff git
git clone https://github.com/drmenguin/lambda.git
cd lambda
make
sudo make PREFIX=/usr install
lambda --version
```

### Windows
The easiest route is WSL with Ubuntu, then the Ubuntu instructions above:

```powershell
wsl --install Ubuntu
```

For a native Windows build, use an MSYS2 UCRT64 shell:

```sh
pacman -S --needed git make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-ncurses
make
```

### macOS
Install Apple's command-line tools, then build from source:

```sh
xcode-select --install
make
sudo make PREFIX=/usr/local install
```

If your compiler cannot find ncurses, install it with Homebrew and pass its
include and library paths to `make`.

### Source Builds
On systems with a C compiler, make, and ncurses:

```sh
make
sudo make PREFIX=/usr install
```

Run local checks:

```sh
make check
```

Clean build products:

```sh
make clean
```

## Commands
The ncurses program is installed as `lambda`.

```sh
lambda                  # start the interactive interface
lambda '(\x.x) y'       # reduce an expression directly
lambda -d 'I=\x.x' 'I y'
lambda --help
```

The plain command-line front end is installed as `lambda-cli`.

```sh
lambda-cli '(\x.x) y'
printf '%s\n' '(\x.x) y' | lambda-cli
lambda-cli --help
```

Manual pages are included:

```sh
man lambda
man lambda-cli
```

## Packaging
This repository includes Arch packaging files for an AUR package named
`lambda`.

```sh
makepkg --verifysource
makepkg -f --noarchive
```

## Development
CI runs on GitHub Actions for pushes and pull requests. It builds both
front ends, runs smoke tests, renders the man pages, and checks the staged
install image.

Suggestions and patches are welcome through the GitHub repository:

https://github.com/drmenguin/lambda

## Author
Copyright (C) 2026 Luke Collins.

Website: https://lc.mt

## License
This project is licensed under the GNU General Public License version 3 or
later. See [LICENSE](LICENSE).
