/*-
 * Copyright (c) 2013 Tsubai Masanari
 * Copyright (c) 2013 Bryan Venteicher <bryanv@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $OpenBSD: src/sys/dev/pci/if_vmx.c,v 1.11 2013/06/22 00:28:10 uebayasi Exp $
 * $FreeBSD: head/sys/dev/vmware/vmxnet3/if_vmx.c 318867 2017-05-25 10:49:56Z avg $
 */

/* Driver for VMware vmxnet3 virtual ethernet devices. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/endian.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/ifq_var.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/vlan/if_vlan_ether.h>
#include <net/vlan/if_vlan_var.h>

#include <net/bpf.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

#include <sys/in_cksum.h>

#include <sys/bus.h>
#include <sys/rman.h>

#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>

#define	VMXNET3_LEGACY_TX 1	/* XXX we need this at the moment */
#include "if_vmxreg.h"
#include "if_vmxvar.h"

#include "opt_inet.h"
#include "opt_inet6.h"

#ifdef VMXNET3_FAILPOINTS
#include <sys/fail.h>
static SYSCTL_NODE(DEBUG_FP, OID_AUTO, vmxnet3, CTLFLAG_RW, 0,
    "vmxnet3 fail points");
#define VMXNET3_FP	_debug_fail_point_vmxnet3
#endif

static int	vmxnet3_probe(device_t);
static int	vmxnet3_attach(device_t);
static int	vmxnet3_detach(device_t);
static int	vmxnet3_shutdown(device_t);

static int	vmxnet3_alloc_resources(struct vmxnet3_softc *);
static void	vmxnet3_free_resources(struct vmxnet3_softc *);
static int	vmxnet3_check_version(struct vmxnet3_softc *);
static void	vmxnet3_initial_config(struct vmxnet3_softc *);
static void	vmxnet3_check_multiqueue(struct vmxnet3_softc *);

#ifdef __FreeBSD__
static int	vmxnet3_alloc_msix_interrupts(struct vmxnet3_softc *);
static int	vmxnet3_alloc_msi_interrupts(struct vmxnet3_softc *);
#else
static int	vmxnet3_alloc_msi_interrupts(struct vmxnet3_softc *);
#endif
static int	vmxnet3_alloc_legacy_interrupts(struct vmxnet3_softc *);
static int	vmxnet3_alloc_interrupt(struct vmxnet3_softc *, int, int,
		    struct vmxnet3_interrupt *);
static int	vmxnet3_alloc_intr_resources(struct vmxnet3_softc *);
#ifdef __FreeBSD__
static int	vmxnet3_setup_msix_interrupts(struct vmxnet3_softc *);
#endif
static int	vmxnet3_setup_legacy_interrupt(struct vmxnet3_softc *);
static int	vmxnet3_setup_interrupts(struct vmxnet3_softc *);
static int	vmxnet3_alloc_interrupts(struct vmxnet3_softc *);

static void	vmxnet3_free_interrupt(struct vmxnet3_softc *,
		    struct vmxnet3_interrupt *);
static void	vmxnet3_free_interrupts(struct vmxnet3_softc *);

#ifndef VMXNET3_LEGACY_TX
static int	vmxnet3_alloc_taskqueue(struct vmxnet3_softc *);
static void	vmxnet3_start_taskqueue(struct vmxnet3_softc *);
static void	vmxnet3_drain_taskqueue(struct vmxnet3_softc *);
static void	vmxnet3_free_taskqueue(struct vmxnet3_softc *);
#endif

static int	vmxnet3_init_rxq(struct vmxnet3_softc *, int);
static int	vmxnet3_init_txq(struct vmxnet3_softc *, int);
static int	vmxnet3_alloc_rxtx_queues(struct vmxnet3_softc *);
static void	vmxnet3_destroy_rxq(struct vmxnet3_rxqueue *);
static void	vmxnet3_destroy_txq(struct vmxnet3_txqueue *);
static void	vmxnet3_free_rxtx_queues(struct vmxnet3_softc *);

static int	vmxnet3_alloc_shared_data(struct vmxnet3_softc *);
static void	vmxnet3_free_shared_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_txq_data(struct vmxnet3_softc *);
static void	vmxnet3_free_txq_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_rxq_data(struct vmxnet3_softc *);
static void	vmxnet3_free_rxq_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_queue_data(struct vmxnet3_softc *);
static void	vmxnet3_free_queue_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_mcast_table(struct vmxnet3_softc *);
static void	vmxnet3_init_shared_data(struct vmxnet3_softc *);
static void	vmxnet3_init_hwassist(struct vmxnet3_softc *);
static void	vmxnet3_reinit_interface(struct vmxnet3_softc *);
static void	vmxnet3_reinit_rss_shared_data(struct vmxnet3_softc *);
static void	vmxnet3_reinit_shared_data(struct vmxnet3_softc *);
static int	vmxnet3_alloc_data(struct vmxnet3_softc *);
static void	vmxnet3_free_data(struct vmxnet3_softc *);
static int	vmxnet3_setup_interface(struct vmxnet3_softc *);

static void	vmxnet3_evintr(struct vmxnet3_softc *);
static void	vmxnet3_txq_eof(struct vmxnet3_txqueue *);
static void	vmxnet3_rx_csum(struct vmxnet3_rxcompdesc *, struct mbuf *);
static int	vmxnet3_newbuf(struct vmxnet3_softc *, struct vmxnet3_rxring *);
static void	vmxnet3_rxq_eof_discard(struct vmxnet3_rxqueue *,
		    struct vmxnet3_rxring *, int);
static void	vmxnet3_rxq_eof(struct vmxnet3_rxqueue *);
static void	vmxnet3_legacy_intr(void *);
#ifdef __FreeBSD__
static void	vmxnet3_txq_intr(void *);
static void	vmxnet3_rxq_intr(void *);
static void	vmxnet3_event_intr(void *);
#endif

static void	vmxnet3_txstop(struct vmxnet3_softc *, struct vmxnet3_txqueue *);
static void	vmxnet3_rxstop(struct vmxnet3_softc *, struct vmxnet3_rxqueue *);
static void	vmxnet3_stop(struct vmxnet3_softc *);

static void	vmxnet3_txinit(struct vmxnet3_softc *, struct vmxnet3_txqueue *);
static int	vmxnet3_rxinit(struct vmxnet3_softc *, struct vmxnet3_rxqueue *);
static int	vmxnet3_reinit_queues(struct vmxnet3_softc *);
static int	vmxnet3_enable_device(struct vmxnet3_softc *);
static void	vmxnet3_reinit_rxfilters(struct vmxnet3_softc *);
static int	vmxnet3_reinit(struct vmxnet3_softc *);
static void	vmxnet3_init_locked(struct vmxnet3_softc *);
static void	vmxnet3_init(void *);

static int	vmxnet3_txq_offload_ctx(struct vmxnet3_txqueue *,struct mbuf *,
		    int *, int *, int *);
static int	vmxnet3_txq_load_mbuf(struct vmxnet3_txqueue *, struct mbuf **,
		    bus_dmamap_t, bus_dma_segment_t [], int *);
static void	vmxnet3_txq_unload_mbuf(struct vmxnet3_txqueue *, bus_dmamap_t);
static int	vmxnet3_txq_encap(struct vmxnet3_txqueue *, struct mbuf **);
#ifdef VMXNET3_LEGACY_TX
static void	vmxnet3_start_locked(struct ifnet *);
static void	vmxnet3_start(struct ifnet *, struct ifaltq_subque *);
#else
static int	vmxnet3_txq_mq_start_locked(struct vmxnet3_txqueue *,
		    struct mbuf *);
static int	vmxnet3_txq_mq_start(struct ifnet *, struct mbuf *);
static void	vmxnet3_txq_tq_deferred(void *, int);
#endif
static void	vmxnet3_txq_start(struct vmxnet3_txqueue *);
static void	vmxnet3_tx_start_all(struct vmxnet3_softc *);

static void	vmxnet3_update_vlan_filter(struct vmxnet3_softc *, int,
		    uint16_t);
static void	vmxnet3_register_vlan(void *, struct ifnet *, uint16_t);
static void	vmxnet3_unregister_vlan(void *, struct ifnet *, uint16_t);
static void	vmxnet3_set_rxfilter(struct vmxnet3_softc *);
static int	vmxnet3_change_mtu(struct vmxnet3_softc *, int);
static int	vmxnet3_ioctl(struct ifnet *, u_long, caddr_t, struct ucred *);

#ifndef VMXNET3_LEGACY_TX
static void	vmxnet3_qflush(struct ifnet *);
#endif

static int	vmxnet3_watchdog(struct vmxnet3_txqueue *);
static void	vmxnet3_refresh_host_stats(struct vmxnet3_softc *);
static void	vmxnet3_txq_accum_stats(struct vmxnet3_txqueue *,
		    struct vmxnet3_txq_stats *);
static void	vmxnet3_rxq_accum_stats(struct vmxnet3_rxqueue *,
		    struct vmxnet3_rxq_stats *);
static void	vmxnet3_tick(void *);
static void	vmxnet3_link_status(struct vmxnet3_softc *);
static void	vmxnet3_media_status(struct ifnet *, struct ifmediareq *);
static int	vmxnet3_media_change(struct ifnet *);
static void	vmxnet3_set_lladdr(struct vmxnet3_softc *);
static void	vmxnet3_get_lladdr(struct vmxnet3_softc *);

static void	vmxnet3_setup_txq_sysctl(struct vmxnet3_txqueue *,
		    struct sysctl_ctx_list *, struct sysctl_oid_list *);
static void	vmxnet3_setup_rxq_sysctl(struct vmxnet3_rxqueue *,
		    struct sysctl_ctx_list *, struct sysctl_oid_list *);
static void	vmxnet3_setup_queue_sysctl(struct vmxnet3_softc *,
		    struct sysctl_ctx_list *, struct sysctl_oid_list *);
static void	vmxnet3_setup_sysctl(struct vmxnet3_softc *);

static void	vmxnet3_write_bar0(struct vmxnet3_softc *, bus_size_t,
		    uint32_t);
static uint32_t	vmxnet3_read_bar1(struct vmxnet3_softc *, bus_size_t);
static void	vmxnet3_write_bar1(struct vmxnet3_softc *, bus_size_t,
		    uint32_t);
static void	vmxnet3_write_cmd(struct vmxnet3_softc *, uint32_t);
static uint32_t	vmxnet3_read_cmd(struct vmxnet3_softc *, uint32_t);

static void	vmxnet3_enable_intr(struct vmxnet3_softc *, int);
static void	vmxnet3_disable_intr(struct vmxnet3_softc *, int);
static void	vmxnet3_enable_all_intrs(struct vmxnet3_softc *);
static void	vmxnet3_disable_all_intrs(struct vmxnet3_softc *);

static int	vmxnet3_dma_malloc(struct vmxnet3_softc *, bus_size_t,
		    bus_size_t, struct vmxnet3_dma_alloc *);
static void	vmxnet3_dma_free(struct vmxnet3_softc *,
		    struct vmxnet3_dma_alloc *);
static int	vmxnet3_tunable_int(struct vmxnet3_softc *,
		    const char *, int);

typedef enum {
	VMXNET3_BARRIER_RD,
	VMXNET3_BARRIER_WR,
	VMXNET3_BARRIER_RDWR,
} vmxnet3_barrier_t;

static void	vmxnet3_barrier(struct vmxnet3_softc *, vmxnet3_barrier_t);

/* Tunables. */
static int vmxnet3_mq_disable = 0;
TUNABLE_INT("hw.vmx.mq_disable", &vmxnet3_mq_disable);
static int vmxnet3_default_txnqueue = VMXNET3_DEF_TX_QUEUES;
TUNABLE_INT("hw.vmx.txnqueue", &vmxnet3_default_txnqueue);
static int vmxnet3_default_rxnqueue = VMXNET3_DEF_RX_QUEUES;
TUNABLE_INT("hw.vmx.rxnqueue", &vmxnet3_default_rxnqueue);
static int vmxnet3_default_txndesc = VMXNET3_DEF_TX_NDESC;
TUNABLE_INT("hw.vmx.txndesc", &vmxnet3_default_txndesc);
static int vmxnet3_default_rxndesc = VMXNET3_DEF_RX_NDESC;
TUNABLE_INT("hw.vmx.rxndesc", &vmxnet3_default_rxndesc);

static device_method_t vmxnet3_methods[] = {
	/* Device interface. */
	DEVMETHOD(device_probe,		vmxnet3_probe),
	DEVMETHOD(device_attach,	vmxnet3_attach),
	DEVMETHOD(device_detach,	vmxnet3_detach),
	DEVMETHOD(device_shutdown,	vmxnet3_shutdown),

	DEVMETHOD_END
};

static driver_t vmxnet3_driver = {
	"vmx", vmxnet3_methods, sizeof(struct vmxnet3_softc)
};

static devclass_t vmxnet3_devclass;
DRIVER_MODULE(vmx, pci, vmxnet3_driver, vmxnet3_devclass, NULL, NULL);

MODULE_DEPEND(vmx, pci, 1, 1, 1);
MODULE_DEPEND(vmx, ether, 1, 1, 1);

#define VMXNET3_VMWARE_VENDOR_ID	0x15AD
#define VMXNET3_VMWARE_DEVICE_ID	0x07B0

static int
vmxnet3_probe(device_t dev)
{

	if (pci_get_vendor(dev) == VMXNET3_VMWARE_VENDOR_ID &&
	    pci_get_device(dev) == VMXNET3_VMWARE_DEVICE_ID) {
		device_set_desc(dev, "VMware VMXNET3 Ethernet Adapter");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
vmxnet3_attach(device_t dev)
{
	struct vmxnet3_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->vmx_dev = dev;

	pci_enable_busmaster(dev);

	VMXNET3_CORE_LOCK_INIT(sc, device_get_nameunit(dev));
	callout_init_lk(&sc->vmx_tick, &sc->vmx_lock);

	vmxnet3_initial_config(sc);

	error = vmxnet3_alloc_resources(sc);
	if (error)
		goto fail;

	error = vmxnet3_check_version(sc);
	if (error)
		goto fail;

	error = vmxnet3_alloc_rxtx_queues(sc);
	if (error)
		goto fail;

#ifndef VMXNET3_LEGACY_TX
	error = vmxnet3_alloc_taskqueue(sc);
	if (error)
		goto fail;
#endif

	error = vmxnet3_alloc_interrupts(sc);
	if (error)
		goto fail;

	vmxnet3_check_multiqueue(sc);

	error = vmxnet3_alloc_data(sc);
	if (error)
		goto fail;

	error = vmxnet3_setup_interface(sc);
	if (error)
		goto fail;

	error = vmxnet3_setup_interrupts(sc);
	if (error) {
		ether_ifdetach(sc->vmx_ifp);
		device_printf(dev, "could not set up interrupt\n");
		goto fail;
	}

	vmxnet3_setup_sysctl(sc);
#ifndef VMXNET3_LEGACY_TX
	vmxnet3_start_taskqueue(sc);
#endif

fail:
	if (error)
		vmxnet3_detach(dev);

	return (error);
}

static int
vmxnet3_detach(device_t dev)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;

	sc = device_get_softc(dev);
	ifp = sc->vmx_ifp;

	if (device_is_attached(dev)) {
		VMXNET3_CORE_LOCK(sc);
		vmxnet3_stop(sc);
		VMXNET3_CORE_UNLOCK(sc);

		callout_terminate(&sc->vmx_tick);
#ifndef VMXNET3_LEGACY_TX
		vmxnet3_drain_taskqueue(sc);
#endif

		ether_ifdetach(ifp);
	}

	if (sc->vmx_vlan_attach != NULL) {
		EVENTHANDLER_DEREGISTER(vlan_config, sc->vmx_vlan_attach);
		sc->vmx_vlan_attach = NULL;
	}
	if (sc->vmx_vlan_detach != NULL) {
		EVENTHANDLER_DEREGISTER(vlan_config, sc->vmx_vlan_detach);
		sc->vmx_vlan_detach = NULL;
	}

#ifndef VMXNET3_LEGACY_TX
	vmxnet3_free_taskqueue(sc);
#endif
	vmxnet3_free_interrupts(sc);

	if (ifp != NULL) {
		if_free(ifp);
		sc->vmx_ifp = NULL;
	}

	ifmedia_removeall(&sc->vmx_media);

	vmxnet3_free_data(sc);
	vmxnet3_free_resources(sc);
	vmxnet3_free_rxtx_queues(sc);

	VMXNET3_CORE_LOCK_DESTROY(sc);

	return (0);
}

static int
vmxnet3_shutdown(device_t dev)
{

	return (0);
}

static int
vmxnet3_alloc_resources(struct vmxnet3_softc *sc)
{
	device_t dev;
	int rid;

	dev = sc->vmx_dev;

	rid = PCIR_BAR(0);
	sc->vmx_res0 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->vmx_res0 == NULL) {
		device_printf(dev,
		    "could not map BAR0 memory\n");
		return (ENXIO);
	}

	sc->vmx_iot0 = rman_get_bustag(sc->vmx_res0);
	sc->vmx_ioh0 = rman_get_bushandle(sc->vmx_res0);

	rid = PCIR_BAR(1);
	sc->vmx_res1 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->vmx_res1 == NULL) {
		device_printf(dev,
		    "could not map BAR1 memory\n");
		return (ENXIO);
	}

	sc->vmx_iot1 = rman_get_bustag(sc->vmx_res1);
	sc->vmx_ioh1 = rman_get_bushandle(sc->vmx_res1);

	if (pci_find_extcap(dev, PCIY_MSIX, NULL) == 0) {
		rid = PCIR_BAR(2);
		sc->vmx_msix_res = bus_alloc_resource_any(dev,
		    SYS_RES_MEMORY, &rid, RF_ACTIVE);
	}

	if (sc->vmx_msix_res == NULL)
		sc->vmx_flags |= VMXNET3_FLAG_NO_MSIX;

	return (0);
}

