#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/poll.h>
#include <sys/callout.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>

#include <dev/usb/usb.h>

#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdevs.h>
#include <dev/usb/usb_quirks.h>
#include <dev/usb/usbhid.h>

//#define OLDNBSD

#ifdef USB_DEBUG
#define DPRINTF(x)	if (upwrdebug) logprintf x
#define DPRINTFN(n,x)	if (upwrebug>(n)) logprintf x
int	upwrdebug = 0;
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

#define	DEBUG
#ifdef	DEBUG
#define	dlog(fmt, ...)	log(LOG_ERR, "%s()@L%u : " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
//#define	dlog(fmt, ...)	printf("%s()@L%u : " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define	dlog(...)	
#endif

#define UPWR_UPDATE_TICK	50 /* ms */

#define	CMD_NONE		0x00
#define CMD_MODEL		0xaa
#define CMD_VERSION		0xa7
#define	CMD_OUTLET1_STATUS	0xa1
#define	CMD_OUTLET2_STATUS	0xa2
#define	CMD_OUTLET3_STATUS	0xac
#define	CMD_POLL1		0xb1
#define	CMD_POLL2		0xb2
#define	CMD_OUTLET1_ON		0x41
#define	CMD_OUTLET1_OFF		0x42
#define	CMD_OUTLET2_ON		0x43
#define	CMD_OUTLET2_OFF		0x44
#define	CMD_OUTLET3_ON		0x45
#define	CMD_OUTLET3_OFF		0x50
#define	CMD_PADDING		0xff

struct upwr_endpoints {
	usb_endpoint_descriptor_t	*edesc;
	usbd_pipe_handle		pipeh;

	int				ep;
	int				size;
	uint8_t				*buf;
};

#define EP_IN	0
#define	EP_OUT	1
struct upwr_softc {
#ifdef OLDNBSD
	USBBASEDEVICE		sc_dev;
#else
	device_t		sc_dev;
#endif
	usbd_device_handle	sc_udev;
	char 			sc_dying;

	struct	upwr_endpoints	sc_endpoints[2];

	usbd_interface_handle	sc_iface;


	uint8_t			sc_issueing_cmd;
	uint8_t			sc_accepted_cmd;

	uint8_t			sc_model;
	uint8_t			sc_version_major;
	uint8_t			sc_version_minor;
	int			sc_current;
	uint8_t			sc_status_outlet1;
	uint8_t			sc_status_outlet2;
	uint8_t			sc_status_outlet3;

	struct	callout		sc_upwr_ch;
};

// 0x04d8, 0x003f
const struct usb_devno upwr_devs[] = {
	{ USB_VENDOR_MICROCHIP, USB_PRODUCT_MICROCHIP_POWERUSB },
};
#define upwr_lookup(v, p) usb_lookup(upwr_devs, v, p)

static void upwr_intr_in(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status);
static usbd_status upwr_send_cmd(struct upwr_softc *sc, uint8_t val);
static usbd_status upwr_set_idle(struct upwr_softc *sc);
static void sysctl_hw_upwr_setup(struct sysctllog **clog);

// XXX: this sucks
static struct upwr_softc *sc0;

USB_DECLARE_DRIVER(upwr);

USB_MATCH(upwr)
{
	USB_MATCH_START(upwr, uaa);

	dlog("vid = %x, pid = %x", uaa->vendor, uaa->product);

	if (upwr_lookup(uaa->vendor, uaa->product) == NULL) {
		dlog("not found");
		return UMATCH_NONE;
	}

	dlog("found");
	return (UMATCH_VENDOR_PRODUCT);
}

