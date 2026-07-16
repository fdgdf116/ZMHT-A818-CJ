#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_utils.h"
#include "log.h"

struct modeset_buf;
struct modeset_dev;
static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_create_fb(int fd, struct modeset_buf *buf);
static void modeset_destroy_fb(int fd, struct modeset_buf *buf);
static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_open(int *out, const char *node);
static int modeset_prepare(int fd);
static void modeset_draw(int fd);
static void modeset_draw_dev(int fd, struct modeset_dev *dev);
static void modeset_cleanup(int fd);

/*
 * modeset_open() stays the same.
 */

static int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t has_dumb;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 ||
	    !has_dumb) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}

/*
 * modeset_buf and modeset_dev stay mostly the same. But 6 new fields are added
 * to modeset_dev: r, g, b, r_up, g_up, b_up. They are used to compute the
 * current color that is drawn on this output device. You can ignore them as
 * they aren't important for this example.
 * The modeset-double-buffered.c example used exactly the same fields but as
 * local variables in modeset_draw().
 *
 * The \pflip_pending variable is true when a page-flip is currently pending,
 * that is, the kernel will flip buffers on the next vertical blank. The
 * \cleanup variable is true if the device is currently cleaned up and no more
 * pageflips should be scheduled. They are used to synchronize the cleanup
 * routines.
 */

struct modeset_buf {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};

struct modeset_dev {
	struct modeset_dev *next;

	unsigned int front_buf;
	struct modeset_buf *bufs;

	drmModeModeInfo mode;
	uint32_t conn;
	uint32_t crtc;
	drmModeCrtc *saved_crtc;

	bool pflip_pending;
	bool cleanup;

	uint8_t r, g, b;
	bool r_up, g_up, b_up;
};

static struct modeset_dev *modeset_list = NULL;
static int drm_fd;
static const char* drm_card = "/dev/dri/card0";
static ring_buffer_t *draw_rb;
static struct modeset_dev *drm_iter;
static unsigned int drm_front_buf_max;
static unsigned int draw_flag;
static pthread_t draw_tid;
/*
 * modeset_prepare() stays the same.
 */

static int modeset_prepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_dev *dev;
	int ret;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn = conn->connector_id;

		/* call helper function to prepare this connector */
		ret = modeset_setup_dev(fd, res, conn, dev);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n",
					i, res->connectors[i], errno);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		dev->next = modeset_list;
		modeset_list = dev;
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

/*
 * modeset_setup_dev() stays the same.
 */

static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	int ret, index, i;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		return -EFAULT;
	}

	dev->bufs = malloc(sizeof(struct modeset_buf) * drm_front_buf_max);
	if(!dev->bufs){
		fprintf(stderr, "malloc struct modeset_buf error \n");
		return -EFAULT;
	}

	/* copy the mode information into our device structure and into both
	 * buffers */
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
	for(index = 0; index < drm_front_buf_max; index++){
		dev->bufs[index].width = conn->modes[0].hdisplay;
		dev->bufs[index].height = conn->modes[0].vdisplay;
	}

	fprintf(stderr, "mode for connector %u is %ux%u\n",
		conn->connector_id, dev->bufs[0].width, dev->bufs[0].height);

	/* find a crtc for this connector */
	ret = modeset_find_crtc(fd, res, conn, dev);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return ret;
	}

	for(index = 0; index < drm_front_buf_max; index++){
		ret = modeset_create_fb(fd, &dev->bufs[index]);
		if (ret) {
			fprintf(stderr, "cannot create framebuffer for connector %u\n",
				conn->connector_id);
			ret = index;
			goto err;
		}
	}

	return 0;
err:
	for(i = 0; i < index; i++){
		modeset_destroy_fb(fd, &dev->bufs[i]);
	}
	free(dev->bufs);

	return ret;
}

/*
 * modeset_find_crtc() stays the same.
 */

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc;
	struct modeset_dev *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

/*
 * modeset_create_fb() stays the same.
 */

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	ret = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride,
			   buf->handle, &buf->fb);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* perform actual memory mapping */
	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

/*
 * modeset_destroy_fb() stays the same.
 */

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	/* unmap buffer */
	munmap(buf->map, buf->size);

	/* delete framebuffer */
	drmModeRmFB(fd, buf->fb);

	/* delete dumb buffer */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

ring_buffer_t * get_draw_rb(void)
{
	return draw_rb;
}

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    void *data)
{
	struct modeset_dev *dev = data;

	dev->pflip_pending = false;
	if (!dev->cleanup)
		modeset_draw_dev(fd, dev);
}