static void
vmxnet3_free_resources(struct vmxnet3_softc *sc)
{
	device_t dev;
	int rid;

	dev = sc->vmx_dev;

	if (sc->vmx_res0 != NULL) {
		rid = PCIR_BAR(0);
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->vmx_res0);
		sc->vmx_res0 = NULL;
	}

	if (sc->vmx_res1 != NULL) {
		rid = PCIR_BAR(1);
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->vmx_res1);
		sc->vmx_res1 = NULL;
	}

	if (sc->vmx_msix_res != NULL) {
		rid = PCIR_BAR(2);
		bus_release_resource(dev, SYS_RES_MEMORY, rid,
		    sc->vmx_msix_res);
		sc->vmx_msix_res = NULL;
	}
}

static int
vmxnet3_check_version(struct vmxnet3_softc *sc)
{
	device_t dev;
	uint32_t version;

	dev = sc->vmx_dev;

	version = vmxnet3_read_bar1(sc, VMXNET3_BAR1_VRRS);
	if ((version & 0x01) == 0) {
		device_printf(dev, "unsupported hardware version %#x\n",
		    version);
		return (ENOTSUP);
	}
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_VRRS, 1);

	version = vmxnet3_read_bar1(sc, VMXNET3_BAR1_UVRS);
	if ((version & 0x01) == 0) {
		device_printf(dev, "unsupported UPT version %#x\n", version);
		return (ENOTSUP);
	}
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_UVRS, 1);

	return (0);
}

static int
trunc_powerof2(int val)
{

	return (1U << (fls(val) - 1));
}

static void
vmxnet3_initial_config(struct vmxnet3_softc *sc)
{
	int nqueue, ndesc;

	nqueue = vmxnet3_tunable_int(sc, "txnqueue", vmxnet3_default_txnqueue);
	if (nqueue > VMXNET3_MAX_TX_QUEUES || nqueue < 1)
		nqueue = VMXNET3_DEF_TX_QUEUES;
	if (nqueue > ncpus)
		nqueue = ncpus;
	sc->vmx_max_ntxqueues = trunc_powerof2(nqueue);

	nqueue = vmxnet3_tunable_int(sc, "rxnqueue", vmxnet3_default_rxnqueue);
	if (nqueue > VMXNET3_MAX_RX_QUEUES || nqueue < 1)
		nqueue = VMXNET3_DEF_RX_QUEUES;
	if (nqueue > ncpus)
		nqueue = ncpus;
	sc->vmx_max_nrxqueues = trunc_powerof2(nqueue);

	if (vmxnet3_tunable_int(sc, "mq_disable", vmxnet3_mq_disable)) {
		sc->vmx_max_nrxqueues = 1;
		sc->vmx_max_ntxqueues = 1;
	}

	ndesc = vmxnet3_tunable_int(sc, "txd", vmxnet3_default_txndesc);
	if (ndesc > VMXNET3_MAX_TX_NDESC || ndesc < VMXNET3_MIN_TX_NDESC)
		ndesc = VMXNET3_DEF_TX_NDESC;
	if (ndesc & VMXNET3_MASK_TX_NDESC)
		ndesc &= ~VMXNET3_MASK_TX_NDESC;
	sc->vmx_ntxdescs = ndesc;

	ndesc = vmxnet3_tunable_int(sc, "rxd", vmxnet3_default_rxndesc);
	if (ndesc > VMXNET3_MAX_RX_NDESC || ndesc < VMXNET3_MIN_RX_NDESC)
		ndesc = VMXNET3_DEF_RX_NDESC;
	if (ndesc & VMXNET3_MASK_RX_NDESC)
		ndesc &= ~VMXNET3_MASK_RX_NDESC;
	sc->vmx_nrxdescs = ndesc;
	sc->vmx_max_rxsegs = VMXNET3_MAX_RX_SEGS;
}

static void
vmxnet3_check_multiqueue(struct vmxnet3_softc *sc)
{

	if (sc->vmx_intr_type != VMXNET3_IT_MSIX)
		goto out;

	/* BMV: Just use the maximum configured for now. */
	sc->vmx_nrxqueues = sc->vmx_max_nrxqueues;
	sc->vmx_ntxqueues = sc->vmx_max_ntxqueues;

	if (sc->vmx_nrxqueues > 1)
		sc->vmx_flags |= VMXNET3_FLAG_RSS;

	return;

out:
	sc->vmx_ntxqueues = 1;
	sc->vmx_nrxqueues = 1;
}

#ifdef __FreeBSD__
static int
vmxnet3_alloc_msix_interrupts(struct vmxnet3_softc *sc)
{
	device_t dev;
	int nmsix, cnt, required;

	dev = sc->vmx_dev;

	if (sc->vmx_flags & VMXNET3_FLAG_NO_MSIX)
		return (1);

	/* Allocate an additional vector for the events interrupt. */
	required = sc->vmx_max_nrxqueues + sc->vmx_max_ntxqueues + 1;

	nmsix = pci_msix_count(dev);
	if (nmsix < required)
		return (1);

	cnt = required;
	if (pci_alloc_msix(dev, &cnt) == 0 && cnt >= required) {
		sc->vmx_nintrs = required;
		return (0);
	} else
		pci_release_msi(dev);

	/* BMV TODO Fallback to sharing MSIX vectors if possible. */

	return (1);
}

static int
vmxnet3_alloc_msi_interrupts(struct vmxnet3_softc *sc)
{
	device_t dev;
	int nmsi, cnt, required;

	dev = sc->vmx_dev;
	required = 1;

	nmsi = pci_msi_count(dev);
	if (nmsi < required)
		return (1);

	cnt = required;
	if (pci_alloc_msi(dev, &cnt) == 0 && cnt >= required) {
		sc->vmx_nintrs = 1;
		return (0);
	} else
		pci_release_msi(dev);

	return (1);
}
#else
static int
vmxnet3_alloc_msi_interrupts(struct vmxnet3_softc *sc)
{
	int irq_flags, rid;
	int enable = 1;

	sc->vmx_irq_type = pci_alloc_1intr(sc->vmx_dev, enable, &rid,
	    &irq_flags);
	sc->vmx_irq_flags = irq_flags;
	sc->vmx_nintrs = 1;
	return (0);
}
#endif

static int
vmxnet3_alloc_legacy_interrupts(struct vmxnet3_softc *sc)
{

	sc->vmx_nintrs = 1;
	return (0);
}

static int
vmxnet3_alloc_interrupt(struct vmxnet3_softc *sc, int rid, int flags,
    struct vmxnet3_interrupt *intr)
{
	struct resource *irq;

	irq = bus_alloc_resource_any(sc->vmx_dev, SYS_RES_IRQ, &rid,
	    sc->vmx_irq_flags);
	if (irq == NULL)
		return (ENXIO);

	intr->vmxi_irq = irq;
	intr->vmxi_rid = rid;

	return (0);
}

static int
vmxnet3_alloc_intr_resources(struct vmxnet3_softc *sc)
{
	int i, rid, flags, error;

	rid = 0;
	flags = RF_ACTIVE;

	if (sc->vmx_intr_type == VMXNET3_IT_LEGACY)
		flags |= RF_SHAREABLE;
	else
		rid = 1;

	for (i = 0; i < sc->vmx_nintrs; i++, rid++) {
		error = vmxnet3_alloc_interrupt(sc, rid, flags,
		    &sc->vmx_intrs[i]);
		if (error)
			return (error);
	}

	return (0);
}

#ifdef __FreeBSD__
static int
vmxnet3_setup_msix_interrupts(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_interrupt *intr;
	int i, error;

	dev = sc->vmx_dev;
	intr = &sc->vmx_intrs[0];

	for (i = 0; i < sc->vmx_ntxqueues; i++, intr++) {
		txq = &sc->vmx_txq[i];
		error = bus_setup_intr(dev, intr->vmxi_irq, INTR_MPSAFE,
		     vmxnet3_txq_intr, txq, &intr->vmxi_handler, NULL);
		if (error)
			return (error);
		bus_describe_intr(dev, intr->vmxi_irq, intr->vmxi_handler,
		    "tq%d", i);
		txq->vxtxq_intr_idx = intr->vmxi_rid - 1;
	}

	for (i = 0; i < sc->vmx_nrxqueues; i++, intr++) {
		rxq = &sc->vmx_rxq[i];
		error = bus_setup_intr(dev, intr->vmxi_irq, INTR_MPSAFE,
		    vmxnet3_rxq_intr, rxq, &intr->vmxi_handler, NULL);
		if (error)
			return (error);
		bus_describe_intr(dev, intr->vmxi_irq, intr->vmxi_handler,
		    "rq%d", i);
		rxq->vxrxq_intr_idx = intr->vmxi_rid - 1;
	}

	error = bus_setup_intr(dev, intr->vmxi_irq, INTR_MPSAFE,
	    vmxnet3_event_intr, sc, &intr->vmxi_handler, NULL);
	if (error)
		return (error);
	bus_describe_intr(dev, intr->vmxi_irq, intr->vmxi_handler, "event");
	sc->vmx_event_intr_idx = intr->vmxi_rid - 1;

	return (0);
}
#endif

static int
vmxnet3_setup_legacy_interrupt(struct vmxnet3_softc *sc)
{
	struct vmxnet3_interrupt *intr;
	int i, error;

	intr = &sc->vmx_intrs[0];
	error = bus_setup_intr(sc->vmx_dev, intr->vmxi_irq,
	    INTR_MPSAFE, vmxnet3_legacy_intr, sc,
	    &intr->vmxi_handler, NULL);

	for (i = 0; i < sc->vmx_ntxqueues; i++)
		sc->vmx_txq[i].vxtxq_intr_idx = 0;
	for (i = 0; i < sc->vmx_nrxqueues; i++)
		sc->vmx_rxq[i].vxrxq_intr_idx = 0;
	sc->vmx_event_intr_idx = 0;

	return (error);
}

static void
vmxnet3_set_interrupt_idx(struct vmxnet3_softc *sc)
{
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txq_shared *txs;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxq_shared *rxs;
	int i;

	sc->vmx_ds->evintr = sc->vmx_event_intr_idx;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_txq[i];
		txs = txq->vxtxq_ts;
		txs->intr_idx = txq->vxtxq_intr_idx;
	}

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		rxq = &sc->vmx_rxq[i];
		rxs = rxq->vxrxq_rs;
		rxs->intr_idx = rxq->vxrxq_intr_idx;
	}
}

static int
vmxnet3_setup_interrupts(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_alloc_intr_resources(sc);
	if (error)
		return (error);

	switch (sc->vmx_intr_type) {
	case VMXNET3_IT_MSIX:
#ifdef __FreeBSD__
		error = vmxnet3_setup_msix_interrupts(sc);
#else
		device_printf(sc->vmx_dev, "VMXNET3_IT_MSIX unsupported\n");
		error = ENXIO;
#endif
		break;
	case VMXNET3_IT_MSI:
	case VMXNET3_IT_LEGACY:
		error = vmxnet3_setup_legacy_interrupt(sc);
		break;
	default:
		panic("%s: invalid interrupt type %d", __func__,
		    sc->vmx_intr_type);
	}

	if (error == 0)
		vmxnet3_set_interrupt_idx(sc);

	return (error);
}

#ifdef __FreeBSD__
static int
vmxnet3_alloc_interrupts(struct vmxnet3_softc *sc)
{
	device_t dev;
	uint32_t config;
	int error;

	dev = sc->vmx_dev;
	config = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_INTRCFG);

	sc->vmx_intr_type = config & 0x03;
	sc->vmx_intr_mask_mode = (config >> 2) & 0x03;

	switch (sc->vmx_intr_type) {
	case VMXNET3_IT_AUTO:
		sc->vmx_intr_type = VMXNET3_IT_MSIX;
		/* FALLTHROUGH */
	case VMXNET3_IT_MSIX:
		error = vmxnet3_alloc_msix_interrupts(sc);
		if (error == 0)
			break;
		sc->vmx_intr_type = VMXNET3_IT_MSI;
		/* FALLTHROUGH */
	case VMXNET3_IT_MSI:
		error = vmxnet3_alloc_msi_interrupts(sc);
		if (error == 0)
			break;
		sc->vmx_intr_type = VMXNET3_IT_LEGACY;
		/* FALLTHROUGH */
	case VMXNET3_IT_LEGACY:
		error = vmxnet3_alloc_legacy_interrupts(sc);
		if (error == 0)
			break;
		/* FALLTHROUGH */
	default:
		sc->vmx_intr_type = -1;
		device_printf(dev, "cannot allocate any interrupt resources\n");
		return (ENXIO);
	}

	return (error);
}
#else
static int
vmxnet3_alloc_interrupts(struct vmxnet3_softc *sc)
{
	device_t dev;
	uint32_t config;
	int error;

	dev = sc->vmx_dev;
	config = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_INTRCFG);

	sc->vmx_intr_type = config & 0x03;
	sc->vmx_intr_mask_mode = (config >> 2) & 0x03;

	switch (sc->vmx_intr_type) {
	case VMXNET3_IT_AUTO:
		sc->vmx_intr_type = VMXNET3_IT_MSI;
		/* FALLTHROUGH */
	case VMXNET3_IT_MSI:
		error = vmxnet3_alloc_msi_interrupts(sc);
		if (error == 0)
			break;
		sc->vmx_intr_type = VMXNET3_IT_LEGACY;
	case VMXNET3_IT_LEGACY:
		error = vmxnet3_alloc_legacy_interrupts(sc);
		if (error == 0)
			break;
		/* FALLTHROUGH */
	case VMXNET3_IT_MSIX:
		/* FALLTHROUGH */
	default:
		sc->vmx_intr_type = -1;
		device_printf(dev, "cannot allocate any interrupt resources\n");
		return (ENXIO);
	}

	return (error);
}
#endif

static void
vmxnet3_free_interrupt(struct vmxnet3_softc *sc,
    struct vmxnet3_interrupt *intr)
{
	device_t dev;

	dev = sc->vmx_dev;

	if (intr->vmxi_handler != NULL) {
		bus_teardown_intr(dev, intr->vmxi_irq, intr->vmxi_handler);
		intr->vmxi_handler = NULL;
	}

	if (intr->vmxi_irq != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, intr->vmxi_rid,
		    intr->vmxi_irq);
		intr->vmxi_irq = NULL;
		intr->vmxi_rid = -1;
	}
}

#ifdef __FreeBSD__
static void
vmxnet3_free_interrupts(struct vmxnet3_softc *sc)
{
	int i;

	for (i = 0; i < sc->vmx_nintrs; i++)
		vmxnet3_free_interrupt(sc, &sc->vmx_intrs[i]);

	if (sc->vmx_intr_type == VMXNET3_IT_MSI ||
	    sc->vmx_intr_type == VMXNET3_IT_MSIX)
		pci_release_msi(sc->vmx_dev);
}
#else
static void
vmxnet3_free_interrupts(struct vmxnet3_softc *sc)
{
	int i;

	for (i = 0; i < sc->vmx_nintrs; i++)
		vmxnet3_free_interrupt(sc, &sc->vmx_intrs[i]);

	if (sc->vmx_irq_type == PCI_INTR_TYPE_MSI)
		pci_release_msi(sc->vmx_dev);
}
#endif

#ifndef VMXNET3_LEGACY_TX
static int
vmxnet3_alloc_taskqueue(struct vmxnet3_softc *sc)
{
	device_t dev;

	dev = sc->vmx_dev;

	sc->vmx_tq = taskqueue_create(device_get_nameunit(dev), M_NOWAIT,
	    taskqueue_thread_enqueue, &sc->vmx_tq);
	if (sc->vmx_tq == NULL)
		return (ENOMEM);

	return (0);
}

static void
vmxnet3_start_taskqueue(struct vmxnet3_softc *sc)
{
	device_t dev;
	int nthreads, error;

	dev = sc->vmx_dev;

	/*
	 * The taskqueue is typically not frequently used, so a dedicated
	 * thread for each queue is unnecessary.
	 */
	nthreads = MAX(1, sc->vmx_ntxqueues / 2);

	/*
	 * Most drivers just ignore the return value - it only fails
	 * with ENOMEM so an error is not likely. It is hard for us
	 * to recover from an error here.
	 */
	error = taskqueue_start_threads(&sc->vmx_tq, nthreads, PI_NET,
	    "%s taskq", device_get_nameunit(dev));
	if (error)
		device_printf(dev, "failed to start taskqueue: %d", error);
}

