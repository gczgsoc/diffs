/*	$OpenBSD: ugen.c,v 1.85 2015/07/10 15:45:57 mpi Exp $ */
/*	$NetBSD: ugen.c,v 1.63 2002/11/26 18:49:48 christos Exp $	*/
/*	$FreeBSD: src/sys/dev/usb/ugen.c,v 1.26 1999/11/17 22:33:41 n_hibma Exp $	*/

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


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/conf.h>
#include <sys/tty.h>
#include <sys/file.h>
#include <sys/selinfo.h>
#include <sys/vnode.h>
#include <sys/poll.h>
#include <sys/rwlock.h>

#include <machine/bus.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usb_mem.h>
#include <dev/usb/usbdevs.h>

#ifdef UGEN_DEBUG
#define DPRINTF(x)	do { if (ugendebug) printf x; } while (0)
#define DPRINTFN(n,x)	do { if (ugendebug>(n)) printf x; } while (0)
int	ugendebug = 0;
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

#define	UGEN_CHUNK	128	/* chunk size for read */
#define	UGEN_IBSIZE	1020	/* buffer size */
#define	UGEN_BBSIZE	1024

#define	UGEN_NISOFRAMES	500	/* 0.5 seconds worth */
#define UGEN_NISOREQS	6	/* number of outstanding xfer requests */
#define UGEN_NISORFRMS	4	/* number of frames (miliseconds) per req */

struct rwlock q_lock;

struct ugen_endpoint {
	struct ugen_softc *sc;
	usb_endpoint_descriptor_t *edesc;
	struct usbd_interface *iface;
	int state;
#define	UGEN_ASLP	0x02	/* waiting for data */
#define UGEN_SHORT_OK	0x04	/* short xfers are OK */
	struct usbd_pipe *pipeh;
	struct clist q;
	struct selinfo rsel;
	u_char *ibuf;		/* start of buffer (circular for isoc) */
	u_char *fill;		/* location for input (isoc) */
	u_char *limit;		/* end of circular buffer (isoc) */
	u_char *cur;		/* current read location (isoc) */
	u_int32_t timeout;
	struct isoreq {
		struct ugen_endpoint *sce;
		struct usbd_xfer *xfer;
		void *dmabuf;
		u_int16_t sizes[UGEN_NISORFRMS];
	} isoreqs[UGEN_NISOREQS];
	TAILQ_HEAD(, usb_ctl_request) submit_queue;
	TAILQ_HEAD(, usb_ctl_request) complete_queue;
};

struct ugen_softc {
	struct device sc_dev;		/* base device */
	struct usbd_device *sc_udev;

	char sc_is_open[USB_MAX_ENDPOINTS];
	struct ugen_endpoint sc_endpoints[USB_MAX_ENDPOINTS][2];
#define OUT 0
#define IN  1

	int sc_refcnt;
	u_char sc_secondary;
};

void ugen_async_callback(struct usbd_xfer *, void *, usbd_status);
int ugen_submit_ctrl(struct ugen_softc *, struct
    usb_ctl_request *, struct proc *p);
int ugen_submit_bulk(struct ugen_softc *, struct
    usb_ctl_request *, struct proc *p);
int ugen_complete_ctrl(struct usb_ctl_request *, struct proc
    *p);
int ugen_complete_bulk(struct usb_ctl_request *, struct proc
    *p);

void ugenintr(struct usbd_xfer *xfer, void *addr, usbd_status status);
void ugen_isoc_rintr(struct usbd_xfer *xfer, void *addr, usbd_status status);
int ugen_do_read(struct ugen_softc *, int, struct uio *, int);
int ugen_do_write(struct ugen_softc *, int, struct uio *, int);
int ugen_do_ioctl(struct ugen_softc *, int, u_long, caddr_t, int,
	struct proc *);
int ugen_do_close(struct ugen_softc *, int, int);
int ugen_set_config(struct ugen_softc *sc, int configno);
int ugen_set_interface(struct ugen_softc *, int, int);
int ugen_get_alt_index(struct ugen_softc *sc, int ifaceidx);

#define UGENUNIT(n) ((minor(n) >> 4) & 0xf)
#define UGENENDPOINT(n) (minor(n) & 0xf)
#define UGENDEV(u, e) (makedev(0, ((u) << 4) | (e)))

int ugen_match(struct device *, void *, void *);
void ugen_attach(struct device *, struct device *, void *);
int ugen_detach(struct device *, int);

struct cfdriver ugen_cd = {
	NULL, "ugen", DV_DULL
};

const struct cfattach ugen_ca = {
	sizeof(struct ugen_softc), ugen_match, ugen_attach, ugen_detach
};

void ugen_async_callback(struct usbd_xfer *xfer, void *priv, usbd_status s) {
	struct usb_ctl_request *req = priv;
	struct ugen_endpoint *sce = (struct ugen_endpoint *)req->ucr_sce;

	if (s == USBD_CANCELLED)
		req->ucr_status = USBD_CANCELLED;

	TAILQ_REMOVE(&sce->submit_queue, req, entries);
	TAILQ_INSERT_TAIL(&sce->complete_queue, req, entries);
	selwakeup(&sce->rsel);
}

int
ugen_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa = aux;

	if (uaa->usegeneric) {
		return (UMATCH_GENERIC);
	} else
		return (UMATCH_NONE);
}

void
ugen_attach(struct device *parent, struct device *self, void *aux)
{
	struct ugen_softc *sc = (struct ugen_softc *)self;
	struct usb_attach_arg *uaa = aux;
	struct usbd_device *udev;
	usbd_status err;
	int conf;

	sc->sc_udev = udev = uaa->device;

	if (usbd_get_devcnt(udev) > 0)
		sc->sc_secondary = 1;

	if (!sc->sc_secondary) {
		/* First set configuration index 0, the default one for ugen. */
		err = usbd_set_config_index(udev, 0, 0);
		if (err) {
			printf("%s: setting configuration index 0 failed\n",
			       sc->sc_dev.dv_xname);
			usbd_deactivate(sc->sc_udev);
			return;
		}
	}
	conf = usbd_get_config_descriptor(udev)->bConfigurationValue;

	/* Set up all the local state for this configuration. */
	err = ugen_set_config(sc, conf);
	if (err) {
		printf("%s: setting configuration %d failed\n",
		       sc->sc_dev.dv_xname, conf);
		usbd_deactivate(sc->sc_udev);
		return;
	}
}