USB_ATTACH(upwr)
{
	/*
	   struct upwr_softc *sc = (struct upwr_softc *)self;
	   struct usb_attach_arg *uaa = aux;
	   */
	USB_ATTACH_START(upwr, sc, uaa);
	usb_interface_descriptor_t *id;
	usb_endpoint_descriptor_t *ed;
	usbd_status err;
	int i;
	u_int8_t niface;
	void *desc;
	int size;

#if 0
#ifdef OLDNBSD
	char devinfo[1024];
#else
	char *devinfo;
#endif
#endif
	// XXX: this sucks
	sc0 = sc;

#ifndef OLDNBSD
	sc->sc_dev = self;
#endif
	sc->sc_udev = uaa->device;
#if 0
#ifdef OLDNBSD
	usbd_devinfo(sc->sc_udev, 0, devinfo, sizeof(devinfo));
#else
	devinfo = usbd_devinfo_alloc(sc->sc_udev, 0);
#endif
	USB_ATTACH_SETUP;
	printf("%s : %s\n", USBDEVNAME(sc->sc_dev), devinfo);
#ifndef OLDNBSD
	usbd_devinfo_free(devinfo);
#endif
#endif

	// XXX: is this right?
#define UPWR_USB_IFACE	0
#define UPWR_USB_CONFIG	1

	if (usbd_get_config_descriptor(sc->sc_udev) != NULL)
		printf("configno = %d\n", usbd_get_config_descriptor(sc->sc_udev)->bConfigurationValue);

	/* set configuration */
	if ((err = usbd_set_config_no(sc->sc_udev, UPWR_USB_CONFIG, 1)) != 0) {
		printf("%s : failed to set config %d : %s\n",
				USBDEVNAME(sc->sc_dev), UPWR_USB_CONFIG, usbd_errstr(err));

		USB_ATTACH_ERROR_RETURN;
	}

	i = usbd_interface_count(sc->sc_udev, &niface);

	dlog("has %d interfaces", niface);

	/* get interface handle*/
	if ((err = usbd_device2interface_handle(sc->sc_udev, UPWR_USB_IFACE,
					&sc->sc_iface)) != 0) {
		printf("%s: failed to get interface %d: %s\n",
				USBDEVNAME(sc->sc_dev), UPWR_USB_IFACE, usbd_errstr(err));

		USB_ATTACH_ERROR_RETURN;
	}

	/* find endpoints */
	sc->sc_endpoints[EP_OUT].ep = sc->sc_endpoints[EP_IN].ep = -1;
	id = usbd_get_interface_descriptor(sc->sc_iface);
	for (i = 0; i < id->bNumEndpoints; i++) {
		dlog("numed %d", i);
		ed = usbd_interface2endpoint_descriptor(sc->sc_iface, i);
		if (ed == NULL) {
			printf("%s: failed to get endpoint %d descriptor\n",
					USBDEVNAME(sc->sc_dev), i);
			return;
		}

		dlog("endpt is %d where IN(%d), OUT(%d). type %d where INTR(%d), BULK(%d)",
				UE_GET_DIR(ed->bEndpointAddress),
				UE_DIR_IN, UE_DIR_OUT, 
				UE_GET_XFERTYPE(ed->bmAttributes),
				UE_INTERRUPT, UE_BULK);
		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
				UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT){
			sc->sc_endpoints[EP_IN].edesc = ed;
			sc->sc_endpoints[EP_IN].ep = ed->bEndpointAddress;
			sc->sc_endpoints[EP_IN].size = UGETW(ed->wMaxPacketSize);
		}

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_OUT &&
				UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT){
			sc->sc_endpoints[EP_OUT].edesc = ed;
			sc->sc_endpoints[EP_OUT].ep = ed->bEndpointAddress;
			sc->sc_endpoints[EP_OUT].size = UGETW(ed->wMaxPacketSize);
		}				

	}

	if (sc->sc_endpoints[EP_IN].ep == -1 || 
			sc->sc_endpoints[EP_OUT].ep == -1) {
		printf("%s: no data endpoint found\n", USBDEVNAME(sc->sc_dev));
		return;
	}

	dlog("control endpoint = %d", USB_CONTROL_ENDPOINT);

	/*  send set idle : do wee need this? */
	upwr_set_idle(sc);

	/* read report desc : ? */
	err = usbd_read_report_desc(sc->sc_iface, &desc, &size, M_USBDEV);
	if (err) {
		printf("could not get report descriptor %s\n",
				/*  sc->sc_dev.dv_xname, */usbd_errstr(err));
	}

	/*  open OUT pipe */
	dlog("out endp = %d", sc->sc_endpoints[EP_OUT].ep);
	err = usbd_open_pipe(sc->sc_iface, sc->sc_endpoints[EP_OUT].ep, 0, &sc->sc_endpoints[EP_OUT].pipeh);

	if (err != USBD_NORMAL_COMPLETION) {
		printf("%s : could not open intr pipe %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}

	/*  open IN pipe */
	dlog("in endp = %d", sc->sc_endpoints[EP_IN].ep);
	sc->sc_endpoints[EP_IN].buf = malloc(sc->sc_endpoints[EP_IN].size, M_USBDEV, M_WAITOK);
	err = usbd_open_pipe_intr(
			sc->sc_iface, 
			sc->sc_endpoints[EP_IN].ep,
			USBD_SHORT_XFER_OK, 
			&sc->sc_endpoints[EP_IN].pipeh, 
			sc, 
			sc->sc_endpoints[EP_IN].buf,
			sc->sc_endpoints[EP_IN].size, 
			upwr_intr_in, 
			UPWR_UPDATE_TICK);

	if (err) {
		printf("%s : could not open IN intr pipe %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}


	/* init */
	/* MODEL */
	err = upwr_send_cmd(sc, CMD_MODEL);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}

	/* VERSION */
	err = upwr_send_cmd(sc, CMD_VERSION);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}

	/* get OUTLET STATUS */
	err = upwr_send_cmd(sc, CMD_OUTLET1_STATUS);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}

	err = upwr_send_cmd(sc, CMD_OUTLET2_STATUS);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}

	err = upwr_send_cmd(sc, CMD_OUTLET3_STATUS);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}

	/* OUTLET* on */
	err = upwr_send_cmd(sc, CMD_OUTLET1_ON);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
	}

	err = upwr_send_cmd(sc, CMD_OUTLET2_ON);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
	}

	err = upwr_send_cmd(sc, CMD_OUTLET3_ON);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
	}
	dlog("done send command");

	usbd_add_drv_event(USB_EVENT_DRIVER_ATTACH, sc->sc_udev, USBDEV(sc->sc_dev));

	sysctl_hw_upwr_setup(NULL);

	USB_ATTACH_SUCCESS_RETURN;
	return;