static void
vmxnet3_drain_taskqueue(struct vmxnet3_softc *sc)
{
	struct vmxnet3_txqueue *txq;
	int i;

	if (sc->vmx_tq != NULL) {
		for (i = 0; i < sc->vmx_max_ntxqueues; i++) {
			txq = &sc->vmx_txq[i];
			taskqueue_drain(sc->vmx_tq, &txq->vxtxq_defrtask);
		}
	}
}

static void
vmxnet3_free_taskqueue(struct vmxnet3_softc *sc)
{
	if (sc->vmx_tq != NULL) {
		taskqueue_free(sc->vmx_tq);
		sc->vmx_tq = NULL;
	}
}
#endif

static int
vmxnet3_init_rxq(struct vmxnet3_softc *sc, int q)
{
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	int i;

	rxq = &sc->vmx_rxq[q];

	ksnprintf(rxq->vxrxq_name, sizeof(rxq->vxrxq_name), "%s-rx%d",
	    device_get_nameunit(sc->vmx_dev), q);
	lockinit(&rxq->vxrxq_lock, rxq->vxrxq_name, 0, 0);

	rxq->vxrxq_sc = sc;
	rxq->vxrxq_id = q;

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_rid = i;
		rxr->vxrxr_ndesc = sc->vmx_nrxdescs;
		rxr->vxrxr_rxbuf = kmalloc(rxr->vxrxr_ndesc *
		    sizeof(struct vmxnet3_rxbuf), M_DEVBUF, M_INTWAIT | M_ZERO);
		if (rxr->vxrxr_rxbuf == NULL)
			return (ENOMEM);

		rxq->vxrxq_comp_ring.vxcr_ndesc += sc->vmx_nrxdescs;
	}

	return (0);
}

static int
vmxnet3_init_txq(struct vmxnet3_softc *sc, int q)
{
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;

	txq = &sc->vmx_txq[q];
	txr = &txq->vxtxq_cmd_ring;

	ksnprintf(txq->vxtxq_name, sizeof(txq->vxtxq_name), "%s-tx%d",
	    device_get_nameunit(sc->vmx_dev), q);
	lockinit(&txq->vxtxq_lock, txq->vxtxq_name, 0, 0);

	txq->vxtxq_sc = sc;
	txq->vxtxq_id = q;

	txr->vxtxr_ndesc = sc->vmx_ntxdescs;
	txr->vxtxr_txbuf = kmalloc(txr->vxtxr_ndesc *
	    sizeof(struct vmxnet3_txbuf), M_DEVBUF, M_INTWAIT | M_ZERO);
	if (txr->vxtxr_txbuf == NULL)
		return (ENOMEM);

	txq->vxtxq_comp_ring.vxcr_ndesc = sc->vmx_ntxdescs;

#ifndef VMXNET3_LEGACY_TX
	TASK_INIT(&txq->vxtxq_defrtask, 0, vmxnet3_txq_tq_deferred, txq);

	txq->vxtxq_br = buf_ring_alloc(VMXNET3_DEF_BUFRING_SIZE, M_DEVBUF,
	    M_NOWAIT, &txq->vxtxq_lock);
	if (txq->vxtxq_br == NULL)
		return (ENOMEM);
#endif

	return (0);
}

static int
vmxnet3_alloc_rxtx_queues(struct vmxnet3_softc *sc)
{
	int i, error;

	/*
	 * Only attempt to create multiple queues if MSIX is available. MSIX is
	 * disabled by default because its apparently broken for devices passed
	 * through by at least ESXi 5.1. The hw.pci.honor_msi_blacklist tunable
	 * must be set to zero for MSIX. This check prevents us from allocating
	 * queue structures that we will not use.
	 */
	if (sc->vmx_flags & VMXNET3_FLAG_NO_MSIX) {
		sc->vmx_max_nrxqueues = 1;
		sc->vmx_max_ntxqueues = 1;
	}

	sc->vmx_rxq = kmalloc(sizeof(struct vmxnet3_rxqueue) *
	    sc->vmx_max_nrxqueues, M_DEVBUF, M_INTWAIT | M_ZERO);
	sc->vmx_txq = kmalloc(sizeof(struct vmxnet3_txqueue) *
	    sc->vmx_max_ntxqueues, M_DEVBUF, M_INTWAIT | M_ZERO);
	if (sc->vmx_rxq == NULL || sc->vmx_txq == NULL)
		return (ENOMEM);

	for (i = 0; i < sc->vmx_max_nrxqueues; i++) {
		error = vmxnet3_init_rxq(sc, i);
		if (error)
			return (error);
	}

	for (i = 0; i < sc->vmx_max_ntxqueues; i++) {
		error = vmxnet3_init_txq(sc, i);
		if (error)
			return (error);
	}

	return (0);
}

static void
vmxnet3_destroy_rxq(struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_rxring *rxr;
	int i;

	rxq->vxrxq_sc = NULL;
	rxq->vxrxq_id = -1;

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];

		if (rxr->vxrxr_rxbuf != NULL) {
			kfree(rxr->vxrxr_rxbuf, M_DEVBUF);
			rxr->vxrxr_rxbuf = NULL;
		}
	}

#if 0 /* XXX */
	if (mtx_initialized(&rxq->vxrxq_lock) != 0)
#endif
		lockuninit(&rxq->vxrxq_lock);
}

static void
vmxnet3_destroy_txq(struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;

	txr = &txq->vxtxq_cmd_ring;

	txq->vxtxq_sc = NULL;
	txq->vxtxq_id = -1;

#ifndef VMXNET3_LEGACY_TX
	if (txq->vxtxq_br != NULL) {
		buf_ring_free(txq->vxtxq_br, M_DEVBUF);
		txq->vxtxq_br = NULL;
	}
#endif

	if (txr->vxtxr_txbuf != NULL) {
		kfree(txr->vxtxr_txbuf, M_DEVBUF);
		txr->vxtxr_txbuf = NULL;
	}

#if 0 /* XXX */
	if (mtx_initialized(&txq->vxtxq_lock) != 0)
#endif
		lockuninit(&txq->vxtxq_lock);
}

static void
vmxnet3_free_rxtx_queues(struct vmxnet3_softc *sc)
{
	int i;

	if (sc->vmx_rxq != NULL) {
		for (i = 0; i < sc->vmx_max_nrxqueues; i++)
			vmxnet3_destroy_rxq(&sc->vmx_rxq[i]);
		kfree(sc->vmx_rxq, M_DEVBUF);
		sc->vmx_rxq = NULL;
	}

	if (sc->vmx_txq != NULL) {
		for (i = 0; i < sc->vmx_max_ntxqueues; i++)
			vmxnet3_destroy_txq(&sc->vmx_txq[i]);
		kfree(sc->vmx_txq, M_DEVBUF);
		sc->vmx_txq = NULL;
	}
}

static int
vmxnet3_alloc_shared_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	uint8_t *kva;
	size_t size;
	int i, error;

	dev = sc->vmx_dev;

	size = sizeof(struct vmxnet3_driver_shared);
	error = vmxnet3_dma_malloc(sc, size, 1, &sc->vmx_ds_dma);
	if (error) {
		device_printf(dev, "cannot alloc shared memory\n");
		return (error);
	}
	sc->vmx_ds = (struct vmxnet3_driver_shared *) sc->vmx_ds_dma.dma_vaddr;

	size = sc->vmx_ntxqueues * sizeof(struct vmxnet3_txq_shared) +
	    sc->vmx_nrxqueues * sizeof(struct vmxnet3_rxq_shared);
	error = vmxnet3_dma_malloc(sc, size, 128, &sc->vmx_qs_dma);
	if (error) {
		device_printf(dev, "cannot alloc queue shared memory\n");
		return (error);
	}
	sc->vmx_qs = (void *) sc->vmx_qs_dma.dma_vaddr;
	kva = sc->vmx_qs;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		sc->vmx_txq[i].vxtxq_ts = (struct vmxnet3_txq_shared *) kva;
		kva += sizeof(struct vmxnet3_txq_shared);
	}
	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		sc->vmx_rxq[i].vxrxq_rs = (struct vmxnet3_rxq_shared *) kva;
		kva += sizeof(struct vmxnet3_rxq_shared);
	}

	if (sc->vmx_flags & VMXNET3_FLAG_RSS) {
		size = sizeof(struct vmxnet3_rss_shared);
		error = vmxnet3_dma_malloc(sc, size, 128, &sc->vmx_rss_dma);
		if (error) {
			device_printf(dev, "cannot alloc rss shared memory\n");
			return (error);
		}
		sc->vmx_rss =
		    (struct vmxnet3_rss_shared *) sc->vmx_rss_dma.dma_vaddr;
	}

	return (0);
}

static void
vmxnet3_free_shared_data(struct vmxnet3_softc *sc)
{

	if (sc->vmx_rss != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_rss_dma);
		sc->vmx_rss = NULL;
	}

	if (sc->vmx_qs != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_qs_dma);
		sc->vmx_qs = NULL;
	}

	if (sc->vmx_ds != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_ds_dma);
		sc->vmx_ds = NULL;
	}
}

static int
vmxnet3_alloc_txq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	size_t descsz, compsz;
	int i, q, error;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_ntxqueues; q++) {
		txq = &sc->vmx_txq[q];
		txr = &txq->vxtxq_cmd_ring;
		txc = &txq->vxtxq_comp_ring;

		descsz = txr->vxtxr_ndesc * sizeof(struct vmxnet3_txdesc);
		compsz = txr->vxtxr_ndesc * sizeof(struct vmxnet3_txcompdesc);

		error = bus_dma_tag_create(bus_get_dma_tag(dev),
		    1, 0,			/* alignment, boundary */
		    BUS_SPACE_MAXADDR,		/* lowaddr */
		    BUS_SPACE_MAXADDR,		/* highaddr */
		    VMXNET3_TX_MAXSIZE,		/* maxsize */
		    VMXNET3_TX_MAXSEGS,		/* nsegments */
		    VMXNET3_TX_MAXSEGSIZE,	/* maxsegsize */
		    0,				/* flags */
		    &txr->vxtxr_txtag);
		if (error) {
			device_printf(dev,
			    "unable to create Tx buffer tag for queue %d\n", q);
			return (error);
		}

		error = vmxnet3_dma_malloc(sc, descsz, 512, &txr->vxtxr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Tx descriptors for "
			    "queue %d error %d\n", q, error);
			return (error);
		}
		txr->vxtxr_txd =
		    (struct vmxnet3_txdesc *) txr->vxtxr_dma.dma_vaddr;

		error = vmxnet3_dma_malloc(sc, compsz, 512, &txc->vxcr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Tx comp descriptors "
			   "for queue %d error %d\n", q, error);
			return (error);
		}
		txc->vxcr_u.txcd =
		    (struct vmxnet3_txcompdesc *) txc->vxcr_dma.dma_vaddr;

		for (i = 0; i < txr->vxtxr_ndesc; i++) {
			error = bus_dmamap_create(txr->vxtxr_txtag, 0,
			    &txr->vxtxr_txbuf[i].vtxb_dmamap);
			if (error) {
				device_printf(dev, "unable to create Tx buf "
				    "dmamap for queue %d idx %d\n", q, i);
				return (error);
			}
		}
	}

	return (0);
}

static void
vmxnet3_free_txq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	struct vmxnet3_txbuf *txb;
	int i, q;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_ntxqueues; q++) {
		txq = &sc->vmx_txq[q];
		txr = &txq->vxtxq_cmd_ring;
		txc = &txq->vxtxq_comp_ring;

		for (i = 0; i < txr->vxtxr_ndesc; i++) {
			txb = &txr->vxtxr_txbuf[i];
			if (txb->vtxb_dmamap != NULL) {
				bus_dmamap_destroy(txr->vxtxr_txtag,
				    txb->vtxb_dmamap);
				txb->vtxb_dmamap = NULL;
			}
		}

		if (txc->vxcr_u.txcd != NULL) {
			vmxnet3_dma_free(sc, &txc->vxcr_dma);
			txc->vxcr_u.txcd = NULL;
		}

		if (txr->vxtxr_txd != NULL) {
			vmxnet3_dma_free(sc, &txr->vxtxr_dma);
			txr->vxtxr_txd = NULL;
		}

		if (txr->vxtxr_txtag != NULL) {
			bus_dma_tag_destroy(txr->vxtxr_txtag);
			txr->vxtxr_txtag = NULL;
		}
	}
}

static int
vmxnet3_alloc_rxq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	int descsz, compsz;
	int i, j, q, error;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		rxq = &sc->vmx_rxq[q];
		rxc = &rxq->vxrxq_comp_ring;
		compsz = 0;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			descsz = rxr->vxrxr_ndesc *
			    sizeof(struct vmxnet3_rxdesc);
			compsz += rxr->vxrxr_ndesc *
			    sizeof(struct vmxnet3_rxcompdesc);

			error = bus_dma_tag_create(bus_get_dma_tag(dev),
			    1, 0,		/* alignment, boundary */
			    BUS_SPACE_MAXADDR,	/* lowaddr */
			    BUS_SPACE_MAXADDR,	/* highaddr */
			    MJUMPAGESIZE,	/* maxsize */
			    1,			/* nsegments */
			    MJUMPAGESIZE,	/* maxsegsize */
			    0,			/* flags */
			    &rxr->vxrxr_rxtag);
			if (error) {
				device_printf(dev,
				    "unable to create Rx buffer tag for "
				    "queue %d\n", q);
				return (error);
			}

			error = vmxnet3_dma_malloc(sc, descsz, 512,
			    &rxr->vxrxr_dma);
			if (error) {
				device_printf(dev, "cannot allocate Rx "
				    "descriptors for queue %d/%d error %d\n",
				    i, q, error);
				return (error);
			}
			rxr->vxrxr_rxd =
			    (struct vmxnet3_rxdesc *) rxr->vxrxr_dma.dma_vaddr;
		}

		error = vmxnet3_dma_malloc(sc, compsz, 512, &rxc->vxcr_dma);
		if (error) {
			device_printf(dev, "cannot alloc Rx comp descriptors "
			    "for queue %d error %d\n", q, error);
			return (error);
		}
		rxc->vxcr_u.rxcd =
		    (struct vmxnet3_rxcompdesc *) rxc->vxcr_dma.dma_vaddr;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			error = bus_dmamap_create(rxr->vxrxr_rxtag, 0,
			    &rxr->vxrxr_spare_dmap);
			if (error) {
				device_printf(dev, "unable to create spare "
				    "dmamap for queue %d/%d error %d\n",
				    q, i, error);
				return (error);
			}

			for (j = 0; j < rxr->vxrxr_ndesc; j++) {
				error = bus_dmamap_create(rxr->vxrxr_rxtag, 0,
				    &rxr->vxrxr_rxbuf[j].vrxb_dmamap);
				if (error) {
					device_printf(dev, "unable to create "
					    "dmamap for queue %d/%d slot %d "
					    "error %d\n",
					    q, i, j, error);
					return (error);
				}
			}
		}
	}

	return (0);
}

static void
vmxnet3_free_rxq_data(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	struct vmxnet3_rxbuf *rxb;
	int i, j, q;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		rxq = &sc->vmx_rxq[q];
		rxc = &rxq->vxrxq_comp_ring;

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			if (rxr->vxrxr_spare_dmap != NULL) {
				bus_dmamap_destroy(rxr->vxrxr_rxtag,
				    rxr->vxrxr_spare_dmap);
				rxr->vxrxr_spare_dmap = NULL;
			}

			for (j = 0; j < rxr->vxrxr_ndesc; j++) {
				rxb = &rxr->vxrxr_rxbuf[j];
				if (rxb->vrxb_dmamap != NULL) {
					bus_dmamap_destroy(rxr->vxrxr_rxtag,
					    rxb->vrxb_dmamap);
					rxb->vrxb_dmamap = NULL;
				}
			}
		}

		if (rxc->vxcr_u.rxcd != NULL) {
			vmxnet3_dma_free(sc, &rxc->vxcr_dma);
			rxc->vxcr_u.rxcd = NULL;
		}

		for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
			rxr = &rxq->vxrxq_cmd_ring[i];

			if (rxr->vxrxr_rxd != NULL) {
				vmxnet3_dma_free(sc, &rxr->vxrxr_dma);
				rxr->vxrxr_rxd = NULL;
			}

			if (rxr->vxrxr_rxtag != NULL) {
				bus_dma_tag_destroy(rxr->vxrxr_rxtag);
				rxr->vxrxr_rxtag = NULL;
			}
		}
	}
}

static int
vmxnet3_alloc_queue_data(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_alloc_txq_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_rxq_data(sc);
	if (error)
		return (error);

	return (0);
}

static void
vmxnet3_free_queue_data(struct vmxnet3_softc *sc)
{

	if (sc->vmx_rxq != NULL)
		vmxnet3_free_rxq_data(sc);

	if (sc->vmx_txq != NULL)
		vmxnet3_free_txq_data(sc);
}

static int
vmxnet3_alloc_mcast_table(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_dma_malloc(sc, VMXNET3_MULTICAST_MAX * ETHER_ADDR_LEN,
	    32, &sc->vmx_mcast_dma);
	if (error)
		device_printf(sc->vmx_dev, "unable to alloc multicast table\n");
	else
		sc->vmx_mcast = sc->vmx_mcast_dma.dma_vaddr;

	return (error);
}