int
ugen_set_config(struct ugen_softc *sc, int configno)
{
	struct usbd_device *dev = sc->sc_udev;
	usb_config_descriptor_t *cdesc;
	usb_interface_descriptor_t *id;
	struct usbd_interface *iface;
	usb_endpoint_descriptor_t *ed;
	struct ugen_endpoint *sce;
	int ifaceno, endptno, endpt;
	int err, dir;

	DPRINTFN(1,("ugen_set_config: %s to configno %d, sc=%p\n",
		    sc->sc_dev.dv_xname, configno, sc));

	/*
	 * We start at 1, not 0, because we don't care whether the
	 * control endpoint is open or not. It is always present.
	 */
	for (endptno = 1; endptno < USB_MAX_ENDPOINTS; endptno++)
		if (sc->sc_is_open[endptno]) {
			DPRINTFN(1,
			     ("ugen_set_config: %s - endpoint %d is open\n",
			      sc->sc_dev.dv_xname, endptno));
			return (USBD_IN_USE);
		}

	/* Avoid setting the current value. */
	cdesc = usbd_get_config_descriptor(dev);
	if (cdesc == NULL || cdesc->bConfigurationValue != configno) {
		if (sc->sc_secondary) {
			printf("%s: secondary, not changing config to %d\n",
			    __func__, configno);
			return (USBD_IN_USE);
		} else {
			err = usbd_set_config_no(dev, configno, 1);
			if (err)
				return (err);
		}
	}

	memset(sc->sc_endpoints, 0, sizeof sc->sc_endpoints);
	for (ifaceno = 0; ifaceno < cdesc->bNumInterface; ifaceno++) {
		DPRINTFN(1,("ugen_set_config: ifaceno %d\n", ifaceno));
		if (usbd_iface_claimed(sc->sc_udev, ifaceno)) {
			DPRINTF(("%s: iface %d not available\n", __func__,
			    ifaceno));
			continue;
		}
		err = usbd_device2interface_handle(dev, ifaceno, &iface);
		if (err)
			return (err);
		id = usbd_get_interface_descriptor(iface);
		for (endptno = 0; endptno < id->bNumEndpoints; endptno++) {
			ed = usbd_interface2endpoint_descriptor(iface,endptno);
			endpt = ed->bEndpointAddress;
			dir = UE_GET_DIR(endpt) == UE_DIR_IN ? IN : OUT;
			sce = &sc->sc_endpoints[UE_GET_ADDR(endpt)][dir];
			DPRINTFN(1,("ugen_set_config: endptno %d, endpt=0x%02x"
				    "(%d,%d), sce=%p\n",
				    endptno, endpt, UE_GET_ADDR(endpt),
				    UE_GET_DIR(endpt), sce));
			sce->sc = sc;
			sce->edesc = ed;
			sce->iface = iface;
		}
	}
	return (0);
}

int
ugenopen(dev_t dev, int flag, int mode, struct proc *p)
{
	struct ugen_softc *sc;
	int unit = UGENUNIT(dev);
	int endpt = UGENENDPOINT(dev);
	usb_endpoint_descriptor_t *edesc;
	struct ugen_endpoint *sce;
	int dir, isize;
	usbd_status err;
	struct usbd_xfer *xfer;
	void *buf;
	int i, j;

	if (unit >= ugen_cd.cd_ndevs)
		return (ENXIO);
	sc = ugen_cd.cd_devs[unit];
	if (sc == NULL)
		return (ENXIO);

	DPRINTFN(5, ("ugenopen: flag=%d, mode=%d, unit=%d endpt=%d\n",
		     flag, mode, unit, endpt));

	if (sc == NULL || usbd_is_dying(sc->sc_udev))
		return (ENXIO);

	if (sc->sc_is_open[endpt])
		return (EBUSY);

	sce = &sc->sc_endpoints[endpt][IN];
	TAILQ_INIT(&sce->submit_queue);
	TAILQ_INIT(&sce->complete_queue);

	rw_init(&q_lock, "q_lock");

	if (endpt == USB_CONTROL_ENDPOINT) {
		sc->sc_is_open[USB_CONTROL_ENDPOINT] = 1;
		return (0);
	}

	/* Make sure there are pipes for all directions. */
	for (dir = OUT; dir <= IN; dir++) {
		if (flag & (dir == OUT ? FWRITE : FREAD)) {
			sce = &sc->sc_endpoints[endpt][dir];
			if (sce == 0 || sce->edesc == 0)
				return (ENXIO);
		}
	}

	/* Actually open the pipes. */
	/* XXX Should back out properly if it fails. */
	for (dir = OUT; dir <= IN; dir++) {
		if (!(flag & (dir == OUT ? FWRITE : FREAD)))
			continue;
		sce = &sc->sc_endpoints[endpt][dir];
		sce->state = 0;
		sce->timeout = USBD_NO_TIMEOUT;
		DPRINTFN(5, ("ugenopen: sc=%p, endpt=%d, dir=%d, sce=%p\n",
			     sc, endpt, dir, sce));
		edesc = sce->edesc;
		switch (edesc->bmAttributes & UE_XFERTYPE) {
		case UE_INTERRUPT:
			if (dir == OUT) {
				err = usbd_open_pipe(sce->iface,
				    edesc->bEndpointAddress, 0, &sce->pipeh);
				if (err)
					return (EIO);
				break;
			}
			isize = UGETW(edesc->wMaxPacketSize);
			if (isize == 0)	/* shouldn't happen */
				return (EINVAL);
			sce->ibuf = malloc(isize, M_USBDEV, M_WAITOK);
			DPRINTFN(5, ("ugenopen: intr endpt=%d,isize=%d\n",
				     endpt, isize));
			clalloc(&sce->q, UGEN_IBSIZE, 0);
			err = usbd_open_pipe_intr(sce->iface,
				  edesc->bEndpointAddress,
				  USBD_SHORT_XFER_OK, &sce->pipeh, sce,
				  sce->ibuf, isize, ugenintr,
				  USBD_DEFAULT_INTERVAL);
			if (err) {
				free(sce->ibuf, M_USBDEV, 0);
				clfree(&sce->q);
				return (EIO);
			}
			DPRINTFN(5, ("ugenopen: interrupt open done\n"));
			break;
		case UE_BULK:
			err = usbd_open_pipe(sce->iface,
				  edesc->bEndpointAddress, 0, &sce->pipeh);
			if (err)
				return (EIO);
			break;
		case UE_ISOCHRONOUS:
			if (dir == OUT)
				return (EINVAL);
			isize = UGETW(edesc->wMaxPacketSize);
			if (isize == 0)	/* shouldn't happen */
				return (EINVAL);
			sce->ibuf = mallocarray(isize, UGEN_NISOFRAMES,
				M_USBDEV, M_WAITOK);
			sce->cur = sce->fill = sce->ibuf;
			sce->limit = sce->ibuf + isize * UGEN_NISOFRAMES;
			DPRINTFN(5, ("ugenopen: isoc endpt=%d, isize=%d\n",
				     endpt, isize));
			err = usbd_open_pipe(sce->iface,
				  edesc->bEndpointAddress, 0, &sce->pipeh);
			if (err) {
				free(sce->ibuf, M_USBDEV, 0);
				return (EIO);
			}
			for(i = 0; i < UGEN_NISOREQS; ++i) {
				sce->isoreqs[i].sce = sce;
				xfer = usbd_alloc_xfer(sc->sc_udev);
				if (xfer == 0)
					goto bad;
				sce->isoreqs[i].xfer = xfer;
				buf = usbd_alloc_buffer
					(xfer, isize * UGEN_NISORFRMS);
				if (buf == 0) {
					i++;
					goto bad;
				}
				sce->isoreqs[i].dmabuf = buf;
				for(j = 0; j < UGEN_NISORFRMS; ++j)
					sce->isoreqs[i].sizes[j] = isize;
				usbd_setup_isoc_xfer(xfer, sce->pipeh,
				    &sce->isoreqs[i], sce->isoreqs[i].sizes,
				    UGEN_NISORFRMS, USBD_NO_COPY |
				    USBD_SHORT_XFER_OK, ugen_isoc_rintr);
				(void)usbd_transfer(xfer);
			}
			DPRINTFN(5, ("ugenopen: isoc open done\n"));
			break;
		bad:
			while (--i >= 0) /* implicit buffer free */
				usbd_free_xfer(sce->isoreqs[i].xfer);
			return (ENOMEM);
		case UE_CONTROL:
			sce->timeout = USBD_DEFAULT_TIMEOUT;
			return (EINVAL);
		}
	}
	sc->sc_is_open[endpt] = 1;
	return (0);
}

