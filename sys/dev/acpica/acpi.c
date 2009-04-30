/*-
 * Copyright (c) 2000 Takanori Watanabe <takawata@jp.freebsd.org>
 * Copyright (c) 2000 Mitsuru IWASAKI <iwasaki@jp.freebsd.org>
 * Copyright (c) 2000, 2001 Michael Smith
 * Copyright (c) 2000 BSDi
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_acpi.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/ioccom.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <sys/ctype.h>
#include <sys/linker.h>
#include <sys/power.h>
#include <sys/sbuf.h>
#ifdef SMP
#include <sys/sched.h>
#endif
#include <sys/smp.h>
#include <sys/timetc.h>

#if defined(__i386__) || defined(__amd64__)
#include <machine/pci_cfgreg.h>
#endif
#include <machine/resource.h>
#include <machine/bus.h>
#include <sys/rman.h>
#include <isa/isavar.h>
#include <isa/pnpvar.h>

#include <contrib/dev/acpica/acpi.h>
#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>
#include <contrib/dev/acpica/achware.h>
#include <contrib/dev/acpica/acnamesp.h>

#include "pci_if.h"
#include <dev/pci/pcivar.h>
#include <dev/pci/pci_private.h>

#include <vm/vm_param.h>

MALLOC_DEFINE(M_ACPIDEV, "acpidev", "ACPI devices");

/* Hooks for the ACPI CA debugging infrastructure */
#define _COMPONENT	ACPI_BUS
ACPI_MODULE_NAME("ACPI")

static d_open_t		acpiopen;
static d_close_t	acpiclose;
static d_ioctl_t	acpiioctl;

static struct cdevsw acpi_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	acpiopen,
	.d_close =	acpiclose,
	.d_ioctl =	acpiioctl,
	.d_name =	"acpi",
};

/* Global mutex for locking access to the ACPI subsystem. */
struct mtx	acpi_mutex;

/* Bitmap of device quirks. */
int		acpi_quirks;

/* Supported sleep states. */
static BOOLEAN	acpi_sleep_states[ACPI_S_STATE_COUNT];

static int	acpi_modevent(struct module *mod, int event, void *junk);
static int	acpi_probe(device_t dev);
static int	acpi_attach(device_t dev);
static int	acpi_suspend(device_t dev);
static int	acpi_resume(device_t dev);
static int	acpi_shutdown(device_t dev);
static device_t	acpi_add_child(device_t bus, int order, const char *name,
			int unit);
static int	acpi_print_child(device_t bus, device_t child);
static void	acpi_probe_nomatch(device_t bus, device_t child);
static void	acpi_driver_added(device_t dev, driver_t *driver);
static int	acpi_read_ivar(device_t dev, device_t child, int index,
			uintptr_t *result);
static int	acpi_write_ivar(device_t dev, device_t child, int index,
			uintptr_t value);
static struct resource_list *acpi_get_rlist(device_t dev, device_t child);
static int	acpi_sysres_alloc(device_t dev);
static struct resource *acpi_alloc_resource(device_t bus, device_t child,
			int type, int *rid, u_long start, u_long end,
			u_long count, u_int flags);
static int	acpi_release_resource(device_t bus, device_t child, int type,
			int rid, struct resource *r);
static void	acpi_delete_resource(device_t bus, device_t child, int type,
		    int rid);
static uint32_t	acpi_isa_get_logicalid(device_t dev);
static int	acpi_isa_get_compatid(device_t dev, uint32_t *cids, int count);
static char	*acpi_device_id_probe(device_t bus, device_t dev, char **ids);
static ACPI_STATUS acpi_device_eval_obj(device_t bus, device_t dev,
		    ACPI_STRING pathname, ACPI_OBJECT_LIST *parameters,
		    ACPI_BUFFER *ret);
static int	acpi_device_pwr_for_sleep(device_t bus, device_t dev,
		    int *dstate);
static ACPI_STATUS acpi_device_scan_cb(ACPI_HANDLE h, UINT32 level,
		    void *context, void **retval);
static ACPI_STATUS acpi_device_scan_children(device_t bus, device_t dev,
		    int max_depth, acpi_scan_cb_t user_fn, void *arg);
static int	acpi_set_powerstate_method(device_t bus, device_t child,
		    int state);
static int	acpi_isa_pnp_probe(device_t bus, device_t child,
		    struct isa_pnp_id *ids);
static void	acpi_probe_children(device_t bus);
static void	acpi_probe_order(ACPI_HANDLE handle, int *order);
static ACPI_STATUS acpi_probe_child(ACPI_HANDLE handle, UINT32 level,
		    void *context, void **status);
static BOOLEAN	acpi_MatchHid(ACPI_HANDLE h, const char *hid);
static void	acpi_sleep_enable(void *arg);
static ACPI_STATUS acpi_sleep_disable(struct acpi_softc *sc);
static ACPI_STATUS acpi_EnterSleepState(struct acpi_softc *sc, int state);
static void	acpi_shutdown_final(void *arg, int howto);
static void	acpi_enable_fixed_events(struct acpi_softc *sc);
static int	acpi_wake_sleep_prep(ACPI_HANDLE handle, int sstate);
static int	acpi_wake_run_prep(ACPI_HANDLE handle, int sstate);
static int	acpi_wake_prep_walk(int sstate);
static int	acpi_wake_sysctl_walk(device_t dev);
static int	acpi_wake_set_sysctl(SYSCTL_HANDLER_ARGS);
static void	acpi_system_eventhandler_sleep(void *arg, int state);
static void	acpi_system_eventhandler_wakeup(void *arg, int state);
static int	acpi_sname2sstate(const char *sname);
static const char *acpi_sstate2sname(int sstate);
static int	acpi_supported_sleep_state_sysctl(SYSCTL_HANDLER_ARGS);
static int	acpi_sleep_state_sysctl(SYSCTL_HANDLER_ARGS);
static int	acpi_pm_func(u_long cmd, void *arg, ...);
static int	acpi_child_location_str_method(device_t acdev, device_t child,
					       char *buf, size_t buflen);
static int	acpi_child_pnpinfo_str_method(device_t acdev, device_t child,
					      char *buf, size_t buflen);
#if defined(__i386__) || defined(__amd64__)
static void	acpi_enable_pcie(void);
#endif
static void	acpi_hint_device_unit(device_t acdev, device_t child,
		    const char *name, int *unitp);

static device_method_t acpi_methods[] = {
    /* Device interface */
    DEVMETHOD(device_probe,		acpi_probe),
    DEVMETHOD(device_attach,		acpi_attach),
    DEVMETHOD(device_shutdown,		acpi_shutdown),
    DEVMETHOD(device_detach,		bus_generic_detach),
    DEVMETHOD(device_suspend,		acpi_suspend),
    DEVMETHOD(device_resume,		acpi_resume),

    /* Bus interface */
    DEVMETHOD(bus_add_child,		acpi_add_child),
    DEVMETHOD(bus_print_child,		acpi_print_child),
    DEVMETHOD(bus_probe_nomatch,	acpi_probe_nomatch),
    DEVMETHOD(bus_driver_added,		acpi_driver_added),
    DEVMETHOD(bus_read_ivar,		acpi_read_ivar),
    DEVMETHOD(bus_write_ivar,		acpi_write_ivar),
    DEVMETHOD(bus_get_resource_list,	acpi_get_rlist),
    DEVMETHOD(bus_set_resource,		bus_generic_rl_set_resource),
    DEVMETHOD(bus_get_resource,		bus_generic_rl_get_resource),
    DEVMETHOD(bus_alloc_resource,	acpi_alloc_resource),
    DEVMETHOD(bus_release_resource,	acpi_release_resource),
    DEVMETHOD(bus_delete_resource,	acpi_delete_resource),
    DEVMETHOD(bus_child_pnpinfo_str,	acpi_child_pnpinfo_str_method),
    DEVMETHOD(bus_child_location_str,	acpi_child_location_str_method),
    DEVMETHOD(bus_activate_resource,	bus_generic_activate_resource),
    DEVMETHOD(bus_deactivate_resource,	bus_generic_deactivate_resource),
    DEVMETHOD(bus_setup_intr,		bus_generic_setup_intr),
    DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),
    DEVMETHOD(bus_hint_device_unit,	acpi_hint_device_unit),

    /* ACPI bus */
    DEVMETHOD(acpi_id_probe,		acpi_device_id_probe),
    DEVMETHOD(acpi_evaluate_object,	acpi_device_eval_obj),
    DEVMETHOD(acpi_pwr_for_sleep,	acpi_device_pwr_for_sleep),
    DEVMETHOD(acpi_scan_children,	acpi_device_scan_children),

    /* PCI emulation */
    DEVMETHOD(pci_set_powerstate,	acpi_set_powerstate_method),

    /* ISA emulation */
    DEVMETHOD(isa_pnp_probe,		acpi_isa_pnp_probe),

    {0, 0}
};

static driver_t acpi_driver = {
    "acpi",
    acpi_methods,
    sizeof(struct acpi_softc),
};

static devclass_t acpi_devclass;
DRIVER_MODULE(acpi, nexus, acpi_driver, acpi_devclass, acpi_modevent, 0);
MODULE_VERSION(acpi, 1);

ACPI_SERIAL_DECL(acpi, "ACPI root bus");

/* Local pools for managing system resources for ACPI child devices. */
static struct rman acpi_rman_io, acpi_rman_mem;

#define ACPI_MINIMUM_AWAKETIME	5

/* Holds the description of the acpi0 device. */
static char acpi_desc[ACPI_OEM_ID_SIZE + ACPI_OEM_TABLE_ID_SIZE + 2];

SYSCTL_NODE(_debug, OID_AUTO, acpi, CTLFLAG_RD, NULL, "ACPI debugging");
static char acpi_ca_version[12];
SYSCTL_STRING(_debug_acpi, OID_AUTO, acpi_ca_version, CTLFLAG_RD,
	      acpi_ca_version, 0, "Version of Intel ACPI-CA");

/*
 * Allow override of whether methods execute in parallel or not.
 * Enable this for serial behavior, which fixes "AE_ALREADY_EXISTS"
 * errors for AML that really can't handle parallel method execution.
 * It is off by default since this breaks recursive methods and
 * some IBMs use such code.
 */
static int acpi_serialize_methods;
TUNABLE_INT("hw.acpi.serialize_methods", &acpi_serialize_methods);

/* Power devices off and on in suspend and resume.  XXX Remove once tested. */
static int acpi_do_powerstate = 1;
TUNABLE_INT("debug.acpi.do_powerstate", &acpi_do_powerstate);
SYSCTL_INT(_debug_acpi, OID_AUTO, do_powerstate, CTLFLAG_RW,
    &acpi_do_powerstate, 1, "Turn off devices when suspending.");

/* Reset system clock while resuming.  XXX Remove once tested. */
static int acpi_reset_clock = 1;
TUNABLE_INT("debug.acpi.reset_clock", &acpi_reset_clock);
SYSCTL_INT(_debug_acpi, OID_AUTO, reset_clock, CTLFLAG_RW,
    &acpi_reset_clock, 1, "Reset system clock while resuming.");

/* Allow users to override quirks. */
TUNABLE_INT("debug.acpi.quirks", &acpi_quirks);

static int acpi_susp_bounce;
SYSCTL_INT(_debug_acpi, OID_AUTO, suspend_bounce, CTLFLAG_RW,
    &acpi_susp_bounce, 0, "Don't actually suspend, just test devices.");

/*
 * ACPI can only be loaded as a module by the loader; activating it after
 * system bootstrap time is not useful, and can be fatal to the system.
 * It also cannot be unloaded, since the entire system bus hierarchy hangs
 * off it.
 */
static int
acpi_modevent(struct module *mod, int event, void *junk)
{
    switch (event) {
    case MOD_LOAD:
	if (!cold) {
	    printf("The ACPI driver cannot be loaded after boot.\n");
	    return (EPERM);
	}
	break;
    case MOD_UNLOAD:
	if (!cold && power_pm_get_type() == POWER_PM_TYPE_ACPI)
	    return (EBUSY);
	break;
    default:
	break;
    }
    return (0);
}

/*
 * Perform early initialization.
 */
ACPI_STATUS
acpi_Startup(void)
{
    static int started = 0;
    ACPI_STATUS status;
    int val;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    /* Only run the startup code once.  The MADT driver also calls this. */
    if (started)
	return_VALUE (AE_OK);
    started = 1;

    /*
     * Pre-allocate space for RSDT/XSDT and DSDT tables and allow resizing
     * if more tables exist.
     */
    if (ACPI_FAILURE(status = AcpiInitializeTables(NULL, 2, TRUE))) {
	printf("ACPI: Table initialisation failed: %s\n",
	    AcpiFormatException(status));
	return_VALUE (status);
    }

    /* Set up any quirks we have for this system. */
    if (acpi_quirks == ACPI_Q_OK)
	acpi_table_quirks(&acpi_quirks);

    /* If the user manually set the disabled hint to 0, force-enable ACPI. */
    if (resource_int_value("acpi", 0, "disabled", &val) == 0 && val == 0)
	acpi_quirks &= ~ACPI_Q_BROKEN;
    if (acpi_quirks & ACPI_Q_BROKEN) {
	printf("ACPI disabled by blacklist.  Contact your BIOS vendor.\n");
	status = AE_SUPPORT;
    }

    return_VALUE (status);
}

/*
 * Detect ACPI and perform early initialisation.
 */
int
acpi_identify(void)
{
    ACPI_TABLE_RSDP	*rsdp;
    ACPI_TABLE_HEADER	*rsdt;
    ACPI_PHYSICAL_ADDRESS paddr;
    struct sbuf		sb;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    if (!cold)
	return (ENXIO);

    /* Check that we haven't been disabled with a hint. */
    if (resource_disabled("acpi", 0))
	return (ENXIO);

    /* Check for other PM systems. */
    if (power_pm_get_type() != POWER_PM_TYPE_NONE &&
	power_pm_get_type() != POWER_PM_TYPE_ACPI) {
	printf("ACPI identify failed, other PM system enabled.\n");
	return (ENXIO);
    }

    /* Initialize root tables. */
    if (ACPI_FAILURE(acpi_Startup())) {
	printf("ACPI: Try disabling either ACPI or apic support.\n");
	return (ENXIO);
    }

    if ((paddr = AcpiOsGetRootPointer()) == 0 ||
	(rsdp = AcpiOsMapMemory(paddr, sizeof(ACPI_TABLE_RSDP))) == NULL)
	return (ENXIO);
    if (rsdp->Revision > 1 && rsdp->XsdtPhysicalAddress != 0)
	paddr = (ACPI_PHYSICAL_ADDRESS)rsdp->XsdtPhysicalAddress;
    else
	paddr = (ACPI_PHYSICAL_ADDRESS)rsdp->RsdtPhysicalAddress;
    AcpiOsUnmapMemory(rsdp, sizeof(ACPI_TABLE_RSDP));

    if ((rsdt = AcpiOsMapMemory(paddr, sizeof(ACPI_TABLE_HEADER))) == NULL)
	return (ENXIO);
    sbuf_new(&sb, acpi_desc, sizeof(acpi_desc), SBUF_FIXEDLEN);
    sbuf_bcat(&sb, rsdt->OemId, ACPI_OEM_ID_SIZE);
    sbuf_trim(&sb);
    sbuf_putc(&sb, ' ');
    sbuf_bcat(&sb, rsdt->OemTableId, ACPI_OEM_TABLE_ID_SIZE);
    sbuf_trim(&sb);
    sbuf_finish(&sb);
    sbuf_delete(&sb);
    AcpiOsUnmapMemory(rsdt, sizeof(ACPI_TABLE_HEADER));

    snprintf(acpi_ca_version, sizeof(acpi_ca_version), "%x", ACPI_CA_VERSION);

    return (0);
}

