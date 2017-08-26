#include <assert.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libudev.h>
#include <wayland-server.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <wlr/backend/session.h>
#include <wlr/backend/session/interface.h>
#include <wlr/util/log.h>

struct device {
	int fd;
	dev_t dev;
	struct wl_signal signal;

	struct wl_list link;
};

extern const struct session_impl session_logind;
extern const struct session_impl session_direct;

static const struct session_impl *impls[] = {
#ifdef HAS_SYSTEMD
	&session_logind,
#endif
	&session_direct,
	NULL,
};

static int udev_event(int fd, uint32_t mask, void *data) {
	struct wlr_session *session = data;

	struct udev_device *udev_dev = udev_monitor_receive_device(session->mon);
	if (!udev_dev) {
		return 1;
	}

	const char *action = udev_device_get_action(udev_dev);

	wlr_log(L_DEBUG, "udev event for %s (%s)",
		udev_device_get_sysname(udev_dev), action);

	if (!action || strcmp(action, "change") != 0) {
		goto out;
	}

	dev_t devnum = udev_device_get_devnum(udev_dev);
	struct device *dev;

	wl_list_for_each(dev, &session->devices, link) {
		if (dev->dev == devnum) {
			wl_signal_emit(&dev->signal, session);
			break;
		}
	}

out:
	udev_device_unref(udev_dev);
	return 1;
}

struct wlr_session *wlr_session_create(struct wl_display *disp) {
	struct wlr_session *session = NULL;
	const struct session_impl **iter;

	for (iter = impls; !session && *iter; ++iter) {
		session = (*iter)->create(disp);
	}

	if (!session) {
		wlr_log(L_ERROR, "Failed to load session backend");
		return NULL;
	}

	session->active = true;
	wl_signal_init(&session->session_signal);
	wl_list_init(&session->devices);

	session->udev = udev_new();
	if (!session->udev) {
		wlr_log_errno(L_ERROR, "Failed to create udev context");
		goto error_session;
	}

	session->mon = udev_monitor_new_from_netlink(session->udev, "udev");
	if (!session->mon) {
		wlr_log_errno(L_ERROR, "Failed to create udev monitor");
		goto error_udev;
	}

	udev_monitor_filter_add_match_subsystem_devtype(session->mon, "drm", NULL);
	udev_monitor_enable_receiving(session->mon);

	struct wl_event_loop *event_loop = wl_display_get_event_loop(disp);
	int fd = udev_monitor_get_fd(session->mon);

	session->udev_event = wl_event_loop_add_fd(event_loop, fd,
		WL_EVENT_READABLE, udev_event, session);
	if (!session->udev_event) {
		wlr_log_errno(L_ERROR, "Failed to create udev event source");
		goto error_mon;
	}

	return session;

error_mon:
	udev_monitor_unref(session->mon);
error_udev:
	udev_unref(session->udev);
error_session:
	wlr_session_destroy(session);
	return NULL;
}

void wlr_session_destroy(struct wlr_session *session) {
	if (!session) {
		return;
	}

	wl_event_source_remove(session->udev_event);
	udev_monitor_unref(session->mon);
	udev_unref(session->udev);

	session->impl->destroy(session);
}

int wlr_session_open_file(struct wlr_session *session, const char *path) {
	int fd = session->impl->open(session, path);
	if (fd < 0) {
		return fd;
	}

	struct device *dev = malloc(sizeof(*dev));
	if (!dev) {
		wlr_log_errno(L_ERROR, "Allocation failed");
		goto error;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		wlr_log_errno(L_ERROR, "Stat failed");
		goto error;
	}

	dev->fd = fd;
	dev->dev = st.st_rdev;
	wl_signal_init(&dev->signal);
	wl_list_insert(&session->devices, &dev->link);

	return fd;

error:
	free(dev);
	return fd;
}

static struct device *find_device(struct wlr_session *session, int fd) {
	struct device *dev;

	wl_list_for_each(dev, &session->devices, link) {
		if (dev->fd == fd) {
			return dev;
		}
	}

	wlr_log(L_ERROR, "Tried to use fd %d not opened by session", fd);
	assert(0);
}

void wlr_session_close_file(struct wlr_session *session, int fd) {
	struct device *dev = find_device(session, fd);

	session->impl->close(session, fd);
	wl_list_remove(&dev->link);
	free(dev);
}

void wlr_session_signal_add(struct wlr_session *session, int fd,
		struct wl_listener *listener) {
	struct device *dev = find_device(session, fd);

	wl_signal_add(&dev->signal, listener);
}

bool wlr_session_change_vt(struct wlr_session *session, unsigned vt) {
	if (!session) {
		return false;
	}

	return session->impl->change_vt(session, vt);
}

/* Tests if 'path' is KMS compatible by trying to open it.
 * It leaves the open device in *fd_out it it succeeds.
 */
static bool device_is_kms(struct wlr_session *restrict session,
	const char *restrict path, int *restrict fd_out) {

	int fd;

	if (!path) {
		return false;
	}

	fd = wlr_session_open_file(session, path);
	if (fd < 0) {
		return false;
	}

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		goto out_fd;
	}

	if (res->count_crtcs <= 0 || res->count_connectors <= 0 ||
		res->count_encoders <= 0) {

		goto out_res;
	}

	if (*fd_out >= 0) {
		wlr_session_close_file(session, *fd_out);
	}

	*fd_out = fd;

	drmModeFreeResources(res);
	return true;

out_res:
	drmModeFreeResources(res);
out_fd:
	wlr_session_close_file(session, fd);
	return false;
}

/* Tries to find the primary GPU by checking for the "boot_vga" attribute.
 * If it's not found, it returns the first valid GPU it finds.
 */
int wlr_session_find_gpu(struct wlr_session *session) {
	struct udev_enumerate *en = udev_enumerate_new(session->udev);
	if (!en) {
		wlr_log(L_ERROR, "Failed to create udev enumeration");
		return -1;
	}

	udev_enumerate_add_match_subsystem(en, "drm");
	udev_enumerate_add_match_sysname(en, "card[0-9]*");
	udev_enumerate_scan_devices(en);

	struct udev_list_entry *entry;
	int fd = -1;

	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
		bool is_boot_vga = false;

		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(session->udev, path);
		if (!dev) {
			continue;
		}

		/*
		const char *seat = udev_device_get_property_value(dev, "ID_SEAT");
		if (!seat)
			seat = "seat0";
		if (strcmp(session->seat, seat) != 0) {
			udev_device_unref(dev);
			continue;
		}
		*/

		// This is owned by 'dev', so we don't need to free it
		struct udev_device *pci =
			udev_device_get_parent_with_subsystem_devtype(dev, "pci", NULL);

		if (pci) {
			const char *id = udev_device_get_sysattr_value(pci, "boot_vga");
			if (id && strcmp(id, "1") == 0) {
				is_boot_vga = true;
			}
		}

		// We already have a valid GPU
		if (!is_boot_vga && fd >= 0) {
			udev_device_unref(dev);
			continue;
		}

		path = udev_device_get_devnode(dev);
		if (!device_is_kms(session, path, &fd)) {
			udev_device_unref(dev);
			continue;
		}

		udev_device_unref(dev);

		// We've found the primary GPU
		if (is_boot_vga) {
			break;
		}
	}

	udev_enumerate_unref(en);

	return fd;
}