static void
vmxnet3_free_mcast_table(struct vmxnet3_softc *sc)
{

	if (sc->vmx_mcast != NULL) {
		vmxnet3_dma_free(sc, &sc->vmx_mcast_dma);
		sc->vmx_mcast = NULL;
	}
}

static void
vmxnet3_init_shared_data(struct vmxnet3_softc *sc)
{
	struct vmxnet3_driver_shared *ds;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txq_shared *txs;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_rxq_shared *rxs;
	int i;

	ds = sc->vmx_ds;

	/*
	 * Initialize fields of the shared data that remains the same across
	 * reinits. Note the shared data is zero'd when allocated.
	 */

	ds->magic = VMXNET3_REV1_MAGIC;

	/* DriverInfo */
	ds->version = VMXNET3_DRIVER_VERSION;
	ds->guest = VMXNET3_GOS_FREEBSD |
#ifdef __LP64__
	    VMXNET3_GOS_64BIT;
#else
	    VMXNET3_GOS_32BIT;
#endif
	ds->vmxnet3_revision = 1;
	ds->upt_version = 1;

	/* Misc. conf */
	ds->driver_data = vtophys(sc);
	ds->driver_data_len = sizeof(struct vmxnet3_softc);
	ds->queue_shared = sc->vmx_qs_dma.dma_paddr;
	ds->queue_shared_len = sc->vmx_qs_dma.dma_size;
	ds->nrxsg_max = sc->vmx_max_rxsegs;

	/* RSS conf */
	if (sc->vmx_flags & VMXNET3_FLAG_RSS) {
		ds->rss.version = 1;
		ds->rss.paddr = sc->vmx_rss_dma.dma_paddr;
		ds->rss.len = sc->vmx_rss_dma.dma_size;
	}

	/* Interrupt control. */
	ds->automask = sc->vmx_intr_mask_mode == VMXNET3_IMM_AUTO;
	ds->nintr = sc->vmx_nintrs;
	ds->evintr = sc->vmx_event_intr_idx;
	ds->ictrl = VMXNET3_ICTRL_DISABLE_ALL;

	for (i = 0; i < sc->vmx_nintrs; i++)
		ds->modlevel[i] = UPT1_IMOD_ADAPTIVE;

	/* Receive filter. */
	ds->mcast_table = sc->vmx_mcast_dma.dma_paddr;
	ds->mcast_tablelen = sc->vmx_mcast_dma.dma_size;

	/* Tx queues */
	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_txq[i];
		txs = txq->vxtxq_ts;

		txs->cmd_ring = txq->vxtxq_cmd_ring.vxtxr_dma.dma_paddr;
		txs->cmd_ring_len = txq->vxtxq_cmd_ring.vxtxr_ndesc;
		txs->comp_ring = txq->vxtxq_comp_ring.vxcr_dma.dma_paddr;
		txs->comp_ring_len = txq->vxtxq_comp_ring.vxcr_ndesc;
		txs->driver_data = vtophys(txq);
		txs->driver_data_len = sizeof(struct vmxnet3_txqueue);
	}

	/* Rx queues */
	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		rxq = &sc->vmx_rxq[i];
		rxs = rxq->vxrxq_rs;

		rxs->cmd_ring[0] = rxq->vxrxq_cmd_ring[0].vxrxr_dma.dma_paddr;
		rxs->cmd_ring_len[0] = rxq->vxrxq_cmd_ring[0].vxrxr_ndesc;
		rxs->cmd_ring[1] = rxq->vxrxq_cmd_ring[1].vxrxr_dma.dma_paddr;
		rxs->cmd_ring_len[1] = rxq->vxrxq_cmd_ring[1].vxrxr_ndesc;
		rxs->comp_ring = rxq->vxrxq_comp_ring.vxcr_dma.dma_paddr;
		rxs->comp_ring_len = rxq->vxrxq_comp_ring.vxcr_ndesc;
		rxs->driver_data = vtophys(rxq);
		rxs->driver_data_len = sizeof(struct vmxnet3_rxqueue);
	}
}

static void
vmxnet3_init_hwassist(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp = sc->vmx_ifp;
	uint64_t hwassist;

	hwassist = 0;
	if (ifp->if_capenable & IFCAP_TXCSUM)
		hwassist |= VMXNET3_CSUM_OFFLOAD;
	if (ifp->if_capenable & IFCAP_TXCSUM_IPV6)
		hwassist |= VMXNET3_CSUM_OFFLOAD_IPV6;
#if 0 /* XXX TSO */
	if (ifp->if_capenable & IFCAP_TSO4)
		hwassist |= CSUM_IP_TSO;
	if (ifp->if_capenable & IFCAP_TSO6)
		hwassist |= CSUM_IP6_TSO;
#endif
	ifp->if_hwassist = hwassist;
}

static void
vmxnet3_reinit_interface(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;

	ifp = sc->vmx_ifp;

	/* Use the current MAC address. */
	bcopy(IF_LLADDR(sc->vmx_ifp), sc->vmx_lladdr, ETHER_ADDR_LEN);
	vmxnet3_set_lladdr(sc);

	vmxnet3_init_hwassist(sc);
}

static void
vmxnet3_reinit_rss_shared_data(struct vmxnet3_softc *sc)
{
	/*
	 * Use the same key as the Linux driver until FreeBSD can do
	 * RSS (presumably Toeplitz) in software.
	 */
	static const uint8_t rss_key[UPT1_RSS_MAX_KEY_SIZE] = {
	    0x3b, 0x56, 0xd1, 0x56, 0x13, 0x4a, 0xe7, 0xac,
	    0xe8, 0x79, 0x09, 0x75, 0xe8, 0x65, 0x79, 0x28,
	    0x35, 0x12, 0xb9, 0x56, 0x7c, 0x76, 0x4b, 0x70,
	    0xd8, 0x56, 0xa3, 0x18, 0x9b, 0x0a, 0xee, 0xf3,
	    0x96, 0xa6, 0x9f, 0x8f, 0x9e, 0x8c, 0x90, 0xc9,
	};

	struct vmxnet3_driver_shared *ds;
	struct vmxnet3_rss_shared *rss;
	int i;

	ds = sc->vmx_ds;
	rss = sc->vmx_rss;

	rss->hash_type =
	    UPT1_RSS_HASH_TYPE_IPV4 | UPT1_RSS_HASH_TYPE_TCP_IPV4 |
	    UPT1_RSS_HASH_TYPE_IPV6 | UPT1_RSS_HASH_TYPE_TCP_IPV6;
	rss->hash_func = UPT1_RSS_HASH_FUNC_TOEPLITZ;
	rss->hash_key_size = UPT1_RSS_MAX_KEY_SIZE;
	rss->ind_table_size = UPT1_RSS_MAX_IND_TABLE_SIZE;
	memcpy(rss->hash_key, rss_key, UPT1_RSS_MAX_KEY_SIZE);

	for (i = 0; i < UPT1_RSS_MAX_IND_TABLE_SIZE; i++)
		rss->ind_table[i] = i % sc->vmx_nrxqueues;
}

static void
vmxnet3_reinit_shared_data(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	struct vmxnet3_driver_shared *ds;

	ifp = sc->vmx_ifp;
	ds = sc->vmx_ds;

	ds->mtu = ifp->if_mtu;
	ds->ntxqueue = sc->vmx_ntxqueues;
	ds->nrxqueue = sc->vmx_nrxqueues;

	ds->upt_features = 0;
	if (ifp->if_capenable & (IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6))
		ds->upt_features |= UPT1_F_CSUM;
	if (ifp->if_capenable & IFCAP_VLAN_HWTAGGING)
		ds->upt_features |= UPT1_F_VLAN;
#if 0 /* XXX LRO */
	if (ifp->if_capenable & IFCAP_LRO)
		ds->upt_features |= UPT1_F_LRO;
#endif

	if (sc->vmx_flags & VMXNET3_FLAG_RSS) {
		ds->upt_features |= UPT1_F_RSS;
		vmxnet3_reinit_rss_shared_data(sc);
	}

	vmxnet3_write_bar1(sc, VMXNET3_BAR1_DSL, sc->vmx_ds_dma.dma_paddr);
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_DSH,
	    (uint64_t) sc->vmx_ds_dma.dma_paddr >> 32);
}

static int
vmxnet3_alloc_data(struct vmxnet3_softc *sc)
{
	int error;

	error = vmxnet3_alloc_shared_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_queue_data(sc);
	if (error)
		return (error);

	error = vmxnet3_alloc_mcast_table(sc);
	if (error)
		return (error);

	vmxnet3_init_shared_data(sc);

	return (0);
}

static void
vmxnet3_free_data(struct vmxnet3_softc *sc)
{

	vmxnet3_free_mcast_table(sc);
	vmxnet3_free_queue_data(sc);
	vmxnet3_free_shared_data(sc);
}

static int
vmxnet3_setup_interface(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct ifnet *ifp;

	dev = sc->vmx_dev;

	ifp = sc->vmx_ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		device_printf(dev, "cannot allocate ifnet structure\n");
		return (ENOSPC);
	}

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_baudrate = IF_Gbps(10ULL);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = vmxnet3_init;
	ifp->if_ioctl = vmxnet3_ioctl;
#if 0 /* XXX TSO */
	ifp->if_hw_tsomax = 65536 - (ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN);
	ifp->if_hw_tsomaxsegcount = VMXNET3_TX_MAXSEGS;
	ifp->if_hw_tsomaxsegsize = VMXNET3_TX_MAXSEGSIZE;
#endif

#ifdef VMXNET3_LEGACY_TX
	ifp->if_start = vmxnet3_start;
	ifq_set_maxlen(&ifp->if_snd, sc->vmx_ntxdescs - 1);
	ifq_set_ready(&ifp->if_snd);
#else
	ifp->if_transmit = vmxnet3_txq_mq_start;
	ifp->if_qflush = vmxnet3_qflush;
#endif

	vmxnet3_get_lladdr(sc);
	ether_ifattach(ifp, sc->vmx_lladdr, NULL);

	ifp->if_capabilities |= IFCAP_RXCSUM | IFCAP_TXCSUM;
	ifp->if_capabilities |= IFCAP_RXCSUM_IPV6 | IFCAP_TXCSUM_IPV6;
#if 0 /* XXX TSO */
	ifp->if_capabilities |= IFCAP_TSO4 | IFCAP_TSO6;
#endif
	ifp->if_capabilities |= IFCAP_VLAN_MTU | IFCAP_VLAN_HWTAGGING |
	    IFCAP_VLAN_HWCSUM;
	ifp->if_capenable = ifp->if_capabilities;

#if 0 /* XXX LRO / VLAN_HWFILTER */
	/* These capabilities are not enabled by default. */
	ifp->if_capabilities |= /* IFCAP_LRO | */ IFCAP_VLAN_HWFILTER;
#endif

	sc->vmx_vlan_attach = EVENTHANDLER_REGISTER(vlan_config,
	    vmxnet3_register_vlan, sc, EVENTHANDLER_PRI_FIRST);
	sc->vmx_vlan_detach = EVENTHANDLER_REGISTER(vlan_config,
	    vmxnet3_unregister_vlan, sc, EVENTHANDLER_PRI_FIRST);

	ifmedia_init(&sc->vmx_media, 0, vmxnet3_media_change,
	    vmxnet3_media_status);
	ifmedia_add(&sc->vmx_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->vmx_media, IFM_ETHER | IFM_AUTO);

	return (0);
}

static void
vmxnet3_evintr(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct ifnet *ifp;
	struct vmxnet3_txq_shared *ts;
	struct vmxnet3_rxq_shared *rs;
	uint32_t event;
	int reset;

	dev = sc->vmx_dev;
	ifp = sc->vmx_ifp;
	reset = 0;

	VMXNET3_CORE_LOCK(sc);

	/* Clear events. */
	event = sc->vmx_ds->event;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_EVENT, event);

	if (event & VMXNET3_EVENT_LINK) {
		vmxnet3_link_status(sc);
		if (sc->vmx_link_active != 0)
			vmxnet3_tx_start_all(sc);
	}

	if (event & (VMXNET3_EVENT_TQERROR | VMXNET3_EVENT_RQERROR)) {
		reset = 1;
		vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_STATUS);
		ts = sc->vmx_txq[0].vxtxq_ts;
		if (ts->stopped != 0)
			device_printf(dev, "Tx queue error %#x\n", ts->error);
		rs = sc->vmx_rxq[0].vxrxq_rs;
		if (rs->stopped != 0)
			device_printf(dev, "Rx queue error %#x\n", rs->error);
		device_printf(dev, "Rx/Tx queue error event ... resetting\n");
	}

	if (event & VMXNET3_EVENT_DIC)
		device_printf(dev, "device implementation change event\n");
	if (event & VMXNET3_EVENT_DEBUG)
		device_printf(dev, "debug event\n");

	if (reset != 0) {
		ifp->if_flags &= ~IFF_RUNNING;
		vmxnet3_init_locked(sc);
	}

	VMXNET3_CORE_UNLOCK(sc);
}