/*
 * Fetch some descriptive data from ACPI to put in our attach message.
 */
static int
acpi_probe(device_t dev)
{

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    device_set_desc(dev, acpi_desc);

    return_VALUE (0);
}

static int
acpi_attach(device_t dev)
{
    struct acpi_softc	*sc;
    ACPI_TABLE_FACS	*facs;
    ACPI_STATUS		status;
    int			error, state;
    UINT32		flags;
    UINT8		TypeA, TypeB;
    char		*env;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    sc = device_get_softc(dev);
    sc->acpi_dev = dev;
    callout_init(&sc->susp_force_to, TRUE);

    error = ENXIO;

    /* Initialize resource manager. */
    acpi_rman_io.rm_type = RMAN_ARRAY;
    acpi_rman_io.rm_start = 0;
    acpi_rman_io.rm_end = 0xffff;
    acpi_rman_io.rm_descr = "ACPI I/O ports";
    if (rman_init(&acpi_rman_io) != 0)
	panic("acpi rman_init IO ports failed");
    acpi_rman_mem.rm_type = RMAN_ARRAY;
    acpi_rman_mem.rm_start = 0;
    acpi_rman_mem.rm_end = ~0ul;
    acpi_rman_mem.rm_descr = "ACPI I/O memory addresses";
    if (rman_init(&acpi_rman_mem) != 0)
	panic("acpi rman_init memory failed");

    /* Initialise the ACPI mutex */
    mtx_init(&acpi_mutex, "ACPI global lock", NULL, MTX_DEF);

    /*
     * Set the globals from our tunables.  This is needed because ACPI-CA
     * uses UINT8 for some values and we have no tunable_byte.
     */
    AcpiGbl_AllMethodsSerialized = acpi_serialize_methods;
    AcpiGbl_EnableInterpreterSlack = TRUE;

    /* Start up the ACPI CA subsystem. */
    status = AcpiInitializeSubsystem();
    if (ACPI_FAILURE(status)) {
	device_printf(dev, "Could not initialize Subsystem: %s\n",
		      AcpiFormatException(status));
	goto out;
    }

    /* Load ACPI name space. */
    status = AcpiLoadTables();
    if (ACPI_FAILURE(status)) {
	device_printf(dev, "Could not load Namespace: %s\n",
		      AcpiFormatException(status));
	goto out;
    }

#if defined(__i386__) || defined(__amd64__)
    /* Handle MCFG table if present. */
    acpi_enable_pcie();
#endif

    /* Install the default address space handlers. */
    status = AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT,
		ACPI_ADR_SPACE_SYSTEM_MEMORY, ACPI_DEFAULT_HANDLER, NULL, NULL);
    if (ACPI_FAILURE(status)) {
	device_printf(dev, "Could not initialise SystemMemory handler: %s\n",
		      AcpiFormatException(status));
	goto out;
    }
    status = AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT,
		ACPI_ADR_SPACE_SYSTEM_IO, ACPI_DEFAULT_HANDLER, NULL, NULL);
    if (ACPI_FAILURE(status)) {
	device_printf(dev, "Could not initialise SystemIO handler: %s\n",
		      AcpiFormatException(status));
	goto out;
    }
    status = AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT,
		ACPI_ADR_SPACE_PCI_CONFIG, ACPI_DEFAULT_HANDLER, NULL, NULL);
    if (ACPI_FAILURE(status)) {
	device_printf(dev, "could not initialise PciConfig handler: %s\n",
		      AcpiFormatException(status));
	goto out;
    }

    /*
     * Note that some systems (specifically, those with namespace evaluation
     * issues that require the avoidance of parts of the namespace) must
     * avoid running _INI and _STA on everything, as well as dodging the final
     * object init pass.
     *
     * For these devices, we set ACPI_NO_DEVICE_INIT and ACPI_NO_OBJECT_INIT).
     *
     * XXX We should arrange for the object init pass after we have attached
     *     all our child devices, but on many systems it works here.
     */
    flags = 0;
    if (testenv("debug.acpi.avoid"))
	flags = ACPI_NO_DEVICE_INIT | ACPI_NO_OBJECT_INIT;

    /* Bring the hardware and basic handlers online. */
    if (ACPI_FAILURE(status = AcpiEnableSubsystem(flags))) {
	device_printf(dev, "Could not enable ACPI: %s\n",
		      AcpiFormatException(status));
	goto out;
    }

    /*
     * Call the ECDT probe function to provide EC functionality before
     * the namespace has been evaluated.
     *
     * XXX This happens before the sysresource devices have been probed and
     * attached so its resources come from nexus0.  In practice, this isn't
     * a problem but should be addressed eventually.
     */
    acpi_ec_ecdt_probe(dev);

    /* Bring device objects and regions online. */
    if (ACPI_FAILURE(status = AcpiInitializeObjects(flags))) {
	device_printf(dev, "Could not initialize ACPI objects: %s\n",
		      AcpiFormatException(status));
	goto out;
    }

    /*
     * Setup our sysctl tree.
     *
     * XXX: This doesn't check to make sure that none of these fail.
     */
    sysctl_ctx_init(&sc->acpi_sysctl_ctx);
    sc->acpi_sysctl_tree = SYSCTL_ADD_NODE(&sc->acpi_sysctl_ctx,
			       SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO,
			       device_get_name(dev), CTLFLAG_RD, 0, "");
    SYSCTL_ADD_PROC(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "supported_sleep_state", CTLTYPE_STRING | CTLFLAG_RD,
	0, 0, acpi_supported_sleep_state_sysctl, "A", "");
    SYSCTL_ADD_PROC(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "power_button_state", CTLTYPE_STRING | CTLFLAG_RW,
	&sc->acpi_power_button_sx, 0, acpi_sleep_state_sysctl, "A", "");
    SYSCTL_ADD_PROC(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "sleep_button_state", CTLTYPE_STRING | CTLFLAG_RW,
	&sc->acpi_sleep_button_sx, 0, acpi_sleep_state_sysctl, "A", "");
    SYSCTL_ADD_PROC(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "lid_switch_state", CTLTYPE_STRING | CTLFLAG_RW,
	&sc->acpi_lid_switch_sx, 0, acpi_sleep_state_sysctl, "A", "");
    SYSCTL_ADD_PROC(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "standby_state", CTLTYPE_STRING | CTLFLAG_RW,
	&sc->acpi_standby_sx, 0, acpi_sleep_state_sysctl, "A", "");
    SYSCTL_ADD_PROC(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "suspend_state", CTLTYPE_STRING | CTLFLAG_RW,
	&sc->acpi_suspend_sx, 0, acpi_sleep_state_sysctl, "A", "");
    SYSCTL_ADD_INT(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "sleep_delay", CTLFLAG_RW, &sc->acpi_sleep_delay, 0,
	"sleep delay");
    SYSCTL_ADD_INT(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "s4bios", CTLFLAG_RW, &sc->acpi_s4bios, 0, "S4BIOS mode");
    SYSCTL_ADD_INT(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "verbose", CTLFLAG_RW, &sc->acpi_verbose, 0, "verbose mode");
    SYSCTL_ADD_INT(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "disable_on_reboot", CTLFLAG_RW,
	&sc->acpi_do_disable, 0, "Disable ACPI when rebooting/halting system");
    SYSCTL_ADD_INT(&sc->acpi_sysctl_ctx, SYSCTL_CHILDREN(sc->acpi_sysctl_tree),
	OID_AUTO, "handle_reboot", CTLFLAG_RW,
	&sc->acpi_handle_reboot, 0, "Use ACPI Reset Register to reboot");

    /*
     * Default to 1 second before sleeping to give some machines time to
     * stabilize.
     */
    sc->acpi_sleep_delay = 1;
    if (bootverbose)
	sc->acpi_verbose = 1;
    if ((env = getenv("hw.acpi.verbose")) != NULL) {
	if (strcmp(env, "0") != 0)
	    sc->acpi_verbose = 1;
	freeenv(env);
    }

    /* Only enable S4BIOS by default if the FACS says it is available. */
    status = AcpiGetTable(ACPI_SIG_FACS, 0, (ACPI_TABLE_HEADER **)&facs);
    if (ACPI_FAILURE(status)) {
	device_printf(dev, "couldn't get FACS: %s\n",
		      AcpiFormatException(status));
	error = ENXIO;
	goto out;
    }
    if (facs->Flags & ACPI_FACS_S4_BIOS_PRESENT)
	sc->acpi_s4bios = 1;

    /* Probe all supported sleep states. */
    acpi_sleep_states[ACPI_STATE_S0] = TRUE;
    for (state = ACPI_STATE_S1; state < ACPI_S_STATE_COUNT; state++)
	if (ACPI_SUCCESS(AcpiGetSleepTypeData(state, &TypeA, &TypeB)))
	    acpi_sleep_states[state] = TRUE;

    /*
     * Dispatch the default sleep state to devices.  The lid switch is set
     * to UNKNOWN by default to avoid surprising users.
     */
    sc->acpi_power_button_sx = acpi_sleep_states[ACPI_STATE_S5] ?
	ACPI_STATE_S5 : ACPI_STATE_UNKNOWN;
    sc->acpi_lid_switch_sx = ACPI_STATE_UNKNOWN;
    sc->acpi_standby_sx = acpi_sleep_states[ACPI_STATE_S1] ?
	ACPI_STATE_S1 : ACPI_STATE_UNKNOWN;
    sc->acpi_suspend_sx = acpi_sleep_states[ACPI_STATE_S3] ?
	ACPI_STATE_S3 : ACPI_STATE_UNKNOWN;

    /* Pick the first valid sleep state for the sleep button default. */
    sc->acpi_sleep_button_sx = ACPI_STATE_UNKNOWN;
    for (state = ACPI_STATE_S1; state <= ACPI_STATE_S4; state++)
	if (acpi_sleep_states[state]) {
	    sc->acpi_sleep_button_sx = state;
	    break;
	}

    acpi_enable_fixed_events(sc);

    /*
     * Scan the namespace and attach/initialise children.
     */

    /* Register our shutdown handler. */
    EVENTHANDLER_REGISTER(shutdown_final, acpi_shutdown_final, sc,
	SHUTDOWN_PRI_LAST);

    /*
     * Register our acpi event handlers.
     * XXX should be configurable eg. via userland policy manager.
     */
    EVENTHANDLER_REGISTER(acpi_sleep_event, acpi_system_eventhandler_sleep,
	sc, ACPI_EVENT_PRI_LAST);
    EVENTHANDLER_REGISTER(acpi_wakeup_event, acpi_system_eventhandler_wakeup,
	sc, ACPI_EVENT_PRI_LAST);

    /* Flag our initial states. */
    sc->acpi_enabled = TRUE;
    sc->acpi_sstate = ACPI_STATE_S0;
    sc->acpi_sleep_disabled = TRUE;

    /* Create the control device */
    sc->acpi_dev_t = make_dev(&acpi_cdevsw, 0, UID_ROOT, GID_WHEEL, 0644,
			      "acpi");
    sc->acpi_dev_t->si_drv1 = sc;

    if ((error = acpi_machdep_init(dev)))
	goto out;

    /* Register ACPI again to pass the correct argument of pm_func. */
    power_pm_register(POWER_PM_TYPE_ACPI, acpi_pm_func, sc);

    if (!acpi_disabled("bus"))
	acpi_probe_children(dev);

    /* Allow sleep request after a while. */
    timeout(acpi_sleep_enable, sc, hz * ACPI_MINIMUM_AWAKETIME);

    error = 0;

 out:
    return_VALUE (error);
}

static int
acpi_suspend(device_t dev)
{
    device_t child, *devlist;
    int error, i, numdevs, pstate;

    GIANT_REQUIRED;

    /* First give child devices a chance to suspend. */
    error = bus_generic_suspend(dev);
    if (error)
	return (error);

    /*
     * Now, set them into the appropriate power state, usually D3.  If the
     * device has an _SxD method for the next sleep state, use that power
     * state instead.
     */
    error = device_get_children(dev, &devlist, &numdevs);
    if (error)
	return (error);
    for (i = 0; i < numdevs; i++) {
	/* If the device is not attached, we've powered it down elsewhere. */
	child = devlist[i];
	if (!device_is_attached(child))
	    continue;

	/*
	 * Default to D3 for all sleep states.  The _SxD method is optional
	 * so set the powerstate even if it's absent.
	 */
	pstate = PCI_POWERSTATE_D3;
	error = acpi_device_pwr_for_sleep(device_get_parent(child),
	    child, &pstate);
	if ((error == 0 || error == ESRCH) && acpi_do_powerstate)
	    pci_set_powerstate(child, pstate);
    }
    free(devlist, M_TEMP);
    error = 0;

    return (error);
}

static int
acpi_resume(device_t dev)
{
    ACPI_HANDLE handle;
    int i, numdevs, error;
    device_t child, *devlist;

    GIANT_REQUIRED;

    /*
     * Put all devices in D0 before resuming them.  Call _S0D on each one
     * since some systems expect this.
     */
    error = device_get_children(dev, &devlist, &numdevs);
    if (error)
	return (error);
    for (i = 0; i < numdevs; i++) {
	child = devlist[i];
	handle = acpi_get_handle(child);
	if (handle)
	    AcpiEvaluateObject(handle, "_S0D", NULL, NULL);
	if (device_is_attached(child) && acpi_do_powerstate)
	    pci_set_powerstate(child, PCI_POWERSTATE_D0);
    }
    free(devlist, M_TEMP);

    return (bus_generic_resume(dev));
}

static int
acpi_shutdown(device_t dev)
{

    GIANT_REQUIRED;

    /* Allow children to shutdown first. */
    bus_generic_shutdown(dev);

    /*
     * Enable any GPEs that are able to power-on the system (i.e., RTC).
     * Also, disable any that are not valid for this state (most).
     */
    acpi_wake_prep_walk(ACPI_STATE_S5);

    return (0);
}

/*
 * Handle a new device being added
 */
static device_t
acpi_add_child(device_t bus, int order, const char *name, int unit)
{
    struct acpi_device	*ad;
    device_t		child;

    if ((ad = malloc(sizeof(*ad), M_ACPIDEV, M_NOWAIT | M_ZERO)) == NULL)
	return (NULL);

    resource_list_init(&ad->ad_rl);

    child = device_add_child_ordered(bus, order, name, unit);
    if (child != NULL)
	device_set_ivars(child, ad);
    else
	free(ad, M_ACPIDEV);
    return (child);
}

static int
acpi_print_child(device_t bus, device_t child)
{
    struct acpi_device	 *adev = device_get_ivars(child);
    struct resource_list *rl = &adev->ad_rl;
    int retval = 0;

    retval += bus_print_child_header(bus, child);
    retval += resource_list_print_type(rl, "port",  SYS_RES_IOPORT, "%#lx");
    retval += resource_list_print_type(rl, "iomem", SYS_RES_MEMORY, "%#lx");
    retval += resource_list_print_type(rl, "irq",   SYS_RES_IRQ,    "%ld");
    retval += resource_list_print_type(rl, "drq",   SYS_RES_DRQ,    "%ld");
    if (device_get_flags(child))
	retval += printf(" flags %#x", device_get_flags(child));
    retval += bus_print_child_footer(bus, child);

    return (retval);
}