#if 0
void drm_fill_draw_buffer(unsigned char* buffer, unsigned char front_buf, unsigned int width, unsigned int height)
{
	struct modeset_buf *buf;
	unsigned int i, off;
	buf = &drm_iter->bufs[front_buf];

	for (i = 0; i < height; ++i) {
        off = buf->stride * i;
        memcpy(buf->map + off, buffer + i*width*4, width*4);
    }
}
#else
void drm_fill_draw_buffer(unsigned char* buffer, unsigned char front_buf, unsigned int width, unsigned int height,unsigned int src_width)
{
	struct modeset_buf *buf;
	unsigned int i, off;
	buf = &drm_iter->bufs[front_buf];

	for (i = 0; i < height; ++i) {
        off = buf->stride * i;
        memcpy(buf->map + off, buffer + i*src_width*4, width*4);
    }
}
#endif

static void *modeset_draw_thread(void* parameter)
{
	int ret;
	fd_set fds;
	time_t start, cur;
	struct timeval v;
	drmEventContext ev;
	struct modeset_dev *iter;

	/* init variables */
	srand(time(&start));
	FD_ZERO(&fds);
	memset(&v, 0, sizeof(v));
	memset(&ev, 0, sizeof(ev));

	ev.version = 2;
	ev.page_flip_handler = modeset_page_flip_event;

	/* redraw all outputs */
	for (iter = modeset_list; iter; iter = iter->next) {
		modeset_draw_dev(drm_fd, iter);
	}

	while (1) {
		FD_SET(0, &fds);
		FD_SET(drm_fd, &fds);
		v.tv_sec = 0;
		v.tv_usec = (100);

		if(draw_flag){
			break;
		}

		ret = select(drm_fd + 1, &fds, NULL, NULL, &v);
		if (FD_ISSET(drm_fd, &fds)) {
			drmHandleEvent(drm_fd, &ev);
		} else if(ret == 0){
			//SLOGI("exit due to user-input\n");
			if(draw_flag){
				break;
			}
		} else if(ret < 0){
			//SLOGI("select() failed with %d: %m\n", errno);
			if(draw_flag){
				break;
			}
		}
	}

	draw_flag = 0;
	pthread_exit((void*)1);
	return NULL;
}


void draw_image_to_queue(draw_frame_info_t *frame_info)
{
    draw_frame_info_t event_param, tmp_param;
    ring_buffer_t *rb = draw_rb;
	
	if (ringbuffer_is_full(rb)) {					
		ringbuffer_get(rb, (uint8_t *)&tmp_param, sizeof(tmp_param));
        SLOGI("[Debug] %s ringbuffer_is_full", __func__);
	}
	memcpy(&event_param,frame_info,sizeof(draw_frame_info_t));
    		
	ringbuffer_put(rb, (uint8_t *)&event_param, sizeof(event_param));
}


int drm_device_init(unsigned int front_buf_max)
{
	int ret, fd;
	unsigned int front_buf_max_rb;
	struct modeset_dev *iter;
	struct modeset_buf *buf;

	/* open the DRM device */
	ret = modeset_open(&fd, drm_card);
	if (ret)
		goto out_return;

	drm_front_buf_max = front_buf_max;

	/* prepare all connectors and CRTCs */
	ret = modeset_prepare(fd);
	if (ret)
		goto out_close;

	/* perform actual modesetting on each found connector+CRTC */
	for (iter = modeset_list; iter; iter = iter->next) {
		iter->saved_crtc = drmModeGetCrtc(fd, iter->crtc);
		buf = &iter->bufs[iter->front_buf];
		ret = drmModeSetCrtc(fd, iter->crtc, buf->fb, 0, 0,
				     &iter->conn, 1, &iter->mode);
		if (ret)
			fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
				iter->conn, errno);
	}

	front_buf_max_rb = (drm_front_buf_max / 2);
	draw_rb = ringbuffer_create(front_buf_max_rb*sizeof(draw_frame_info_t), sizeof(draw_frame_info_t));
	if(!draw_rb){
		printf("[ERROR] draw rb init error \n");
		goto draw_rb_error;
	}

	drm_fd = fd;
	drm_iter = modeset_list;
	draw_flag = 0;

    ret = pthread_create(&draw_tid, NULL, &modeset_draw_thread, NULL);
    if (ret != 0) {
        printf("[ERROR] Create modeset_draw_thread failed %d\n",ret);
        goto draw_pthread_error;
    }

	return 0;

draw_pthread_error:
	ringbuffer_destory(draw_rb);
draw_rb_error:
	modeset_cleanup(fd);
out_close:
	close(fd);
out_return:
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modeset failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "exiting\n");
	}
	return ret;
}

void drm_device_destory(void)
{
	unsigned int count;
	count = 0;
	while(1){
		draw_flag = 1;
		count+=1;
		usleep(10*1000);
		if(!draw_flag){
			break;
		}
		if(count >= 100){
			pthread_cancel(draw_tid);
			SLOGI("[DEBUG]----------- %s-- \n", __func__);
			break;
		}
	}

	ringbuffer_destory(draw_rb);
    modeset_cleanup(drm_fd);
    close(drm_fd);
}

