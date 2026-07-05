# libmicrovm - NVMM-based PVH microVM monitor library

LIB=		microvm
SHLIB_MAJOR=	0

SRCS=		vmm.c loader.c uart.c lapic.c ioapic.c mptable.c \
		virtio.c virtio_blk.c virtio_console.c virtio_net.c \
		virtio_vsock.c
INCS=		microvm.h
MAN=		libmicrovm.3

CSTD=		gnu99
WARNS?=		6

# The shared library leaves nvmm/pthread symbols to be resolved by the
# consumer (see usr.sbin/microvm), matching how libnvmm is consumed.

.include <bsd.lib.mk>