/*
 * If this device is an ACPI child but no one claimed it, attempt
 * to power it off.  We'll power it back up when a driver is added.
 *
 * XXX Disabled for now since many necessary devices (like fdc and
 * ATA) don't claim the devices we created for them but still expect
 * them to be powered up.
 */
static void
acpi_probe_nomatch(device_t bus, device_t child)
{
#ifdef ACPI_ENABLE_POWERDOWN_NODRIVER
    pci_set_powerstate(child, PCI_POWERSTATE_D3);
#endif
}

/*
 * If a new driver has a chance to probe a child, first power it up.
 *
 * XXX Disabled for now (see acpi_probe_nomatch for details).
 */
static void
acpi_driver_added(device_t dev, driver_t *driver)
{
    device_t child, *devlist;
    int i, numdevs;

    DEVICE_IDENTIFY(driver, dev);
    if (device_get_children(dev, &devlist, &numdevs))
	    return;
    for (i = 0; i < numdevs; i++) {
	child = devlist[i];
	if (device_get_state(child) == DS_NOTPRESENT) {
#ifdef ACPI_ENABLE_POWERDOWN_NODRIVER
	    pci_set_powerstate(child, PCI_POWERSTATE_D0);
	    if (device_probe_and_attach(child) != 0)
		pci_set_powerstate(child, PCI_POWERSTATE_D3);
#else
	    device_probe_and_attach(child);
#endif
	}
    }
    free(devlist, M_TEMP);
}

/* Location hint for devctl(8) */
static int
acpi_child_location_str_method(device_t cbdev, device_t child, char *buf,
    size_t buflen)
{
    struct acpi_device *dinfo = device_get_ivars(child);

    if (dinfo->ad_handle)
	snprintf(buf, buflen, "handle=%s", acpi_name(dinfo->ad_handle));
    else
	snprintf(buf, buflen, "unknown");
    return (0);
}

/* PnP information for devctl(8) */
static int
acpi_child_pnpinfo_str_method(device_t cbdev, device_t child, char *buf,
    size_t buflen)
{
    ACPI_BUFFER adbuf = {ACPI_ALLOCATE_BUFFER, NULL}; 
    ACPI_DEVICE_INFO *adinfo;
    struct acpi_device *dinfo = device_get_ivars(child);
    char *end;
    int error;

    error = AcpiGetObjectInfo(dinfo->ad_handle, &adbuf);
    adinfo = (ACPI_DEVICE_INFO *) adbuf.Pointer;
    if (error)
	snprintf(buf, buflen, "unknown");
    else
	snprintf(buf, buflen, "_HID=%s _UID=%lu",
		 (adinfo->Valid & ACPI_VALID_HID) ?
		 adinfo->HardwareId.Value : "none",
		 (adinfo->Valid & ACPI_VALID_UID) ?
		 strtoul(adinfo->UniqueId.Value, &end, 10) : 0);
    if (adinfo)
	AcpiOsFree(adinfo);

    return (0);
}

/*
 * Handle per-device ivars
 */
static int
acpi_read_ivar(device_t dev, device_t child, int index, uintptr_t *result)
{
    struct acpi_device	*ad;

    if ((ad = device_get_ivars(child)) == NULL) {
	device_printf(child, "device has no ivars\n");
	return (ENOENT);
    }

    /* ACPI and ISA compatibility ivars */
    switch(index) {
    case ACPI_IVAR_HANDLE:
	*(ACPI_HANDLE *)result = ad->ad_handle;
	break;
    case ACPI_IVAR_MAGIC:
	*(uintptr_t *)result = ad->ad_magic;
	break;
    case ACPI_IVAR_PRIVATE:
	*(void **)result = ad->ad_private;
	break;
    case ACPI_IVAR_FLAGS:
	*(int *)result = ad->ad_flags;
	break;
    case ISA_IVAR_VENDORID:
    case ISA_IVAR_SERIAL:
    case ISA_IVAR_COMPATID:
	*(int *)result = -1;
	break;
    case ISA_IVAR_LOGICALID:
	*(int *)result = acpi_isa_get_logicalid(child);
	break;
    default:
	return (ENOENT);
    }

    return (0);
}

static int
acpi_write_ivar(device_t dev, device_t child, int index, uintptr_t value)
{
    struct acpi_device	*ad;

    if ((ad = device_get_ivars(child)) == NULL) {
	device_printf(child, "device has no ivars\n");
	return (ENOENT);
    }

    switch(index) {
    case ACPI_IVAR_HANDLE:
	ad->ad_handle = (ACPI_HANDLE)value;
	break;
    case ACPI_IVAR_MAGIC:
	ad->ad_magic = (uintptr_t)value;
	break;
    case ACPI_IVAR_PRIVATE:
	ad->ad_private = (void *)value;
	break;
    case ACPI_IVAR_FLAGS:
	ad->ad_flags = (int)value;
	break;
    default:
	panic("bad ivar write request (%d)", index);
	return (ENOENT);
    }

    return (0);
}

/*
 * Handle child resource allocation/removal
 */
static struct resource_list *
acpi_get_rlist(device_t dev, device_t child)
{
    struct acpi_device		*ad;

    ad = device_get_ivars(child);
    return (&ad->ad_rl);
}

static int
acpi_match_resource_hint(device_t dev, int type, long value)
{
    struct acpi_device *ad = device_get_ivars(dev);
    struct resource_list *rl = &ad->ad_rl;
    struct resource_list_entry *rle;

    STAILQ_FOREACH(rle, rl, link) {
	if (rle->type != type)
	    continue;
	if (rle->start <= value && rle->end >= value)
	    return (1);
    }
    return (0);
}

/*
 * Wire device unit numbers based on resource matches in hints.
 */
static void
acpi_hint_device_unit(device_t acdev, device_t child, const char *name,
    int *unitp)
{
    const char *s;
    long value;
    int line, matches, unit;

    /*
     * Iterate over all the hints for the devices with the specified
     * name to see if one's resources are a subset of this device.
     */
    line = 0;
    for (;;) {
	if (resource_find_dev(&line, name, &unit, "at", NULL) != 0)
	    break;

	/* Must have an "at" for acpi or isa. */
	resource_string_value(name, unit, "at", &s);
	if (!(strcmp(s, "acpi0") == 0 || strcmp(s, "acpi") == 0 ||
	    strcmp(s, "isa0") == 0 || strcmp(s, "isa") == 0))
	    continue;

	/*
	 * Check for matching resources.  We must have at least one,
	 * and all resources specified have to match.
	 *
	 * XXX: We may want to revisit this to be more lenient and wire
	 * as long as it gets one match.
	 */
	matches = 0;
	if (resource_long_value(name, unit, "port", &value) == 0) {
	    if (acpi_match_resource_hint(child, SYS_RES_IOPORT, value))
		matches++;
	    else
		continue;
	}
	if (resource_long_value(name, unit, "maddr", &value) == 0) {
	    if (acpi_match_resource_hint(child, SYS_RES_MEMORY, value))
		matches++;
	    else
		continue;
	}
	if (resource_long_value(name, unit, "irq", &value) == 0) {
	    if (acpi_match_resource_hint(child, SYS_RES_IRQ, value))
		matches++;
	    else
		continue;
	}
	if (resource_long_value(name, unit, "drq", &value) == 0) {
	    if (acpi_match_resource_hint(child, SYS_RES_DRQ, value))
		matches++;
	    else
		continue;
	}

	if (matches > 0) {
	    /* We have a winner! */
	    *unitp = unit;
	    break;
	}
    }
}

/*
 * Pre-allocate/manage all memory and IO resources.  Since rman can't handle
 * duplicates, we merge any in the sysresource attach routine.
 */
static int
acpi_sysres_alloc(device_t dev)
{
    struct resource *res;
    struct resource_list *rl;
    struct resource_list_entry *rle;
    struct rman *rm;
    char *sysres_ids[] = { "PNP0C01", "PNP0C02", NULL };
    device_t *children;
    int child_count, i;

    /*
     * Probe/attach any sysresource devices.  This would be unnecessary if we
     * had multi-pass probe/attach.
     */
    if (device_get_children(dev, &children, &child_count) != 0)
	return (ENXIO);
    for (i = 0; i < child_count; i++) {
	if (ACPI_ID_PROBE(dev, children[i], sysres_ids) != NULL)
	    device_probe_and_attach(children[i]);
    }
    free(children, M_TEMP);

    rl = BUS_GET_RESOURCE_LIST(device_get_parent(dev), dev);
    STAILQ_FOREACH(rle, rl, link) {
	if (rle->res != NULL) {
	    device_printf(dev, "duplicate resource for %lx\n", rle->start);
	    continue;
	}

	/* Only memory and IO resources are valid here. */
	switch (rle->type) {
	case SYS_RES_IOPORT:
	    rm = &acpi_rman_io;
	    break;
	case SYS_RES_MEMORY:
	    rm = &acpi_rman_mem;
	    break;
	default:
	    continue;
	}

	/* Pre-allocate resource and add to our rman pool. */
	res = BUS_ALLOC_RESOURCE(device_get_parent(dev), dev, rle->type,
	    &rle->rid, rle->start, rle->start + rle->count - 1, rle->count, 0);
	if (res != NULL) {
	    rman_manage_region(rm, rman_get_start(res), rman_get_end(res));
	    rle->res = res;
	} else
	    device_printf(dev, "reservation of %lx, %lx (%d) failed\n",
		rle->start, rle->count, rle->type);
    }
    return (0);
}

static struct resource *
acpi_alloc_resource(device_t bus, device_t child, int type, int *rid,
    u_long start, u_long end, u_long count, u_int flags)
{
    ACPI_RESOURCE ares;
    struct acpi_device *ad = device_get_ivars(child);
    struct resource_list *rl = &ad->ad_rl;
    struct resource_list_entry *rle;
    struct resource *res;
    struct rman *rm;

    res = NULL;

    /* We only handle memory and IO resources through rman. */
    switch (type) {
    case SYS_RES_IOPORT:
	rm = &acpi_rman_io;
	break;
    case SYS_RES_MEMORY:
	rm = &acpi_rman_mem;
	break;
    default:
	rm = NULL;
    }
	    
    ACPI_SERIAL_BEGIN(acpi);

    /*
     * If this is an allocation of the "default" range for a given RID, and
     * we know what the resources for this device are (i.e., they're on the
     * child's resource list), use those start/end values.
     */
    if (bus == device_get_parent(child) && start == 0UL && end == ~0UL) {
	rle = resource_list_find(rl, type, *rid);
	if (rle == NULL)
	    goto out;
	start = rle->start;
	end = rle->end;
	count = rle->count;
    }

    /*
     * If this is an allocation of a specific range, see if we can satisfy
     * the request from our system resource regions.  If we can't, pass the
     * request up to the parent.
     */
    if (start + count - 1 == end && rm != NULL)
	res = rman_reserve_resource(rm, start, end, count, flags & ~RF_ACTIVE,
	    child);
    if (res == NULL) {
	res = BUS_ALLOC_RESOURCE(device_get_parent(bus), child, type, rid,
	    start, end, count, flags);
    } else {
	rman_set_rid(res, *rid);

	/* If requested, activate the resource using the parent's method. */
	if (flags & RF_ACTIVE)
	    if (bus_activate_resource(child, type, *rid, res) != 0) {
		rman_release_resource(res);
		res = NULL;
		goto out;
	    }
    }

    if (res != NULL && device_get_parent(child) == bus)
	switch (type) {
	case SYS_RES_IRQ:
	    /*
	     * Since bus_config_intr() takes immediate effect, we cannot
	     * configure the interrupt associated with a device when we
	     * parse the resources but have to defer it until a driver
	     * actually allocates the interrupt via bus_alloc_resource().
	     *
	     * XXX: Should we handle the lookup failing?
	     */
	    if (ACPI_SUCCESS(acpi_lookup_irq_resource(child, *rid, res, &ares)))
		acpi_config_intr(child, &ares);
	    break;
	}

out:
    ACPI_SERIAL_END(acpi);
    return (res);
}

static int
acpi_release_resource(device_t bus, device_t child, int type, int rid,
    struct resource *r)
{
    struct rman *rm;
    int ret;

    /* We only handle memory and IO resources through rman. */
    switch (type) {
    case SYS_RES_IOPORT:
	rm = &acpi_rman_io;
	break;
    case SYS_RES_MEMORY:
	rm = &acpi_rman_mem;
	break;
    default:
	rm = NULL;
    }

    ACPI_SERIAL_BEGIN(acpi);

    /*
     * If this resource belongs to one of our internal managers,
     * deactivate it and release it to the local pool.  If it doesn't,
     * pass this request up to the parent.
     */
    if (rm != NULL && rman_is_region_manager(r, rm)) {
	if (rman_get_flags(r) & RF_ACTIVE) {
	    ret = bus_deactivate_resource(child, type, rid, r);
	    if (ret != 0)
		goto out;
	}
	ret = rman_release_resource(r);
    } else
	ret = BUS_RELEASE_RESOURCE(device_get_parent(bus), child, type, rid, r);

out:
    ACPI_SERIAL_END(acpi);
    return (ret);
}

static void
acpi_delete_resource(device_t bus, device_t child, int type, int rid)
{
    struct resource_list *rl;

    rl = acpi_get_rlist(bus, child);
    resource_list_delete(rl, type, rid);
}

/* Allocate an IO port or memory resource, given its GAS. */
int
acpi_bus_alloc_gas(device_t dev, int *type, int *rid, ACPI_GENERIC_ADDRESS *gas,
    struct resource **res, u_int flags)
{
    int error, res_type;

    error = ENOMEM;
    if (type == NULL || rid == NULL || gas == NULL || res == NULL)
	return (EINVAL);

    /* We only support memory and IO spaces. */
    switch (gas->SpaceId) {
    case ACPI_ADR_SPACE_SYSTEM_MEMORY:
	res_type = SYS_RES_MEMORY;
	break;
    case ACPI_ADR_SPACE_SYSTEM_IO:
	res_type = SYS_RES_IOPORT;
	break;
    default:
	return (EOPNOTSUPP);
    }

    /*
     * If the register width is less than 8, assume the BIOS author means
     * it is a bit field and just allocate a byte.
     */
    if (gas->BitWidth && gas->BitWidth < 8)
	gas->BitWidth = 8;

    /* Validate the address after we're sure we support the space. */
    if (gas->Address == 0 || gas->BitWidth == 0)
	return (EINVAL);

    bus_set_resource(dev, res_type, *rid, gas->Address,
	gas->BitWidth / 8);
    *res = bus_alloc_resource_any(dev, res_type, rid, RF_ACTIVE | flags);
    if (*res != NULL) {
	*type = res_type;
	error = 0;
    } else
	bus_delete_resource(dev, res_type, *rid);

    return (error);
}

