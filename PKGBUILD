# Maintainer: Siyia <eutychios23@gmail.com>
pkgname=iso-commander
pkgver=6.9.7
pkgrel=1
pkgdesc='The Fastest ISO Manager on the Planet, written in C++'
arch=('x86_64')
url="https://github.com/siyia2/iso-commander"
license=('GPL-3.0-only')        # SPDX format (modern standard)
depends=('coreutils' 'glibc' 'readline' 'util-linux' 'xz' 'zstd')
makedepends=('gcc' 'make')
source=("$pkgname-$pkgver.tar.gz::$url/archive/v$pkgver.tar.gz")
md5sums=('28d56118c91abf755a78ca6e444ee9a2')

build() {
    cd "$srcdir/$pkgname-$pkgver"
    make
}

package() {
    cd "$srcdir/$pkgname-$pkgver"
    install -Dm755 isocmd           "$pkgdir/usr/bin/isocmd"
    install -Dm644 man/isocmd.1     "$pkgdir/usr/share/man/man1/isocmd.1"
    install -Dm644 LICENSE          "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 README.md        "$pkgdir/usr/share/doc/$pkgname/README.md"
}