fail:
	printf("welcome to the real world\n");
	if (sc->sc_endpoints[EP_IN].pipeh != NULL) {
		usbd_abort_pipe(sc->sc_endpoints[EP_IN].pipeh);
		usbd_close_pipe(sc->sc_endpoints[EP_IN].pipeh);
	}
	if (sc->sc_endpoints[EP_OUT].pipeh != NULL) {
		usbd_abort_pipe(sc->sc_endpoints[EP_OUT].pipeh);
		usbd_close_pipe(sc->sc_endpoints[EP_OUT].pipeh);
	}

	if (sc->sc_endpoints[EP_IN].buf != NULL)
		free(sc->sc_endpoints[EP_IN].buf, M_USBDEV);
	if (sc->sc_endpoints[EP_OUT].buf != NULL)
		free(sc->sc_endpoints[EP_OUT].buf, M_USBDEV);

	USB_ATTACH_ERROR_RETURN;
}

	int
upwr_activate(device_ptr_t self, enum devact act)
{
	struct upwr_softc *sc = (struct upwr_softc *)self;

	switch (act) {
		case DVACT_ACTIVATE:
			return EOPNOTSUPP;
		case DVACT_DEACTIVATE:
			dlog("deactivated");
			sc->sc_dying = 1;
			break;
	}
	return (0);
}

USB_DETACH(upwr)
{
	USB_DETACH_START(upwr, sc);

	dlog("upwr_detach: sc=%p flags=%d\n", sc, flags);

	if (sc->sc_endpoints[EP_IN].pipeh != NULL) {
		usbd_abort_pipe(sc->sc_endpoints[EP_IN].pipeh);
		usbd_close_pipe(sc->sc_endpoints[EP_IN].pipeh);
	}
	if (sc->sc_endpoints[EP_OUT].pipeh != NULL) {
		usbd_abort_pipe(sc->sc_endpoints[EP_OUT].pipeh);
		usbd_close_pipe(sc->sc_endpoints[EP_OUT].pipeh);
	}

	if (sc->sc_endpoints[EP_IN].buf != NULL)
		free(sc->sc_endpoints[EP_IN].buf, M_USBDEV);
	if (sc->sc_endpoints[EP_OUT].buf != NULL)
		free(sc->sc_endpoints[EP_OUT].buf, M_USBDEV);

	usbd_add_drv_event(USB_EVENT_DRIVER_DETACH, sc->sc_udev,
			sc->sc_dev);

	return (0);
}