/* Probe _HID and _CID for compatible ISA PNP ids. */
static uint32_t
acpi_isa_get_logicalid(device_t dev)
{
    ACPI_DEVICE_INFO	*devinfo;
    ACPI_BUFFER		buf;
    ACPI_HANDLE		h;
    ACPI_STATUS		error;
    u_int32_t		pnpid;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    pnpid = 0;
    buf.Pointer = NULL;
    buf.Length = ACPI_ALLOCATE_BUFFER;

    /* Fetch and validate the HID. */
    if ((h = acpi_get_handle(dev)) == NULL)
	goto out;
    error = AcpiGetObjectInfo(h, &buf);
    if (ACPI_FAILURE(error))
	goto out;
    devinfo = (ACPI_DEVICE_INFO *)buf.Pointer;

    if ((devinfo->Valid & ACPI_VALID_HID) != 0)
	pnpid = PNP_EISAID(devinfo->HardwareId.Value);

out:
    if (buf.Pointer != NULL)
	AcpiOsFree(buf.Pointer);
    return_VALUE (pnpid);
}

static int
acpi_isa_get_compatid(device_t dev, uint32_t *cids, int count)
{
    ACPI_DEVICE_INFO	*devinfo;
    ACPI_BUFFER		buf;
    ACPI_HANDLE		h;
    ACPI_STATUS		error;
    uint32_t		*pnpid;
    int			valid, i;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    pnpid = cids;
    valid = 0;
    buf.Pointer = NULL;
    buf.Length = ACPI_ALLOCATE_BUFFER;

    /* Fetch and validate the CID */
    if ((h = acpi_get_handle(dev)) == NULL)
	goto out;
    error = AcpiGetObjectInfo(h, &buf);
    if (ACPI_FAILURE(error))
	goto out;
    devinfo = (ACPI_DEVICE_INFO *)buf.Pointer;
    if ((devinfo->Valid & ACPI_VALID_CID) == 0)
	goto out;

    if (devinfo->CompatibilityId.Count < count)
	count = devinfo->CompatibilityId.Count;
    for (i = 0; i < count; i++) {
	if (strncmp(devinfo->CompatibilityId.Id[i].Value, "PNP", 3) != 0)
	    continue;
	*pnpid++ = PNP_EISAID(devinfo->CompatibilityId.Id[i].Value);
	valid++;
    }

out:
    if (buf.Pointer != NULL)
	AcpiOsFree(buf.Pointer);
    return_VALUE (valid);
}

static char *
acpi_device_id_probe(device_t bus, device_t dev, char **ids) 
{
    ACPI_HANDLE h;
    int i;

    h = acpi_get_handle(dev);
    if (ids == NULL || h == NULL || acpi_get_type(dev) != ACPI_TYPE_DEVICE)
	return (NULL);

    /* Try to match one of the array of IDs with a HID or CID. */
    for (i = 0; ids[i] != NULL; i++) {
	if (acpi_MatchHid(h, ids[i]))
	    return (ids[i]);
    }
    return (NULL);
}

static ACPI_STATUS
acpi_device_eval_obj(device_t bus, device_t dev, ACPI_STRING pathname,
    ACPI_OBJECT_LIST *parameters, ACPI_BUFFER *ret)
{
    ACPI_HANDLE h;

    if (dev == NULL)
	h = ACPI_ROOT_OBJECT;
    else if ((h = acpi_get_handle(dev)) == NULL)
	return (AE_BAD_PARAMETER);
    return (AcpiEvaluateObject(h, pathname, parameters, ret));
}

static int
acpi_device_pwr_for_sleep(device_t bus, device_t dev, int *dstate)
{
    struct acpi_softc *sc;
    ACPI_HANDLE handle;
    ACPI_STATUS status;
    char sxd[8];
    int error;

    sc = device_get_softc(bus);
    handle = acpi_get_handle(dev);

    /*
     * XXX If we find these devices, don't try to power them down.
     * The serial and IRDA ports on my T23 hang the system when
     * set to D3 and it appears that such legacy devices may
     * need special handling in their drivers.
     */
    if (handle == NULL ||
	acpi_MatchHid(handle, "PNP0500") ||
	acpi_MatchHid(handle, "PNP0501") ||
	acpi_MatchHid(handle, "PNP0502") ||
	acpi_MatchHid(handle, "PNP0510") ||
	acpi_MatchHid(handle, "PNP0511"))
	return (ENXIO);

    /*
     * Override next state with the value from _SxD, if present.  If no
     * dstate argument was provided, don't fetch the return value.
     */
    snprintf(sxd, sizeof(sxd), "_S%dD", sc->acpi_sstate);
    if (dstate)
	status = acpi_GetInteger(handle, sxd, dstate);
    else
	status = AcpiEvaluateObject(handle, sxd, NULL, NULL);

    switch (status) {
    case AE_OK:
	error = 0;
	break;
    case AE_NOT_FOUND:
	error = ESRCH;
	break;
    default:
	error = ENXIO;
	break;
    }

    return (error);
}

/* Callback arg for our implementation of walking the namespace. */
struct acpi_device_scan_ctx {
    acpi_scan_cb_t	user_fn;
    void		*arg;
    ACPI_HANDLE		parent;
};

static ACPI_STATUS
acpi_device_scan_cb(ACPI_HANDLE h, UINT32 level, void *arg, void **retval)
{
    struct acpi_device_scan_ctx *ctx;
    device_t dev, old_dev;
    ACPI_STATUS status;
    ACPI_OBJECT_TYPE type;

    /*
     * Skip this device if we think we'll have trouble with it or it is
     * the parent where the scan began.
     */
    ctx = (struct acpi_device_scan_ctx *)arg;
    if (acpi_avoid(h) || h == ctx->parent)
	return (AE_OK);

    /* If this is not a valid device type (e.g., a method), skip it. */
    if (ACPI_FAILURE(AcpiGetType(h, &type)))
	return (AE_OK);
    if (type != ACPI_TYPE_DEVICE && type != ACPI_TYPE_PROCESSOR &&
	type != ACPI_TYPE_THERMAL && type != ACPI_TYPE_POWER)
	return (AE_OK);

    /*
     * Call the user function with the current device.  If it is unchanged
     * afterwards, return.  Otherwise, we update the handle to the new dev.
     */
    old_dev = acpi_get_device(h);
    dev = old_dev;
    status = ctx->user_fn(h, &dev, level, ctx->arg);
    if (ACPI_FAILURE(status) || old_dev == dev)
	return (status);

    /* Remove the old child and its connection to the handle. */
    if (old_dev != NULL) {
	device_delete_child(device_get_parent(old_dev), old_dev);
	AcpiDetachData(h, acpi_fake_objhandler);
    }

    /* Recreate the handle association if the user created a device. */
    if (dev != NULL)
	AcpiAttachData(h, acpi_fake_objhandler, dev);

    return (AE_OK);
}

static ACPI_STATUS
acpi_device_scan_children(device_t bus, device_t dev, int max_depth,
    acpi_scan_cb_t user_fn, void *arg)
{
    ACPI_HANDLE h;
    struct acpi_device_scan_ctx ctx;

    if (acpi_disabled("children"))
	return (AE_OK);

    if (dev == NULL)
	h = ACPI_ROOT_OBJECT;
    else if ((h = acpi_get_handle(dev)) == NULL)
	return (AE_BAD_PARAMETER);
    ctx.user_fn = user_fn;
    ctx.arg = arg;
    ctx.parent = h;
    return (AcpiWalkNamespace(ACPI_TYPE_ANY, h, max_depth,
	acpi_device_scan_cb, &ctx, NULL));
}

/*
 * Even though ACPI devices are not PCI, we use the PCI approach for setting
 * device power states since it's close enough to ACPI.
 */
static int
acpi_set_powerstate_method(device_t bus, device_t child, int state)
{
    ACPI_HANDLE h;
    ACPI_STATUS status;
    int error;

    error = 0;
    h = acpi_get_handle(child);
    if (state < ACPI_STATE_D0 || state > ACPI_D_STATES_MAX)
	return (EINVAL);
    if (h == NULL)
	return (0);

    /* Ignore errors if the power methods aren't present. */
    status = acpi_pwr_switch_consumer(h, state);
    if (ACPI_FAILURE(status) && status != AE_NOT_FOUND
	&& status != AE_BAD_PARAMETER)
	device_printf(bus, "failed to set ACPI power state D%d on %s: %s\n",
	    state, acpi_name(h), AcpiFormatException(status));

    return (error);
}

static int
acpi_isa_pnp_probe(device_t bus, device_t child, struct isa_pnp_id *ids)
{
    int			result, cid_count, i;
    uint32_t		lid, cids[8];

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    /*
     * ISA-style drivers attached to ACPI may persist and
     * probe manually if we return ENOENT.  We never want
     * that to happen, so don't ever return it.
     */
    result = ENXIO;

    /* Scan the supplied IDs for a match */
    lid = acpi_isa_get_logicalid(child);
    cid_count = acpi_isa_get_compatid(child, cids, 8);
    while (ids && ids->ip_id) {
	if (lid == ids->ip_id) {
	    result = 0;
	    goto out;
	}
	for (i = 0; i < cid_count; i++) {
	    if (cids[i] == ids->ip_id) {
		result = 0;
		goto out;
	    }
	}
	ids++;
    }

 out:
    if (result == 0 && ids->ip_desc)
	device_set_desc(child, ids->ip_desc);

    return_VALUE (result);
}

#if defined(__i386__) || defined(__amd64__)
/*
 * Look for a MCFG table.  If it is present, use the settings for
 * domain (segment) 0 to setup PCI config space access via the memory
 * map.
 */
static void
acpi_enable_pcie(void)
{
	ACPI_TABLE_HEADER *hdr;
	ACPI_MCFG_ALLOCATION *alloc, *end;
	ACPI_STATUS status;

	status = AcpiGetTable(ACPI_SIG_MCFG, 1, &hdr);
	if (ACPI_FAILURE(status))
		return;

	end = (ACPI_MCFG_ALLOCATION *)((char *)hdr + hdr->Length);
	alloc = (ACPI_MCFG_ALLOCATION *)((ACPI_TABLE_MCFG *)hdr + 1);
	while (alloc < end) {
		if (alloc->PciSegment == 0) {
			pcie_cfgregopen(alloc->Address, alloc->StartBusNumber,
			    alloc->EndBusNumber);
			return;
		}
		alloc++;
	}
}
#endif

/*
 * Scan all of the ACPI namespace and attach child devices.
 *
 * We should only expect to find devices in the \_PR, \_TZ, \_SI, and
 * \_SB scopes, and \_PR and \_TZ became obsolete in the ACPI 2.0 spec.
 * However, in violation of the spec, some systems place their PCI link
 * devices in \, so we have to walk the whole namespace.  We check the
 * type of namespace nodes, so this should be ok.
 */
static void
acpi_probe_children(device_t bus)
{

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    /*
     * Scan the namespace and insert placeholders for all the devices that
     * we find.  We also probe/attach any early devices.
     *
     * Note that we use AcpiWalkNamespace rather than AcpiGetDevices because
     * we want to create nodes for all devices, not just those that are
     * currently present. (This assumes that we don't want to create/remove
     * devices as they appear, which might be smarter.)
     */
    ACPI_DEBUG_PRINT((ACPI_DB_OBJECTS, "namespace scan\n"));
    AcpiWalkNamespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT, 100, acpi_probe_child,
	bus, NULL);

    /* Pre-allocate resources for our rman from any sysresource devices. */
    acpi_sysres_alloc(bus);

    /* Create any static children by calling device identify methods. */
    ACPI_DEBUG_PRINT((ACPI_DB_OBJECTS, "device identify routines\n"));
    bus_generic_probe(bus);

    /* Probe/attach all children, created staticly and from the namespace. */
    ACPI_DEBUG_PRINT((ACPI_DB_OBJECTS, "first bus_generic_attach\n"));
    bus_generic_attach(bus);

    /*
     * Some of these children may have attached others as part of their attach
     * process (eg. the root PCI bus driver), so rescan.
     */
    ACPI_DEBUG_PRINT((ACPI_DB_OBJECTS, "second bus_generic_attach\n"));
    bus_generic_attach(bus);

    /* Attach wake sysctls. */
    acpi_wake_sysctl_walk(bus);

    ACPI_DEBUG_PRINT((ACPI_DB_OBJECTS, "done attaching children\n"));
    return_VOID;
}

/*
 * Determine the probe order for a given device.
 */
static void
acpi_probe_order(ACPI_HANDLE handle, int *order)
{
    ACPI_OBJECT_TYPE type;

    /*
     * 1. I/O port and memory system resource holders
     * 2. Embedded controllers (to handle early accesses)
     * 3. PCI Link Devices
     * 100000. CPUs
     */
    AcpiGetType(handle, &type);
    if (acpi_MatchHid(handle, "PNP0C01") || acpi_MatchHid(handle, "PNP0C02"))
	*order = 1;
    else if (acpi_MatchHid(handle, "PNP0C09"))
	*order = 2;
    else if (acpi_MatchHid(handle, "PNP0C0F"))
	*order = 3;
    else if (type == ACPI_TYPE_PROCESSOR)
	*order = 100000;
}

/*
 * Evaluate a child device and determine whether we might attach a device to
 * it.
 */
static ACPI_STATUS
acpi_probe_child(ACPI_HANDLE handle, UINT32 level, void *context, void **status)
{
    ACPI_OBJECT_TYPE type;
    ACPI_HANDLE h;
    device_t bus, child;
    int order;
    char *handle_str, **search;
    static char *scopes[] = {"\\_PR_", "\\_TZ_", "\\_SI_", "\\_SB_", NULL};

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    /* Skip this device if we think we'll have trouble with it. */
    if (acpi_avoid(handle))
	return_ACPI_STATUS (AE_OK);

    bus = (device_t)context;
    if (ACPI_SUCCESS(AcpiGetType(handle, &type))) {
	switch (type) {
	case ACPI_TYPE_DEVICE:
	case ACPI_TYPE_PROCESSOR:
	case ACPI_TYPE_THERMAL:
	case ACPI_TYPE_POWER:
	    if (acpi_disabled("children"))
		break;

	    /*
	     * Since we scan from \, be sure to skip system scope objects.
	     * At least \_SB and \_TZ are detected as devices (ACPI-CA bug?)
	     */
	    handle_str = acpi_name(handle);
	    for (search = scopes; *search != NULL; search++) {
		if (strcmp(handle_str, *search) == 0)
		    break;
	    }
	    if (*search != NULL)
		break;

	    /* 
	     * Create a placeholder device for this node.  Sort the
	     * placeholder so that the probe/attach passes will run
	     * breadth-first.  Orders less than ACPI_DEV_BASE_ORDER
	     * are reserved for special objects (i.e., system
	     * resources).  CPU devices have a very high order to
	     * ensure they are probed after other devices.
	     */
	    ACPI_DEBUG_PRINT((ACPI_DB_OBJECTS, "scanning '%s'\n", handle_str));
	    order = level * 10 + 100;
	    acpi_probe_order(handle, &order);
	    child = BUS_ADD_CHILD(bus, order, NULL, -1);
	    if (child == NULL)
		break;

	    /* Associate the handle with the device_t and vice versa. */
	    acpi_set_handle(child, handle);
	    AcpiAttachData(handle, acpi_fake_objhandler, child);

	    /*
	     * Check that the device is present.  If it's not present,
	     * leave it disabled (so that we have a device_t attached to
	     * the handle, but we don't probe it).
	     *
	     * XXX PCI link devices sometimes report "present" but not
	     * "functional" (i.e. if disabled).  Go ahead and probe them
	     * anyway since we may enable them later.
	     */
	    if (type == ACPI_TYPE_DEVICE && !acpi_DeviceIsPresent(child)) {
		/* Never disable PCI link devices. */
		if (acpi_MatchHid(handle, "PNP0C0F"))
		    break;
		/*
		 * Docking stations should remain enabled since the system
		 * may be undocked at boot.
		 */
		if (ACPI_SUCCESS(AcpiGetHandle(handle, "_DCK", &h)))
		    break;

		device_disable(child);
		break;
	    }

	    /*
	     * Get the device's resource settings and attach them.
	     * Note that if the device has _PRS but no _CRS, we need
	     * to decide when it's appropriate to try to configure the
	     * device.  Ignore the return value here; it's OK for the
	     * device not to have any resources.
	     */
	    acpi_parse_resources(child, handle, &acpi_res_parse_set, NULL);
	    break;
	}
    }

    return_ACPI_STATUS (AE_OK);
}