int
ugenclose(dev_t dev, int flag, int mode, struct proc *p)
{
	struct ugen_softc *sc = ugen_cd.cd_devs[UGENUNIT(dev)];
	int endpt = UGENENDPOINT(dev);
	int error;

	if (sc == NULL || usbd_is_dying(sc->sc_udev))
		return (EIO);

	DPRINTFN(5, ("ugenclose: flag=%d, mode=%d, unit=%d, endpt=%d\n",
		     flag, mode, UGENUNIT(dev), endpt));

	sc->sc_refcnt++;
	error = ugen_do_close(sc, endpt, flag);
	if (--sc->sc_refcnt < 0)
		usb_detach_wakeup(&sc->sc_dev);

	return (error);
}

int
ugen_do_close(struct ugen_softc *sc, int endpt, int flag)
{
	struct ugen_endpoint *sce;
	int dir, i;
	struct usb_ctl_request *req;
	int s;

#ifdef DIAGNOSTIC
	if (!sc->sc_is_open[endpt]) {
		printf("ugenclose: not open\n");
		return (EINVAL);
	}
#endif
	sce = &sc->sc_endpoints[endpt][IN];
	s = splusb();
	rw_enter_write(&q_lock);
	//while ((req = TAILQ_FIRST(&sce->submit_queue))) {
	//	TAILQ_REMOVE(&sce->submit_queue, req, entries);
	//	usbd_free_xfer(req->xfer);
	//	free(req, M_TEMP, sizeof(*req));
	//}
	while ((req = TAILQ_FIRST(&sce->complete_queue))) {
		TAILQ_REMOVE(&sce->complete_queue, req, entries);
		usbd_free_xfer(req->xfer);
		free(req, M_TEMP, sizeof(*req));
	}
	rw_exit_write(&q_lock);
	splx(s);

	if (endpt == USB_CONTROL_ENDPOINT) {
		DPRINTFN(5, ("ugenclose: close control\n"));
		sc->sc_is_open[endpt] = 0;
		return (0);
	}

	for (dir = OUT; dir <= IN; dir++) {
		if (!(flag & (dir == OUT ? FWRITE : FREAD)))
			continue;
		sce = &sc->sc_endpoints[endpt][dir];
		if (sce == NULL || sce->pipeh == NULL)
			continue;
		DPRINTFN(5, ("ugenclose: endpt=%d dir=%d sce=%p\n",
			     endpt, dir, sce));

		usbd_close_pipe(sce->pipeh);
		sce->pipeh = NULL;

		switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
		case UE_INTERRUPT:
			ndflush(&sce->q, sce->q.c_cc);
			clfree(&sce->q);
			break;
		case UE_ISOCHRONOUS:
			for (i = 0; i < UGEN_NISOREQS; ++i)
				usbd_free_xfer(sce->isoreqs[i].xfer);

		default:
			break;
		}

		if (sce->ibuf != NULL) {
			free(sce->ibuf, M_USBDEV, 0);
			sce->ibuf = NULL;
		}
	}
	sc->sc_is_open[endpt] = 0;

	return (0);
}

int
ugen_do_read(struct ugen_softc *sc, int endpt, struct uio *uio, int flag)
{
	struct ugen_endpoint *sce = &sc->sc_endpoints[endpt][IN];
	u_int32_t n, tn;
	char buf[UGEN_BBSIZE];
	struct usbd_xfer *xfer;
	usbd_status err;
	int s;
	int flags, error = 0;
	u_char buffer[UGEN_CHUNK];

	DPRINTFN(5, ("%s: ugenread: %d\n", sc->sc_dev.dv_xname, endpt));

	if (usbd_is_dying(sc->sc_udev))
		return (EIO);

	if (endpt == USB_CONTROL_ENDPOINT)
		return (ENODEV);

#ifdef DIAGNOSTIC
	if (sce->edesc == NULL) {
		printf("ugenread: no edesc\n");
		return (EIO);
	}
	if (sce->pipeh == NULL) {
		printf("ugenread: no pipe\n");
		return (EIO);
	}
#endif

	switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
	case UE_INTERRUPT:
		/* Block until activity occurred. */
		s = splusb();
		while (sce->q.c_cc == 0) {
			if (flag & IO_NDELAY) {
				splx(s);
				return (EWOULDBLOCK);
			}
			sce->state |= UGEN_ASLP;
			DPRINTFN(5, ("ugenread: sleep on %p\n", sce));
			error = tsleep(sce, PZERO | PCATCH, "ugenri",
			    (sce->timeout * hz) / 1000);
			sce->state &= ~UGEN_ASLP;
			DPRINTFN(5, ("ugenread: woke, error=%d\n", error));
			if (usbd_is_dying(sc->sc_udev))
				error = EIO;
			if (error == EWOULDBLOCK) {	/* timeout, return 0 */
				error = 0;
				break;
			}
			if (error)
				break;
		}
		splx(s);

		/* Transfer as many chunks as possible. */
		while (sce->q.c_cc > 0 && uio->uio_resid > 0 && !error) {
			n = min(sce->q.c_cc, uio->uio_resid);
			if (n > sizeof(buffer))
				n = sizeof(buffer);

			/* Remove a small chunk from the input queue. */
			q_to_b(&sce->q, buffer, n);
			DPRINTFN(5, ("ugenread: got %d chars\n", n));

			/* Copy the data to the user process. */
			error = uiomovei(buffer, n, uio);
			if (error)
				break;
		}
		break;
	case UE_BULK:
		xfer = usbd_alloc_xfer(sc->sc_udev);
		if (xfer == 0)
			return (ENOMEM);
		flags = USBD_SYNCHRONOUS;
		if (sce->state & UGEN_SHORT_OK)
			flags |= USBD_SHORT_XFER_OK;
		if (sce->timeout == 0)
			flags |= USBD_CATCH;
		while ((n = min(UGEN_BBSIZE, uio->uio_resid)) != 0) {
			DPRINTFN(1, ("ugenread: start transfer %d bytes\n",n));
			usbd_setup_xfer(xfer, sce->pipeh, 0, buf, n,
			    flags, sce->timeout, NULL);
			err = usbd_transfer(xfer);
			if (err) {
				usbd_clear_endpoint_stall(sce->pipeh);
				if (err == USBD_INTERRUPTED)
					error = EINTR;
				else if (err == USBD_TIMEOUT)
					error = ETIMEDOUT;
				else
					error = EIO;
				break;
			}
			usbd_get_xfer_status(xfer, NULL, NULL, &tn, NULL);
			DPRINTFN(1, ("ugenread: got %d bytes\n", tn));
			error = uiomovei(buf, tn, uio);
			if (error || tn < n)
				break;
		}
		usbd_free_xfer(xfer);
		break;
	case UE_ISOCHRONOUS:
		s = splusb();
		while (sce->cur == sce->fill) {
			if (flag & IO_NDELAY) {
				splx(s);
				return (EWOULDBLOCK);
			}
			sce->state |= UGEN_ASLP;
			DPRINTFN(5, ("ugenread: sleep on %p\n", sce));
			error = tsleep(sce, PZERO | PCATCH, "ugenri",
			    (sce->timeout * hz) / 1000);
			sce->state &= ~UGEN_ASLP;
			DPRINTFN(5, ("ugenread: woke, error=%d\n", error));
			if (usbd_is_dying(sc->sc_udev))
				error = EIO;
			if (error == EWOULDBLOCK) {	/* timeout, return 0 */
				error = 0;
				break;
			}
			if (error)
				break;
		}

		while (sce->cur != sce->fill && uio->uio_resid > 0 && !error) {
			if(sce->fill > sce->cur)
				n = min(sce->fill - sce->cur, uio->uio_resid);
			else
				n = min(sce->limit - sce->cur, uio->uio_resid);

			DPRINTFN(5, ("ugenread: isoc got %d chars\n", n));

			/* Copy the data to the user process. */
			error = uiomovei(sce->cur, n, uio);
			if (error)
				break;
			sce->cur += n;
			if(sce->cur >= sce->limit)
				sce->cur = sce->ibuf;
		}
		splx(s);
		break;


	default:
		return (ENXIO);
	}
	return (error);
}