/*
 * modeset_draw_dev() is a new function that redraws the screen of a single
 * output. It takes the DRM-fd and the output devices as arguments, redraws a
 * new frame and schedules the page-flip for the next vsync.
 *
 * This function does the same as modeset_draw() did in the previous examples
 * but only for a single output device now.
 * After we are done rendering a frame, we have to swap the buffers. Instead of
 * calling drmModeSetCrtc() as we did previously, we now want to schedule this
 * page-flip for the next vertical-blank (vblank). We use drmModePageFlip() for
 * this. It takes the CRTC-id and FB-id and will asynchronously swap the buffers
 * when the next vblank occurs. Note that this is done by the kernel, so neither
 * a thread is started nor any other magic is done in libdrm.
 * The DRM_MODE_PAGE_FLIP_EVENT flag tells drmModePageFlip() to send us a
 * page-flip event on the DRM-fd when the page-flip happened. The last argument
 * is a data-pointer that is returned with this event.
 * If we wouldn't pass this flag, we would not get notified when the page-flip
 * happened.
 *
 * Note: If you called drmModePageFlip() and directly call it again, it will
 * return EBUSY if the page-flip hasn't happened in between. So you almost
 * always want to pass DRM_MODE_PAGE_FLIP_EVENT to get notified when the
 * page-flip happens so you know when to render the next frame.
 * If you scheduled a page-flip but call drmModeSetCrtc() before the next
 * vblank, then the scheduled page-flip will become a no-op. However, you will
 * still get notified when it happens and you still cannot call
 * drmModePageFlip() again until it finished. So to sum it up: there is no way
 * to effectively cancel a page-flip.
 *
 * If you wonder why drmModePageFlip() takes fewer arguments than
 * drmModeSetCrtc(), then you should take into account, that drmModePageFlip()
 * reuses the arguments from drmModeSetCrtc(). So things like connector-ids,
 * x/y-offsets and so on have to be set via drmModeSetCrtc() first before you
 * can use drmModePageFlip()! We do this in main() as all the previous examples
 * did, too.
 */

static void modeset_draw_dev(int fd, struct modeset_dev *dev)
{
	int ret;
	struct timeval time;
	unsigned int i, off;
	struct modeset_buf *buf;
	draw_frame_info_t frame_info = {0};

	while(ringbuffer_is_empty(draw_rb)){
        time.tv_sec=0;
        time.tv_usec=(10);
        select(0,NULL,NULL,NULL,&time);      
    }
	ringbuffer_get(draw_rb, (uint8_t *)&frame_info, sizeof(draw_frame_info_t));

	buf = &dev->bufs[frame_info.front_buf];
	ret = drmModePageFlip(fd, dev->crtc, buf->fb,
			      DRM_MODE_PAGE_FLIP_EVENT, dev);
	if (ret) {
		SLOGI("cannot flip CRTC for connector %u \n",dev->conn);
	}
}

/*
 * modeset_cleanup() stays mostly the same. However, before resetting a CRTC to
 * its previous state, we wait for any outstanding page-flip to complete. This
 * isn't strictly neccessary, however, some DRM drivers are known to be buggy if
 * we call drmModeSetCrtc() if there is a pending page-flip.
 * Furthermore, we don't want any pending page-flips when our application exist.
 * Because another application might pick up the DRM device and try to schedule
 * their own page-flips which might then fail as long as our page-flip is
 * pending.
 * So lets be safe here and simply wait for any page-flips to complete. This is
 * a blocking operation, but it's mostly just <16ms so we can ignore that.
 */

static void modeset_cleanup(int fd)
{
	struct modeset_dev *iter;
	drmEventContext ev;
	int ret;
	unsigned int index;

	/* init variables */
	memset(&ev, 0, sizeof(ev));
	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler = modeset_page_flip_event;

	while (modeset_list) {
		/* remove from global list */
		iter = modeset_list;
		modeset_list = iter->next;

		/* if a pageflip is pending, wait for it to complete */
		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			ret = drmHandleEvent(fd, &ev);
			if (ret)
				break;
		}

		/* restore saved CRTC configuration */
		if (!iter->pflip_pending)
			drmModeSetCrtc(fd,
				       iter->saved_crtc->crtc_id,
				       iter->saved_crtc->buffer_id,
				       iter->saved_crtc->x,
				       iter->saved_crtc->y,
				       &iter->conn,
				       1,
				       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		/* destroy framebuffers */
		for(index = 0; index < drm_front_buf_max; index++){
			modeset_destroy_fb(fd, &iter->bufs[index]);
		}
		free(iter->bufs);
		/* free allocated memory */
		free(iter);
	}
}