/*
 * AcpiAttachData() requires an object handler but never uses it.  This is a
 * placeholder object handler so we can store a device_t in an ACPI_HANDLE.
 */
void
acpi_fake_objhandler(ACPI_HANDLE h, UINT32 fn, void *data)
{
}

static void
acpi_shutdown_final(void *arg, int howto)
{
    struct acpi_softc *sc = (struct acpi_softc *)arg;
    ACPI_STATUS status;

    /*
     * XXX Shutdown code should only run on the BSP (cpuid 0).
     * Some chipsets do not power off the system correctly if called from
     * an AP.
     */
    if ((howto & RB_POWEROFF) != 0) {
	status = AcpiEnterSleepStatePrep(ACPI_STATE_S5);
	if (ACPI_FAILURE(status)) {
	    device_printf(sc->acpi_dev, "AcpiEnterSleepStatePrep failed - %s\n",
		AcpiFormatException(status));
	    return;
	}
	device_printf(sc->acpi_dev, "Powering system off\n");
	ACPI_DISABLE_IRQS();
	status = AcpiEnterSleepState(ACPI_STATE_S5);
	if (ACPI_FAILURE(status))
	    device_printf(sc->acpi_dev, "power-off failed - %s\n",
		AcpiFormatException(status));
	else {
	    DELAY(1000000);
	    device_printf(sc->acpi_dev, "power-off failed - timeout\n");
	}
    } else if ((howto & RB_HALT) == 0 &&
	(AcpiGbl_FADT.Flags & ACPI_FADT_RESET_REGISTER) &&
	sc->acpi_handle_reboot) {
	/* Reboot using the reset register. */
	status = AcpiHwLowLevelWrite(
	    AcpiGbl_FADT.ResetRegister.BitWidth,
	    AcpiGbl_FADT.ResetValue, &AcpiGbl_FADT.ResetRegister);
	if (ACPI_FAILURE(status))
	    device_printf(sc->acpi_dev, "reset failed - %s\n",
		AcpiFormatException(status));
	else {
	    DELAY(1000000);
	    device_printf(sc->acpi_dev, "reset failed - timeout\n");
	}
    } else if (sc->acpi_do_disable && panicstr == NULL) {
	/*
	 * Only disable ACPI if the user requested.  On some systems, writing
	 * the disable value to SMI_CMD hangs the system.
	 */
	device_printf(sc->acpi_dev, "Shutting down\n");
	AcpiTerminate();
    }
}

static void
acpi_enable_fixed_events(struct acpi_softc *sc)
{
    static int	first_time = 1;

    /* Enable and clear fixed events and install handlers. */
    if ((AcpiGbl_FADT.Flags & ACPI_FADT_POWER_BUTTON) == 0) {
	AcpiClearEvent(ACPI_EVENT_POWER_BUTTON);
	AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON,
				     acpi_event_power_button_sleep, sc);
	if (first_time)
	    device_printf(sc->acpi_dev, "Power Button (fixed)\n");
    }
    if ((AcpiGbl_FADT.Flags & ACPI_FADT_SLEEP_BUTTON) == 0) {
	AcpiClearEvent(ACPI_EVENT_SLEEP_BUTTON);
	AcpiInstallFixedEventHandler(ACPI_EVENT_SLEEP_BUTTON,
				     acpi_event_sleep_button_sleep, sc);
	if (first_time)
	    device_printf(sc->acpi_dev, "Sleep Button (fixed)\n");
    }

    first_time = 0;
}

/*
 * Returns true if the device is actually present and should
 * be attached to.  This requires the present, enabled, UI-visible 
 * and diagnostics-passed bits to be set.
 */
BOOLEAN
acpi_DeviceIsPresent(device_t dev)
{
    ACPI_DEVICE_INFO	*devinfo;
    ACPI_HANDLE		h;
    ACPI_BUFFER		buf;
    ACPI_STATUS		error;
    int			ret;

    ret = FALSE;
    if ((h = acpi_get_handle(dev)) == NULL)
	return (FALSE);
    buf.Pointer = NULL;
    buf.Length = ACPI_ALLOCATE_BUFFER;
    error = AcpiGetObjectInfo(h, &buf);
    if (ACPI_FAILURE(error))
	return (FALSE);
    devinfo = (ACPI_DEVICE_INFO *)buf.Pointer;

    /* If no _STA method, must be present */
    if ((devinfo->Valid & ACPI_VALID_STA) == 0)
	ret = TRUE;

    /* Return true for 'present' and 'functioning' */
    if (ACPI_DEVICE_PRESENT(devinfo->CurrentStatus))
	ret = TRUE;

    AcpiOsFree(buf.Pointer);
    return (ret);
}

/*
 * Returns true if the battery is actually present and inserted.
 */
BOOLEAN
acpi_BatteryIsPresent(device_t dev)
{
    ACPI_DEVICE_INFO	*devinfo;
    ACPI_HANDLE		h;
    ACPI_BUFFER		buf;
    ACPI_STATUS		error;
    int			ret;

    ret = FALSE;
    if ((h = acpi_get_handle(dev)) == NULL)
	return (FALSE);
    buf.Pointer = NULL;
    buf.Length = ACPI_ALLOCATE_BUFFER;
    error = AcpiGetObjectInfo(h, &buf);
    if (ACPI_FAILURE(error))
	return (FALSE);
    devinfo = (ACPI_DEVICE_INFO *)buf.Pointer;

    /* If no _STA method, must be present */
    if ((devinfo->Valid & ACPI_VALID_STA) == 0)
	ret = TRUE;

    /* Return true for 'present', 'battery present', and 'functioning' */
    if (ACPI_BATTERY_PRESENT(devinfo->CurrentStatus))
	ret = TRUE;

    AcpiOsFree(buf.Pointer);
    return (ret);
}

/*
 * Match a HID string against a handle
 */
static BOOLEAN
acpi_MatchHid(ACPI_HANDLE h, const char *hid) 
{
    ACPI_DEVICE_INFO	*devinfo;
    ACPI_BUFFER		buf;
    ACPI_STATUS		error;
    int			ret, i;

    ret = FALSE;
    if (hid == NULL || h == NULL)
	return (ret);
    buf.Pointer = NULL;
    buf.Length = ACPI_ALLOCATE_BUFFER;
    error = AcpiGetObjectInfo(h, &buf);
    if (ACPI_FAILURE(error))
	return (ret);
    devinfo = (ACPI_DEVICE_INFO *)buf.Pointer;

    if ((devinfo->Valid & ACPI_VALID_HID) != 0 &&
	strcmp(hid, devinfo->HardwareId.Value) == 0)
	    ret = TRUE;
    else if ((devinfo->Valid & ACPI_VALID_CID) != 0) {
	for (i = 0; i < devinfo->CompatibilityId.Count; i++) {
	    if (strcmp(hid, devinfo->CompatibilityId.Id[i].Value) == 0) {
		ret = TRUE;
		break;
	    }
	}
    }

    AcpiOsFree(buf.Pointer);
    return (ret);
}

/*
 * Return the handle of a named object within our scope, ie. that of (parent)
 * or one if its parents.
 */
ACPI_STATUS
acpi_GetHandleInScope(ACPI_HANDLE parent, char *path, ACPI_HANDLE *result)
{
    ACPI_HANDLE		r;
    ACPI_STATUS		status;

    /* Walk back up the tree to the root */
    for (;;) {
	status = AcpiGetHandle(parent, path, &r);
	if (ACPI_SUCCESS(status)) {
	    *result = r;
	    return (AE_OK);
	}
	/* XXX Return error here? */
	if (status != AE_NOT_FOUND)
	    return (AE_OK);
	if (ACPI_FAILURE(AcpiGetParent(parent, &r)))
	    return (AE_NOT_FOUND);
	parent = r;
    }
}

/* Find the difference between two PM tick counts. */
uint32_t
acpi_TimerDelta(uint32_t end, uint32_t start)
{
    uint32_t delta;

    if (end >= start)
	delta = end - start;
    else if (AcpiGbl_FADT.Flags & ACPI_FADT_32BIT_TIMER)
	delta = ((0xFFFFFFFF - start) + end + 1);
    else
	delta = ((0x00FFFFFF - start) + end + 1) & 0x00FFFFFF;
    return (delta);
}

/*
 * Allocate a buffer with a preset data size.
 */
ACPI_BUFFER *
acpi_AllocBuffer(int size)
{
    ACPI_BUFFER	*buf;

    if ((buf = malloc(size + sizeof(*buf), M_ACPIDEV, M_NOWAIT)) == NULL)
	return (NULL);
    buf->Length = size;
    buf->Pointer = (void *)(buf + 1);
    return (buf);
}

ACPI_STATUS
acpi_SetInteger(ACPI_HANDLE handle, char *path, UINT32 number)
{
    ACPI_OBJECT arg1;
    ACPI_OBJECT_LIST args;

    arg1.Type = ACPI_TYPE_INTEGER;
    arg1.Integer.Value = number;
    args.Count = 1;
    args.Pointer = &arg1;

    return (AcpiEvaluateObject(handle, path, &args, NULL));
}

/*
 * Evaluate a path that should return an integer.
 */
ACPI_STATUS
acpi_GetInteger(ACPI_HANDLE handle, char *path, UINT32 *number)
{
    ACPI_STATUS	status;
    ACPI_BUFFER	buf;
    ACPI_OBJECT	param;

    if (handle == NULL)
	handle = ACPI_ROOT_OBJECT;

    /*
     * Assume that what we've been pointed at is an Integer object, or
     * a method that will return an Integer.
     */
    buf.Pointer = &param;
    buf.Length = sizeof(param);
    status = AcpiEvaluateObject(handle, path, NULL, &buf);
    if (ACPI_SUCCESS(status)) {
	if (param.Type == ACPI_TYPE_INTEGER)
	    *number = param.Integer.Value;
	else
	    status = AE_TYPE;
    }

    /* 
     * In some applications, a method that's expected to return an Integer
     * may instead return a Buffer (probably to simplify some internal
     * arithmetic).  We'll try to fetch whatever it is, and if it's a Buffer,
     * convert it into an Integer as best we can.
     *
     * This is a hack.
     */
    if (status == AE_BUFFER_OVERFLOW) {
	if ((buf.Pointer = AcpiOsAllocate(buf.Length)) == NULL) {
	    status = AE_NO_MEMORY;
	} else {
	    status = AcpiEvaluateObject(handle, path, NULL, &buf);
	    if (ACPI_SUCCESS(status))
		status = acpi_ConvertBufferToInteger(&buf, number);
	    AcpiOsFree(buf.Pointer);
	}
    }
    return (status);
}

ACPI_STATUS
acpi_ConvertBufferToInteger(ACPI_BUFFER *bufp, UINT32 *number)
{
    ACPI_OBJECT	*p;
    UINT8	*val;
    int		i;

    p = (ACPI_OBJECT *)bufp->Pointer;
    if (p->Type == ACPI_TYPE_INTEGER) {
	*number = p->Integer.Value;
	return (AE_OK);
    }
    if (p->Type != ACPI_TYPE_BUFFER)
	return (AE_TYPE);
    if (p->Buffer.Length > sizeof(int))
	return (AE_BAD_DATA);

    *number = 0;
    val = p->Buffer.Pointer;
    for (i = 0; i < p->Buffer.Length; i++)
	*number += val[i] << (i * 8);
    return (AE_OK);
}

/*
 * Iterate over the elements of an a package object, calling the supplied
 * function for each element.
 *
 * XXX possible enhancement might be to abort traversal on error.
 */
ACPI_STATUS
acpi_ForeachPackageObject(ACPI_OBJECT *pkg,
	void (*func)(ACPI_OBJECT *comp, void *arg), void *arg)
{
    ACPI_OBJECT	*comp;
    int		i;

    if (pkg == NULL || pkg->Type != ACPI_TYPE_PACKAGE)
	return (AE_BAD_PARAMETER);

    /* Iterate over components */
    i = 0;
    comp = pkg->Package.Elements;
    for (; i < pkg->Package.Count; i++, comp++)
	func(comp, arg);

    return (AE_OK);
}

/*
 * Find the (index)th resource object in a set.
 */
ACPI_STATUS
acpi_FindIndexedResource(ACPI_BUFFER *buf, int index, ACPI_RESOURCE **resp)
{
    ACPI_RESOURCE	*rp;
    int			i;

    rp = (ACPI_RESOURCE *)buf->Pointer;
    i = index;
    while (i-- > 0) {
	/* Range check */
	if (rp > (ACPI_RESOURCE *)((u_int8_t *)buf->Pointer + buf->Length))
	    return (AE_BAD_PARAMETER);

	/* Check for terminator */
	if (rp->Type == ACPI_RESOURCE_TYPE_END_TAG || rp->Length == 0)
	    return (AE_NOT_FOUND);
	rp = ACPI_NEXT_RESOURCE(rp);
    }
    if (resp != NULL)
	*resp = rp;

    return (AE_OK);
}

/*
 * Append an ACPI_RESOURCE to an ACPI_BUFFER.
 *
 * Given a pointer to an ACPI_RESOURCE structure, expand the ACPI_BUFFER
 * provided to contain it.  If the ACPI_BUFFER is empty, allocate a sensible
 * backing block.  If the ACPI_RESOURCE is NULL, return an empty set of
 * resources.
 */
#define ACPI_INITIAL_RESOURCE_BUFFER_SIZE	512

ACPI_STATUS
acpi_AppendBufferResource(ACPI_BUFFER *buf, ACPI_RESOURCE *res)
{
    ACPI_RESOURCE	*rp;
    void		*newp;

    /* Initialise the buffer if necessary. */
    if (buf->Pointer == NULL) {
	buf->Length = ACPI_INITIAL_RESOURCE_BUFFER_SIZE;
	if ((buf->Pointer = AcpiOsAllocate(buf->Length)) == NULL)
	    return (AE_NO_MEMORY);
	rp = (ACPI_RESOURCE *)buf->Pointer;
	rp->Type = ACPI_RESOURCE_TYPE_END_TAG;
	rp->Length = 0;
    }
    if (res == NULL)
	return (AE_OK);

    /*
     * Scan the current buffer looking for the terminator.
     * This will either find the terminator or hit the end
     * of the buffer and return an error.
     */
    rp = (ACPI_RESOURCE *)buf->Pointer;
    for (;;) {
	/* Range check, don't go outside the buffer */
	if (rp >= (ACPI_RESOURCE *)((u_int8_t *)buf->Pointer + buf->Length))
	    return (AE_BAD_PARAMETER);
	if (rp->Type == ACPI_RESOURCE_TYPE_END_TAG || rp->Length == 0)
	    break;
	rp = ACPI_NEXT_RESOURCE(rp);
    }

    /*
     * Check the size of the buffer and expand if required.
     *
     * Required size is:
     *	size of existing resources before terminator + 
     *	size of new resource and header +
     * 	size of terminator.
     *
     * Note that this loop should really only run once, unless
     * for some reason we are stuffing a *really* huge resource.
     */
    while ((((u_int8_t *)rp - (u_int8_t *)buf->Pointer) + 
	    res->Length + ACPI_RS_SIZE_NO_DATA +
	    ACPI_RS_SIZE_MIN) >= buf->Length) {
	if ((newp = AcpiOsAllocate(buf->Length * 2)) == NULL)
	    return (AE_NO_MEMORY);
	bcopy(buf->Pointer, newp, buf->Length);
	rp = (ACPI_RESOURCE *)((u_int8_t *)newp +
			       ((u_int8_t *)rp - (u_int8_t *)buf->Pointer));
	AcpiOsFree(buf->Pointer);
	buf->Pointer = newp;
	buf->Length += buf->Length;
    }

    /* Insert the new resource. */
    bcopy(res, rp, res->Length + ACPI_RS_SIZE_NO_DATA);

    /* And add the terminator. */
    rp = ACPI_NEXT_RESOURCE(rp);
    rp->Type = ACPI_RESOURCE_TYPE_END_TAG;
    rp->Length = 0;

    return (AE_OK);
}