int
ugenread(dev_t dev, struct uio *uio, int flag)
{
	int endpt = UGENENDPOINT(dev);
	struct ugen_softc *sc;
	int error;

	sc = ugen_cd.cd_devs[UGENUNIT(dev)];

	sc->sc_refcnt++;
	error = ugen_do_read(sc, endpt, uio, flag);
	if (--sc->sc_refcnt < 0)
		usb_detach_wakeup(&sc->sc_dev);
	return (error);
}

int
ugen_do_write(struct ugen_softc *sc, int endpt, struct uio *uio, int flag)
{
	struct ugen_endpoint *sce = &sc->sc_endpoints[endpt][OUT];
	u_int32_t n;
	int flags, error = 0;
	char buf[UGEN_BBSIZE];
	struct usbd_xfer *xfer;
	usbd_status err;

	DPRINTFN(5, ("%s: ugenwrite: %d\n", sc->sc_dev.dv_xname, endpt));

	if (usbd_is_dying(sc->sc_udev))
		return (EIO);

	if (endpt == USB_CONTROL_ENDPOINT)
		return (ENODEV);

#ifdef DIAGNOSTIC
	if (sce->edesc == NULL) {
		printf("ugenwrite: no edesc\n");
		return (EIO);
	}
	if (sce->pipeh == NULL) {
		printf("ugenwrite: no pipe\n");
		return (EIO);
	}
#endif
	flags = USBD_SYNCHRONOUS;
	if (sce->timeout == 0)
		flags |= USBD_CATCH;

	switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
	case UE_BULK:
		xfer = usbd_alloc_xfer(sc->sc_udev);
		if (xfer == 0)
			return (EIO);
		while ((n = min(UGEN_BBSIZE, uio->uio_resid)) != 0) {
			error = uiomovei(buf, n, uio);
			if (error)
				break;
			DPRINTFN(1, ("ugenwrite: transfer %d bytes\n", n));
			usbd_setup_xfer(xfer, sce->pipeh, 0, buf, n,
			    flags, sce->timeout, NULL);
			err = usbd_transfer(xfer);
			if (err) {
				usbd_clear_endpoint_stall(sce->pipeh);
				if (err == USBD_INTERRUPTED)
					error = EINTR;
				else if (err == USBD_TIMEOUT)
					error = ETIMEDOUT;
				else
					error = EIO;
				break;
			}
		}
		usbd_free_xfer(xfer);
		break;
	case UE_INTERRUPT:
		xfer = usbd_alloc_xfer(sc->sc_udev);
		if (xfer == 0)
			return (EIO);
		while ((n = min(UGETW(sce->edesc->wMaxPacketSize),
		    uio->uio_resid)) != 0) {
			error = uiomovei(buf, n, uio);
			if (error)
				break;
			DPRINTFN(1, ("ugenwrite: transfer %d bytes\n", n));
			usbd_setup_xfer(xfer, sce->pipeh, 0, buf, n,
			    flags, sce->timeout, NULL);
			err = usbd_transfer(xfer);
			if (err) {
				usbd_clear_endpoint_stall(sce->pipeh);
				if (err == USBD_INTERRUPTED)
					error = EINTR;
				else if (err == USBD_TIMEOUT)
					error = ETIMEDOUT;
				else
					error = EIO;
				break;
			}
		}
		usbd_free_xfer(xfer);
		break;
	default:
		return (ENXIO);
	}
	return (error);
}

int
ugenwrite(dev_t dev, struct uio *uio, int flag)
{
	int endpt = UGENENDPOINT(dev);
	struct ugen_softc *sc;
	int error;

	sc = ugen_cd.cd_devs[UGENUNIT(dev)];

	sc->sc_refcnt++;
	error = ugen_do_write(sc, endpt, uio, flag);
	if (--sc->sc_refcnt < 0)
		usb_detach_wakeup(&sc->sc_dev);
	return (error);
}

int
ugen_detach(struct device *self, int flags)
{
	struct ugen_softc *sc = (struct ugen_softc *)self;
	struct ugen_endpoint *sce;
	int i, dir, endptno;
	int s, maj, mn;

	DPRINTF(("ugen_detach: sc=%p flags=%d\n", sc, flags));

	/* Abort all pipes.  Causes processes waiting for transfer to wake. */
	for (i = 0; i < USB_MAX_ENDPOINTS; i++) {
		for (dir = OUT; dir <= IN; dir++) {
			sce = &sc->sc_endpoints[i][dir];
			if (sce && sce->pipeh)
				usbd_abort_pipe(sce->pipeh);
		}
	}

	s = splusb();
	if (--sc->sc_refcnt >= 0) {
		/* Wake everyone */
		for (i = 0; i < USB_MAX_ENDPOINTS; i++)
			wakeup(&sc->sc_endpoints[i][IN]);
		/* Wait for processes to go away. */
		usb_detach_wait(&sc->sc_dev);
	}
	splx(s);

	/* locate the major number */
	for (maj = 0; maj < nchrdev; maj++)
		if (cdevsw[maj].d_open == ugenopen)
			break;

	/* Nuke the vnodes for any open instances (calls close). */
	mn = self->dv_unit * USB_MAX_ENDPOINTS;
	vdevgone(maj, mn, mn + USB_MAX_ENDPOINTS - 1, VCHR);

	for (endptno = 0; endptno < USB_MAX_ENDPOINTS; endptno++) {
		if (sc->sc_is_open[endptno])
			ugen_do_close(sc, endptno, FREAD|FWRITE);
	}
	return (0);
}

void
ugenintr(struct usbd_xfer *xfer, void *addr, usbd_status status)
{
	struct ugen_endpoint *sce = addr;
	/*struct ugen_softc *sc = sce->sc;*/
	u_int32_t count;
	u_char *ibuf;

	if (status == USBD_CANCELLED)
		return;

	if (status != USBD_NORMAL_COMPLETION) {
		DPRINTF(("ugenintr: status=%d\n", status));
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sce->pipeh);
		return;
	}

	usbd_get_xfer_status(xfer, NULL, NULL, &count, NULL);
	ibuf = sce->ibuf;

	DPRINTFN(5, ("ugenintr: xfer=%p status=%d count=%d\n",
		     xfer, status, count));
	DPRINTFN(5, ("          data = %02x %02x %02x\n",
		     ibuf[0], ibuf[1], ibuf[2]));

	(void)b_to_q(ibuf, count, &sce->q);

	if (sce->state & UGEN_ASLP) {
		sce->state &= ~UGEN_ASLP;
		DPRINTFN(5, ("ugen_intr: waking %p\n", sce));
		wakeup(sce);
	}
	selwakeup(&sce->rsel);
}