static void
vmxnet3_txq_eof(struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;
	struct vmxnet3_txcompdesc *txcd;
	struct vmxnet3_txbuf *txb;
	struct mbuf *m;
	u_int sop;

	sc = txq->vxtxq_sc;
	ifp = sc->vmx_ifp;
	txr = &txq->vxtxq_cmd_ring;
	txc = &txq->vxtxq_comp_ring;

	VMXNET3_TXQ_LOCK_ASSERT(txq);

	for (;;) {
		txcd = &txc->vxcr_u.txcd[txc->vxcr_next];
		if (txcd->gen != txc->vxcr_gen)
			break;
		vmxnet3_barrier(sc, VMXNET3_BARRIER_RD);

		if (++txc->vxcr_next == txc->vxcr_ndesc) {
			txc->vxcr_next = 0;
			txc->vxcr_gen ^= 1;
		}

		sop = txr->vxtxr_next;
		txb = &txr->vxtxr_txbuf[sop];

		if ((m = txb->vtxb_m) != NULL) {
			bus_dmamap_sync(txr->vxtxr_txtag, txb->vtxb_dmamap,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(txr->vxtxr_txtag, txb->vtxb_dmamap);

			txq->vxtxq_stats.vmtxs_opackets++;
			txq->vxtxq_stats.vmtxs_obytes += m->m_pkthdr.len;
			if (m->m_flags & M_MCAST)
				txq->vxtxq_stats.vmtxs_omcasts++;

			m_freem(m);
			txb->vtxb_m = NULL;
		}

		txr->vxtxr_next = (txcd->eop_idx + 1) % txr->vxtxr_ndesc;
	}

	if (txr->vxtxr_head == txr->vxtxr_next)
		txq->vxtxq_watchdog = 0;
}

static int
vmxnet3_newbuf(struct vmxnet3_softc *sc, struct vmxnet3_rxring *rxr)
{
	struct ifnet *ifp;
	struct mbuf *m;
	struct vmxnet3_rxdesc *rxd;
	struct vmxnet3_rxbuf *rxb;
	bus_dma_tag_t tag;
	bus_dmamap_t dmap;
	bus_dma_segment_t segs[1];
	int idx, clsize, btype, flags, nsegs, error;

	ifp = sc->vmx_ifp;
	tag = rxr->vxrxr_rxtag;
	dmap = rxr->vxrxr_spare_dmap;
	idx = rxr->vxrxr_fill;
	rxd = &rxr->vxrxr_rxd[idx];
	rxb = &rxr->vxrxr_rxbuf[idx];

#ifdef VMXNET3_FAILPOINTS
	KFAIL_POINT_CODE(VMXNET3_FP, newbuf, return ENOBUFS);
	if (rxr->vxrxr_rid != 0)
		KFAIL_POINT_CODE(VMXNET3_FP, newbuf_body_only, return ENOBUFS);
#endif

	if (rxr->vxrxr_rid == 0 && (idx % sc->vmx_rx_max_chain) == 0) {
		flags = M_PKTHDR;
		clsize = MCLBYTES;
		btype = VMXNET3_BTYPE_HEAD;
	} else {
		flags = M_PKTHDR;
		clsize = MJUMPAGESIZE;
		btype = VMXNET3_BTYPE_BODY;
	}

	m = m_getjcl(M_NOWAIT, MT_DATA, flags, clsize);
	if (m == NULL) {
		sc->vmx_stats.vmst_mgetcl_failed++;
		return (ENOBUFS);
	}

	if (btype == VMXNET3_BTYPE_HEAD) {
		m->m_len = m->m_pkthdr.len = clsize;
		m_adj(m, ETHER_ALIGN);
	} else
		m->m_len = clsize;

	error = bus_dmamap_load_mbuf_segment(tag, dmap, m, &segs[0], 1, &nsegs,
	    BUS_DMA_NOWAIT);
	if (error) {
		m_freem(m);
		sc->vmx_stats.vmst_mbuf_load_failed++;
		return (error);
	}
	KASSERT(nsegs == 1,
	    ("%s: mbuf %p with too many segments %d", __func__, m, nsegs));
	if (btype == VMXNET3_BTYPE_BODY)
		m->m_flags &= ~M_PKTHDR;

	if (rxb->vrxb_m != NULL) {
		bus_dmamap_sync(tag, rxb->vrxb_dmamap, BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(tag, rxb->vrxb_dmamap);
	}

	rxr->vxrxr_spare_dmap = rxb->vrxb_dmamap;
	rxb->vrxb_dmamap = dmap;
	rxb->vrxb_m = m;

	rxd->addr = segs[0].ds_addr;
	rxd->len = segs[0].ds_len;
	rxd->btype = btype;
	rxd->gen = rxr->vxrxr_gen;

	vmxnet3_rxr_increment_fill(rxr);
	return (0);
}

static void
vmxnet3_rxq_eof_discard(struct vmxnet3_rxqueue *rxq,
    struct vmxnet3_rxring *rxr, int idx)
{
	struct vmxnet3_rxdesc *rxd;

	rxd = &rxr->vxrxr_rxd[idx];
	rxd->gen = rxr->vxrxr_gen;
	vmxnet3_rxr_increment_fill(rxr);
}

static void
vmxnet3_rxq_discard_chain(struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	struct vmxnet3_rxcompdesc *rxcd;
	int idx, eof;

	sc = rxq->vxrxq_sc;
	rxc = &rxq->vxrxq_comp_ring;

	do {
		rxcd = &rxc->vxcr_u.rxcd[rxc->vxcr_next];
		if (rxcd->gen != rxc->vxcr_gen)
			break;		/* Not expected. */
		vmxnet3_barrier(sc, VMXNET3_BARRIER_RD);

		if (++rxc->vxcr_next == rxc->vxcr_ndesc) {
			rxc->vxcr_next = 0;
			rxc->vxcr_gen ^= 1;
		}

		idx = rxcd->rxd_idx;
		eof = rxcd->eop;
		if (rxcd->qid < sc->vmx_nrxqueues)
			rxr = &rxq->vxrxq_cmd_ring[0];
		else
			rxr = &rxq->vxrxq_cmd_ring[1];
		vmxnet3_rxq_eof_discard(rxq, rxr, idx);
	} while (!eof);
}

static void
vmxnet3_rx_csum(struct vmxnet3_rxcompdesc *rxcd, struct mbuf *m)
{

	if (rxcd->ipv4) {
		m->m_pkthdr.csum_flags |= CSUM_IP_CHECKED;
		if (rxcd->ipcsum_ok)
			m->m_pkthdr.csum_flags |= CSUM_IP_VALID;
	}

	if (!rxcd->fragment) {
		if (rxcd->csum_ok && (rxcd->tcp || rxcd->udp)) {
			m->m_pkthdr.csum_flags |= CSUM_DATA_VALID |
			    CSUM_PSEUDO_HDR;
			m->m_pkthdr.csum_data = 0xFFFF;
		}
	}
}

static void
vmxnet3_rxq_input(struct vmxnet3_rxqueue *rxq,
    struct vmxnet3_rxcompdesc *rxcd, struct mbuf *m)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;

	sc = rxq->vxrxq_sc;
	ifp = sc->vmx_ifp;

	if (rxcd->error) {
		rxq->vxrxq_stats.vmrxs_ierrors++;
		m_freem(m);
		return;
	}

#if 0
#ifdef notyet
	switch (rxcd->rss_type) {
	case VMXNET3_RCD_RSS_TYPE_IPV4:
		m->m_pkthdr.flowid = rxcd->rss_hash;
		M_HASHTYPE_SET(m, M_HASHTYPE_RSS_IPV4);
		break;
	case VMXNET3_RCD_RSS_TYPE_TCPIPV4:
		m->m_pkthdr.flowid = rxcd->rss_hash;
		M_HASHTYPE_SET(m, M_HASHTYPE_RSS_TCP_IPV4);
		break;
	case VMXNET3_RCD_RSS_TYPE_IPV6:
		m->m_pkthdr.flowid = rxcd->rss_hash;
		M_HASHTYPE_SET(m, M_HASHTYPE_RSS_IPV6);
		break;
	case VMXNET3_RCD_RSS_TYPE_TCPIPV6:
		m->m_pkthdr.flowid = rxcd->rss_hash;
		M_HASHTYPE_SET(m, M_HASHTYPE_RSS_TCP_IPV6);
		break;
	default: /* VMXNET3_RCD_RSS_TYPE_NONE */
		m->m_pkthdr.flowid = rxq->vxrxq_id;
		M_HASHTYPE_SET(m, M_HASHTYPE_OPAQUE);
		break;
	}
#else
	m->m_pkthdr.flowid = rxq->vxrxq_id;
	M_HASHTYPE_SET(m, M_HASHTYPE_OPAQUE);
#endif
#endif

	if (!rxcd->no_csum)
		vmxnet3_rx_csum(rxcd, m);
	if (rxcd->vlan) {
		m->m_flags |= M_VLANTAG;
		m->m_pkthdr.ether_vlantag = rxcd->vtag;
	}

	rxq->vxrxq_stats.vmrxs_ipackets++;
	rxq->vxrxq_stats.vmrxs_ibytes += m->m_pkthdr.len;

	VMXNET3_RXQ_UNLOCK(rxq);
	(*ifp->if_input)(ifp, m, NULL, -1);
	VMXNET3_RXQ_LOCK(rxq);
}

static void
vmxnet3_rxq_eof(struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	struct vmxnet3_rxdesc *rxd;
	struct vmxnet3_rxcompdesc *rxcd;
	struct mbuf *m, *m_head, *m_tail;
	int idx, length;

	sc = rxq->vxrxq_sc;
	ifp = sc->vmx_ifp;
	rxc = &rxq->vxrxq_comp_ring;

	VMXNET3_RXQ_LOCK_ASSERT(rxq);

	if ((ifp->if_flags & IFF_RUNNING) == 0)
		return;

	m_head = rxq->vxrxq_mhead;
	rxq->vxrxq_mhead = NULL;
	m_tail = rxq->vxrxq_mtail;
	rxq->vxrxq_mtail = NULL;
	KKASSERT(m_head == NULL || m_tail != NULL);

	for (;;) {
		rxcd = &rxc->vxcr_u.rxcd[rxc->vxcr_next];
		if (rxcd->gen != rxc->vxcr_gen) {
			rxq->vxrxq_mhead = m_head;
			rxq->vxrxq_mtail = m_tail;
			break;
		}
		vmxnet3_barrier(sc, VMXNET3_BARRIER_RD);

		if (++rxc->vxcr_next == rxc->vxcr_ndesc) {
			rxc->vxcr_next = 0;
			rxc->vxcr_gen ^= 1;
		}

		idx = rxcd->rxd_idx;
		length = rxcd->len;
		if (rxcd->qid < sc->vmx_nrxqueues)
			rxr = &rxq->vxrxq_cmd_ring[0];
		else
			rxr = &rxq->vxrxq_cmd_ring[1];
		rxd = &rxr->vxrxr_rxd[idx];

		m = rxr->vxrxr_rxbuf[idx].vrxb_m;
		KASSERT(m != NULL, ("%s: queue %d idx %d without mbuf",
		    __func__, rxcd->qid, idx));

		/*
		 * The host may skip descriptors. We detect this when this
		 * descriptor does not match the previous fill index. Catch
		 * up with the host now.
		 */
		if (__predict_false(rxr->vxrxr_fill != idx)) {
			while (rxr->vxrxr_fill != idx) {
				rxr->vxrxr_rxd[rxr->vxrxr_fill].gen =
				    rxr->vxrxr_gen;
				vmxnet3_rxr_increment_fill(rxr);
			}
		}

		if (rxcd->sop) {
			KASSERT(rxd->btype == VMXNET3_BTYPE_HEAD,
			    ("%s: start of frame w/o head buffer", __func__));
			KASSERT(rxr == &rxq->vxrxq_cmd_ring[0],
			    ("%s: start of frame not in ring 0", __func__));
			KASSERT((idx % sc->vmx_rx_max_chain) == 0,
			    ("%s: start of frame at unexcepted index %d (%d)",
			     __func__, idx, sc->vmx_rx_max_chain));
			KASSERT(m_head == NULL,
			    ("%s: duplicate start of frame?", __func__));

			if (length == 0) {
				/* Just ignore this descriptor. */
				vmxnet3_rxq_eof_discard(rxq, rxr, idx);
				goto nextp;
			}

			if (vmxnet3_newbuf(sc, rxr) != 0) {
				rxq->vxrxq_stats.vmrxs_iqdrops++;
				vmxnet3_rxq_eof_discard(rxq, rxr, idx);
				if (!rxcd->eop)
					vmxnet3_rxq_discard_chain(rxq);
				goto nextp;
			}

			m->m_pkthdr.rcvif = ifp;
			m->m_pkthdr.len = m->m_len = length;
			m->m_pkthdr.csum_flags = 0;
			m_head = m_tail = m;

		} else {
			KASSERT(rxd->btype == VMXNET3_BTYPE_BODY,
			    ("%s: non start of frame w/o body buffer", __func__));

			if (m_head == NULL && m_tail == NULL) {
				/*
				 * This is a continuation of a packet that we
				 * started to drop, but could not drop entirely
				 * because this segment was still owned by the
				 * host.  So, drop the remainder now.
				 */
				vmxnet3_rxq_eof_discard(rxq, rxr, idx);
				if (!rxcd->eop)
					vmxnet3_rxq_discard_chain(rxq);
				goto nextp;
			}

			KASSERT(m_head != NULL,
			    ("%s: frame not started?", __func__));

			if (vmxnet3_newbuf(sc, rxr) != 0) {
				rxq->vxrxq_stats.vmrxs_iqdrops++;
				vmxnet3_rxq_eof_discard(rxq, rxr, idx);
				if (!rxcd->eop)
					vmxnet3_rxq_discard_chain(rxq);
				m_freem(m_head);
				m_head = m_tail = NULL;
				goto nextp;
			}

			m->m_len = length;
			m_head->m_pkthdr.len += length;
			m_tail->m_next = m;
			m_tail = m;
		}

		if (rxcd->eop) {
			vmxnet3_rxq_input(rxq, rxcd, m_head);
			m_head = m_tail = NULL;

			/* Must recheck after dropping the Rx lock. */
			if ((ifp->if_flags & IFF_RUNNING) == 0)
				break;
		}

nextp:
		if (__predict_false(rxq->vxrxq_rs->update_rxhead)) {
			int qid = rxcd->qid;
			bus_size_t r;

			idx = (idx + 1) % rxr->vxrxr_ndesc;
			if (qid >= sc->vmx_nrxqueues) {
				qid -= sc->vmx_nrxqueues;
				r = VMXNET3_BAR0_RXH2(qid);
			} else
				r = VMXNET3_BAR0_RXH1(qid);
			vmxnet3_write_bar0(sc, r, idx);
		}
	}
}

static void
vmxnet3_legacy_intr(void *xsc)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_txqueue *txq;

	sc = xsc;
	rxq = &sc->vmx_rxq[0];
	txq = &sc->vmx_txq[0];

	if (sc->vmx_intr_type == VMXNET3_IT_LEGACY) {
		if (vmxnet3_read_bar1(sc, VMXNET3_BAR1_INTR) == 0)
			return;
	}
	if (sc->vmx_intr_mask_mode == VMXNET3_IMM_ACTIVE)
		vmxnet3_disable_all_intrs(sc);

	if (sc->vmx_ds->event != 0)
		vmxnet3_evintr(sc);

	VMXNET3_RXQ_LOCK(rxq);
	vmxnet3_rxq_eof(rxq);
	VMXNET3_RXQ_UNLOCK(rxq);

	VMXNET3_TXQ_LOCK(txq);
	vmxnet3_txq_eof(txq);
	vmxnet3_txq_start(txq);
	VMXNET3_TXQ_UNLOCK(txq);

	vmxnet3_enable_all_intrs(sc);
}

#ifdef __FreeBSD__
static void
vmxnet3_txq_intr(void *xtxq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;

	txq = xtxq;
	sc = txq->vxtxq_sc;

	if (sc->vmx_intr_mask_mode == VMXNET3_IMM_ACTIVE)
		vmxnet3_disable_intr(sc, txq->vxtxq_intr_idx);

	VMXNET3_TXQ_LOCK(txq);
	vmxnet3_txq_eof(txq);
	vmxnet3_txq_start(txq);
	VMXNET3_TXQ_UNLOCK(txq);

	vmxnet3_enable_intr(sc, txq->vxtxq_intr_idx);
}

static void
vmxnet3_rxq_intr(void *xrxq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_rxqueue *rxq;

	rxq = xrxq;
	sc = rxq->vxrxq_sc;

	if (sc->vmx_intr_mask_mode == VMXNET3_IMM_ACTIVE)
		vmxnet3_disable_intr(sc, rxq->vxrxq_intr_idx);

	VMXNET3_RXQ_LOCK(rxq);
	vmxnet3_rxq_eof(rxq);
	VMXNET3_RXQ_UNLOCK(rxq);

	vmxnet3_enable_intr(sc, rxq->vxrxq_intr_idx);
}

static void
vmxnet3_event_intr(void *xsc)
{
	struct vmxnet3_softc *sc;

	sc = xsc;

	if (sc->vmx_intr_mask_mode == VMXNET3_IMM_ACTIVE)
		vmxnet3_disable_intr(sc, sc->vmx_event_intr_idx);

	if (sc->vmx_ds->event != 0)
		vmxnet3_evintr(sc);

	vmxnet3_enable_intr(sc, sc->vmx_event_intr_idx);
}
#endif

static void
vmxnet3_txstop(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;
	struct vmxnet3_txbuf *txb;
	int i;

	txr = &txq->vxtxq_cmd_ring;

	for (i = 0; i < txr->vxtxr_ndesc; i++) {
		txb = &txr->vxtxr_txbuf[i];

		if (txb->vtxb_m == NULL)
			continue;

		bus_dmamap_sync(txr->vxtxr_txtag, txb->vtxb_dmamap,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(txr->vxtxr_txtag, txb->vtxb_dmamap);
		m_freem(txb->vtxb_m);
		txb->vtxb_m = NULL;
	}
}

static void
vmxnet3_rxstop(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq)
{
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_rxbuf *rxb;
	int i, j;

	if (rxq->vxrxq_mhead != NULL) {
		m_freem(rxq->vxrxq_mhead);
		rxq->vxrxq_mhead = NULL;
		rxq->vxrxq_mtail = NULL;
	}

	for (i = 0; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];

		for (j = 0; j < rxr->vxrxr_ndesc; j++) {
			rxb = &rxr->vxrxr_rxbuf[j];

			if (rxb->vrxb_m == NULL)
				continue;

			bus_dmamap_sync(rxr->vxrxr_rxtag, rxb->vrxb_dmamap,
			    BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(rxr->vxrxr_rxtag, rxb->vrxb_dmamap);
			m_freem(rxb->vrxb_m);
			rxb->vrxb_m = NULL;
		}
	}
}

static void
vmxnet3_stop_rendezvous(struct vmxnet3_softc *sc)
{
	struct vmxnet3_rxqueue *rxq;
	struct vmxnet3_txqueue *txq;
	int i;

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		rxq = &sc->vmx_rxq[i];
		VMXNET3_RXQ_LOCK(rxq);
		VMXNET3_RXQ_UNLOCK(rxq);
	}

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_txq[i];
		VMXNET3_TXQ_LOCK(txq);
		VMXNET3_TXQ_UNLOCK(txq);
	}
}

static void
vmxnet3_stop(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	int q;

	ifp = sc->vmx_ifp;
	VMXNET3_CORE_LOCK_ASSERT(sc);

	ifp->if_flags &= ~IFF_RUNNING;
	sc->vmx_link_active = 0;
	callout_stop(&sc->vmx_tick);

	/* Disable interrupts. */
	vmxnet3_disable_all_intrs(sc);
	vmxnet3_write_cmd(sc, VMXNET3_CMD_DISABLE);

	vmxnet3_stop_rendezvous(sc);

	for (q = 0; q < sc->vmx_ntxqueues; q++)
		vmxnet3_txstop(sc, &sc->vmx_txq[q]);
	for (q = 0; q < sc->vmx_nrxqueues; q++)
		vmxnet3_rxstop(sc, &sc->vmx_rxq[q]);

	vmxnet3_write_cmd(sc, VMXNET3_CMD_RESET);
}

static void
vmxnet3_txinit(struct vmxnet3_softc *sc, struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_txring *txr;
	struct vmxnet3_comp_ring *txc;

	txr = &txq->vxtxq_cmd_ring;
	txr->vxtxr_head = 0;
	txr->vxtxr_next = 0;
	txr->vxtxr_gen = VMXNET3_INIT_GEN;
	bzero(txr->vxtxr_txd,
	    txr->vxtxr_ndesc * sizeof(struct vmxnet3_txdesc));

	txc = &txq->vxtxq_comp_ring;
	txc->vxcr_next = 0;
	txc->vxcr_gen = VMXNET3_INIT_GEN;
	bzero(txc->vxcr_u.txcd,
	    txc->vxcr_ndesc * sizeof(struct vmxnet3_txcompdesc));
}

