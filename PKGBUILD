# Maintainer: Chris Sutcliff <chris@sutcliff.me>
pkgname=keychron-battery-dkms
pkgver=0.1.0
pkgrel=1
pkgdesc="HID driver for Keychron mouse battery reporting via power_supply subsystem"
arch=('x86_64')
url="https://github.com/csutcliff/keychron-battery-dkms"
license=('GPL-2.0-only')
depends=('dkms')
makedepends=()
optdepends=('linux-headers: build the module against the Arch stock kernel'
            'linux-lts-headers: build the module against the LTS kernel'
            'linux-cachyos-headers: build the module against the CachyOS kernel')
provides=('keychron-battery')
conflicts=('keychron-battery')
source=('keychron_battery.c'
        'Makefile'
        'dkms.conf')
sha256sums=('d1812e7d567c611bc339ae69a9830f91d8142e87c7faffd7b6bc9eb54bd0fa77'
            'bf1ea239f3983ce1aaa076c74ca88c50bbc4596e4e15c9082453c0c632852544'
            'b1fc4f96e7f17155e3b3e87b2d7d103d84d78ab4468a797ec96fb3d3f7ac7d5c')

package() {
    local install_dir="${pkgdir}/usr/src/${pkgname%-dkms}-${pkgver}"

    install -Dm644 keychron_battery.c "${install_dir}/keychron_battery.c"
    install -Dm644 Makefile "${install_dir}/Makefile"
    install -Dm644 dkms.conf "${install_dir}/dkms.conf"
}
