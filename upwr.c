/*
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

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

#include <dev/usb/usb.h>

#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdevs.h>
#include <dev/usb/usb_quirks.h>
#include <dev/usb/usbhid.h>

#define USE_INIT_AT_ATTACH

#define USE_INTR_TRANSFER
//#define USE_SET_REPORT
//#define USE_UPWR_INTR_OUT
//#define USE_RAW_DO_REQUEST
#define THISISIT
//#define USE_SET_IDLE
//#define USE_READ_REPORT_DESC
#define OUTISOUT
//#define	USE_CALLOUT
//#define USE_OUT
//#define USE_RAW_INTR_TRANSFER

#define USE_SEND_CMD

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

/*
 * XXX: naming
 */
struct upwr_softc {
	USBBASEDEVICE		sc_dev;
	usbd_device_handle	sc_udev;
	usbd_interface_handle	sc_iface;
	char 			sc_dying;

	/* OUT */
	usbd_pipe_handle	sc_opipe;
	int			sc_osize;
	usbd_xfer_handle	sc_oxfer;
	uint8_t			*sc_obuf;
	int			ep_ointr;

	/* IN */
	usbd_pipe_handle	sc_ipipe;
	int			sc_isize;
	usbd_xfer_handle	sc_ixfer;
	uint8_t			*sc_ibuf;
	int			ep_iintr;

	uint8_t			sc_issueing_cmd;
	uint8_t			sc_accepted_cmd;

	struct	callout		sc_upwr_ch;
};

// 0x04d8, 0x003f
const struct usb_devno upwr_devs[] = {
	{ USB_VENDOR_MICROCHIP, USB_PRODUCT_MICROCHIP_POWERUSB },
};
#define upwr_lookup(v, p) usb_lookup(upwr_devs, v, p)

//static void upwr_intr(struct uhidev *addr, void *ibuf, u_int len);
#ifdef USE_UPWR_INTR_OUT
static void upwr_intr_out(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status);
#endif
static void upwr_intr_in(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status);
#ifdef USE_SEND_CMD
static usbd_status upwr_send_cmd(struct upwr_softc *sc, uint8_t val);
#endif
#ifdef USE_SET_IDLE
static usbd_status upwr_set_idle(struct upwr_softc *sc);
#endif

#ifdef USE_CALLOUT
static void upwr_callout(void *);
#endif


//Static int	upwr_ioctl(void *, u_long, caddr_t, int, usb_proc_ptr );

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
#if 0
#ifdef THISISIT
	char devinfo[1024];
#else
	char *devinfo;
#endif
#endif

	sc->sc_udev = uaa->device;
#if 0
#ifdef THISISIT
	usbd_devinfo(sc->sc_udev, 0, devinfo, sizeof(devinfo));
#else
	devinfo = usbd_devinfo_alloc(sc->sc_udev, 0);
#endif
	USB_ATTACH_SETUP;
	printf("%s : %s\n", USBDEVNAME(sc->sc_dev), devinfo);
#ifndef THISISIT
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

	sc->ep_ointr = sc->ep_iintr = -1;
	dlog("");
	/* find endpoints */
	id = usbd_get_interface_descriptor(sc->sc_iface);
	for (i = 0; i < id->bNumEndpoints; i++) {
		dlog("numed %d", i);
		ed = usbd_interface2endpoint_descriptor(sc->sc_iface, i);
		if (ed == NULL) {
			printf("%s: failed to get endpoint %d descriptor\n",
			    USBDEVNAME(sc->sc_dev), i);
			return;
		}

	dlog("");
#if 0
		/*  won't come here does it?  */
		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			dlog("%d is UE_DIR_IN && UE_BULK", i);
			ep_ibulk = ed->bEndpointAddress;
		}
		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_OUT &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			dlog("%d is UE_DIR_OUT_&& UE_BULK", i);
			ep_obulk = ed->bEndpointAddress;
		}