static int
vmxnet3_rxinit(struct vmxnet3_softc *sc, struct vmxnet3_rxqueue *rxq)
{
	struct ifnet *ifp;
	struct vmxnet3_rxring *rxr;
	struct vmxnet3_comp_ring *rxc;
	int i, populate, idx, frame_size, error;

	ifp = sc->vmx_ifp;
	frame_size = ETHER_ALIGN + sizeof(struct ether_vlan_header) +
	    ifp->if_mtu;

	/*
	 * If the MTU causes us to exceed what a regular sized cluster can
	 * handle, we allocate a second MJUMPAGESIZE cluster after it in
	 * ring 0. If in use, ring 1 always contains MJUMPAGESIZE clusters.
	 *
	 * Keep rx_max_chain a divisor of the maximum Rx ring size to make
	 * our life easier. We do not support changing the ring size after
	 * the attach.
	 */
	if (frame_size <= MCLBYTES)
		sc->vmx_rx_max_chain = 1;
	else
		sc->vmx_rx_max_chain = 2;

	/*
	 * Only populate ring 1 if the configuration will take advantage
	 * of it. That is either when LRO is enabled or the frame size
	 * exceeds what ring 0 can contain.
	 */
#if 0 /* XXX LRO */
	if ((ifp->if_capenable & IFCAP_LRO) == 0 &&
#else
	if (
#endif
	    frame_size <= MCLBYTES + MJUMPAGESIZE)
		populate = 1;
	else
		populate = VMXNET3_RXRINGS_PERQ;

	for (i = 0; i < populate; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_fill = 0;
		rxr->vxrxr_gen = VMXNET3_INIT_GEN;
		bzero(rxr->vxrxr_rxd,
		    rxr->vxrxr_ndesc * sizeof(struct vmxnet3_rxdesc));

		for (idx = 0; idx < rxr->vxrxr_ndesc; idx++) {
			error = vmxnet3_newbuf(sc, rxr);
			if (error)
				return (error);
		}
	}

	for (/**/; i < VMXNET3_RXRINGS_PERQ; i++) {
		rxr = &rxq->vxrxq_cmd_ring[i];
		rxr->vxrxr_fill = 0;
		rxr->vxrxr_gen = 0;
		bzero(rxr->vxrxr_rxd,
		    rxr->vxrxr_ndesc * sizeof(struct vmxnet3_rxdesc));
	}

	rxc = &rxq->vxrxq_comp_ring;
	rxc->vxcr_next = 0;
	rxc->vxcr_gen = VMXNET3_INIT_GEN;
	bzero(rxc->vxcr_u.rxcd,
	    rxc->vxcr_ndesc * sizeof(struct vmxnet3_rxcompdesc));

	return (0);
}

static int
vmxnet3_reinit_queues(struct vmxnet3_softc *sc)
{
	device_t dev;
	int q, error;

	dev = sc->vmx_dev;

	for (q = 0; q < sc->vmx_ntxqueues; q++)
		vmxnet3_txinit(sc, &sc->vmx_txq[q]);

	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		error = vmxnet3_rxinit(sc, &sc->vmx_rxq[q]);
		if (error) {
			device_printf(dev, "cannot populate Rx queue %d\n", q);
			return (error);
		}
	}

	return (0);
}

static int
vmxnet3_enable_device(struct vmxnet3_softc *sc)
{
	int q;

	if (vmxnet3_read_cmd(sc, VMXNET3_CMD_ENABLE) != 0) {
		device_printf(sc->vmx_dev, "device enable command failed!\n");
		return (1);
	}

	/* Reset the Rx queue heads. */
	for (q = 0; q < sc->vmx_nrxqueues; q++) {
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_RXH1(q), 0);
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_RXH2(q), 0);
	}

	return (0);
}

static void
vmxnet3_reinit_rxfilters(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;

	ifp = sc->vmx_ifp;

	vmxnet3_set_rxfilter(sc);

#if 0 /* VLAN_HWFILTER */
	if (ifp->if_capenable & IFCAP_VLAN_HWFILTER)
		bcopy(sc->vmx_vlan_filter, sc->vmx_ds->vlan_filter,
		    sizeof(sc->vmx_ds->vlan_filter));
	else
#endif
		bzero(sc->vmx_ds->vlan_filter,
		    sizeof(sc->vmx_ds->vlan_filter));
	vmxnet3_write_cmd(sc, VMXNET3_CMD_VLAN_FILTER);
}

static int
vmxnet3_reinit(struct vmxnet3_softc *sc)
{

	vmxnet3_reinit_interface(sc);
	vmxnet3_reinit_shared_data(sc);

	if (vmxnet3_reinit_queues(sc) != 0)
		return (ENXIO);

	if (vmxnet3_enable_device(sc) != 0)
		return (ENXIO);

	vmxnet3_reinit_rxfilters(sc);

	return (0);
}

static void
vmxnet3_init_locked(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;

	ifp = sc->vmx_ifp;

	if (ifp->if_flags & IFF_RUNNING)
		return;

	vmxnet3_stop(sc);

	if (vmxnet3_reinit(sc) != 0) {
		vmxnet3_stop(sc);
		return;
	}

	ifp->if_flags |= IFF_RUNNING;
	vmxnet3_link_status(sc);

	vmxnet3_enable_all_intrs(sc);
	callout_reset(&sc->vmx_tick, hz, vmxnet3_tick, sc);
}

static void
vmxnet3_init(void *xsc)
{
	struct vmxnet3_softc *sc;

	sc = xsc;

	VMXNET3_CORE_LOCK(sc);
	vmxnet3_init_locked(sc);
	VMXNET3_CORE_UNLOCK(sc);
}

/*
 * BMV: Much of this can go away once we finally have offsets in
 * the mbuf packet header. Bug andre@.
 */
static int
vmxnet3_txq_offload_ctx(struct vmxnet3_txqueue *txq, struct mbuf *m,
    int *etype, int *proto, int *start)
{
	struct ether_vlan_header *evh;
	int offset;
#if defined(INET)
	struct ip *ip = NULL;
#endif
#if defined(INET6)
	struct ip6_hdr *ip6 = NULL;
#endif

	evh = mtod(m, struct ether_vlan_header *);
	if (evh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		/* BMV: We should handle nested VLAN tags too. */
		*etype = ntohs(evh->evl_proto);
		offset = sizeof(struct ether_vlan_header);
	} else {
		*etype = ntohs(evh->evl_encap_proto);
		offset = sizeof(struct ether_header);
	}

	switch (*etype) {
#if defined(INET)
	case ETHERTYPE_IP:
		if (__predict_false(m->m_len < offset + sizeof(struct ip))) {
			m = m_pullup(m, offset + sizeof(struct ip));
			if (m == NULL)
				return (EINVAL);
		}

		ip = (struct ip *)(mtod(m, uint8_t *) + offset);
		*proto = ip->ip_p;
		*start = offset + (ip->ip_hl << 2);
		break;
#endif
#if defined(INET6)
	case ETHERTYPE_IPV6:
		if (__predict_false(m->m_len <
		    offset + sizeof(struct ip6_hdr))) {
			m = m_pullup(m, offset + sizeof(struct ip6_hdr));
			if (m == NULL)
				return (EINVAL);
		}

		ip6 = (struct ip6_hdr *)(mtod(m, uint8_t *) + offset);
		*proto = -1;
		*start = ip6_lasthdr(m, offset, IPPROTO_IPV6, proto);
		/* Assert the network stack sent us a valid packet. */
		KASSERT(*start > offset,
		    ("%s: mbuf %p start %d offset %d proto %d", __func__, m,
		    *start, offset, *proto));
		break;
#endif
	default:
		return (EINVAL);
	}

#if 0 /* XXX TSO */
	if (m->m_pkthdr.csum_flags & CSUM_TSO) {
		struct tcphdr *tcp;

		if (__predict_false(*proto != IPPROTO_TCP)) {
			/* Likely failed to correctly parse the mbuf. */
			return (EINVAL);
		}

		if (m->m_len < *start + sizeof(struct tcphdr)) {
			m = m_pullup(m, *start + sizeof(struct tcphdr));
			if (m == NULL)
				return (EINVAL);
		}

		tcp = (struct tcphdr *)(mtod(m, uint8_t *) + *start);
		*start += (tcp->th_off << 2);

		txq->vxtxq_stats.vmtxs_tso++;
	} else
#endif
		txq->vxtxq_stats.vmtxs_csum++;

	return (0);
}

static int
vmxnet3_txq_load_mbuf(struct vmxnet3_txqueue *txq, struct mbuf **m0,
    bus_dmamap_t dmap, bus_dma_segment_t segs[], int *nsegs)
{
	struct vmxnet3_txring *txr;
	struct mbuf *m;
	bus_dma_tag_t tag;
	int error;

	txr = &txq->vxtxq_cmd_ring;
	m = *m0;
	tag = txr->vxtxr_txtag;

	error = bus_dmamap_load_mbuf_segment(tag, dmap, m, segs, 1, nsegs,
	    BUS_DMA_NOWAIT);
	if (error == 0 || error != EFBIG)
		return (error);

	m = m_defrag(m, M_NOWAIT);
	if (m != NULL) {
		*m0 = m;
		error = bus_dmamap_load_mbuf_segment(tag, dmap, m, segs,
		    1, nsegs, BUS_DMA_NOWAIT);
	} else
		error = ENOBUFS;

	if (error) {
		m_freem(*m0);
		*m0 = NULL;
		txq->vxtxq_sc->vmx_stats.vmst_defrag_failed++;
	} else
		txq->vxtxq_sc->vmx_stats.vmst_defragged++;

	return (error);
}

static void
vmxnet3_txq_unload_mbuf(struct vmxnet3_txqueue *txq, bus_dmamap_t dmap)
{
	struct vmxnet3_txring *txr;

	txr = &txq->vxtxq_cmd_ring;
	bus_dmamap_unload(txr->vxtxr_txtag, dmap);
}

static int
vmxnet3_txq_encap(struct vmxnet3_txqueue *txq, struct mbuf **m0)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txring *txr;
	struct vmxnet3_txdesc *txd, *sop;
	struct mbuf *m;
	bus_dmamap_t dmap;
	bus_dma_segment_t segs[VMXNET3_TX_MAXSEGS];
	int i, gen, nsegs, etype, proto, start, error;

	sc = txq->vxtxq_sc;
	start = 0;
	txd = NULL;
	txr = &txq->vxtxq_cmd_ring;
	dmap = txr->vxtxr_txbuf[txr->vxtxr_head].vtxb_dmamap;

	error = vmxnet3_txq_load_mbuf(txq, m0, dmap, segs, &nsegs);
	if (error)
		return (error);

	m = *m0;
	M_ASSERTPKTHDR(m);
	KASSERT(nsegs <= VMXNET3_TX_MAXSEGS,
	    ("%s: mbuf %p with too many segments %d", __func__, m, nsegs));

	if (VMXNET3_TXRING_AVAIL(txr) < nsegs) {
		txq->vxtxq_stats.vmtxs_full++;
		vmxnet3_txq_unload_mbuf(txq, dmap);
		return (ENOSPC);
	} else if (m->m_pkthdr.csum_flags & VMXNET3_CSUM_ALL_OFFLOAD) {
		error = vmxnet3_txq_offload_ctx(txq, m, &etype, &proto, &start);
		if (error) {
			txq->vxtxq_stats.vmtxs_offload_failed++;
			vmxnet3_txq_unload_mbuf(txq, dmap);
			m_freem(m);
			*m0 = NULL;
			return (error);
		}
	}

	txr->vxtxr_txbuf[txr->vxtxr_head].vtxb_m = m;
	sop = &txr->vxtxr_txd[txr->vxtxr_head];
	gen = txr->vxtxr_gen ^ 1;	/* Owned by cpu (yet) */

	for (i = 0; i < nsegs; i++) {
		txd = &txr->vxtxr_txd[txr->vxtxr_head];

		txd->addr = segs[i].ds_addr;
		txd->len = segs[i].ds_len;
		txd->gen = gen;
		txd->dtype = 0;
		txd->offload_mode = VMXNET3_OM_NONE;
		txd->offload_pos = 0;
		txd->hlen = 0;
		txd->eop = 0;
		txd->compreq = 0;
		txd->vtag_mode = 0;
		txd->vtag = 0;

		if (++txr->vxtxr_head == txr->vxtxr_ndesc) {
			txr->vxtxr_head = 0;
			txr->vxtxr_gen ^= 1;
		}
		gen = txr->vxtxr_gen;
	}
	txd->eop = 1;
	txd->compreq = 1;

	if (m->m_flags & M_VLANTAG) {
		sop->vtag_mode = 1;
		sop->vtag = m->m_pkthdr.ether_vlantag;
	}


#if 0 /* XXX TSO */
	if (m->m_pkthdr.csum_flags & CSUM_TSO) {
		sop->offload_mode = VMXNET3_OM_TSO;
		sop->hlen = start;
		sop->offload_pos = m->m_pkthdr.tso_segsz;
	} else
#endif
	if (m->m_pkthdr.csum_flags & (VMXNET3_CSUM_OFFLOAD |
	    VMXNET3_CSUM_OFFLOAD_IPV6))	{
		sop->offload_mode = VMXNET3_OM_CSUM;
		sop->hlen = start;
		sop->offload_pos = start + m->m_pkthdr.csum_data;
	}

	/* Finally, change the ownership. */
	vmxnet3_barrier(sc, VMXNET3_BARRIER_WR);
	sop->gen ^= 1;

	txq->vxtxq_ts->npending += nsegs;
	if (txq->vxtxq_ts->npending >= txq->vxtxq_ts->intr_threshold) {
		txq->vxtxq_ts->npending = 0;
		vmxnet3_write_bar0(sc, VMXNET3_BAR0_TXH(txq->vxtxq_id),
		    txr->vxtxr_head);
	}

	return (0);
}

#ifdef VMXNET3_LEGACY_TX

static void
vmxnet3_start_locked(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;
	struct vmxnet3_txring *txr;
	struct mbuf *m_head;
	int tx, avail;

	sc = ifp->if_softc;
	txq = &sc->vmx_txq[0];
	txr = &txq->vxtxq_cmd_ring;
	tx = 0;

	VMXNET3_TXQ_LOCK_ASSERT(txq);

	if ((ifp->if_flags & IFF_RUNNING) == 0 ||
	    sc->vmx_link_active == 0)
		return;

	while (!ifq_is_empty(&ifp->if_snd)) {
		if ((avail = VMXNET3_TXRING_AVAIL(txr)) < 2)
			break;

		m_head = ifq_dequeue(&ifp->if_snd);
		if (m_head == NULL)
			break;

		/* Assume worse case if this mbuf is the head of a chain. */
		if (m_head->m_next != NULL && avail < VMXNET3_TX_MAXSEGS) {
			ifq_prepend(&ifp->if_snd, m_head);
			break;
		}

		if (vmxnet3_txq_encap(txq, &m_head) != 0) {
			if (m_head != NULL)
				ifq_prepend(&ifp->if_snd, m_head);
			break;
		}

		tx++;
		ETHER_BPF_MTAP(ifp, m_head);
	}

	if (tx > 0)
		txq->vxtxq_watchdog = VMXNET3_WATCHDOG_TIMEOUT;
}

static void
vmxnet3_start(struct ifnet *ifp, struct ifaltq_subque *ifsq)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;

	sc = ifp->if_softc;
	txq = &sc->vmx_txq[0];

	VMXNET3_TXQ_LOCK(txq);
	vmxnet3_start_locked(ifp);
	VMXNET3_TXQ_UNLOCK(txq);
}

#else /* !VMXNET3_LEGACY_TX */

static int
vmxnet3_txq_mq_start_locked(struct vmxnet3_txqueue *txq, struct mbuf *m)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txring *txr;
	struct buf_ring *br;
	struct ifnet *ifp;
	int tx, avail, error;

	sc = txq->vxtxq_sc;
	br = txq->vxtxq_br;
	ifp = sc->vmx_ifp;
	txr = &txq->vxtxq_cmd_ring;
	tx = 0;
	error = 0;

	VMXNET3_TXQ_LOCK_ASSERT(txq);

	if ((ifp->if_flags & IFF_RUNNING) == 0 ||
	    sc->vmx_link_active == 0) {
		if (m != NULL)
			error = drbr_enqueue(ifp, br, m);
		return (error);
	}

	if (m != NULL) {
		error = drbr_enqueue(ifp, br, m);
		if (error)
			return (error);
	}

	while ((avail = VMXNET3_TXRING_AVAIL(txr)) >= 2) {
		m = drbr_peek(ifp, br);
		if (m == NULL)
			break;

		/* Assume worse case if this mbuf is the head of a chain. */
		if (m->m_next != NULL && avail < VMXNET3_TX_MAXSEGS) {
			drbr_putback(ifp, br, m);
			break;
		}

		if (vmxnet3_txq_encap(txq, &m) != 0) {
			if (m != NULL)
				drbr_putback(ifp, br, m);
			else
				drbr_advance(ifp, br);
			break;
		}
		drbr_advance(ifp, br);

		tx++;
		ETHER_BPF_MTAP(ifp, m);
	}

	if (tx > 0)
		txq->vxtxq_watchdog = VMXNET3_WATCHDOG_TIMEOUT;

	return (0);
}