void
ugen_isoc_rintr(struct usbd_xfer *xfer, void *addr, usbd_status status)
{
	struct isoreq *req = addr;
	struct ugen_endpoint *sce = req->sce;
	u_int32_t count, n;
	int i, isize;

	/* Return if we are aborting. */
	if (status == USBD_CANCELLED)
		return;

	usbd_get_xfer_status(xfer, NULL, NULL, &count, NULL);
	DPRINTFN(5,("%s: xfer %ld, count=%d\n", __func__, req - sce->isoreqs,
	    count));

	/* throw away oldest input if the buffer is full */
	if(sce->fill < sce->cur && sce->cur <= sce->fill + count) {
		sce->cur += count;
		if(sce->cur >= sce->limit)
			sce->cur = sce->ibuf + (sce->limit - sce->cur);
		DPRINTFN(5, ("%s: throwing away %d bytes\n", __func__, count));
	}

	isize = UGETW(sce->edesc->wMaxPacketSize);
	for (i = 0; i < UGEN_NISORFRMS; i++) {
		u_int32_t actlen = req->sizes[i];
		char const *buf = (char const *)req->dmabuf + isize * i;

		/* copy data to buffer */
		while (actlen > 0) {
			n = min(actlen, sce->limit - sce->fill);
			memcpy(sce->fill, buf, n);

			buf += n;
			actlen -= n;
			sce->fill += n;
			if(sce->fill == sce->limit)
				sce->fill = sce->ibuf;
		}

		/* setup size for next transfer */
		req->sizes[i] = isize;
	}

	usbd_setup_isoc_xfer(xfer, sce->pipeh, req, req->sizes, UGEN_NISORFRMS,
	    USBD_NO_COPY | USBD_SHORT_XFER_OK, ugen_isoc_rintr);
	(void)usbd_transfer(xfer);

	if (sce->state & UGEN_ASLP) {
		sce->state &= ~UGEN_ASLP;
		DPRINTFN(5, ("ugen_isoc_rintr: waking %p\n", sce));
		wakeup(sce);
	}
	selwakeup(&sce->rsel);
}

int 
ugen_set_interface(struct ugen_softc *sc, int ifaceidx, int altno)
{
	struct usbd_interface *iface;
	usb_config_descriptor_t *cdesc;
	usb_interface_descriptor_t *id;
	usb_endpoint_descriptor_t *ed;
	struct ugen_endpoint *sce;
	uint8_t  endptno, endpt;
	int dir, err;

	DPRINTFN(15, ("ugen_set_interface %d %d\n", ifaceidx, altno));

	cdesc = usbd_get_config_descriptor(sc->sc_udev);
	if (ifaceidx < 0 || ifaceidx >= cdesc->bNumInterface ||
	    usbd_iface_claimed(sc->sc_udev, ifaceidx))
		return (USBD_INVAL);

	err = usbd_device2interface_handle(sc->sc_udev, ifaceidx, &iface);
	if (err)
		return (err);
	id = usbd_get_interface_descriptor(iface);
	for (endptno = 0; endptno < id->bNumEndpoints; endptno++) {
		ed = usbd_interface2endpoint_descriptor(iface,endptno);
		endpt = ed->bEndpointAddress;
		dir = UE_GET_DIR(endpt) == UE_DIR_IN ? IN : OUT;
		sce = &sc->sc_endpoints[UE_GET_ADDR(endpt)][dir];
		sce->sc = 0;
		sce->edesc = 0;
		sce->iface = 0;
	}

	/* change setting */
	err = usbd_set_interface(iface, altno);
	if (err)
		return (err);

	id = usbd_get_interface_descriptor(iface);
	for (endptno = 0; endptno < id->bNumEndpoints; endptno++) {
		ed = usbd_interface2endpoint_descriptor(iface,endptno);
		endpt = ed->bEndpointAddress;
		dir = UE_GET_DIR(endpt) == UE_DIR_IN ? IN : OUT;
		sce = &sc->sc_endpoints[UE_GET_ADDR(endpt)][dir];
		sce->sc = sc;
		sce->edesc = ed;
		sce->iface = iface;
	}
	return (err);
}

int
ugen_get_alt_index(struct ugen_softc *sc, int ifaceidx)
{
	struct usbd_interface *iface;
	usbd_status err;

	err = usbd_device2interface_handle(sc->sc_udev, ifaceidx, &iface);
	if (err)
		return (-1);
	return (usbd_get_interface_altindex(iface));
}

int ugen_submit_ctrl(struct ugen_softc *sc, struct
    usb_ctl_request *req, struct proc *p) {
	struct usb_ctl_request *kreq;
	struct usbd_xfer *xfer;
	int len;
	void *buf;
	struct uio uio;
	struct iovec iov;
	int error = 0;
	int err;
	int flags = 0;
	struct ugen_endpoint *sce = req->ucr_sce;
	int s;

	len = UGETW(req->ucr_request.wLength);

	/* Avoid requests that would damage the bus integrity. */
	if ((req->ucr_request.bmRequestType == UT_WRITE_DEVICE &&
	     req->ucr_request.bRequest == UR_SET_ADDRESS) ||
	    (req->ucr_request.bmRequestType == UT_WRITE_DEVICE &&
	     req->ucr_request.bRequest == UR_SET_CONFIG) ||
	    (req->ucr_request.bmRequestType == UT_WRITE_INTERFACE &&
	     req->ucr_request.bRequest == UR_SET_INTERFACE))
		return (EINVAL);
	if (len < 0 || len > 32767)
		return (EINVAL);

	kreq = malloc(sizeof(*kreq), M_TEMP, M_WAITOK);
	if (kreq == NULL)
		return (ENOMEM);
	*kreq = *req;

	xfer = usbd_alloc_xfer(sc->sc_udev);
	if (xfer == NULL) {
		free(kreq, M_TEMP, sizeof(*kreq));
		return (ENOMEM);
	}
	if (len != 0) {
		iov.iov_base = (caddr_t)req->ucr_data;
		iov.iov_len = len;
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_resid = len;
		uio.uio_offset = 0;
		uio.uio_segflg = UIO_USERSPACE;
		uio.uio_rw =
		    req->ucr_request.bmRequestType & UT_READ ?
		    UIO_READ : UIO_WRITE;
		uio.uio_procp = p;
		buf = usbd_alloc_buffer(xfer, len);
		if (buf == NULL) {
			usbd_free_xfer(xfer);
			free(kreq, M_TEMP, sizeof(*kreq));
			return (ENOMEM);
		}
		if (uio.uio_rw == UIO_WRITE) {
			error = uiomove(buf, len, &uio);
			if (error) {
				usbd_free_xfer(xfer);
				free(kreq, M_TEMP, sizeof(*kreq));
				return (error);
			}
		}
	}
	if (kreq->ucr_flags & USBD_SHORT_XFER_OK)
		flags = USBD_SHORT_XFER_OK;
	usbd_setup_default_xfer(xfer, xfer->device, kreq,
	    kreq->ucr_timeout, &kreq->ucr_request,
	    NULL, len, flags | USBD_NO_COPY, ugen_async_callback);
	kreq->xfer = xfer;
	s = splusb();
	err = usbd_transfer(xfer);
	if (err != USBD_IN_PROGRESS) {
		splx(s);
		usbd_clear_endpoint_stall(sce->pipeh);
		if (err == USBD_INTERRUPTED)
			error = EINTR;
		else if (err == USBD_TIMEOUT)
			error = ETIMEDOUT;
		else
			error = EIO;
		usbd_free_xfer(xfer);
		free(kreq, M_TEMP, sizeof(*kreq));
		return (error);
	}
	rw_enter_write(&q_lock);
	TAILQ_INSERT_TAIL(&sce->submit_queue, kreq, entries);
	rw_exit_write(&q_lock);
	splx(s);
	return (0);
}

