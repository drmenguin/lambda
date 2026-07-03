# Maintainer: Luke
pkgname=lambda
pkgver=0.1.0
pkgrel=1
pkgdesc='Lambda calculus beta-reduction playground with ncurses and CLI front ends'
arch=('x86_64')
url='https://github.com/drmenguin/lambda'
license=('GPL-3.0-or-later')
depends=('glibc' 'ncurses')
source=(
  'Makefile'
  'lambda.c'
  'lambda.h'
  'main_plain.c'
  'main_ncurses.c'
  'lambda.1'
  'lambda-cli.1'
  'LICENSE'
)
sha256sums=(
  'e37a8480fee7bdfb31f10e86919862b3b1279a3419e349b8704cc49c55f83c19'
  '91d6b5478b4dde578ae2bd1ca78c0a95258394b524510448411056f0c9c919f8'
  '7eb465000283109726fab76fba89213ee8e4164a5b700dae2e20e42d2fbae749'
  'bbb853cb83c155c71551e6c4c59e4e9036e056671363bad1735c660b62d963a7'
  '438ff5cb2bb06a42d97373597f06e8372414bdfa708dbb904dab64901af5057f'
  '168df4dad3c6df33cc2444f09cd5e6386081aa7114c43a6c6d08e3724d4175fd'
  '88aac36a1fcb3fc192a470cc0ff4fd347c767f1817e15b4a35f3d1b26f0bb143'
  'fb981668c18a279e285fc4d83fba1e836cc84dd4daa73c9697d3cfd2d8aca6e0'
)

build() {
  make clean
  make
}

package() {
  make DESTDIR="$pkgdir" PREFIX=/usr install
}