static int
vmxnet3_txq_mq_start(struct ifnet *ifp, struct mbuf *m)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;
	int i, ntxq, error;

	sc = ifp->if_softc;
	ntxq = sc->vmx_ntxqueues;

	/* check if flowid is set */
	if (M_HASHTYPE_GET(m) != M_HASHTYPE_NONE)
		i = m->m_pkthdr.flowid % ntxq;
	else
		i = curcpu % ntxq;

	txq = &sc->vmx_txq[i];

	if (VMXNET3_TXQ_TRYLOCK(txq) != 0) {
		error = vmxnet3_txq_mq_start_locked(txq, m);
		VMXNET3_TXQ_UNLOCK(txq);
	} else {
		error = drbr_enqueue(ifp, txq->vxtxq_br, m);
		taskqueue_enqueue(sc->vmx_tq, &txq->vxtxq_defrtask);
	}

	return (error);
}

static void
vmxnet3_txq_tq_deferred(void *xtxq, int pending)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;

	txq = xtxq;
	sc = txq->vxtxq_sc;

	VMXNET3_TXQ_LOCK(txq);
	if (!drbr_empty(sc->vmx_ifp, txq->vxtxq_br))
		vmxnet3_txq_mq_start_locked(txq, NULL);
	VMXNET3_TXQ_UNLOCK(txq);
}

#endif /* VMXNET3_LEGACY_TX */

static void
vmxnet3_txq_start(struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;

	sc = txq->vxtxq_sc;
	ifp = sc->vmx_ifp;

#ifdef VMXNET3_LEGACY_TX
	if (!ifq_is_empty(&ifp->if_snd))
		vmxnet3_start_locked(ifp);
#else
	if (!drbr_empty(ifp, txq->vxtxq_br))
		vmxnet3_txq_mq_start_locked(txq, NULL);
#endif
}

static void
vmxnet3_tx_start_all(struct vmxnet3_softc *sc)
{
	struct vmxnet3_txqueue *txq;
	int i;

	VMXNET3_CORE_LOCK_ASSERT(sc);

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_txq[i];

		VMXNET3_TXQ_LOCK(txq);
		vmxnet3_txq_start(txq);
		VMXNET3_TXQ_UNLOCK(txq);
	}
}

static void
vmxnet3_update_vlan_filter(struct vmxnet3_softc *sc, int add, uint16_t tag)
{
	struct ifnet *ifp;
	int idx, bit;

	ifp = sc->vmx_ifp;
	idx = (tag >> 5) & 0x7F;
	bit = tag & 0x1F;

	if (tag == 0 || tag > 4095)
		return;

	VMXNET3_CORE_LOCK(sc);

	/* Update our private VLAN bitvector. */
	if (add)
		sc->vmx_vlan_filter[idx] |= (1 << bit);
	else
		sc->vmx_vlan_filter[idx] &= ~(1 << bit);

#if 0 /* VLAN_HWFILTER */
	if (ifp->if_capenable & IFCAP_VLAN_HWFILTER) {
		if (add)
			sc->vmx_ds->vlan_filter[idx] |= (1 << bit);
		else
			sc->vmx_ds->vlan_filter[idx] &= ~(1 << bit);
		vmxnet3_write_cmd(sc, VMXNET3_CMD_VLAN_FILTER);
	}
#endif

	VMXNET3_CORE_UNLOCK(sc);
}

static void
vmxnet3_register_vlan(void *arg, struct ifnet *ifp, uint16_t tag)
{

	if (ifp->if_softc == arg)
		vmxnet3_update_vlan_filter(arg, 1, tag);
}

static void
vmxnet3_unregister_vlan(void *arg, struct ifnet *ifp, uint16_t tag)
{

	if (ifp->if_softc == arg)
		vmxnet3_update_vlan_filter(arg, 0, tag);
}

static void
vmxnet3_set_rxfilter(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	struct vmxnet3_driver_shared *ds;
	struct ifmultiaddr *ifma;
	u_int mode;

	ifp = sc->vmx_ifp;
	ds = sc->vmx_ds;

	mode = VMXNET3_RXMODE_UCAST | VMXNET3_RXMODE_BCAST;
	if (ifp->if_flags & IFF_PROMISC)
		mode |= VMXNET3_RXMODE_PROMISC;
	if (ifp->if_flags & IFF_ALLMULTI)
		mode |= VMXNET3_RXMODE_ALLMULTI;
	else {
		int cnt = 0, overflow = 0;

		TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
			if (ifma->ifma_addr->sa_family != AF_LINK)
				continue;
			else if (cnt == VMXNET3_MULTICAST_MAX) {
				overflow = 1;
				break;
			}

			bcopy(LLADDR((struct sockaddr_dl *)ifma->ifma_addr),
			   &sc->vmx_mcast[cnt*ETHER_ADDR_LEN], ETHER_ADDR_LEN);
			cnt++;
		}

		if (overflow != 0) {
			cnt = 0;
			mode |= VMXNET3_RXMODE_ALLMULTI;
		} else if (cnt > 0)
			mode |= VMXNET3_RXMODE_MCAST;
		ds->mcast_tablelen = cnt * ETHER_ADDR_LEN;
	}

	ds->rxmode = mode;

	vmxnet3_write_cmd(sc, VMXNET3_CMD_SET_FILTER);
	vmxnet3_write_cmd(sc, VMXNET3_CMD_SET_RXMODE);
}

static int
vmxnet3_change_mtu(struct vmxnet3_softc *sc, int mtu)
{
	struct ifnet *ifp;

	ifp = sc->vmx_ifp;

	if (mtu < VMXNET3_MIN_MTU || mtu > VMXNET3_MAX_MTU)
		return (EINVAL);

	ifp->if_mtu = mtu;

	if (ifp->if_flags & IFF_RUNNING) {
		ifp->if_flags &= ~IFF_RUNNING;
		vmxnet3_init_locked(sc);
	}

	return (0);
}

static int
vmxnet3_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data, struct ucred *cred)
{
	struct vmxnet3_softc *sc;
	struct ifreq *ifr;
	int reinit, mask, error;

	sc = ifp->if_softc;
	ifr = (struct ifreq *) data;
	error = 0;

	switch (cmd) {
	case SIOCSIFMTU:
		if (ifp->if_mtu != ifr->ifr_mtu) {
			VMXNET3_CORE_LOCK(sc);
			error = vmxnet3_change_mtu(sc, ifr->ifr_mtu);
			VMXNET3_CORE_UNLOCK(sc);
		}
		break;

	case SIOCSIFFLAGS:
		VMXNET3_CORE_LOCK(sc);
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_flags & IFF_RUNNING)) {
				if ((ifp->if_flags ^ sc->vmx_if_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) {
					vmxnet3_set_rxfilter(sc);
				}
			} else
				vmxnet3_init_locked(sc);
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				vmxnet3_stop(sc);
		}
		sc->vmx_if_flags = ifp->if_flags;
		VMXNET3_CORE_UNLOCK(sc);
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		VMXNET3_CORE_LOCK(sc);
		if (ifp->if_flags & IFF_RUNNING)
			vmxnet3_set_rxfilter(sc);
		VMXNET3_CORE_UNLOCK(sc);
		break;

	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->vmx_media, cmd);
		break;

	case SIOCSIFCAP:
		VMXNET3_CORE_LOCK(sc);
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;

		if (mask & IFCAP_TXCSUM)
			ifp->if_capenable ^= IFCAP_TXCSUM;
		if (mask & IFCAP_TXCSUM_IPV6)
			ifp->if_capenable ^= IFCAP_TXCSUM_IPV6;
#if 0 /* XXX TSO */
		if (mask & IFCAP_TSO4)
			ifp->if_capenable ^= IFCAP_TSO4;
		if (mask & IFCAP_TSO6)
			ifp->if_capenable ^= IFCAP_TSO6;
#endif

		if (mask & (IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6 | /* IFCAP_LRO | */
		    IFCAP_VLAN_HWTAGGING /* | IFCAP_VLAN_HWFILTER */)) {
			/* Changing these features requires us to reinit. */
			reinit = 1;

			if (mask & IFCAP_RXCSUM)
				ifp->if_capenable ^= IFCAP_RXCSUM;
			if (mask & IFCAP_RXCSUM_IPV6)
				ifp->if_capenable ^= IFCAP_RXCSUM_IPV6;
#if 0 /* XXX LRO */
			if (mask & IFCAP_LRO)
				ifp->if_capenable ^= IFCAP_LRO;
#endif
			if (mask & IFCAP_VLAN_HWTAGGING)
				ifp->if_capenable ^= IFCAP_VLAN_HWTAGGING;
#if 0 /* XXX VLAN_HWFILTER */
			if (mask & IFCAP_VLAN_HWFILTER)
				ifp->if_capenable ^= IFCAP_VLAN_HWFILTER;
#endif
		} else
			reinit = 0;

#if 0 /* XXX TSO */
		if (mask & IFCAP_VLAN_HWTSO)
			ifp->if_capenable ^= IFCAP_VLAN_HWTSO;
#endif

		if (reinit && (ifp->if_flags & IFF_RUNNING)) {
			ifp->if_flags &= ~IFF_RUNNING;
			vmxnet3_init_locked(sc);
		} else {
			vmxnet3_init_hwassist(sc);
		}

		VMXNET3_CORE_UNLOCK(sc);
#if 0 /* XXX */
		VLAN_CAPABILITIES(ifp);
#endif
		break;

	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	VMXNET3_CORE_LOCK_ASSERT_NOTOWNED(sc);

	return (error);
}

#ifndef VMXNET3_LEGACY_TX
static void
vmxnet3_qflush(struct ifnet *ifp)
{
	struct vmxnet3_softc *sc;
	struct vmxnet3_txqueue *txq;
	struct mbuf *m;
	int i;

	sc = ifp->if_softc;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		txq = &sc->vmx_txq[i];

		VMXNET3_TXQ_LOCK(txq);
		while ((m = buf_ring_dequeue_sc(txq->vxtxq_br)) != NULL)
			m_freem(m);
		VMXNET3_TXQ_UNLOCK(txq);
	}

	if_qflush(ifp);
}
#endif

static int
vmxnet3_watchdog(struct vmxnet3_txqueue *txq)
{
	struct vmxnet3_softc *sc;

	sc = txq->vxtxq_sc;

	VMXNET3_TXQ_LOCK(txq);
	if (txq->vxtxq_watchdog == 0 || --txq->vxtxq_watchdog) {
		VMXNET3_TXQ_UNLOCK(txq);
		return (0);
	}
	VMXNET3_TXQ_UNLOCK(txq);

	if_printf(sc->vmx_ifp, "watchdog timeout on queue %d\n",
	    txq->vxtxq_id);
	return (1);
}

static void
vmxnet3_refresh_host_stats(struct vmxnet3_softc *sc)
{

	vmxnet3_write_cmd(sc, VMXNET3_CMD_GET_STATS);
}

static void
vmxnet3_txq_accum_stats(struct vmxnet3_txqueue *txq,
    struct vmxnet3_txq_stats *accum)
{
	struct vmxnet3_txq_stats *st;

	st = &txq->vxtxq_stats;

	accum->vmtxs_opackets += st->vmtxs_opackets;
	accum->vmtxs_obytes += st->vmtxs_obytes;
	accum->vmtxs_omcasts += st->vmtxs_omcasts;
	accum->vmtxs_csum += st->vmtxs_csum;
	accum->vmtxs_tso += st->vmtxs_tso;
	accum->vmtxs_full += st->vmtxs_full;
	accum->vmtxs_offload_failed += st->vmtxs_offload_failed;
}

static void
vmxnet3_rxq_accum_stats(struct vmxnet3_rxqueue *rxq,
    struct vmxnet3_rxq_stats *accum)
{
	struct vmxnet3_rxq_stats *st;

	st = &rxq->vxrxq_stats;

	accum->vmrxs_ipackets += st->vmrxs_ipackets;
	accum->vmrxs_ibytes += st->vmrxs_ibytes;
	accum->vmrxs_iqdrops += st->vmrxs_iqdrops;
	accum->vmrxs_ierrors += st->vmrxs_ierrors;
}

static void
vmxnet3_accumulate_stats(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	struct vmxnet3_statistics *st;
	struct vmxnet3_txq_stats txaccum;
	struct vmxnet3_rxq_stats rxaccum;
	int i;

	ifp = sc->vmx_ifp;
	st = &sc->vmx_stats;

	bzero(&txaccum, sizeof(struct vmxnet3_txq_stats));
	bzero(&rxaccum, sizeof(struct vmxnet3_rxq_stats));

	for (i = 0; i < sc->vmx_ntxqueues; i++)
		vmxnet3_txq_accum_stats(&sc->vmx_txq[i], &txaccum);
	for (i = 0; i < sc->vmx_nrxqueues; i++)
		vmxnet3_rxq_accum_stats(&sc->vmx_rxq[i], &rxaccum);

	/*
	 * With the exception of if_ierrors, these ifnet statistics are
	 * only updated in the driver, so just set them to our accumulated
	 * values. if_ierrors is updated in ether_input() for malformed
	 * frames that we should have already discarded.
	 */
	ifp->if_ipackets = rxaccum.vmrxs_ipackets;
	ifp->if_iqdrops = rxaccum.vmrxs_iqdrops;
	ifp->if_ierrors = rxaccum.vmrxs_ierrors;
	ifp->if_opackets = txaccum.vmtxs_opackets;
#ifndef VMXNET3_LEGACY_TX
	ifp->if_obytes = txaccum.vmtxs_obytes;
	ifp->if_omcasts = txaccum.vmtxs_omcasts;
#endif
}

static void
vmxnet3_tick(void *xsc)
{
	struct vmxnet3_softc *sc;
	struct ifnet *ifp;
	int i, timedout;

	sc = xsc;
	ifp = sc->vmx_ifp;
	timedout = 0;

	VMXNET3_CORE_LOCK_ASSERT(sc);

	vmxnet3_accumulate_stats(sc);
	vmxnet3_refresh_host_stats(sc);

	for (i = 0; i < sc->vmx_ntxqueues; i++)
		timedout |= vmxnet3_watchdog(&sc->vmx_txq[i]);

	if (timedout != 0) {
		ifp->if_flags &= ~IFF_RUNNING;
		vmxnet3_init_locked(sc);
	} else
		callout_reset(&sc->vmx_tick, hz, vmxnet3_tick, sc);
}

static int
vmxnet3_link_is_up(struct vmxnet3_softc *sc)
{
	uint32_t status;

	/* Also update the link speed while here. */
	status = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_LINK);
	sc->vmx_link_speed = status >> 16;
	return !!(status & 0x1);
}

static void
vmxnet3_link_status(struct vmxnet3_softc *sc)
{
	struct ifnet *ifp;
	int link;

	ifp = sc->vmx_ifp;
	link = vmxnet3_link_is_up(sc);

	if (link != 0 && sc->vmx_link_active == 0) {
		sc->vmx_link_active = 1;
		ifp->if_link_state = LINK_STATE_UP;
		if_link_state_change(ifp);
	} else if (link == 0 && sc->vmx_link_active != 0) {
		sc->vmx_link_active = 0;
		ifp->if_link_state = LINK_STATE_DOWN;
		if_link_state_change(ifp);
	}
}

static void
vmxnet3_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct vmxnet3_softc *sc;

	sc = ifp->if_softc;

	ifmr->ifm_active = IFM_ETHER | IFM_AUTO;
	ifmr->ifm_status = IFM_AVALID;

	VMXNET3_CORE_LOCK(sc);
	if (vmxnet3_link_is_up(sc) != 0)
		ifmr->ifm_status |= IFM_ACTIVE;
	else
		ifmr->ifm_status |= IFM_NONE;
	VMXNET3_CORE_UNLOCK(sc);
}

static int
vmxnet3_media_change(struct ifnet *ifp)
{

	/* Ignore. */
	return (0);
}

static void
vmxnet3_set_lladdr(struct vmxnet3_softc *sc)
{
	uint32_t ml, mh;

	ml  = sc->vmx_lladdr[0];
	ml |= sc->vmx_lladdr[1] << 8;
	ml |= sc->vmx_lladdr[2] << 16;
	ml |= sc->vmx_lladdr[3] << 24;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_MACL, ml);

	mh  = sc->vmx_lladdr[4];
	mh |= sc->vmx_lladdr[5] << 8;
	vmxnet3_write_bar1(sc, VMXNET3_BAR1_MACH, mh);
}

static void
vmxnet3_get_lladdr(struct vmxnet3_softc *sc)
{
	uint32_t ml, mh;

	ml = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_MACL);
	mh = vmxnet3_read_cmd(sc, VMXNET3_CMD_GET_MACH);

	sc->vmx_lladdr[0] = ml;
	sc->vmx_lladdr[1] = ml >> 8;
	sc->vmx_lladdr[2] = ml >> 16;
	sc->vmx_lladdr[3] = ml >> 24;
	sc->vmx_lladdr[4] = mh;
	sc->vmx_lladdr[5] = mh >> 8;
}