void
upwr_intr_in(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status)
{
	struct upwr_softc *sc = (struct upwr_softc *)priv;
	uint8_t buf[64];

	if (sc->sc_dying)
		return;

	if (status != USBD_NORMAL_COMPLETION) {
		dlog("error intr");
		if (status == USBD_NOT_STARTED || status == USBD_CANCELLED) {
			return;
		}
		if (status == USBD_STALLED) {
			usbd_clear_endpoint_stall_async(sc->sc_endpoints[EP_IN].pipeh);
		}
		return;
	}	

	memcpy(buf, (char *)sc->sc_endpoints[EP_IN].buf, 64);
	sc->sc_accepted_cmd = sc->sc_issueing_cmd;
	switch(sc->sc_issueing_cmd) {
		case CMD_MODEL: /* model */
			dlog("CMD_MODEL buf[0] = %d\n", buf[0]);
			sc->sc_model = buf[0];
			break;
		case CMD_VERSION: /* version */
			dlog("CMD_VERSION");
			dlog("buf[0] = %d.%d\n", buf[0], buf[1]);
			sc->sc_version_major = buf[0];
			sc->sc_version_minor = buf[1];
			break;
		case CMD_POLL1:
			dlog("CMD_POLL1");
			dlog("current = %d mA\n", buf[0] << 8 | buf[1]);
			sc->sc_current = buf[0] << 8 | buf[1];
			break;
		case CMD_POLL2:
			dlog("CMD_POLL2");
			break;
		case CMD_OUTLET1_STATUS:
			dlog("CMD_OUTLET1_STATUS : %d", buf[0]);
			sc->sc_status_outlet1 = buf[0];
			break;

		case CMD_OUTLET2_STATUS:
			dlog("CMD_OUTLET2_STATUS : %d", buf[0]);
			sc->sc_status_outlet2 = buf[0];
			break;
		case CMD_OUTLET3_STATUS:
			dlog("CMD_OUTLET3_STATUS : %d", buf[0]);
			sc->sc_status_outlet3 = buf[0];
			break;
		case CMD_OUTLET1_ON:
		case CMD_OUTLET2_ON:
		case CMD_OUTLET3_ON:
			dlog("CMD_OUTLET*_ON : %x %d", sc->sc_issueing_cmd, buf[0]);
			break;
		case CMD_OUTLET1_OFF:
		case CMD_OUTLET2_OFF:
		case CMD_OUTLET3_OFF:
			dlog("CMD_OUTLET*_OFF : %x %d", sc->sc_issueing_cmd, buf[0]);
			break;
		case CMD_NONE:
			break;
		default:
			dlog("????");
			break;
	}
	sc->sc_issueing_cmd = CMD_NONE;
	sc->sc_accepted_cmd = sc->sc_issueing_cmd;

	return;
}

static usbd_status
upwr_set_idle(struct upwr_softc *sc)
{
	usb_device_request_t req;
	usbd_status err;
	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = 0x0a;
	USETW(req.wValue, 0);
	USETW(req.wIndex, 0);
	USETW(req.wLength, 0);

	err = usbd_do_request(sc->sc_udev, &req, &sc->sc_endpoints[EP_IN].buf);
	if (err) {
		printf("%s : could not issue command %s\n",
				USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		return (EIO);
	}

	return 0;
}

static usbd_status
upwr_send_cmd(struct upwr_softc *sc, uint8_t cmd)
{
	usbd_status err;	
	int i;
	uint8_t	buf[64];
	usbd_xfer_handle	xfer;

	xfer = usbd_alloc_xfer(sc->sc_udev);

	memset(buf, CMD_PADDING, sc->sc_endpoints[EP_OUT].size);
	buf[0] = cmd;

	sc->sc_issueing_cmd = cmd;
	sc->sc_accepted_cmd = CMD_NONE;

	for (i = 0; i < 64; i++) {
		printf("%x ", buf[i]);
	}
	printf("\n");

	err = usbd_intr_transfer(
			xfer,
			sc->sc_endpoints[EP_OUT].pipeh,
			0, 
			1, 
			buf, 
			&sc->sc_endpoints[EP_OUT].size, "upwr"
			);
	usbd_free_xfer(xfer);

	if (err) {
		if (err == USBD_INTERRUPTED)
			return EINTR;
		if (err == USBD_TIMEOUT)
			return ETIMEDOUT;
		return EIO;
	}

	/* wait ack? : do we need this? */
	tsleep(sc, 0, "upwr", hz/10);

	return 0;
}

#if 0
static int
upwr_sysctl_verify(SYSCTLFN_ARGS)
{
	int error;
	sysctl_
	
#define TEST_UPWER(name) \
	if (node.sysctl_num == nodes.name) { \
        u = inl(GBEQOS##port##_BASE + GBEQOS_##reg); \
        fpga_counters[port].reg = u; \
        goto done; \
    }

	node = *rnode;

	if (node.sysctl_num == nodes.outlet1) {
		printf("outlet1 current date = %d\n", node.sysctl_data);
		goto done;
	}

	if (node.sysctl_num == nodes.outlet2) {
		printf("outlet2 current date = %d\n", node.sysctl_data);
		goto done;
	}

	if (node.sysctl_num == nodes.outlet3) {
		printf("outlet3 current date = %d\n", node.sysctl_data);
		goto done;
	}

	return EINVAL;
done:
	node.sysctl_data = &u;
	newp = &u;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL) {
		return error;
	}
	return 0;
}
#endif

static int
upwr_sysctl_outlet1(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	int error;
	int op = 0; 

	if (newp != NULL)
		op = *(const int *)newp;

	node = *rnode;

	// do something here
	if (newp) {
		op = *(const int *)newp;
		switch(op) {
		case 0: // off
			upwr_send_cmd(sc0, CMD_OUTLET1_OFF);
			break;
		case 1: // on
			upwr_send_cmd(sc0, CMD_OUTLET1_ON);
			break;
		default:
			// ignore
			break;
		}
	}

	node.sysctl_data = &sc0->sc_status_outlet1;

	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL) {
		return error;
	}

	return 0;
}
static int
upwr_sysctl_outlet2(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	int error;
	int op = 0;

	node = *rnode;

	// do something here
	if (newp) {
		op = *(const int *)newp;
		switch(op) {
		case 0: // off
			upwr_send_cmd(sc0, CMD_OUTLET2_OFF);
			break;
		case 1: // on
			upwr_send_cmd(sc0, CMD_OUTLET2_ON);
			break;
		default:
			// ignore
			break;
		}
	}

	node.sysctl_data = &sc0->sc_status_outlet2;

	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL) {
		return error;
	}

	return 0;
}