int ugen_submit_bulk(struct ugen_softc *sc, struct
    usb_ctl_request *req, struct proc *p) {
	struct usb_ctl_request *kreq;
	struct usbd_xfer *xfer;
	int len;
	void *buf;
	struct uio uio;
	struct iovec iov;
	int error = 0;
	int err;
	int flags = 0;
	struct ugen_endpoint *sce = req->ucr_sce;
	int s;

	len = req->ucr_actlen;

	if (len < 0) /* are bulk transfers of length zero allowed? */
		return (EINVAL);

	kreq = malloc(sizeof(*kreq), M_TEMP, M_WAITOK);
	if (kreq == NULL)
		return (ENOMEM);
	*kreq = *req;

	xfer = usbd_alloc_xfer(sc->sc_udev);
	if (xfer == NULL) {
		free(kreq, M_TEMP, sizeof(*kreq));
		return (ENOMEM);
	}
	if (len != 0) {
		iov.iov_base = (caddr_t)req->ucr_data;
		iov.iov_len = len;
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_resid = len;
		uio.uio_offset = 0;
		uio.uio_segflg = UIO_USERSPACE;
		uio.uio_rw = req->ucr_read ?
		    UIO_READ : UIO_WRITE;
		uio.uio_procp = p;
		buf = usbd_alloc_buffer(xfer, len);
		if (buf == NULL) {
			usbd_free_xfer(xfer);
			free(kreq, M_TEMP, sizeof(*kreq));
			return (ENOMEM);
		}
		if (uio.uio_rw == UIO_WRITE) {
			error = uiomove(buf, len, &uio);
			if (error) {
				usbd_free_xfer(xfer);
				free(kreq, M_TEMP, sizeof(*kreq));
				return (error);
			}
		}
	}
	if (kreq->ucr_flags & USBD_FORCE_SHORT_XFER)
		flags |= USBD_FORCE_SHORT_XFER;
	if (kreq->ucr_flags & USBD_SHORT_XFER_OK)
		flags |= USBD_SHORT_XFER_OK;
	usbd_setup_xfer(xfer, sce->pipeh, kreq, NULL, len,
	    flags | USBD_NO_COPY,
	    kreq->ucr_timeout, (usbd_callback) ugen_async_callback);
	kreq->xfer = xfer;
	s = splusb();
	err = usbd_transfer(xfer);
	if (err != USBD_IN_PROGRESS) {
		splx(s);
		usbd_clear_endpoint_stall(sce->pipeh);
		if (err == USBD_INTERRUPTED)
			error = EINTR;
		else if (err == USBD_TIMEOUT)
			error = ETIMEDOUT;
		else
			error = EIO;
		usbd_free_xfer(xfer);
		free(kreq, M_TEMP, sizeof(*kreq));
		return (error);
	}
	rw_enter_write(&q_lock);
	TAILQ_INSERT_TAIL(&sce->submit_queue, kreq, entries);
	rw_exit_write(&q_lock);
	splx(s);
	return (0);
}

int ugen_complete_ctrl(struct usb_ctl_request *req, struct
    proc *p) {
	struct usbd_xfer *xfer;
	struct uio uio;
	struct iovec iov;
	int len;
	int error = 0;

	xfer = req->xfer;
	if (req->ucr_status == USBD_CANCELLED) {
		usbd_free_xfer(xfer);
		return (0);
	}
	req->ucr_status = xfer->status;
	if (xfer->status == USBD_NORMAL_COMPLETION) {
		len = UGETW(req->ucr_request.wLength);
		if (len > xfer->actlen)
			len = xfer->actlen;
		req->ucr_actlen = len;
		if (len != 0) {
			iov.iov_base = (caddr_t)req->ucr_data;
			iov.iov_len = len;
			uio.uio_iov = &iov;
			uio.uio_iovcnt = 1;
			uio.uio_resid = len;
			uio.uio_offset = 0;
			uio.uio_segflg = UIO_USERSPACE;
			uio.uio_rw =
				req->ucr_request.bmRequestType & UT_READ ?
				UIO_READ : UIO_WRITE;
			uio.uio_procp = p;
			if (uio.uio_rw == UIO_READ) {
				error = uiomove(KERNADDR(&xfer->dmabuf, 0), len, &uio);
				if (error) {
					req->ucr_status = USBD_IOERROR;
					usbd_free_xfer(xfer);
					return (0);
				}
			}
		}
	}
	usbd_free_xfer(xfer);
	return (0);
}

int ugen_complete_bulk(struct usb_ctl_request *req, struct
    proc *p) {
	struct usbd_xfer *xfer;
	struct uio uio;
	struct iovec iov;
	int len;
	int error = 0;

	xfer = req->xfer;
	if (req->ucr_status == USBD_CANCELLED) {
		usbd_free_xfer(xfer);
		return (0);
	}
	req->ucr_status = xfer->status;
	if (xfer->status == USBD_NORMAL_COMPLETION) {
		len = req->ucr_actlen;
		if (len > xfer->actlen)
			len = xfer->actlen;
		req->ucr_actlen = len;
		if (len != 0) {
			iov.iov_base = (caddr_t)req->ucr_data;
			iov.iov_len = len;
			uio.uio_iov = &iov;
			uio.uio_iovcnt = 1;
			uio.uio_resid = len;
			uio.uio_offset = 0;
			uio.uio_segflg = UIO_USERSPACE;
			uio.uio_rw =
				req->ucr_read ?
				UIO_READ : UIO_WRITE;
			uio.uio_procp = p;
			if (uio.uio_rw == UIO_READ) {
				error = uiomove(KERNADDR(&xfer->dmabuf, 0), len, &uio);
				if (error) {
					req->ucr_status = USBD_IOERROR;
					usbd_free_xfer(xfer);
					return (0);
				}
			}
		}
	}
	usbd_free_xfer(xfer);
	return (0);
}

