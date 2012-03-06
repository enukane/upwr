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

#define USE_SET_REPORT

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
#else
#define	dlog(...)	
#endif

#define UPWR_UPDATE_TICK	3	

#define CMD_INIT	0xaa
#define CMD_INIT0	0xa7
#define	CMD_INIT1	0xa1
#define	CMD_INIT2	0xa2
#define	CMD_INIT3	0xac
#define	CMD_POLL1	0xb1
#define	CMD_POLL2	0xb2
#define	CMD_OUTLET1_ON	0x41
#define	CMD_OUTLET1_OFF	0x42
#define	CMD_OUTLET2_ON	0x43
#define	CMD_OUTLET2_OFF	0x44
#define	CMD_OUTLET3_ON	0x45
#define	CMD_OUTLET3_OFF	0x50
#define	CMD_PADDING	0xff

struct upwr_softc {
	USBBASEDEVICE		sc_dev;
	usbd_device_handle	sc_udev;
	usbd_interface_handle	sc_iface;
	usbd_pipe_handle	sc_ipipe;
	int			sc_isize;
	usbd_xfer_handle	sc_xfer;
	uint8_t			*sc_buf;
	uint8_t			*sc_intrbuf;

	char			sc_dying;

	uint8_t			sc_issuing_cmd;
};

// 0x04d8, 0x003f
const struct usb_devno upwr_devs[] = {
	{ USB_VENDOR_MICROCHIP, USB_PRODUCT_MICROCHIP_POWERUSB },
};
#define upwr_lookup(v, p) usb_lookup(upwr_devs, v, p)

//static void upwr_intr(struct uhidev *addr, void *ibuf, u_int len);
static void upwr_intr(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status);
static usbd_status upwr_send_cmd(struct upwr_softc *sc, uint8_t val);
static usbd_status upwr_set_idle(struct upwr_softc *sc);

//static void upwr_callout(void *);


//Static int	upwr_ioctl(void *, u_long, caddr_t, int, usb_proc_ptr );

USB_DECLARE_DRIVER(upwr);

USB_MATCH(upwr)
{
	USB_MATCH_START(upwr, uaa);

	dlog("vid = %x, pid = %x", uaa->vendor, uaa->product);

	if (uaa->iface != NULL)
		return UMATCH_NONE;

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
	int ep_ibulk, ep_obulk, ep_intr;
	usbd_status err;
	int i;
	char devinfo[1024];
	int size;
	void *desc;

	sc->sc_udev = uaa->device;

	usbd_devinfo(sc->sc_udev, 0, devinfo, sizeof(devinfo));
	USB_ATTACH_SETUP;
	printf("%s : %s\n", USBDEVNAME(sc->sc_dev), devinfo);

// XXX: is this right?
#define UPWR_USB_IFACE	0
#define UPWR_USB_CONFIG	1

	/* set configuration */
	if ((err = usbd_set_config_no(sc->sc_udev, UPWR_USB_CONFIG, 1)) != 0) {
		printf("%s : failed to set config %d : %s\n",
				sc->sc_dev.dv_xname, UPWR_USB_CONFIG, usbd_errstr(err));

		USB_ATTACH_ERROR_RETURN;
	}

	/* get interface handle*/
	if ((err = usbd_device2interface_handle(sc->sc_udev, UPWR_USB_IFACE,
			&sc->sc_iface)) != 0) {
		printf("%s: failed to get interface %d: %s\n",
				sc->sc_dev.dv_xname, UPWR_USB_IFACE, usbd_errstr(err));

		USB_ATTACH_ERROR_RETURN;
	}

	/* find endpoints */
	ep_ibulk = ep_obulk = ep_intr = -1;
	id = usbd_get_interface_descriptor(sc->sc_iface);
	for (i = 0; i < id->bNumEndpoints; i++) {
		dlog("numed %d", i);
		ed = usbd_interface2endpoint_descriptor(sc->sc_iface, i);
		if (ed == NULL) {
			printf("%s: failed to get endpoint %d descriptor\n",
			    sc->sc_dev.dv_xname, i);
			return;
		}
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
		// in this case only got here
		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT){
			ep_intr = ed->bEndpointAddress;
			sc->sc_isize = UGETW(ed->wMaxPacketSize);
			dlog("%d is UE_DIR_IN && UE_INTERRUPT", i);
			dlog("%d ep_intr = %d, isize = %d", i, ep_intr, sc->sc_isize);
		}
	}

	if (ep_intr == -1) {
		printf("%s: no data endpoint found\n", sc->sc_dev.dv_xname);
		return;
	}

	if (sc->sc_xfer == NULL) {
		sc->sc_xfer = usbd_alloc_xfer(sc->sc_udev);
		if (sc->sc_xfer == NULL)
			goto fail;
		sc->sc_buf = usbd_alloc_buffer(sc->sc_xfer, sc->sc_isize);
		if (sc->sc_buf == NULL)
			goto fail;
	}
	/* open interrupt endpoint */