#endif

		dlog("endpt is %d where IN(%d), OUT(%d). type %d where INTR(%d), BULK(%d)",
				UE_GET_DIR(ed->bEndpointAddress),
				UE_DIR_IN, UE_DIR_OUT, 
				UE_GET_XFERTYPE(ed->bmAttributes),
				UE_INTERRUPT, UE_BULK);
		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT){
			sc->ep_iintr = ed->bEndpointAddress;
			sc->sc_isize = UGETW(ed->wMaxPacketSize);
			dlog("%d is UE_DIR_IN && UE_INTERRUPT", i);
			dlog("%d ep_intr = %d, isize = %d", i, sc->ep_iintr, sc->sc_isize);
		}

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_OUT &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT){
			sc->ep_ointr = ed->bEndpointAddress;
			sc->sc_osize = UGETW(ed->wMaxPacketSize);
			dlog("%d is UE_DIR_OUT && UE_INTERRUPT", i);
			dlog("%d ep_intr = %d, osize = %d", i, sc->ep_ointr, sc->sc_osize);
		}				

	}

	dlog("");
	if (sc->ep_ointr == -1 || sc->ep_iintr == -1) {
		printf("%s: no data endpoint found\n", USBDEVNAME(sc->sc_dev));
		return;
	}

#ifdef ALLOC_ON_ATTACH
	if (sc->sc_oxfer == NULL) {
		sc->sc_oxfer = usbd_alloc_xfer(sc->sc_udev);
		if (sc->sc_oxfer == NULL)
			goto fail;
		sc->sc_obuf = usbd_alloc_buffer(sc->sc_oxfer, sc->sc_osize);
		if (sc->sc_obuf == NULL)
			goto fail;
	}

	dlog("");
	if (sc->sc_ixfer == NULL) {
		sc->sc_ixfer = usbd_alloc_xfer(sc->sc_udev);
		if (sc->sc_ixfer == NULL)
			goto fail;
		sc->sc_ibuf = usbd_alloc_buffer(sc->sc_ixfer, sc->sc_isize);
		if (sc->sc_ibuf == NULL)
			goto fail;
	}
#endif
	dlog("control endpoint = %d", USB_CONTROL_ENDPOINT);
	
#ifdef USE_OUT
	/*  open OUT pipe */
	dlog("out endp = %d", sc->ep_ointr);
#ifdef OUT_PIPE_INTR
	err =  usbd_open_pipe_intr(sc->sc_iface, sc->ep_ointr,
		USBD_SHORT_XFER_OK, &sc->sc_opipe, sc, sc->sc_obuf,
		sc->sc_osize, upwr_intr_out , UPWR_UPDATE_TICK);
#else
	err = usbd_open_pipe(sc->sc_iface, sc->ep_ointr, 0, &sc->sc_opipe);