/*
 * Set interrupt model.
 */
ACPI_STATUS
acpi_SetIntrModel(int model)
{

    return (acpi_SetInteger(ACPI_ROOT_OBJECT, "_PIC", model));
}

/*
 * DEPRECATED.  This interface has serious deficiencies and will be
 * removed.
 *
 * Immediately enter the sleep state.  In the old model, acpiconf(8) ran
 * rc.suspend and rc.resume so we don't have to notify devd(8) to do this.
 */
ACPI_STATUS
acpi_SetSleepState(struct acpi_softc *sc, int state)
{
    static int once;

    if (!once) {
	device_printf(sc->acpi_dev,
"warning: acpi_SetSleepState() deprecated, need to update your software\n");
	once = 1;
    }
    return (acpi_EnterSleepState(sc, state));
}

#if defined(__amd64__) || defined(__i386__)
static void
acpi_sleep_force(void *arg)
{
    struct acpi_softc *sc = (struct acpi_softc *)arg;

    device_printf(sc->acpi_dev,
	"suspend request timed out, forcing sleep now\n");
    if (ACPI_FAILURE(acpi_EnterSleepState(sc, sc->acpi_next_sstate)))
	device_printf(sc->acpi_dev, "force sleep state S%d failed\n",
	    sc->acpi_next_sstate);
}
#endif

/*
 * Request that the system enter the given suspend state.  All /dev/apm
 * devices and devd(8) will be notified.  Userland then has a chance to
 * save state and acknowledge the request.  The system sleeps once all
 * acks are in.
 */
int
acpi_ReqSleepState(struct acpi_softc *sc, int state)
{
#if defined(__i386__)
    struct apm_clone_data *clone;
#endif

    if (state < ACPI_STATE_S1 || state > ACPI_S_STATES_MAX)
	return (EINVAL);
    if (!acpi_sleep_states[state])
	return (EOPNOTSUPP);

    /* S5 (soft-off) should be entered directly with no waiting. */
    if (state == ACPI_STATE_S5) {
	if (ACPI_SUCCESS(acpi_EnterSleepState(sc, state)))
	    return (0);
	else
	    return (ENXIO);
    }

#if defined(__amd64__) || defined(__i386__)
    /* If a suspend request is already in progress, just return. */
    ACPI_LOCK(acpi);
    if (sc->acpi_next_sstate != 0) {
    	ACPI_UNLOCK(acpi);
	return (0);
    }

    /* Record the pending state and notify all apm devices. */
    sc->acpi_next_sstate = state;
#if defined(__i386__)
    STAILQ_FOREACH(clone, &sc->apm_cdevs, entries) {
	clone->notify_status = APM_EV_NONE;
	if ((clone->flags & ACPI_EVF_DEVD) == 0) {
	    selwakeuppri(&clone->sel_read, PZERO);
	    KNOTE_UNLOCKED(&clone->sel_read.si_note, 0);
	}
    }
#endif

    /* If devd(8) is not running, immediately enter the sleep state. */
    if (!devctl_process_running()) {
	ACPI_UNLOCK(acpi);
	if (ACPI_SUCCESS(acpi_EnterSleepState(sc, sc->acpi_next_sstate))) {
	    return (0);
	} else {
	    return (ENXIO);
	}
    }

    /*
     * Set a timeout to fire if userland doesn't ack the suspend request
     * in time.  This way we still eventually go to sleep if we were
     * overheating or running low on battery, even if userland is hung.
     * We cancel this timeout once all userland acks are in or the
     * suspend request is aborted.
     */
    callout_reset(&sc->susp_force_to, 10 * hz, acpi_sleep_force, sc);
    ACPI_UNLOCK(acpi);

    /* Now notify devd(8) also. */
    acpi_UserNotify("Suspend", ACPI_ROOT_OBJECT, state);

    return (0);
#else
    /* This platform does not support acpi suspend/resume. */
    return (EOPNOTSUPP);
#endif
}

/*
 * Acknowledge (or reject) a pending sleep state.  The caller has
 * prepared for suspend and is now ready for it to proceed.  If the
 * error argument is non-zero, it indicates suspend should be cancelled
 * and gives an errno value describing why.  Once all votes are in,
 * we suspend the system.
 */
int
acpi_AckSleepState(struct apm_clone_data *clone, int error)
{
#if defined(__amd64__) || defined(__i386__)
    struct acpi_softc *sc;
    int ret, sleeping;

    /* If no pending sleep state, return an error. */
    ACPI_LOCK(acpi);
    sc = clone->acpi_sc;
    if (sc->acpi_next_sstate == 0) {
    	ACPI_UNLOCK(acpi);
	return (ENXIO);
    }

    /* Caller wants to abort suspend process. */
    if (error) {
	sc->acpi_next_sstate = 0;
	callout_stop(&sc->susp_force_to);
	device_printf(sc->acpi_dev,
	    "listener on %s cancelled the pending suspend\n",
	    devtoname(clone->cdev));
    	ACPI_UNLOCK(acpi);
	return (0);
    }

    /*
     * Mark this device as acking the suspend request.  Then, walk through
     * all devices, seeing if they agree yet.  We only count devices that
     * are writable since read-only devices couldn't ack the request.
     */
    sleeping = TRUE;
#if defined(__i386__)
    clone->notify_status = APM_EV_ACKED;
    STAILQ_FOREACH(clone, &sc->apm_cdevs, entries) {
	if ((clone->flags & ACPI_EVF_WRITE) != 0 &&
	    clone->notify_status != APM_EV_ACKED) {
	    sleeping = FALSE;
	    break;
	}
    }
#endif

    /* If all devices have voted "yes", we will suspend now. */
    if (sleeping)
	callout_stop(&sc->susp_force_to);
    ACPI_UNLOCK(acpi);
    ret = 0;
    if (sleeping) {
	if (ACPI_FAILURE(acpi_EnterSleepState(sc, sc->acpi_next_sstate)))
		ret = ENODEV;
    }
    return (ret);
#else
    /* This platform does not support acpi suspend/resume. */
    return (EOPNOTSUPP);
#endif
}

static void
acpi_sleep_enable(void *arg)
{
    struct acpi_softc	*sc = (struct acpi_softc *)arg;

    /* Reschedule if the system is not fully up and running. */
    if (!AcpiGbl_SystemAwakeAndRunning) {
	timeout(acpi_sleep_enable, sc, hz * ACPI_MINIMUM_AWAKETIME);
	return;
    }

    ACPI_LOCK(acpi);
    sc->acpi_sleep_disabled = FALSE;
    ACPI_UNLOCK(acpi);
}

static ACPI_STATUS
acpi_sleep_disable(struct acpi_softc *sc)
{
    ACPI_STATUS		status;

    /* Fail if the system is not fully up and running. */
    if (!AcpiGbl_SystemAwakeAndRunning)
	return (AE_ERROR);

    ACPI_LOCK(acpi);
    status = sc->acpi_sleep_disabled ? AE_ERROR : AE_OK;
    sc->acpi_sleep_disabled = TRUE;
    ACPI_UNLOCK(acpi);

    return (status);
}

enum acpi_sleep_state {
    ACPI_SS_NONE,
    ACPI_SS_GPE_SET,
    ACPI_SS_DEV_SUSPEND,
    ACPI_SS_SLP_PREP,
    ACPI_SS_SLEPT,
};

/*
 * Enter the desired system sleep state.
 *
 * Currently we support S1-S5 but S4 is only S4BIOS
 */
static ACPI_STATUS
acpi_EnterSleepState(struct acpi_softc *sc, int state)
{
    ACPI_STATUS	status;
    enum acpi_sleep_state slp_state;

    ACPI_FUNCTION_TRACE_U32((char *)(uintptr_t)__func__, state);

    if (state < ACPI_STATE_S1 || state > ACPI_S_STATES_MAX)
	return_ACPI_STATUS (AE_BAD_PARAMETER);
    if (!acpi_sleep_states[state]) {
	device_printf(sc->acpi_dev, "Sleep state S%d not supported by BIOS\n",
	    state);
	return (AE_SUPPORT);
    }

    /* Re-entry once we're suspending is not allowed. */
    status = acpi_sleep_disable(sc);
    if (ACPI_FAILURE(status)) {
	device_printf(sc->acpi_dev,
	    "suspend request ignored (not ready yet)\n");
	return (status);
    }

    if (state == ACPI_STATE_S5) {
	/*
	 * Shut down cleanly and power off.  This will call us back through the
	 * shutdown handlers.
	 */
	shutdown_nice(RB_POWEROFF);
	return_ACPI_STATUS (AE_OK);
    }

#ifdef SMP
    thread_lock(curthread);
    sched_bind(curthread, 0);
    thread_unlock(curthread);
#endif

    /*
     * Be sure to hold Giant across DEVICE_SUSPEND/RESUME since non-MPSAFE
     * drivers need this.
     */
    mtx_lock(&Giant);

    slp_state = ACPI_SS_NONE;

    sc->acpi_sstate = state;

    /* Enable any GPEs as appropriate and requested by the user. */
    acpi_wake_prep_walk(state);
    slp_state = ACPI_SS_GPE_SET;

    /*
     * Inform all devices that we are going to sleep.  If at least one
     * device fails, DEVICE_SUSPEND() automatically resumes the tree.
     *
     * XXX Note that a better two-pass approach with a 'veto' pass
     * followed by a "real thing" pass would be better, but the current
     * bus interface does not provide for this.
     */
    if (DEVICE_SUSPEND(root_bus) != 0) {
	device_printf(sc->acpi_dev, "device_suspend failed\n");
	goto backout;
    }
    slp_state = ACPI_SS_DEV_SUSPEND;

    /* If testing device suspend only, back out of everything here. */
    if (acpi_susp_bounce)
	goto backout;

    status = AcpiEnterSleepStatePrep(state);
    if (ACPI_FAILURE(status)) {
	device_printf(sc->acpi_dev, "AcpiEnterSleepStatePrep failed - %s\n",
		      AcpiFormatException(status));
	goto backout;
    }
    slp_state = ACPI_SS_SLP_PREP;

    if (sc->acpi_sleep_delay > 0)
	DELAY(sc->acpi_sleep_delay * 1000000);

    if (state != ACPI_STATE_S1) {
	acpi_sleep_machdep(sc, state);

	/* Re-enable ACPI hardware on wakeup from sleep state 4. */
	if (state == ACPI_STATE_S4)
	    AcpiEnable();
    } else {
	ACPI_DISABLE_IRQS();
	status = AcpiEnterSleepState(state);
	if (ACPI_FAILURE(status)) {
	    device_printf(sc->acpi_dev, "AcpiEnterSleepState failed - %s\n",
			  AcpiFormatException(status));
	    goto backout;
	}
    }
    slp_state = ACPI_SS_SLEPT;

    /*
     * Back out state according to how far along we got in the suspend
     * process.  This handles both the error and success cases.
     */
backout:
    sc->acpi_next_sstate = 0;
    if (slp_state >= ACPI_SS_GPE_SET) {
	acpi_wake_prep_walk(state);
	sc->acpi_sstate = ACPI_STATE_S0;
    }
    if (slp_state >= ACPI_SS_SLP_PREP)
	AcpiLeaveSleepState(state);
    if (slp_state >= ACPI_SS_DEV_SUSPEND)
	DEVICE_RESUME(root_bus);
    if (slp_state >= ACPI_SS_SLEPT)
	acpi_enable_fixed_events(sc);

    mtx_unlock(&Giant);

#ifdef SMP
    thread_lock(curthread);
    sched_unbind(curthread);
    thread_unlock(curthread);
#endif

    /* Allow another sleep request after a while. */
    timeout(acpi_sleep_enable, sc, hz * ACPI_MINIMUM_AWAKETIME);

    /* Run /etc/rc.resume after we are back. */
    if (devctl_process_running())
	acpi_UserNotify("Resume", ACPI_ROOT_OBJECT, state);

    return_ACPI_STATUS (status);
}

void
acpi_resync_clock(struct acpi_softc *sc)
{

    if (!acpi_reset_clock)
	return;

    /*
     * Warm up timecounter again and reset system clock.
     */
    (void)timecounter->tc_get_timecount(timecounter);
    (void)timecounter->tc_get_timecount(timecounter);
    inittodr(time_second + sc->acpi_sleep_delay);
}

/* Initialize a device's wake GPE. */
int
acpi_wake_init(device_t dev, int type)
{
    struct acpi_prw_data prw;

    /* Evaluate _PRW to find the GPE. */
    if (acpi_parse_prw(acpi_get_handle(dev), &prw) != 0)
	return (ENXIO);

    /* Set the requested type for the GPE (runtime, wake, or both). */
    if (ACPI_FAILURE(AcpiSetGpeType(prw.gpe_handle, prw.gpe_bit, type))) {
	device_printf(dev, "set GPE type failed\n");
	return (ENXIO);
    }

    return (0);
}

/* Enable or disable the device's wake GPE. */
int
acpi_wake_set_enable(device_t dev, int enable)
{
    struct acpi_prw_data prw;
    ACPI_STATUS status;
    int flags;

    /* Make sure the device supports waking the system and get the GPE. */
    if (acpi_parse_prw(acpi_get_handle(dev), &prw) != 0)
	return (ENXIO);

    flags = acpi_get_flags(dev);
    if (enable) {
	status = AcpiEnableGpe(prw.gpe_handle, prw.gpe_bit, ACPI_NOT_ISR);
	if (ACPI_FAILURE(status)) {
	    device_printf(dev, "enable wake failed\n");
	    return (ENXIO);
	}
	acpi_set_flags(dev, flags | ACPI_FLAG_WAKE_ENABLED);
    } else {
	status = AcpiDisableGpe(prw.gpe_handle, prw.gpe_bit, ACPI_NOT_ISR);
	if (ACPI_FAILURE(status)) {
	    device_printf(dev, "disable wake failed\n");
	    return (ENXIO);
	}
	acpi_set_flags(dev, flags & ~ACPI_FLAG_WAKE_ENABLED);
    }

    return (0);
}