int
ugen_do_ioctl(struct ugen_softc *sc, int endpt, u_long cmd, caddr_t addr,
    int flag, struct proc *p)
{
	struct ugen_endpoint *sce;
	int err;
	struct usbd_interface *iface;
	struct usb_config_desc *cd;
	usb_config_descriptor_t *cdesc;
	struct usb_interface_desc *id;
	usb_interface_descriptor_t *idesc;
	struct usb_endpoint_desc *ed;
	usb_endpoint_descriptor_t *edesc;
	struct usb_alt_interface *ai;
	struct usb_string_desc *si;
	u_int8_t conf, alt;

	DPRINTFN(5, ("ugenioctl: cmd=%08lx\n", cmd));
	if (usbd_is_dying(sc->sc_udev))
		return (EIO);

	switch (cmd) {
	case FIONBIO:
		/* All handled in the upper FS layer. */
		return (0);
	case USB_SET_SHORT_XFER:
		if (endpt == USB_CONTROL_ENDPOINT)
			return (EINVAL);
		/* This flag only affects read */
		sce = &sc->sc_endpoints[endpt][IN];
		if (sce == NULL || sce->pipeh == NULL)
			return (EINVAL);
		if (*(int *)addr)
			sce->state |= UGEN_SHORT_OK;
		else
			sce->state &= ~UGEN_SHORT_OK;
		return (0);
	case USB_SET_TIMEOUT:
		sce = &sc->sc_endpoints[endpt][IN];
		if (sce == NULL)
			return (EINVAL);
		sce->timeout = *(int *)addr;
		sce = &sc->sc_endpoints[endpt][OUT];
		if (sce == NULL)
			return (EINVAL);
		sce->timeout = *(int *)addr;
		return (0);
	case USB_DO_REQUEST:
	{
		struct usb_ctl_request *req = (void *)addr;
		int error = 0;

		sce = &sc->sc_endpoints[endpt][IN];
		req->ucr_sce = sce;

		if (endpt == USB_CONTROL_ENDPOINT) {
			if (!(flag & FWRITE))
				return (EPERM);
			error = ugen_submit_ctrl(sc, req, p);
			if (error)
				return (error);
		} else {
			if (!sce->edesc) {
				printf("ugenioctl: no edesc\n");
				return (EINVAL);
			}
			switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
			case UE_INTERRUPT:
			case UE_ISOCHRONOUS:
				return (EINVAL);
			case UE_BULK:
				error = ugen_submit_bulk(sc, req, p);
				if (error)
					return (error);
				break;
			default:
				return (EINVAL);
			}
		}
		return (0);
	}
	case USB_GET_COMPLETED:
	{

		struct usb_ctl_request *req = (void *)addr;
		struct usb_ctl_request *kreq;
		int s;
		int error = 0;

		sce = &sc->sc_endpoints[endpt][IN];

		s = splusb();
		rw_enter_write(&q_lock);
		kreq = TAILQ_FIRST(&sce->complete_queue);
		if (kreq == NULL) {
			rw_exit_write(&q_lock);
			splx(s);
			return (EIO);
		}
		TAILQ_REMOVE(&sce->complete_queue, kreq, entries);
		rw_exit_write(&q_lock);
		splx(s);

		if (endpt == USB_CONTROL_ENDPOINT) {
			error = ugen_complete_ctrl(kreq, p);
			if (error)
				return (error);
		} else {
			if (!sce->edesc) {
				printf("ugenioctl: no edesc\n");
				return (EINVAL);
			}
			switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
			case UE_INTERRUPT:
			case UE_ISOCHRONOUS:
				return (EINVAL);
			case UE_BULK:
				error = ugen_complete_bulk(kreq, p);
				if (error)
					return (error);
				break;
			default:
				return (EINVAL);
			}
		}

		*req = *kreq;
		free(kreq, M_TEMP, sizeof(*kreq));
		return (0);
	}
	case USB_CANCEL:
	{
		struct usb_ctl_request *req = (void *)addr;
		struct usb_ctl_request *kreq;
		struct usb_ctl_request *np;
		int s;

		sce = &sc->sc_endpoints[endpt][IN];

		s = splusb();
		rw_enter_write(&q_lock);
		kreq = NULL;
		TAILQ_FOREACH(np, &sce->submit_queue, entries) {
			if (np->ucr_context == req->ucr_context) {
				kreq = np;
				break;
			}
		}
		if (kreq == NULL) {
			TAILQ_FOREACH(np, &sce->complete_queue, entries) {
				if (np->ucr_context == req->ucr_context) {
					kreq = np;
					break;
				}
			}
			if (kreq == NULL) {
				/* error, neither completed
				 * nor submitted */
				rw_exit_write(&q_lock);
				splx(s);
				return (EINVAL);
			} else {
				kreq->ucr_status = USBD_CANCELLED;
			}
		} else {
			usbd_abort_transfer(kreq->xfer);
			//kreq->ucr_status = USBD_CANCELLED;
		}
		rw_exit_write(&q_lock);
		splx(s);
		return (0);
	}
	default:
		break;
	}

	if (endpt != USB_CONTROL_ENDPOINT)
		return (EINVAL);

	switch (cmd) {
#ifdef UGEN_DEBUG
	case USB_SETDEBUG:
		ugendebug = *(int *)addr;
		break;
#endif
	case USB_GET_CONFIG:
		err = usbd_get_config(sc->sc_udev, &conf);
		if (err)
			return (EIO);
		*(int *)addr = conf;
		break;
	case USB_SET_CONFIG:
		if (!(flag & FWRITE))
			return (EPERM);
		err = ugen_set_config(sc, *(int *)addr);
		switch (err) {
		case USBD_NORMAL_COMPLETION:
			break;
		case USBD_IN_USE:
			return (EBUSY);
		default:
			return (EIO);
		}
		break;
	case USB_GET_ALTINTERFACE:
		ai = (struct usb_alt_interface *)addr;
		err = usbd_device2interface_handle(sc->sc_udev,
			  ai->uai_interface_index, &iface);
		if (err)
			return (EINVAL);
		idesc = usbd_get_interface_descriptor(iface);
		if (idesc == NULL)
			return (EIO);
		ai->uai_alt_no = idesc->bAlternateSetting;
		break;
	case USB_SET_ALTINTERFACE:
		if (!(flag & FWRITE))
			return (EPERM);
		ai = (struct usb_alt_interface *)addr;
		err = usbd_device2interface_handle(sc->sc_udev,
			  ai->uai_interface_index, &iface);
		if (err)
			return (EINVAL);
		err = ugen_set_interface(sc, ai->uai_interface_index,
		    ai->uai_alt_no);
		if (err)
			return (EINVAL);
		break;
	case USB_GET_NO_ALT:
		ai = (struct usb_alt_interface *)addr;
		cdesc = usbd_get_cdesc(sc->sc_udev, ai->uai_config_index, 0);
		if (cdesc == NULL)
			return (EINVAL);
		idesc = usbd_find_idesc(cdesc, ai->uai_interface_index, 0);
		if (idesc == NULL) {
			free(cdesc, M_TEMP, 0);
			return (EINVAL);
		}
		ai->uai_alt_no = usbd_get_no_alts(cdesc,
		    idesc->bInterfaceNumber);
		free(cdesc, M_TEMP, 0);
		break;
	case USB_GET_DEVICE_DESC:
		*(usb_device_descriptor_t *)addr =
			*usbd_get_device_descriptor(sc->sc_udev);
		break;
	case USB_GET_CONFIG_DESC:
		cd = (struct usb_config_desc *)addr;
		cdesc = usbd_get_cdesc(sc->sc_udev, cd->ucd_config_index, 0);
		if (cdesc == NULL)
			return (EINVAL);
		cd->ucd_desc = *cdesc;
		free(cdesc, M_TEMP, 0);
		break;
	case USB_GET_INTERFACE_DESC:
		id = (struct usb_interface_desc *)addr;
		cdesc = usbd_get_cdesc(sc->sc_udev, id->uid_config_index, 0);
		if (cdesc == NULL)
			return (EINVAL);
		if (id->uid_config_index == USB_CURRENT_CONFIG_INDEX &&
		    id->uid_alt_index == USB_CURRENT_ALT_INDEX)
			alt = ugen_get_alt_index(sc, id->uid_interface_index);
		else
			alt = id->uid_alt_index;
		idesc = usbd_find_idesc(cdesc, id->uid_interface_index, alt);
		if (idesc == NULL) {
			free(cdesc, M_TEMP, 0);
			return (EINVAL);
		}
		id->uid_desc = *idesc;
		free(cdesc, M_TEMP, 0);
		break;
	case USB_GET_ENDPOINT_DESC:
		ed = (struct usb_endpoint_desc *)addr;
		cdesc = usbd_get_cdesc(sc->sc_udev, ed->ued_config_index, 0);
		if (cdesc == NULL)
			return (EINVAL);
		if (ed->ued_config_index == USB_CURRENT_CONFIG_INDEX &&
		    ed->ued_alt_index == USB_CURRENT_ALT_INDEX)
			alt = ugen_get_alt_index(sc, ed->ued_interface_index);
		else
			alt = ed->ued_alt_index;
		edesc = usbd_find_edesc(cdesc, ed->ued_interface_index,
					alt, ed->ued_endpoint_index);
		if (edesc == NULL) {
			free(cdesc, M_TEMP, 0);
			return (EINVAL);
		}
		ed->ued_desc = *edesc;
		free(cdesc, M_TEMP, 0);
		break;
	case USB_GET_FULL_DESC:
	{
		int len;
		struct iovec iov;
		struct uio uio;
		struct usb_full_desc *fd = (struct usb_full_desc *)addr;
		int error;

		cdesc = usbd_get_cdesc(sc->sc_udev, fd->ufd_config_index, &len);
		if (cdesc == NULL)
			return (EINVAL);
		if (len > fd->ufd_size)
			len = fd->ufd_size;
		iov.iov_base = (caddr_t)fd->ufd_data;
		iov.iov_len = len;
		uio.uio_iov = &iov;
		uio.uio_iovcnt = 1;
		uio.uio_resid = len;
		uio.uio_offset = 0;
		uio.uio_segflg = UIO_USERSPACE;
		uio.uio_rw = UIO_READ;
		uio.uio_procp = p;
		error = uiomovei((void *)cdesc, len, &uio);
		free(cdesc, M_TEMP, 0);
		return (error);
	}
	case USB_GET_STRING_DESC:
	{
		int len;
		si = (struct usb_string_desc *)addr;
		err = usbd_get_string_desc(sc->sc_udev, si->usd_string_index,
			si->usd_language_id, &si->usd_desc, &len);
		if (err)
			return (EINVAL);
		break;
	}
	case USB_GET_DEVICEINFO:
		usbd_fill_deviceinfo(sc->sc_udev,
				     (struct usb_device_info *)addr, 1);
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

int
ugenioctl(dev_t dev, u_long cmd, caddr_t addr, int flag, struct proc *p)
{
	int endpt = UGENENDPOINT(dev);
	struct ugen_softc *sc;
	int error;

	sc = ugen_cd.cd_devs[UGENUNIT(dev)];

	sc->sc_refcnt++;
	error = ugen_do_ioctl(sc, endpt, cmd, addr, flag, p);
	if (--sc->sc_refcnt < 0)
		usb_detach_wakeup(&sc->sc_dev);
	return (error);
}

int
ugenpoll(dev_t dev, int events, struct proc *p)
{
	struct ugen_softc *sc;
	struct ugen_endpoint *sce;
	int revents = 0;
	int s;

	sc = ugen_cd.cd_devs[UGENUNIT(dev)];

	if (usbd_is_dying(sc->sc_udev))
		return (POLLERR);

	/* XXX always IN */
	sce = &sc->sc_endpoints[UGENENDPOINT(dev)][IN];
	if (sce == NULL)
		return (POLLERR);
#ifdef DIAGNOSTIC
	if (UGENENDPOINT(dev) != USB_CONTROL_ENDPOINT) {
		if (!sce->edesc) {
			printf("ugenpoll: no edesc\n");
			return (POLLERR);
		}
		if (!sce->pipeh) {
			printf("ugenpoll: no pipe\n");
			return (POLLERR);
		}
	}
#endif
	s = splusb();
	rw_enter_write(&q_lock);
	if (UGENENDPOINT(dev) == USB_CONTROL_ENDPOINT) {
		if (events & (POLLIN | POLLRDNORM)) {
			if (!TAILQ_EMPTY(&sce->complete_queue))
				revents |= events & (POLLIN | POLLRDNORM);
			else
				selrecord(p, &sce->rsel);
		}
	} else {
		switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
		case UE_INTERRUPT:
			if (events & (POLLIN | POLLRDNORM)) {
				if (sce->q.c_cc > 0)
					revents |= events & (POLLIN | POLLRDNORM);
				else
					selrecord(p, &sce->rsel);
			}
			break;
		case UE_ISOCHRONOUS:
			if (events & (POLLIN | POLLRDNORM)) {
				if (sce->cur != sce->fill)
					revents |= events & (POLLIN | POLLRDNORM);
				else
					selrecord(p, &sce->rsel);
			}
			break;
		case UE_BULK:
			if (events & (POLLIN | POLLRDNORM)) {
				if (!TAILQ_EMPTY(&sce->complete_queue))
					revents |= events & (POLLIN | POLLRDNORM);
				else
					selrecord(p, &sce->rsel);
			}
			break;
		default:
			break;
		}
	}
	rw_exit_write(&q_lock);
	splx(s);
	return (revents);
}