#endif
	
	dlog("");
	if (err) {
		printf("%s : could not open intr pipe %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}
#endif

	dlog("in endp = %d", sc->ep_iintr);
	/*  open IN pipe */
	sc->sc_ibuf = malloc(sc->sc_isize, M_USBDEV, M_WAITOK);
	err = usbd_open_pipe_intr(sc->sc_iface, sc->ep_iintr,
		USBD_SHORT_XFER_OK, &sc->sc_ipipe, sc, sc->sc_ibuf,
		sc->sc_isize, upwr_intr_in, UPWR_UPDATE_TICK);

	dlog("");
	if (err) {
		printf("%s : could not open IN intr pipe %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		goto fail;
	}

	/*  no need? */
#ifdef USE_SET_IDLE
	upwr_set_idle(sc);
#endif

	dlog("hgoge");

	// need this?
#ifdef USE_READ_REPORT_DESC
	void *desc;
	int size;
	err = usbd_read_report_desc(sc->sc_iface, &desc, &size, M_USBDEV);
	if (err) {
		printf("%s : could not get report descriptor %s\n",
				sc->sc_dev.dv_xname, usbd_errstr(err));
	}
#endif
	dlog("gogo");
	dlog("gaga");

	/* init */

#ifdef USE_INIT_AT_ATTACH
#ifdef USE_CALLOUT 
	usb_callout_init(sc->sc_upwr_ch);
	usb_callout(sc->sc_upwr_ch, hz, upwr_callout, sc);
#else
	err = upwr_send_cmd(sc, CMD_MODEL);
	dlog("hoge");
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
	}
	dlog("model done");
	
	err = upwr_send_cmd(sc, CMD_OUTLET1_ON);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
	}
	dlog("done send command");
#endif
#endif

	dlog("");
	/* init done ? */
	dlog("init done");

	usbd_add_drv_event(USB_EVENT_DRIVER_ATTACH, sc->sc_udev, USBDEV(sc->sc_dev));
	dlog("done add");

	USB_ATTACH_SUCCESS_RETURN;
fail:
	if (sc->sc_oxfer != NULL && sc->sc_obuf != NULL)
		usbd_free_buffer(sc->sc_oxfer);
	if (sc->sc_ixfer != NULL && sc->sc_ibuf != NULL)
		usbd_free_buffer(sc->sc_ixfer);
	if (sc->sc_ipipe != NULL)
		usbd_close_pipe(sc->sc_ipipe);
	if (sc->sc_opipe != NULL)
		usbd_close_pipe(sc->sc_opipe);
	if (sc->sc_oxfer != NULL)
		usbd_free_xfer(sc->sc_oxfer);
	if (sc->sc_ixfer != NULL)
		usbd_free_xfer(sc->sc_ixfer);

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

	if (sc->sc_oxfer != NULL && sc->sc_obuf != NULL)
		usbd_free_buffer(sc->sc_oxfer);
	if (sc->sc_ixfer != NULL && sc->sc_ibuf != NULL)
		usbd_free_buffer(sc->sc_ixfer);
	if (sc->sc_ipipe != NULL)
		usbd_close_pipe(sc->sc_ipipe);
	if (sc->sc_opipe != NULL)
		usbd_close_pipe(sc->sc_opipe);
	if (sc->sc_oxfer != NULL)
		usbd_free_xfer(sc->sc_oxfer);
	if (sc->sc_ixfer != NULL)
		usbd_free_xfer(sc->sc_ixfer);

	usbd_add_drv_event(USB_EVENT_DRIVER_DETACH, sc->sc_udev,
			USBDEV(sc->sc_dev));

	return (0);
}

#ifdef USE_UPWR_INTR_OUT
void
upwr_intr_out(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status)
{
	struct upwr_softc *sc = (struct upwr_softc *)priv;
	uint8_t buf[64];
	int i;

	if (sc->sc_dying) {
		dlog("dying");
		return;
	}

	if (status != USBD_NORMAL_COMPLETION) {
		if (status == USBD_NOT_STARTED || status == USBD_CANCELLED) {
			return;
		}
		if (status == USBD_STALLED) {
			usbd_clear_endpoint_stall_async(sc->sc_opipe);
		}

		return;
	}

	memcpy(buf, (char *)sc->sc_obuf, 64);
	for (i = 0; i < 64; i++) {
//		printf("%x ", buf[i]);
	}
//	printf("\n");
	switch(sc->sc_issueing_cmd) {
	case CMD_MODEL: /* model */
		dlog("CMD_MODEL");
		dlog("buf[0] = %d\n", buf[0]);
	memcpy(buf, (char *)sc->sc_obuf, 64);
	for (i = 0; i < 64; i++) {
		printf("%x ", buf[i]);
	}
	printf("\n");
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_VERSION: /* version */
		dlog("CMD_VERSION");
		dlog("buf[0] = %d.%d\n", buf[0], buf[1]);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_POLL1:
		dlog("CMD_POLL1");
		dlog("current = %d mA\n", buf[0] << 8 | buf[1]);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_POLL2:
		dlog("CMD_POLL2");
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_OUTLET1_STATUS:
	case CMD_OUTLET2_STATUS:
	case CMD_OUTLET3_STATUS:
		dlog("CMD_OUTLET*_STATUS : %x", sc->sc_issueing_cmd);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_OUTLET1_ON:
	case CMD_OUTLET2_ON:
	case CMD_OUTLET3_ON:
		dlog("CMD_OUTLET*_ON : %x", sc->sc_issueing_cmd);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_OUTLET1_OFF:
	case CMD_OUTLET2_OFF:
	case CMD_OUTLET3_OFF:
		dlog("CMD_OUTLET*_OFF : %x", sc->sc_issueing_cmd);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_NONE:
		break;
	default:
		dlog("????");
		sc->sc_issueing_cmd = CMD_NONE;
		break;
	}	

}
#endif