static int
acpi_wake_sleep_prep(ACPI_HANDLE handle, int sstate)
{
    struct acpi_prw_data prw;
    device_t dev;

    /* Check that this is a wake-capable device and get its GPE. */
    if (acpi_parse_prw(handle, &prw) != 0)
	return (ENXIO);
    dev = acpi_get_device(handle);

    /*
     * The destination sleep state must be less than (i.e., higher power)
     * or equal to the value specified by _PRW.  If this GPE cannot be
     * enabled for the next sleep state, then disable it.  If it can and
     * the user requested it be enabled, turn on any required power resources
     * and set _PSW.
     */
    if (sstate > prw.lowest_wake) {
	AcpiDisableGpe(prw.gpe_handle, prw.gpe_bit, ACPI_NOT_ISR);
	if (bootverbose)
	    device_printf(dev, "wake_prep disabled wake for %s (S%d)\n",
		acpi_name(handle), sstate);
    } else if (dev && (acpi_get_flags(dev) & ACPI_FLAG_WAKE_ENABLED) != 0) {
	acpi_pwr_wake_enable(handle, 1);
	acpi_SetInteger(handle, "_PSW", 1);
	if (bootverbose)
	    device_printf(dev, "wake_prep enabled for %s (S%d)\n",
		acpi_name(handle), sstate);
    }

    return (0);
}

static int
acpi_wake_run_prep(ACPI_HANDLE handle, int sstate)
{
    struct acpi_prw_data prw;
    device_t dev;

    /*
     * Check that this is a wake-capable device and get its GPE.  Return
     * now if the user didn't enable this device for wake.
     */
    if (acpi_parse_prw(handle, &prw) != 0)
	return (ENXIO);
    dev = acpi_get_device(handle);
    if (dev == NULL || (acpi_get_flags(dev) & ACPI_FLAG_WAKE_ENABLED) == 0)
	return (0);

    /*
     * If this GPE couldn't be enabled for the previous sleep state, it was
     * disabled before going to sleep so re-enable it.  If it was enabled,
     * clear _PSW and turn off any power resources it used.
     */
    if (sstate > prw.lowest_wake) {
	AcpiEnableGpe(prw.gpe_handle, prw.gpe_bit, ACPI_NOT_ISR);
	if (bootverbose)
	    device_printf(dev, "run_prep re-enabled %s\n", acpi_name(handle));
    } else {
	acpi_SetInteger(handle, "_PSW", 0);
	acpi_pwr_wake_enable(handle, 0);
	if (bootverbose)
	    device_printf(dev, "run_prep cleaned up for %s\n",
		acpi_name(handle));
    }

    return (0);
}

static ACPI_STATUS
acpi_wake_prep(ACPI_HANDLE handle, UINT32 level, void *context, void **status)
{
    int sstate;

    /* If suspending, run the sleep prep function, otherwise wake. */
    sstate = *(int *)context;
    if (AcpiGbl_SystemAwakeAndRunning)
	acpi_wake_sleep_prep(handle, sstate);
    else
	acpi_wake_run_prep(handle, sstate);
    return (AE_OK);
}

/* Walk the tree rooted at acpi0 to prep devices for suspend/resume. */
static int
acpi_wake_prep_walk(int sstate)
{
    ACPI_HANDLE sb_handle;

    if (ACPI_SUCCESS(AcpiGetHandle(ACPI_ROOT_OBJECT, "\\_SB_", &sb_handle)))
	AcpiWalkNamespace(ACPI_TYPE_DEVICE, sb_handle, 100,
	    acpi_wake_prep, &sstate, NULL);
    return (0);
}

/* Walk the tree rooted at acpi0 to attach per-device wake sysctls. */
static int
acpi_wake_sysctl_walk(device_t dev)
{
    int error, i, numdevs;
    device_t *devlist;
    device_t child;
    ACPI_STATUS status;

    error = device_get_children(dev, &devlist, &numdevs);
    if (error != 0 || numdevs == 0) {
	if (numdevs == 0)
	    free(devlist, M_TEMP);
	return (error);
    }
    for (i = 0; i < numdevs; i++) {
	child = devlist[i];
	acpi_wake_sysctl_walk(child);
	if (!device_is_attached(child))
	    continue;
	status = AcpiEvaluateObject(acpi_get_handle(child), "_PRW", NULL, NULL);
	if (ACPI_SUCCESS(status)) {
	    SYSCTL_ADD_PROC(device_get_sysctl_ctx(child),
		SYSCTL_CHILDREN(device_get_sysctl_tree(child)), OID_AUTO,
		"wake", CTLTYPE_INT | CTLFLAG_RW, child, 0,
		acpi_wake_set_sysctl, "I", "Device set to wake the system");
	}
    }
    free(devlist, M_TEMP);

    return (0);
}

/* Enable or disable wake from userland. */
static int
acpi_wake_set_sysctl(SYSCTL_HANDLER_ARGS)
{
    int enable, error;
    device_t dev;

    dev = (device_t)arg1;
    enable = (acpi_get_flags(dev) & ACPI_FLAG_WAKE_ENABLED) ? 1 : 0;

    error = sysctl_handle_int(oidp, &enable, 0, req);
    if (error != 0 || req->newptr == NULL)
	return (error);
    if (enable != 0 && enable != 1)
	return (EINVAL);

    return (acpi_wake_set_enable(dev, enable));
}

/* Parse a device's _PRW into a structure. */
int
acpi_parse_prw(ACPI_HANDLE h, struct acpi_prw_data *prw)
{
    ACPI_STATUS			status;
    ACPI_BUFFER			prw_buffer;
    ACPI_OBJECT			*res, *res2;
    int				error, i, power_count;

    if (h == NULL || prw == NULL)
	return (EINVAL);

    /*
     * The _PRW object (7.2.9) is only required for devices that have the
     * ability to wake the system from a sleeping state.
     */
    error = EINVAL;
    prw_buffer.Pointer = NULL;
    prw_buffer.Length = ACPI_ALLOCATE_BUFFER;
    status = AcpiEvaluateObject(h, "_PRW", NULL, &prw_buffer);
    if (ACPI_FAILURE(status))
	return (ENOENT);
    res = (ACPI_OBJECT *)prw_buffer.Pointer;
    if (res == NULL)
	return (ENOENT);
    if (!ACPI_PKG_VALID(res, 2))
	goto out;

    /*
     * Element 1 of the _PRW object:
     * The lowest power system sleeping state that can be entered while still
     * providing wake functionality.  The sleeping state being entered must
     * be less than (i.e., higher power) or equal to this value.
     */
    if (acpi_PkgInt32(res, 1, &prw->lowest_wake) != 0)
	goto out;

    /*
     * Element 0 of the _PRW object:
     */
    switch (res->Package.Elements[0].Type) {
    case ACPI_TYPE_INTEGER:
	/*
	 * If the data type of this package element is numeric, then this
	 * _PRW package element is the bit index in the GPEx_EN, in the
	 * GPE blocks described in the FADT, of the enable bit that is
	 * enabled for the wake event.
	 */
	prw->gpe_handle = NULL;
	prw->gpe_bit = res->Package.Elements[0].Integer.Value;
	error = 0;
	break;
    case ACPI_TYPE_PACKAGE:
	/*
	 * If the data type of this package element is a package, then this
	 * _PRW package element is itself a package containing two
	 * elements.  The first is an object reference to the GPE Block
	 * device that contains the GPE that will be triggered by the wake
	 * event.  The second element is numeric and it contains the bit
	 * index in the GPEx_EN, in the GPE Block referenced by the
	 * first element in the package, of the enable bit that is enabled for
	 * the wake event.
	 *
	 * For example, if this field is a package then it is of the form:
	 * Package() {\_SB.PCI0.ISA.GPE, 2}
	 */
	res2 = &res->Package.Elements[0];
	if (!ACPI_PKG_VALID(res2, 2))
	    goto out;
	prw->gpe_handle = acpi_GetReference(NULL, &res2->Package.Elements[0]);
	if (prw->gpe_handle == NULL)
	    goto out;
	if (acpi_PkgInt32(res2, 1, &prw->gpe_bit) != 0)
	    goto out;
	error = 0;
	break;
    default:
	goto out;
    }

    /* Elements 2 to N of the _PRW object are power resources. */
    power_count = res->Package.Count - 2;
    if (power_count > ACPI_PRW_MAX_POWERRES) {
	printf("ACPI device %s has too many power resources\n", acpi_name(h));
	power_count = 0;
    }
    prw->power_res_count = power_count;
    for (i = 0; i < power_count; i++)
	prw->power_res[i] = res->Package.Elements[i];

out:
    if (prw_buffer.Pointer != NULL)
	AcpiOsFree(prw_buffer.Pointer);
    return (error);
}

/*
 * ACPI Event Handlers
 */

/* System Event Handlers (registered by EVENTHANDLER_REGISTER) */

static void
acpi_system_eventhandler_sleep(void *arg, int state)
{
    struct acpi_softc *sc = (struct acpi_softc *)arg;
    int ret;

    ACPI_FUNCTION_TRACE_U32((char *)(uintptr_t)__func__, state);

    /* Check if button action is disabled or unknown. */
    if (state == ACPI_STATE_UNKNOWN)
	return;

    /* Request that the system prepare to enter the given suspend state. */
    ret = acpi_ReqSleepState(sc, state);
    if (ret != 0)
	device_printf(sc->acpi_dev,
	    "request to enter state S%d failed (err %d)\n", state, ret);

    return_VOID;
}

static void
acpi_system_eventhandler_wakeup(void *arg, int state)
{

    ACPI_FUNCTION_TRACE_U32((char *)(uintptr_t)__func__, state);

    /* Currently, nothing to do for wakeup. */

    return_VOID;
}

/* 
 * ACPICA Event Handlers (FixedEvent, also called from button notify handler)
 */
UINT32
acpi_event_power_button_sleep(void *context)
{
    struct acpi_softc	*sc = (struct acpi_softc *)context;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    EVENTHANDLER_INVOKE(acpi_sleep_event, sc->acpi_power_button_sx);

    return_VALUE (ACPI_INTERRUPT_HANDLED);
}

UINT32
acpi_event_power_button_wake(void *context)
{
    struct acpi_softc	*sc = (struct acpi_softc *)context;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    EVENTHANDLER_INVOKE(acpi_wakeup_event, sc->acpi_power_button_sx);

    return_VALUE (ACPI_INTERRUPT_HANDLED);
}

UINT32
acpi_event_sleep_button_sleep(void *context)
{
    struct acpi_softc	*sc = (struct acpi_softc *)context;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    EVENTHANDLER_INVOKE(acpi_sleep_event, sc->acpi_sleep_button_sx);

    return_VALUE (ACPI_INTERRUPT_HANDLED);
}

UINT32
acpi_event_sleep_button_wake(void *context)
{
    struct acpi_softc	*sc = (struct acpi_softc *)context;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    EVENTHANDLER_INVOKE(acpi_wakeup_event, sc->acpi_sleep_button_sx);

    return_VALUE (ACPI_INTERRUPT_HANDLED);
}

/*
 * XXX This static buffer is suboptimal.  There is no locking so only
 * use this for single-threaded callers.
 */
char *
acpi_name(ACPI_HANDLE handle)
{
    ACPI_BUFFER buf;
    static char data[256];

    buf.Length = sizeof(data);
    buf.Pointer = data;

    if (handle && ACPI_SUCCESS(AcpiGetName(handle, ACPI_FULL_PATHNAME, &buf)))
	return (data);
    return ("(unknown)");
}

/*
 * Debugging/bug-avoidance.  Avoid trying to fetch info on various
 * parts of the namespace.
 */
int
acpi_avoid(ACPI_HANDLE handle)
{
    char	*cp, *env, *np;
    int		len;

    np = acpi_name(handle);
    if (*np == '\\')
	np++;
    if ((env = getenv("debug.acpi.avoid")) == NULL)
	return (0);

    /* Scan the avoid list checking for a match */
    cp = env;
    for (;;) {
	while (*cp != 0 && isspace(*cp))
	    cp++;
	if (*cp == 0)
	    break;
	len = 0;
	while (cp[len] != 0 && !isspace(cp[len]))
	    len++;
	if (!strncmp(cp, np, len)) {
	    freeenv(env);
	    return(1);
	}
	cp += len;
    }
    freeenv(env);

    return (0);
}

/*
 * Debugging/bug-avoidance.  Disable ACPI subsystem components.
 */
int
acpi_disabled(char *subsys)
{
    char	*cp, *env;
    int		len;

    if ((env = getenv("debug.acpi.disabled")) == NULL)
	return (0);
    if (strcmp(env, "all") == 0) {
	freeenv(env);
	return (1);
    }

    /* Scan the disable list, checking for a match. */
    cp = env;
    for (;;) {
	while (*cp != '\0' && isspace(*cp))
	    cp++;
	if (*cp == '\0')
	    break;
	len = 0;
	while (cp[len] != '\0' && !isspace(cp[len]))
	    len++;
	if (strncmp(cp, subsys, len) == 0) {
	    freeenv(env);
	    return (1);
	}
	cp += len;
    }
    freeenv(env);

    return (0);
}

/*
 * Control interface.
 *
 * We multiplex ioctls for all participating ACPI devices here.  Individual 
 * drivers wanting to be accessible via /dev/acpi should use the
 * register/deregister interface to make their handlers visible.
 */
struct acpi_ioctl_hook
{
    TAILQ_ENTRY(acpi_ioctl_hook) link;
    u_long			 cmd;
    acpi_ioctl_fn		 fn;
    void			 *arg;
};

static TAILQ_HEAD(,acpi_ioctl_hook)	acpi_ioctl_hooks;
static int				acpi_ioctl_hooks_initted;

int
acpi_register_ioctl(u_long cmd, acpi_ioctl_fn fn, void *arg)
{
    struct acpi_ioctl_hook	*hp;

    if ((hp = malloc(sizeof(*hp), M_ACPIDEV, M_NOWAIT)) == NULL)
	return (ENOMEM);
    hp->cmd = cmd;
    hp->fn = fn;
    hp->arg = arg;

    ACPI_LOCK(acpi);
    if (acpi_ioctl_hooks_initted == 0) {
	TAILQ_INIT(&acpi_ioctl_hooks);
	acpi_ioctl_hooks_initted = 1;
    }
    TAILQ_INSERT_TAIL(&acpi_ioctl_hooks, hp, link);
    ACPI_UNLOCK(acpi);

    return (0);
}

void
acpi_deregister_ioctl(u_long cmd, acpi_ioctl_fn fn)
{
    struct acpi_ioctl_hook	*hp;

    ACPI_LOCK(acpi);
    TAILQ_FOREACH(hp, &acpi_ioctl_hooks, link)
	if (hp->cmd == cmd && hp->fn == fn)
	    break;

    if (hp != NULL) {
	TAILQ_REMOVE(&acpi_ioctl_hooks, hp, link);
	free(hp, M_ACPIDEV);
    }
    ACPI_UNLOCK(acpi);
}

static int
acpiopen(struct cdev *dev, int flag, int fmt, d_thread_t *td)
{
    return (0);
}

static int
acpiclose(struct cdev *dev, int flag, int fmt, d_thread_t *td)
{
    return (0);
}