static void
vmxnet3_setup_txq_sysctl(struct vmxnet3_txqueue *txq,
    struct sysctl_ctx_list *ctx, struct sysctl_oid_list *child)
{
	struct sysctl_oid *node, *txsnode;
	struct sysctl_oid_list *list, *txslist;
	struct vmxnet3_txq_stats *stats;
	struct UPT1_TxStats *txstats;
	char namebuf[16];

	stats = &txq->vxtxq_stats;
	txstats = &txq->vxtxq_ts->stats;

	ksnprintf(namebuf, sizeof(namebuf), "txq%d", txq->vxtxq_id);
	node = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, namebuf, CTLFLAG_RD,
	    NULL, "Transmit Queue");
	txq->vxtxq_sysctl = list = SYSCTL_CHILDREN(node);

	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "opackets", CTLFLAG_RD,
	    &stats->vmtxs_opackets, 0, "Transmit packets");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "obytes", CTLFLAG_RD,
	    &stats->vmtxs_obytes, 0, "Transmit bytes");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "omcasts", CTLFLAG_RD,
	    &stats->vmtxs_omcasts, 0, "Transmit multicasts");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "csum", CTLFLAG_RD,
	    &stats->vmtxs_csum, 0, "Transmit checksum offloaded");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "tso", CTLFLAG_RD,
	    &stats->vmtxs_tso, 0, "Transmit TCP segmentation offloaded");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "ringfull", CTLFLAG_RD,
	    &stats->vmtxs_full, 0, "Transmit ring full");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "offload_failed", CTLFLAG_RD,
	    &stats->vmtxs_offload_failed, 0, "Transmit checksum offload failed");

	/*
	 * Add statistics reported by the host. These are updated once
	 * per second.
	 */
	txsnode = SYSCTL_ADD_NODE(ctx, list, OID_AUTO, "hstats", CTLFLAG_RD,
	    NULL, "Host Statistics");
	txslist = SYSCTL_CHILDREN(txsnode);
#if 0 /* XXX TSO */
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "tso_packets", CTLFLAG_RD,
	    &txstats->TSO_packets, 0, "TSO packets");
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "tso_bytes", CTLFLAG_RD,
	    &txstats->TSO_bytes, 0, "TSO bytes");
#endif
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "ucast_packets", CTLFLAG_RD,
	    &txstats->ucast_packets, 0, "Unicast packets");
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "unicast_bytes", CTLFLAG_RD,
	    &txstats->ucast_bytes, 0, "Unicast bytes");
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "mcast_packets", CTLFLAG_RD,
	    &txstats->mcast_packets, 0, "Multicast packets");
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "mcast_bytes", CTLFLAG_RD,
	    &txstats->mcast_bytes, 0, "Multicast bytes");
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "error", CTLFLAG_RD,
	    &txstats->error, 0, "Errors");
	SYSCTL_ADD_UQUAD(ctx, txslist, OID_AUTO, "discard", CTLFLAG_RD,
	    &txstats->discard, 0, "Discards");
}

static void
vmxnet3_setup_rxq_sysctl(struct vmxnet3_rxqueue *rxq,
    struct sysctl_ctx_list *ctx, struct sysctl_oid_list *child)
{
	struct sysctl_oid *node, *rxsnode;
	struct sysctl_oid_list *list, *rxslist;
	struct vmxnet3_rxq_stats *stats;
	struct UPT1_RxStats *rxstats;
	char namebuf[16];

	stats = &rxq->vxrxq_stats;
	rxstats = &rxq->vxrxq_rs->stats;

	ksnprintf(namebuf, sizeof(namebuf), "rxq%d", rxq->vxrxq_id);
	node = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, namebuf, CTLFLAG_RD,
	    NULL, "Receive Queue");
	rxq->vxrxq_sysctl = list = SYSCTL_CHILDREN(node);

	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "ipackets", CTLFLAG_RD,
	    &stats->vmrxs_ipackets, 0, "Receive packets");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "ibytes", CTLFLAG_RD,
	    &stats->vmrxs_ibytes, 0, "Receive bytes");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "iqdrops", CTLFLAG_RD,
	    &stats->vmrxs_iqdrops, 0, "Receive drops");
	SYSCTL_ADD_UQUAD(ctx, list, OID_AUTO, "ierrors", CTLFLAG_RD,
	    &stats->vmrxs_ierrors, 0, "Receive errors");

	/*
	 * Add statistics reported by the host. These are updated once
	 * per second.
	 */
	rxsnode = SYSCTL_ADD_NODE(ctx, list, OID_AUTO, "hstats", CTLFLAG_RD,
	    NULL, "Host Statistics");
	rxslist = SYSCTL_CHILDREN(rxsnode);
#if 0 /* XXX LRO */
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "lro_packets", CTLFLAG_RD,
	    &rxstats->LRO_packets, 0, "LRO packets");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "lro_bytes", CTLFLAG_RD,
	    &rxstats->LRO_bytes, 0, "LRO bytes");
#endif
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "ucast_packets", CTLFLAG_RD,
	    &rxstats->ucast_packets, 0, "Unicast packets");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "unicast_bytes", CTLFLAG_RD,
	    &rxstats->ucast_bytes, 0, "Unicast bytes");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "mcast_packets", CTLFLAG_RD,
	    &rxstats->mcast_packets, 0, "Multicast packets");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "mcast_bytes", CTLFLAG_RD,
	    &rxstats->mcast_bytes, 0, "Multicast bytes");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "bcast_packets", CTLFLAG_RD,
	    &rxstats->bcast_packets, 0, "Broadcast packets");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "bcast_bytes", CTLFLAG_RD,
	    &rxstats->bcast_bytes, 0, "Broadcast bytes");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "nobuffer", CTLFLAG_RD,
	    &rxstats->nobuffer, 0, "No buffer");
	SYSCTL_ADD_UQUAD(ctx, rxslist, OID_AUTO, "error", CTLFLAG_RD,
	    &rxstats->error, 0, "Errors");
}

static void
vmxnet3_setup_debug_sysctl(struct vmxnet3_softc *sc,
    struct sysctl_ctx_list *ctx, struct sysctl_oid_list *child)
{
	struct sysctl_oid *node;
	struct sysctl_oid_list *list;
	int i;

	for (i = 0; i < sc->vmx_ntxqueues; i++) {
		struct vmxnet3_txqueue *txq = &sc->vmx_txq[i];

		node = SYSCTL_ADD_NODE(ctx, txq->vxtxq_sysctl, OID_AUTO,
		    "debug", CTLFLAG_RD, NULL, "");
		list = SYSCTL_CHILDREN(node);

		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "cmd_head", CTLFLAG_RD,
		    &txq->vxtxq_cmd_ring.vxtxr_head, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "cmd_next", CTLFLAG_RD,
		    &txq->vxtxq_cmd_ring.vxtxr_next, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "cmd_ndesc", CTLFLAG_RD,
		    &txq->vxtxq_cmd_ring.vxtxr_ndesc, 0, "");
		SYSCTL_ADD_INT(ctx, list, OID_AUTO, "cmd_gen", CTLFLAG_RD,
		    &txq->vxtxq_cmd_ring.vxtxr_gen, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "comp_next", CTLFLAG_RD,
		    &txq->vxtxq_comp_ring.vxcr_next, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "comp_ndesc", CTLFLAG_RD,
		    &txq->vxtxq_comp_ring.vxcr_ndesc, 0,"");
		SYSCTL_ADD_INT(ctx, list, OID_AUTO, "comp_gen", CTLFLAG_RD,
		    &txq->vxtxq_comp_ring.vxcr_gen, 0, "");
	}

	for (i = 0; i < sc->vmx_nrxqueues; i++) {
		struct vmxnet3_rxqueue *rxq = &sc->vmx_rxq[i];

		node = SYSCTL_ADD_NODE(ctx, rxq->vxrxq_sysctl, OID_AUTO,
		    "debug", CTLFLAG_RD, NULL, "");
		list = SYSCTL_CHILDREN(node);

		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "cmd0_fill", CTLFLAG_RD,
		    &rxq->vxrxq_cmd_ring[0].vxrxr_fill, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "cmd0_ndesc", CTLFLAG_RD,
		    &rxq->vxrxq_cmd_ring[0].vxrxr_ndesc, 0, "");
		SYSCTL_ADD_INT(ctx, list, OID_AUTO, "cmd0_gen", CTLFLAG_RD,
		    &rxq->vxrxq_cmd_ring[0].vxrxr_gen, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "cmd1_fill", CTLFLAG_RD,
		    &rxq->vxrxq_cmd_ring[1].vxrxr_fill, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "cmd1_ndesc", CTLFLAG_RD,
		    &rxq->vxrxq_cmd_ring[1].vxrxr_ndesc, 0, "");
		SYSCTL_ADD_INT(ctx, list, OID_AUTO, "cmd1_gen", CTLFLAG_RD,
		    &rxq->vxrxq_cmd_ring[1].vxrxr_gen, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "comp_next", CTLFLAG_RD,
		    &rxq->vxrxq_comp_ring.vxcr_next, 0, "");
		SYSCTL_ADD_UINT(ctx, list, OID_AUTO, "comp_ndesc", CTLFLAG_RD,
		    &rxq->vxrxq_comp_ring.vxcr_ndesc, 0,"");
		SYSCTL_ADD_INT(ctx, list, OID_AUTO, "comp_gen", CTLFLAG_RD,
		    &rxq->vxrxq_comp_ring.vxcr_gen, 0, "");
	}
}

static void
vmxnet3_setup_queue_sysctl(struct vmxnet3_softc *sc,
    struct sysctl_ctx_list *ctx, struct sysctl_oid_list *child)
{
	int i;

	for (i = 0; i < sc->vmx_ntxqueues; i++)
		vmxnet3_setup_txq_sysctl(&sc->vmx_txq[i], ctx, child);
	for (i = 0; i < sc->vmx_nrxqueues; i++)
		vmxnet3_setup_rxq_sysctl(&sc->vmx_rxq[i], ctx, child);

	vmxnet3_setup_debug_sysctl(sc, ctx, child);
}

static void
vmxnet3_setup_sysctl(struct vmxnet3_softc *sc)
{
	device_t dev;
	struct vmxnet3_statistics *stats;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	struct sysctl_oid_list *child;

	dev = sc->vmx_dev;
	ctx = device_get_sysctl_ctx(dev);
	tree = device_get_sysctl_tree(dev);
	child = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "max_ntxqueues", CTLFLAG_RD,
	    &sc->vmx_max_ntxqueues, 0, "Maximum number of Tx queues");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "max_nrxqueues", CTLFLAG_RD,
	    &sc->vmx_max_nrxqueues, 0, "Maximum number of Rx queues");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "ntxqueues", CTLFLAG_RD,
	    &sc->vmx_ntxqueues, 0, "Number of Tx queues");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "nrxqueues", CTLFLAG_RD,
	    &sc->vmx_nrxqueues, 0, "Number of Rx queues");

	stats = &sc->vmx_stats;
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "defragged", CTLFLAG_RD,
	    &stats->vmst_defragged, 0, "Tx mbuf chains defragged");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "defrag_failed", CTLFLAG_RD,
	    &stats->vmst_defrag_failed, 0,
	    "Tx mbuf dropped because defrag failed");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "mgetcl_failed", CTLFLAG_RD,
	    &stats->vmst_mgetcl_failed, 0, "mbuf cluster allocation failed");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "mbuf_load_failed", CTLFLAG_RD,
	    &stats->vmst_mbuf_load_failed, 0, "mbuf load segments failed");

	vmxnet3_setup_queue_sysctl(sc, ctx, child);
}

static void
vmxnet3_write_bar0(struct vmxnet3_softc *sc, bus_size_t r, uint32_t v)
{

	bus_space_write_4(sc->vmx_iot0, sc->vmx_ioh0, r, v);
}

static uint32_t
vmxnet3_read_bar1(struct vmxnet3_softc *sc, bus_size_t r)
{

	return (bus_space_read_4(sc->vmx_iot1, sc->vmx_ioh1, r));
}

static void
vmxnet3_write_bar1(struct vmxnet3_softc *sc, bus_size_t r, uint32_t v)
{

	bus_space_write_4(sc->vmx_iot1, sc->vmx_ioh1, r, v);
}

static void
vmxnet3_write_cmd(struct vmxnet3_softc *sc, uint32_t cmd)
{

	vmxnet3_write_bar1(sc, VMXNET3_BAR1_CMD, cmd);
}

static uint32_t
vmxnet3_read_cmd(struct vmxnet3_softc *sc, uint32_t cmd)
{

	vmxnet3_write_cmd(sc, cmd);
	bus_space_barrier(sc->vmx_iot1, sc->vmx_ioh1, 0, 0,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	return (vmxnet3_read_bar1(sc, VMXNET3_BAR1_CMD));
}

static void
vmxnet3_enable_intr(struct vmxnet3_softc *sc, int irq)
{

	vmxnet3_write_bar0(sc, VMXNET3_BAR0_IMASK(irq), 0);
}

static void
vmxnet3_disable_intr(struct vmxnet3_softc *sc, int irq)
{

	vmxnet3_write_bar0(sc, VMXNET3_BAR0_IMASK(irq), 1);
}

static void
vmxnet3_enable_all_intrs(struct vmxnet3_softc *sc)
{
	int i;

	sc->vmx_ds->ictrl &= ~VMXNET3_ICTRL_DISABLE_ALL;
	for (i = 0; i < sc->vmx_nintrs; i++)
		vmxnet3_enable_intr(sc, i);
}

static void
vmxnet3_disable_all_intrs(struct vmxnet3_softc *sc)
{
	int i;

	sc->vmx_ds->ictrl |= VMXNET3_ICTRL_DISABLE_ALL;
	for (i = 0; i < sc->vmx_nintrs; i++)
		vmxnet3_disable_intr(sc, i);
}

static void
vmxnet3_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *baddr = arg;

	if (error == 0)
		*baddr = segs->ds_addr;
}

static int
vmxnet3_dma_malloc(struct vmxnet3_softc *sc, bus_size_t size, bus_size_t align,
    struct vmxnet3_dma_alloc *dma)
{
	device_t dev;
	int error;

	dev = sc->vmx_dev;
	bzero(dma, sizeof(struct vmxnet3_dma_alloc));

	error = bus_dma_tag_create(bus_get_dma_tag(dev),
	    align, 0,		/* alignment, bounds */
	    BUS_SPACE_MAXADDR,	/* lowaddr */
	    BUS_SPACE_MAXADDR,	/* highaddr */
	    size,		/* maxsize */
	    1,			/* nsegments */
	    size,		/* maxsegsize */
	    BUS_DMA_ALLOCNOW,	/* flags */
	    &dma->dma_tag);
	if (error) {
		device_printf(dev, "bus_dma_tag_create failed: %d\n", error);
		goto fail;
	}

	error = bus_dmamem_alloc(dma->dma_tag, (void **)&dma->dma_vaddr,
	    BUS_DMA_ZERO | BUS_DMA_NOWAIT, &dma->dma_map);
	if (error) {
		device_printf(dev, "bus_dmamem_alloc failed: %d\n", error);
		goto fail;
	}

	error = bus_dmamap_load(dma->dma_tag, dma->dma_map, dma->dma_vaddr,
	    size, vmxnet3_dmamap_cb, &dma->dma_paddr, BUS_DMA_NOWAIT);
	if (error) {
		device_printf(dev, "bus_dmamap_load failed: %d\n", error);
		goto fail;
	}

	dma->dma_size = size;

fail:
	if (error)
		vmxnet3_dma_free(sc, dma);

	return (error);
}

static void
vmxnet3_dma_free(struct vmxnet3_softc *sc, struct vmxnet3_dma_alloc *dma)
{

	if (dma->dma_tag != NULL) {
		if (dma->dma_paddr != 0) {
			bus_dmamap_sync(dma->dma_tag, dma->dma_map,
			    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(dma->dma_tag, dma->dma_map);
		}

		if (dma->dma_vaddr != NULL) {
			bus_dmamem_free(dma->dma_tag, dma->dma_vaddr,
			    dma->dma_map);
		}

		bus_dma_tag_destroy(dma->dma_tag);
	}
	bzero(dma, sizeof(struct vmxnet3_dma_alloc));
}

static int
vmxnet3_tunable_int(struct vmxnet3_softc *sc, const char *knob, int def)
{
	char path[64];

	ksnprintf(path, sizeof(path),
	    "hw.vmx.%d.%s", device_get_unit(sc->vmx_dev), knob);
	TUNABLE_INT_FETCH(path, &def);

	return (def);
}

#define mb()	__asm volatile("mfence" ::: "memory")
#define wmb()	__asm volatile("sfence" ::: "memory")
#define rmb()	__asm volatile("lfence" ::: "memory")

/*
 * Since this is a purely paravirtualized device, we do not have
 * to worry about DMA coherency. But at times, we must make sure
 * both the compiler and CPU do not reorder memory operations.
 */
static inline void
vmxnet3_barrier(struct vmxnet3_softc *sc, vmxnet3_barrier_t type)
{

	switch (type) {
	case VMXNET3_BARRIER_RD:
		rmb();
		break;
	case VMXNET3_BARRIER_WR:
		wmb();
		break;
	case VMXNET3_BARRIER_RDWR:
		mb();
		break;
	default:
		panic("%s: bad barrier type %d", __func__, type);
	}
}