void
upwr_intr_in(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status)
{
	struct upwr_softc *sc = (struct upwr_softc *)priv;
	uint8_t buf[64];
	int i;

	if (sc->sc_dying)
		return;

	if (status != USBD_NORMAL_COMPLETION) {
		dlog("error intr");
		if (status == USBD_NOT_STARTED || status == USBD_CANCELLED) {
			return;
		}
		if (status == USBD_STALLED) {
			usbd_clear_endpoint_stall_async(sc->sc_ipipe);
		}
		return;
	}	

	memcpy(buf, (char *)sc->sc_ibuf, 64);
	switch(sc->sc_issueing_cmd) {
	case CMD_MODEL: /* model */
		dlog("CMD_MODEL");
		dlog("buf[0] = %d\n", buf[0]);
	memcpy(buf, (char *)sc->sc_obuf, 64);
	for (i = 0; i < 64; i++) {
		printf("%x ", buf[i]);
	}
	printf("\n");
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_VERSION: /* version */
		dlog("CMD_VERSION");
		dlog("buf[0] = %d.%d\n", buf[0], buf[1]);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_POLL1:
		dlog("CMD_POLL1");
		dlog("current = %d mA\n", buf[0] << 8 | buf[1]);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_POLL2:
		dlog("CMD_POLL2");
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_OUTLET1_STATUS:
	case CMD_OUTLET2_STATUS:
	case CMD_OUTLET3_STATUS:
		dlog("CMD_OUTLET*_STATUS : %x", sc->sc_issueing_cmd);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_OUTLET1_ON:
	case CMD_OUTLET2_ON:
	case CMD_OUTLET3_ON:
		dlog("CMD_OUTLET*_ON : %x", sc->sc_issueing_cmd);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_OUTLET1_OFF:
	case CMD_OUTLET2_OFF:
	case CMD_OUTLET3_OFF:
		dlog("CMD_OUTLET*_OFF : %x", sc->sc_issueing_cmd);
		sc->sc_issueing_cmd = CMD_NONE;
		sc->sc_accepted_cmd = CMD_MODEL;
		break;
	case CMD_NONE:
		break;
	default:
		dlog("????");
		sc->sc_issueing_cmd = CMD_NONE;
		break;
	}

	return;
}

#ifdef USE_SET_IDLE
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

	err = usbd_do_request(sc->sc_udev, &req, &sc->sc_ibuf);
	if (err) {
		printf("%s : could not issue command %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		return (EIO);
	}

	return 0;
}

#endif

#ifdef USE_RAW_INTR_TRANSFER
static void
upwr_transfer_cb(usbd_xfer_handle xfer, usbd_private_handle priv,
		      usbd_status status)
{
	wakeup(xfer);
}
#endif