static int
acpiioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag, d_thread_t *td)
{
    struct acpi_softc		*sc;
    struct acpi_ioctl_hook	*hp;
    int				error, state;

    error = 0;
    hp = NULL;
    sc = dev->si_drv1;

    /*
     * Scan the list of registered ioctls, looking for handlers.
     */
    ACPI_LOCK(acpi);
    if (acpi_ioctl_hooks_initted)
	TAILQ_FOREACH(hp, &acpi_ioctl_hooks, link) {
	    if (hp->cmd == cmd)
		break;
	}
    ACPI_UNLOCK(acpi);
    if (hp)
	return (hp->fn(cmd, addr, hp->arg));

    /*
     * Core ioctls are not permitted for non-writable user.
     * Currently, other ioctls just fetch information.
     * Not changing system behavior.
     */
    if ((flag & FWRITE) == 0)
	return (EPERM);

    /* Core system ioctls. */
    switch (cmd) {
    case ACPIIO_REQSLPSTATE:
	state = *(int *)addr;
	if (state != ACPI_STATE_S5)
	    return (acpi_ReqSleepState(sc, state));
	device_printf(sc->acpi_dev, "power off via acpi ioctl not supported\n");
	error = EOPNOTSUPP;
	break;
    case ACPIIO_ACKSLPSTATE:
	error = *(int *)addr;
	error = acpi_AckSleepState(sc->acpi_clone, error);
	break;
    case ACPIIO_SETSLPSTATE:	/* DEPRECATED */
	state = *(int *)addr;
	if (state < ACPI_STATE_S0 || state > ACPI_S_STATES_MAX)
	    return (EINVAL);
	if (!acpi_sleep_states[state])
	    return (EOPNOTSUPP);
	if (ACPI_FAILURE(acpi_SetSleepState(sc, state)))
	    error = ENXIO;
	break;
    default:
	error = ENXIO;
	break;
    }

    return (error);
}

static int
acpi_sname2sstate(const char *sname)
{
    int sstate;

    if (toupper(sname[0]) == 'S') {
	sstate = sname[1] - '0';
	if (sstate >= ACPI_STATE_S0 && sstate <= ACPI_STATE_S5 &&
	    sname[2] == '\0')
	    return (sstate);
    } else if (strcasecmp(sname, "NONE") == 0)
	return (ACPI_STATE_UNKNOWN);
    return (-1);
}

static const char *
acpi_sstate2sname(int sstate)
{
    static const char *snames[] = { "S0", "S1", "S2", "S3", "S4", "S5" };

    if (sstate >= ACPI_STATE_S0 && sstate <= ACPI_STATE_S5)
	return (snames[sstate]);
    else if (sstate == ACPI_STATE_UNKNOWN)
	return ("NONE");
    return (NULL);
}

static int
acpi_supported_sleep_state_sysctl(SYSCTL_HANDLER_ARGS)
{
    int error;
    struct sbuf sb;
    UINT8 state;

    sbuf_new(&sb, NULL, 32, SBUF_AUTOEXTEND);
    for (state = ACPI_STATE_S1; state < ACPI_S_STATE_COUNT; state++)
	if (acpi_sleep_states[state])
	    sbuf_printf(&sb, "%s ", acpi_sstate2sname(state));
    sbuf_trim(&sb);
    sbuf_finish(&sb);
    error = sysctl_handle_string(oidp, sbuf_data(&sb), sbuf_len(&sb), req);
    sbuf_delete(&sb);
    return (error);
}

static int
acpi_sleep_state_sysctl(SYSCTL_HANDLER_ARGS)
{
    char sleep_state[10];
    int error, new_state, old_state;

    old_state = *(int *)oidp->oid_arg1;
    strlcpy(sleep_state, acpi_sstate2sname(old_state), sizeof(sleep_state));
    error = sysctl_handle_string(oidp, sleep_state, sizeof(sleep_state), req);
    if (error == 0 && req->newptr != NULL) {
	new_state = acpi_sname2sstate(sleep_state);
	if (new_state < ACPI_STATE_S1)
	    return (EINVAL);
	if (new_state < ACPI_S_STATES_MAX && !acpi_sleep_states[new_state])
	    return (EOPNOTSUPP);
	if (new_state != old_state)
	    *(int *)oidp->oid_arg1 = new_state;
    }
    return (error);
}

/* Inform devctl(4) when we receive a Notify. */
void
acpi_UserNotify(const char *subsystem, ACPI_HANDLE h, uint8_t notify)
{
    char		notify_buf[16];
    ACPI_BUFFER		handle_buf;
    ACPI_STATUS		status;

    if (subsystem == NULL)
	return;

    handle_buf.Pointer = NULL;
    handle_buf.Length = ACPI_ALLOCATE_BUFFER;
    status = AcpiNsHandleToPathname(h, &handle_buf);
    if (ACPI_FAILURE(status))
	return;
    snprintf(notify_buf, sizeof(notify_buf), "notify=0x%02x", notify);
    devctl_notify("ACPI", subsystem, handle_buf.Pointer, notify_buf);
    AcpiOsFree(handle_buf.Pointer);
}

#ifdef ACPI_DEBUG
/*
 * Support for parsing debug options from the kernel environment.
 *
 * Bits may be set in the AcpiDbgLayer and AcpiDbgLevel debug registers
 * by specifying the names of the bits in the debug.acpi.layer and
 * debug.acpi.level environment variables.  Bits may be unset by 
 * prefixing the bit name with !.
 */
struct debugtag
{
    char	*name;
    UINT32	value;
};

static struct debugtag	dbg_layer[] = {
    {"ACPI_UTILITIES",		ACPI_UTILITIES},
    {"ACPI_HARDWARE",		ACPI_HARDWARE},
    {"ACPI_EVENTS",		ACPI_EVENTS},
    {"ACPI_TABLES",		ACPI_TABLES},
    {"ACPI_NAMESPACE",		ACPI_NAMESPACE},
    {"ACPI_PARSER",		ACPI_PARSER},
    {"ACPI_DISPATCHER",		ACPI_DISPATCHER},
    {"ACPI_EXECUTER",		ACPI_EXECUTER},
    {"ACPI_RESOURCES",		ACPI_RESOURCES},
    {"ACPI_CA_DEBUGGER",	ACPI_CA_DEBUGGER},
    {"ACPI_OS_SERVICES",	ACPI_OS_SERVICES},
    {"ACPI_CA_DISASSEMBLER",	ACPI_CA_DISASSEMBLER},
    {"ACPI_ALL_COMPONENTS",	ACPI_ALL_COMPONENTS},

    {"ACPI_AC_ADAPTER",		ACPI_AC_ADAPTER},
    {"ACPI_BATTERY",		ACPI_BATTERY},
    {"ACPI_BUS",		ACPI_BUS},
    {"ACPI_BUTTON",		ACPI_BUTTON},
    {"ACPI_EC", 		ACPI_EC},
    {"ACPI_FAN",		ACPI_FAN},
    {"ACPI_POWERRES",		ACPI_POWERRES},
    {"ACPI_PROCESSOR",		ACPI_PROCESSOR},
    {"ACPI_THERMAL",		ACPI_THERMAL},
    {"ACPI_TIMER",		ACPI_TIMER},
    {"ACPI_ALL_DRIVERS",	ACPI_ALL_DRIVERS},
    {NULL, 0}
};

static struct debugtag dbg_level[] = {
    {"ACPI_LV_ERROR",		ACPI_LV_ERROR},
    {"ACPI_LV_WARN",		ACPI_LV_WARN},
    {"ACPI_LV_INIT",		ACPI_LV_INIT},
    {"ACPI_LV_DEBUG_OBJECT",	ACPI_LV_DEBUG_OBJECT},
    {"ACPI_LV_INFO",		ACPI_LV_INFO},
    {"ACPI_LV_ALL_EXCEPTIONS",	ACPI_LV_ALL_EXCEPTIONS},

    /* Trace verbosity level 1 [Standard Trace Level] */
    {"ACPI_LV_INIT_NAMES",	ACPI_LV_INIT_NAMES},
    {"ACPI_LV_PARSE",		ACPI_LV_PARSE},
    {"ACPI_LV_LOAD",		ACPI_LV_LOAD},
    {"ACPI_LV_DISPATCH",	ACPI_LV_DISPATCH},
    {"ACPI_LV_EXEC",		ACPI_LV_EXEC},
    {"ACPI_LV_NAMES",		ACPI_LV_NAMES},
    {"ACPI_LV_OPREGION",	ACPI_LV_OPREGION},
    {"ACPI_LV_BFIELD",		ACPI_LV_BFIELD},
    {"ACPI_LV_TABLES",		ACPI_LV_TABLES},
    {"ACPI_LV_VALUES",		ACPI_LV_VALUES},
    {"ACPI_LV_OBJECTS",		ACPI_LV_OBJECTS},
    {"ACPI_LV_RESOURCES",	ACPI_LV_RESOURCES},
    {"ACPI_LV_USER_REQUESTS",	ACPI_LV_USER_REQUESTS},
    {"ACPI_LV_PACKAGE",		ACPI_LV_PACKAGE},
    {"ACPI_LV_VERBOSITY1",	ACPI_LV_VERBOSITY1},

    /* Trace verbosity level 2 [Function tracing and memory allocation] */
    {"ACPI_LV_ALLOCATIONS",	ACPI_LV_ALLOCATIONS},
    {"ACPI_LV_FUNCTIONS",	ACPI_LV_FUNCTIONS},
    {"ACPI_LV_OPTIMIZATIONS",	ACPI_LV_OPTIMIZATIONS},
    {"ACPI_LV_VERBOSITY2",	ACPI_LV_VERBOSITY2},
    {"ACPI_LV_ALL",		ACPI_LV_ALL},

    /* Trace verbosity level 3 [Threading, I/O, and Interrupts] */
    {"ACPI_LV_MUTEX",		ACPI_LV_MUTEX},
    {"ACPI_LV_THREADS",		ACPI_LV_THREADS},
    {"ACPI_LV_IO",		ACPI_LV_IO},
    {"ACPI_LV_INTERRUPTS",	ACPI_LV_INTERRUPTS},
    {"ACPI_LV_VERBOSITY3",	ACPI_LV_VERBOSITY3},

    /* Exceptionally verbose output -- also used in the global "DebugLevel"  */
    {"ACPI_LV_AML_DISASSEMBLE",	ACPI_LV_AML_DISASSEMBLE},
    {"ACPI_LV_VERBOSE_INFO",	ACPI_LV_VERBOSE_INFO},
    {"ACPI_LV_FULL_TABLES",	ACPI_LV_FULL_TABLES},
    {"ACPI_LV_EVENTS",		ACPI_LV_EVENTS},
    {"ACPI_LV_VERBOSE",		ACPI_LV_VERBOSE},
    {NULL, 0}
};    

static void
acpi_parse_debug(char *cp, struct debugtag *tag, UINT32 *flag)
{
    char	*ep;
    int		i, l;
    int		set;

    while (*cp) {
	if (isspace(*cp)) {
	    cp++;
	    continue;
	}
	ep = cp;
	while (*ep && !isspace(*ep))
	    ep++;
	if (*cp == '!') {
	    set = 0;
	    cp++;
	    if (cp == ep)
		continue;
	} else {
	    set = 1;
	}
	l = ep - cp;
	for (i = 0; tag[i].name != NULL; i++) {
	    if (!strncmp(cp, tag[i].name, l)) {
		if (set)
		    *flag |= tag[i].value;
		else
		    *flag &= ~tag[i].value;
	    }
	}
	cp = ep;
    }
}

static void
acpi_set_debugging(void *junk)
{
    char	*layer, *level;

    if (cold) {
	AcpiDbgLayer = 0;
	AcpiDbgLevel = 0;
    }

    layer = getenv("debug.acpi.layer");
    level = getenv("debug.acpi.level");
    if (layer == NULL && level == NULL)
	return;

    printf("ACPI set debug");
    if (layer != NULL) {
	if (strcmp("NONE", layer) != 0)
	    printf(" layer '%s'", layer);
	acpi_parse_debug(layer, &dbg_layer[0], &AcpiDbgLayer);
	freeenv(layer);
    }
    if (level != NULL) {
	if (strcmp("NONE", level) != 0)
	    printf(" level '%s'", level);
	acpi_parse_debug(level, &dbg_level[0], &AcpiDbgLevel);
	freeenv(level);
    }
    printf("\n");
}

SYSINIT(acpi_debugging, SI_SUB_TUNABLES, SI_ORDER_ANY, acpi_set_debugging,
	NULL);

static int
acpi_debug_sysctl(SYSCTL_HANDLER_ARGS)
{
    int		 error, *dbg;
    struct	 debugtag *tag;
    struct	 sbuf sb;

    if (sbuf_new(&sb, NULL, 128, SBUF_AUTOEXTEND) == NULL)
	return (ENOMEM);
    if (strcmp(oidp->oid_arg1, "debug.acpi.layer") == 0) {
	tag = &dbg_layer[0];
	dbg = &AcpiDbgLayer;
    } else {
	tag = &dbg_level[0];
	dbg = &AcpiDbgLevel;
    }

    /* Get old values if this is a get request. */
    ACPI_SERIAL_BEGIN(acpi);
    if (*dbg == 0) {
	sbuf_cpy(&sb, "NONE");
    } else if (req->newptr == NULL) {
	for (; tag->name != NULL; tag++) {
	    if ((*dbg & tag->value) == tag->value)
		sbuf_printf(&sb, "%s ", tag->name);
	}
    }
    sbuf_trim(&sb);
    sbuf_finish(&sb);

    /* Copy out the old values to the user. */
    error = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb));
    sbuf_delete(&sb);

    /* If the user is setting a string, parse it. */
    if (error == 0 && req->newptr != NULL) {
	*dbg = 0;
	setenv((char *)oidp->oid_arg1, (char *)req->newptr);
	acpi_set_debugging(NULL);
    }
    ACPI_SERIAL_END(acpi);

    return (error);
}

SYSCTL_PROC(_debug_acpi, OID_AUTO, layer, CTLFLAG_RW | CTLTYPE_STRING,
	    "debug.acpi.layer", 0, acpi_debug_sysctl, "A", "");
SYSCTL_PROC(_debug_acpi, OID_AUTO, level, CTLFLAG_RW | CTLTYPE_STRING,
	    "debug.acpi.level", 0, acpi_debug_sysctl, "A", "");
#endif /* ACPI_DEBUG */

static int
acpi_pm_func(u_long cmd, void *arg, ...)
{
	int	state, acpi_state;
	int	error;
	struct	acpi_softc *sc;
	va_list	ap;

	error = 0;
	switch (cmd) {
	case POWER_CMD_SUSPEND:
		sc = (struct acpi_softc *)arg;
		if (sc == NULL) {
			error = EINVAL;
			goto out;
		}

		va_start(ap, arg);
		state = va_arg(ap, int);
		va_end(ap);

		switch (state) {
		case POWER_SLEEP_STATE_STANDBY:
			acpi_state = sc->acpi_standby_sx;
			break;
		case POWER_SLEEP_STATE_SUSPEND:
			acpi_state = sc->acpi_suspend_sx;
			break;
		case POWER_SLEEP_STATE_HIBERNATE:
			acpi_state = ACPI_STATE_S4;
			break;
		default:
			error = EINVAL;
			goto out;
		}

		if (ACPI_FAILURE(acpi_EnterSleepState(sc, acpi_state)))
			error = ENXIO;
		break;
	default:
		error = EINVAL;
		goto out;
	}

out:
	return (error);
}

static void
acpi_pm_register(void *arg)
{
    if (!cold || resource_disabled("acpi", 0))
	return;

    power_pm_register(POWER_PM_TYPE_ACPI, acpi_pm_func, NULL);
}

SYSINIT(power, SI_SUB_KLD, SI_ORDER_ANY, acpi_pm_register, 0);