void filt_ugenrdetach(struct knote *);
int filt_ugenread_intr(struct knote *, long);
int filt_ugenread_isoc(struct knote *, long);
int ugenkqfilter(dev_t, struct knote *);

void
filt_ugenrdetach(struct knote *kn)
{
	struct ugen_endpoint *sce = (void *)kn->kn_hook;
	int s;

	s = splusb();
	SLIST_REMOVE(&sce->rsel.si_note, kn, knote, kn_selnext);
	splx(s);
}

int
filt_ugenread_intr(struct knote *kn, long hint)
{
	struct ugen_endpoint *sce = (void *)kn->kn_hook;

	kn->kn_data = sce->q.c_cc;
	return (kn->kn_data > 0);
}

int
filt_ugenread_isoc(struct knote *kn, long hint)
{
	struct ugen_endpoint *sce = (void *)kn->kn_hook;

	if (sce->cur == sce->fill)
		return (0);

	if (sce->cur < sce->fill)
		kn->kn_data = sce->fill - sce->cur;
	else
		kn->kn_data = (sce->limit - sce->cur) +
		    (sce->fill - sce->ibuf);

	return (1);
}

struct filterops ugenread_intr_filtops =
	{ 1, NULL, filt_ugenrdetach, filt_ugenread_intr };

struct filterops ugenread_isoc_filtops =
	{ 1, NULL, filt_ugenrdetach, filt_ugenread_isoc };

struct filterops ugen_seltrue_filtops =
	{ 1, NULL, filt_ugenrdetach, filt_seltrue };

int
ugenkqfilter(dev_t dev, struct knote *kn)
{
	struct ugen_softc *sc;
	struct ugen_endpoint *sce;
	struct klist *klist;
	int s;

	sc = ugen_cd.cd_devs[UGENUNIT(dev)];

	if (usbd_is_dying(sc->sc_udev))
		return (ENXIO);

	/* XXX always IN */
	sce = &sc->sc_endpoints[UGENENDPOINT(dev)][IN];
	if (sce == NULL)
		return (EINVAL);

	switch (kn->kn_filter) {
	case EVFILT_READ:
		klist = &sce->rsel.si_note;
		switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
		case UE_INTERRUPT:
			kn->kn_fop = &ugenread_intr_filtops;
			break;
		case UE_ISOCHRONOUS:
			kn->kn_fop = &ugenread_isoc_filtops;
			break;
		case UE_BULK:
			/* 
			 * We have no easy way of determining if a read will
			 * yield any data or a write will happen.
			 * So, emulate "seltrue".
			 */
			kn->kn_fop = &ugen_seltrue_filtops;
			break;
		default:
			return (EINVAL);
		}
		break;

	case EVFILT_WRITE:
		klist = &sce->rsel.si_note;
		switch (sce->edesc->bmAttributes & UE_XFERTYPE) {
		case UE_INTERRUPT:
		case UE_ISOCHRONOUS:
			/* XXX poll doesn't support this */
			return (EINVAL);

		case UE_BULK:
			/*
			 * We have no easy way of determining if a read will
			 * yield any data or a write will happen.
			 * So, emulate "seltrue".
			 */
			kn->kn_fop = &ugen_seltrue_filtops;
			break;
		default:
			return (EINVAL);
		}
		break;

	default:
		return (EINVAL);
	}

	kn->kn_hook = (void *)sce;

	s = splusb();
	SLIST_INSERT_HEAD(klist, kn, kn_selnext);
	splx(s);

	return (0);
}