static int
upwr_sysctl_outlet3(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	int error;
	int op = 0;

	node = *rnode;

	if (newp) {
		op = *(const int *)newp;
		switch(op) {
		case 0: // off
			upwr_send_cmd(sc0, CMD_OUTLET3_OFF);
			break;
		case 1: // on
			upwr_send_cmd(sc0, CMD_OUTLET3_ON);
			break;
		default:
			// ignore
			break;
		}
	}
	node.sysctl_data = &sc0->sc_status_outlet3;

	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL) {
		return error;
	}

	return 0;
}
/* 
static void
sysctl_hw_upwr_setup(struct sysctllog **clog) {
*/
SYSCTL_SETUP(sysctl_hw_upwr_setup, "sysctl setup for upwr") 
{
	int rc;
	const struct sysctlnode *node_root = NULL;
	printf("%u %s\n", __LINE__, __func__);

	if ((rc = sysctl_createv(
			clog, 0, NULL, NULL,
			CTLFLAG_PERMANENT,
			CTLTYPE_NODE, "hw", NULL,
			NULL, 0, NULL, 0,
			CTL_HW, CTL_EOL)) != 0) {
		goto err;
	}

	printf("%u %s\n", __LINE__, __func__);
	if ((rc = sysctl_createv(
			clog, 0, NULL, &node_root,
			CTLFLAG_PERMANENT, 
			CTLTYPE_NODE, "upwr", SYSCTL_DESCR("upwr controls"),
			NULL, 0, NULL, 0,
			CTL_HW, CTL_CREATE, CTL_EOL)) != 0) {
		goto err;
	}	
	if (node_root == NULL) {
		goto err;
	}

	printf("%u %s\n", __LINE__, __func__);
#define UPWR_NEW_SYSCTL(param) \
	if ((rc = sysctl_createv(\
			clog, 0, &node_root, NULL, \
			CTLFLAG_PERMANENT|CTLFLAG_READWRITE, \
			CTLTYPE_INT, #param, \
			SYSCTL_DESCR(#param), \
			upwr_sysctl_##param, 0, NULL, sizeof(u_int32_t), \
			CTL_CREATE, CTL_EOL)) != 0) { \
		aprint_error("%s: " #param " failed to create", __func__); \
		goto err; \
	} \

	printf("%u %s\n", __LINE__, __func__);
	UPWR_NEW_SYSCTL(outlet1);
	printf("%u %s\n", __LINE__, __func__);
	UPWR_NEW_SYSCTL(outlet2);
	printf("%u %s\n", __LINE__, __func__);
	UPWR_NEW_SYSCTL(outlet3);

	printf("%u %s\n", __LINE__, __func__);
	return;
err:
	aprint_error("%s: sysctl_createv failed (rc = %d)\n", __func__, rc);
}

