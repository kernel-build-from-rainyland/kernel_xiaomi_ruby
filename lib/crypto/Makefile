# SPDX-License-Identifier: GPL-2.0

# chacha is used by the /dev/random driver which is always builtin
obj-y						+= chacha.o
obj-$(CONFIG_CRYPTO_LIB_CHACHA_GENERIC)		+= libchacha.o

obj-$(CONFIG_CRYPTO_LIB_POLY1305_GENERIC)	+= libpoly1305.o
libpoly1305-y					:= poly1305-donna32.o
libpoly1305-$(CONFIG_ARCH_SUPPORTS_INT128)	:= poly1305-donna64.o
libpoly1305-y					+= poly1305.o

# blake2s is used by the /dev/random driver which is always builtin
obj-y						+= libblake2s.o
libblake2s-y					:= blake2s.o
libblake2s-$(CONFIG_CRYPTO_LIB_BLAKE2S_GENERIC)	+= blake2s-generic.o

# Only include chacha20poly1305 if not using Zinc
ifneq ($(CONFIG_WIREGUARD),y)
obj-$(CONFIG_CRYPTO_LIB_CHACHA20POLY1305)	+= libchacha20poly1305.o
libchacha20poly1305-y				+= chacha20poly1305.o
endif

# Only include curve25519 libs if not using Zinc
ifneq ($(CONFIG_WIREGUARD),y)
obj-$(CONFIG_CRYPTO_LIB_CURVE25519_GENERIC)	+= libcurve25519-generic.o
libcurve25519-generic-y				:= curve25519-fiat32.o
libcurve25519-generic-$(CONFIG_ARCH_SUPPORTS_INT128)	:= curve25519-hacl64.o
libcurve25519-generic-y				+= curve25519-generic.o

obj-$(CONFIG_CRYPTO_LIB_CURVE25519)		+= libcurve25519.o
libcurve25519-y					+= curve25519.o
endif

ifneq ($(CONFIG_CRYPTO_MANAGER_DISABLE_TESTS),y)
libblake2s-y					+= blake2s-selftest.o
ifneq ($(CONFIG_WIREGUARD),y)
libchacha20poly1305-y				+= chacha20poly1305-selftest.o
libcurve25519-y					+= curve25519-selftest.o
endif
endif