#ifdef USE_SEND_CMD
static usbd_status
upwr_send_cmd(struct upwr_softc *sc, uint8_t cmd)
{
#ifdef USE_INTR_TRANSFER
	usbd_status err;	
	int i;
	uint8_t	buf[64];
#ifdef USE_RAW_INTR_TRANSFER
	int s;
#endif

	sc->sc_oxfer = usbd_alloc_xfer(sc->sc_udev);
	if (sc->sc_oxfer == NULL)
		return EIO;

//	memset(sc->sc_obuf, CMD_PADDING, sc->sc_osize);
	memset(buf, CMD_PADDING, sc->sc_osize);
//	sc->sc_obuf[0] = cmd;
	buf[0] = cmd;

	sc->sc_issueing_cmd = cmd;
	sc->sc_accepted_cmd = CMD_NONE;

	for (i = 0; i < 64; i++) {
//		printf("%x ", sc->sc_obuf[i]);
		printf("%x ", buf[i]);
	}
	printf("\n");

#ifndef USE_RAW_INTR_TRANSFER
	err = usbd_intr_transfer(sc->sc_oxfer, sc->sc_opipe,
			0, 1, buf, &sc->sc_osize, "upwr");
#else
	usbd_setup_xfer(sc->sc_oxfer, sc->sc_opipe, 0, buf, sc->sc_osize,
			0, 1, upwr_transfer_cb);
	s = splusb();
	err = usbd_transfer(sc->sc_oxfer);
	if (err != USBD_IN_PROGRESS) {
		splx(s);
		dlog("failed setup xfer %s", usbd_errstr(err));
		return err;
	}
	//error = tsleep(xfer, PZERO |PCATCH, "upwr", 0);
	splx(s);
	usbd_get_xfer_status(sc->sc_oxfer, NULL, NULL, &i, &err);
	dlog("transfer size %d", i);
	if (err) {
		dlog("errored %s\n", usbd_errstr(err));
		usbd_clear_endpoint_stall_async(sc->sc_opipe);
	}

	return (err);


#endif
	dlog("after usbd_intr_transfer");
	usbd_free_xfer(sc->sc_oxfer);

	if (err) {
		dlog("transfer failed %s", usbd_errstr(err));
		if (err == USBD_INTERRUPTED)
			return EINTR;
		if (err == USBD_TIMEOUT)
			return ETIMEDOUT;
		return EIO;
	}

	dlog("transfer is ok");

	/* wait ack? : do we need this? */
//	tsleep(sc, 0, "upwr", hz/10);

	return 0;
#endif

#ifdef USE_RAW_DO_REQUEST
	usb_device_request_t req;
	usbd_status err;
	uint8_t	buf[62]


	//req.bmRequestType = UT_READ_CLASS_INTERFACE;//UT_WRITE_VENDOR_DEVICE;
	req.bmRequestType = cmd;
	req.bRequest = 0xff;
	USETW(req.wValue, 0);
	USETW(req.wIndex, 0);
	USETW(req.wLength, 64);

	err = usbd_do_request(sc->sc_udev, &req, &buf);
	if (err) {
		printf("%s : could not issue command %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
		return (EIO);
	}

	return 0;
#endif
#ifdef USE_SET_REPORT



	uint8_t req[64];
	usbd_status err;
	int i = 0;
	int size =0;
	int s;

	printf("send_cmd xfer=%p opipe=%p\n", sc->sc_oxfer, sc->sc_opipe);
	
	memset(req, CMD_PADDING, sizeof(req));
	req[0] = cmd;

	for (i = 0; i < 64; i++) {
		printf("%x ", req[i]);
	}
	printf("\n");

	/* need something ? */

	size = sizeof(req);
	/*
	// won't work?
	err = usbd_intr_transfer(sc->sc_xfer, sc->sc_ipipe,
			0, hz, req, &size, "upwr");
	*/

	// this does send setup transaction, and then DATA1 output report.
	// In windows, only DATA0 output report goes.
	// Something nasty?
	err = usbd_set_report_async(sc->sc_iface, UHID_OUTPUT_REPORT, 0, req,
			144);

	if (err) {
		printf("send_cmd : %s\n", usbd_errstr(err));
		dlog("usbd_set_report_error : EIO %d\n",err);
		return err;
	}
	printf("done transfer");

	
	/* wait ack? : do we need this? */
	//tsleep(sc, 0, "upwr", 1);

	return 0;
#endif
}
#endif

#ifdef USE_CALLOUT
static void
upwr_callout(void *arg)
{
	struct upwr_softc *sc = arg;
	usbd_status err;

	err = upwr_send_cmd(sc, CMD_MODEL);
	dlog("hoge");
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
	}
	dlog("model done");
	
	err = upwr_send_cmd(sc, CMD_OUTLET1_ON);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
			USBDEVNAME(sc->sc_dev), usbd_errstr(err));
	}
	dlog("done send command");
}
#endif