//	sc->sc_intrbuf = malloc(sc->sc_isize, M_USBDEV, M_WAITOK);
//	if (sc->sc_intrbuf == NULL)
//		goto fail;
	err = usbd_open_pipe_intr(sc->sc_iface, ep_intr,
		USBD_SHORT_XFER_OK, &sc->sc_ipipe, sc, sc->sc_buf,
		sc->sc_isize, upwr_intr, UPWR_UPDATE_TICK);
	
	if (err) {
		printf("%s : could not open intr pipe %s\n",
			sc->sc_dev.dv_xname, usbd_errstr(err));
		goto fail;
	}

	/* init done ? */
	dlog("init done");

	upwr_set_idle(sc);

	// need this?
	err = usbd_read_report_desc(sc->sc_iface, &desc, &size, M_USBDEV);
	if (err) {
		printf("%s : could not get report descriptor %s\n",
				sc->sc_dev.dv_xname, usbd_errstr(err));
	}

	dlog("desc @ %p, size = %d", desc, size);

	/* init */
	err = upwr_send_cmd(sc, CMD_INIT);
	if (err) {
		printf("%s : upwr_send_cmd failed : %s\n",
			sc->sc_dev.dv_xname, usbd_errstr(err));
	}
	

	dlog("done send command");

	usbd_add_drv_event(USB_EVENT_DRIVER_ATTACH, sc->sc_udev, USBDEV(sc->sc_dev));

	USB_ATTACH_SUCCESS_RETURN;
fail:
	if (sc->sc_xfer != NULL && sc->sc_buf != NULL)
		usbd_free_buffer(sc->sc_xfer);
	if (sc->sc_ipipe != NULL)
		usbd_close_pipe(sc->sc_ipipe);
	if (sc->sc_xfer != NULL)
		usbd_free_xfer(sc->sc_xfer);
//	if (sc->sc_intrbuf != NULL)
//		free(sc->sc_intrbuf, M_USBDEV);

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
			sc->sc_dying = 1;
			break;
	}
	return (0);
}

USB_DETACH(upwr)
{
	USB_DETACH_START(upwr, sc);
	int s;

	dlog("upwr_detach: sc=%p flags=%d\n", sc, flags);

	s = splusb();

	if (sc->sc_ipipe != NULL) {
		usbd_abort_pipe(sc->sc_ipipe);
		usbd_close_pipe(sc->sc_ipipe);
//		if (sc->sc_intrbuf != NULL)
//			free(sc->sc_intrbuf, M_USBDEV);
		sc->sc_ipipe = NULL;
	}

	if (sc->sc_xfer != NULL)
		usbd_free_xfer(sc->sc_xfer);

	splx(s);
	
	
	usbd_add_drv_event(USB_EVENT_DRIVER_DETACH, sc->sc_udev,
			USBDEV(sc->sc_dev));

	return (0);
}

void
upwr_intr(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status)
{
	struct upwr_softc *sc = (struct upwr_softc *)priv;
	uint8_t buf[64];
	int i;

	if (sc->sc_dying)
		return;

	if (status != USBD_NORMAL_COMPLETION) {
		if (status == USBD_NOT_STARTED || status == USBD_CANCELLED)
			return;
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->sc_ipipe);
		return;
	}	

//	if (sc->sc_intrbuf == NULL)
//		return;

	memcpy(buf, (char *)sc->sc_intrbuf, 64);
	for (i = 0; i < 64; i++) {
		printf("%x ", buf[i]);
	}
	printf("\n");

	switch(sc->sc_issuing_cmd) {
		default:
			dlog("????");
	}

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

	err = usbd_do_request(sc->sc_udev, &req, &sc->sc_buf);
	if (err) {
		printf("%s : could not issue command %s\n",
			sc->sc_dev.dv_xname, usbd_errstr(err));
		return (EIO);
	}

	return 0;
}

static usbd_status
upwr_send_cmd(struct upwr_softc *sc, uint8_t cmd)
{

#if USE_RAW_DO_REQUEST
	usb_device_request_t req;
	usbd_status err;


	//req.bmRequestType = UT_READ_CLASS_INTERFACE;//UT_WRITE_VENDOR_DEVICE;
	req.bmRequestType = cmd;
	req.bRequest = 0xff;
	USETW(req.wValue, 0);
	USETW(req.wIndex, 0);
	USETW(req.wLength, 64);

	err = usbd_do_request(sc->sc_udev, &req, &sc->sc_buf);
	if (err) {
		printf("%s : could not issue command %s\n",
			sc->sc_dev.dv_xname, usbd_errstr(err));
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

	printf("send_cmd xfer=%p ipipe=%p\n", sc->sc_xfer, sc->sc_ipipe);
	
	memset(req, CMD_PADDING, sizeof(req));
	req[0] = cmd;

	for (i = 0; i < 64; i++) {
		printf("%x ", req[i]);
	}
	printf("\n");

	/* need something ? */
	s = splusb();

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

	splx(s);;
	
	/* wait ack? : do we need this? */
	tsleep(sc, 0, "upwr", hz/10);

	return 0;
#endif
}

#if 0
static void
upwr_callout(void *arg)
{
	struct upwr_softc *sc = arg;
	uint8_t cmd;

	cmd = CMD_OUTLET1_ON;
	if (sc->sc_oldval == cmd) cmd = CMD_OUTLET1_OFF;

	upwr_send_cmd(sc, cmd);
	sc->sc_oldval = cmd;

	callout_reset(&sc->sc_callout, UPWR_UPDATE_TICK * hz, upwr_callout, sc);
}
#endif
