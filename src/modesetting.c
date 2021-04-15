#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <alloca.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <event_loop.h>

#include <modesetting.h>

#define HAS_ACTIVE_CRTC(kms_presenter) ((kms_presenter)->active_crtc_info == NULL)
#define DEBUG_ASSERT_HAS_ACTIVE_CRTC(kms_presenter) DEBUG_ASSERT(HAS_ACTIVE_CRTC(kms_presenter))
#define CHECK_HAS_ACTIVE_CRTC_OR_RETURN_EINVAL(kms_presenter) do { \
        if (HAS_ACTIVE_CRTC(kms_presenter)) { \
            LOG_MODESETTING_ERROR("%s: No active CRTC\n", __func__); \
            return EINVAL; \
        } \
    } while (false)

#define IS_VALID_ZPOS(kms_presenter, zpos) (((kms_presenter)->active_crtc->min_zpos <= zpos) && ((kms_presenter)->active_crtc->max_zpos >= zpos))
#define DEBUG_ASSERT_VALID_ZPOS(kms_presenter, zpos) DEBUG_ASSERT(IS_VALID_ZPOS(kms_presenter, zpos))
#define CHECK_VALID_ZPOS_OR_RETURN_EINVAL(kms_presenter, zpos) do { \
        if (IS_VALID_ZPOS(kms_presenter, zpos)) { \
            LOG_MODESETTING_ERROR("%s: Invalid zpos\n", __func__); \
            return EINVAL; \
        } \
    } while (false)

#define IS_VALID_CRTC_INDEX(kmsdev, crtc_index) (((crtc_index) >= 0) && ((crtc_index) < (kmsdev)->n_crtcs))
#define DEBUG_ASSERT_VALID_CRTC_INDEX(kmsdev, crtc_index) DEBUG_ASSERT(IS_VALID_CRTC_INDEX(kmsdev, crtc_index))

#define IS_VALID_CONNECTOR_INDEX(kmsdev, connector_index) (((connector_index) >= 0) && ((connector_index) < (kmsdev)->n_connectors))
#define DEBUG_ASSERT_VALID_CONNECTOR_INDEX(kmsdev, connector_index) DEBUG_ASSERT(IS_VALID_CONNECTOR_INDEX(kmsdev, connector_index))

#define PRESENTER_PRIVATE_KMS(p_presenter) ((struct kms_presenter *) (p_presenter)->private)
#define PRESENTER_PRIVATE_FBDEV(p_presenter) ((struct fbdev_presenter *) (p_presenter)->private)

#define FBDEV_FORMAT_MAPPING(_fourcc, r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset) \
    { \
        .r = {.length = r_length, .offset = r_offset, .msb_right = 0}, \
        .g = {.length = g_length, .offset = g_offset, .msb_right = 0}, \
        .b = {.length = b_length, .offset = b_offset, .msb_right = 0}, \
        .a = {.length = a_length, .offset = a_offset, .msb_right = 0}, \
        .fourcc = _fourcc \
    }

struct drm_connector {
    drmModeConnector *connector;
	drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct drm_encoder {
    drmModeEncoder *encoder;
};

struct drm_crtc {
    drmModeCrtc *crtc;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;

    uint32_t bitmask;
    uint8_t index;

    int selected_connector_index;
    struct drm_connector *selected_connector;
    drmModeModeInfo selected_mode;
    uint32_t selected_mode_blob_id;

    bool supports_hardware_cursor;

    bool supports_zpos;
    int min_zpos, max_zpos;
    
    size_t n_formats;
    uint32_t *formats;

    presenter_scanout_callback_t scanout_cb;
    void *scanout_cb_userdata;
};

struct drm_plane {
    drmModePlane *plane;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;

    struct {
        uint32_t crtc_id, fb_id,
            src_x, src_y, src_w, src_h,
            crtc_x, crtc_y, crtc_w, crtc_h,
            rotation, zpos;
    } property_ids;

    int type;
    int min_zpos, max_zpos;
    uint32_t supported_rotations;
};

/**
 * @brief Helper structure that is attached to GBM BOs as userdata to track the DRM FB with which they can
 * be scanned out (and later destroyed) 
 */
struct drm_fb {
    struct kmsdev *dev;
    uint32_t fb_id;
};

struct kms_cursor {
    uint32_t handle;
    int width, height;
    int hot_x, hot_y;
};

struct kms_cursor_state {
    int x, y;
    bool enabled;
    struct kms_cursor *cursor;
};

struct kmsdev {
    int fd;
    bool use_atomic_modesetting;
    bool supports_explicit_fencing;

    size_t n_connectors;
    struct drm_connector *connectors;

    size_t n_encoders;
    struct drm_encoder *encoders;

    size_t n_crtcs;
    struct drm_crtc *crtcs;

    size_t n_planes;
    struct drm_plane *planes;

    drmModeRes *res;
    drmModePlaneRes *plane_res;

    drmEventContext pageflip_evctx;

    struct kms_cursor_state cursor_state[32];
};

struct fbdev {
    int fd;
    
    /**
     * @brief fixinfo is the info that is fixed per framebuffer and can't be changed.
     */
    struct fb_fix_screeninfo fixinfo;

    /**
     * @brief varinfo is the info that can potentially be changed, but we don't do that and
     * instead rely on something like `fbset` configuring the fbdev for us.
     */
    struct fb_var_screeninfo varinfo;

    /**
     * @brief The physical width & height of the frame buffer and the pixel format as a fourcc code.
     */
    uint32_t width, height, format;
    
    /**
     * @brief The mapped video memory of this fbdev.
     * Basically the result of mmap(fd)
     */ 
    uint8_t *vmem;

    /**
     * @brief How many bytes of vmem we mapped.
     */
    size_t size_vmem;
};

struct kms_presenter {
    struct kmsdev *dev;

    /**
     * @brief Bitmask of the planes that are currently free to use.
     */
    uint32_t free_planes;

    int active_crtc_index;
    struct drm_crtc *active_crtc;

    int current_zpos;
    drmModeAtomicReq *req;

    bool has_primary_layer;
    struct drm_fb_layer primary_layer;

    struct {
        void (*callback)(int crtc_index, void *userdata);
        void *userdata;
    } scanout_callbacks[32];
};

struct fbdev_presenter {
    struct fbdev *fbdev;

    presenter_scanout_callback_t scanout_cb;
    void *scanout_cb_userdata;
};

struct presenter_private;

struct presenter {
    struct presenter_private *private;
    int (*get_supported_formats)(struct presenter *presenter, const uint32_t *const *formats_out, size_t *n_formats_out);
    int (*set_active_crtc)(struct presenter *presenter, int crtc_id);
    int (*get_active_crtc)(struct presenter *presenter, int *crtc_id_out);
    int (*set_logical_zpos)(struct presenter *presenter, int logical_zpos);
    int (*get_zpos)(struct presenter *presenter);
    int (*set_scanout_callback)(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata);
    int (*push_drm_fb_layer)(struct presenter *presenter, const struct drm_fb_layer *layer);
    int (*push_gbm_bo_layer)(struct presenter *presenter, const struct gbm_bo_layer *layer);
    int (*push_sw_fb_layer)(struct presenter *presenter, const struct sw_fb_layer *layer);
    int (*push_placeholder_layer)(struct presenter *presenter, int n_reserved_layers);
    int (*flush)(struct presenter *presenter);
    int (*destroy)(struct presenter *presenter);
};

struct fbdev_format_mapping {
    struct fb_bitfield r, g, b, a;
    uint32_t fourcc;
};

static const struct fbdev_format_mapping mappings[] = {
    /**
     * 16-bit (high color)
     */
    FBDEV_FORMAT_MAPPING(GBM_FORMAT_RGB565, 5, 11, 6, 5, 5, 0, 0, 0),
    
    /**
     * 24- or 32-bit (true color)
     */
    FBDEV_FORMAT_MAPPING(GBM_FORMAT_ARGB8888, 8, 16, 8, 8, 8, 0, 8, 24)
};

static const size_t n_mappings = sizeof(mappings) / sizeof(*mappings);

/*****************************************************************************
 *                                  FBDEV                                    *
 *****************************************************************************/
static inline bool fb_bitfield_equals(const struct fb_bitfield *a, const struct fb_bitfield *b) {
    return (a->offset == b->offset) && (a->length == b->length) && ((a->msb_right != 0) == (b->msb_right != 0));
}

struct fbdev *fbdev_new_from_fd(int fd) {
    struct fbdev *dev;
    uint32_t format;
    void *vmem;
    int ok, i;

    dev = malloc(sizeof *dev);
    if (dev == NULL) {
        return NULL;
    }

    ok = ioctl(fd, FBIOGET_FSCREENINFO, &dev->fixinfo);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't get fbdev fix_screeninfo. ioctl: %s\n", strerror(-ok));
        goto fail_free_dev;
    }

    ok = ioctl(fd, FBIOGET_VSCREENINFO, &dev->varinfo);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't get fbdev var_screeninfo. ioctl: %s\n", strerror(-ok));
        goto fail_free_dev;
    }

    if (dev->fixinfo.visual != FB_TYPE_PACKED_PIXELS) {
        LOG_MODESETTING_ERROR("A fbdev type other than FB_TYPE_PACKED_PIXELS is not supported.\n");
        goto fail_free_dev;
    }
    
    if (dev->fixinfo.visual != FB_VISUAL_TRUECOLOR) {
        LOG_MODESETTING_ERROR("A fbdev visual other than FB_VISUAL_TRUECOLOR is not supported.\n");
        goto fail_free_dev;
    }

    // look for the fourcc code for this fbdev format
    format = 0;
    for (i = 0; i < n_mappings; i++) {
        if (fb_bitfield_equals(&dev->varinfo.red, &mappings[i].r) &&
            fb_bitfield_equals(&dev->varinfo.green, &mappings[i].g) &&
            fb_bitfield_equals(&dev->varinfo.blue, &mappings[i].b) &&
            fb_bitfield_equals(&dev->varinfo.transp, &mappings[i].a)
        ) {
            format = mappings[i].fourcc;
            break;
        }
    }

    // if the format is still 0 we couldn't find a corresponding DRM fourcc format.
    if (format == 0) {
        LOG_MODESETTING_ERROR(
            "Didn't find a corresponding fourcc format for fbdev format rgba = %" PRIu32 "/%" PRIu32 ",%" PRIu32 "/%" PRIu32 ",%" PRIu32 "/%" PRIu32 ",%" PRIu32"/%" PRIu32 ".\n",
            dev->varinfo.red.length, dev->varinfo.red.offset,
            dev->varinfo.green.length, dev->varinfo.green.offset,
            dev->varinfo.blue.length, dev->varinfo.blue.offset,
            dev->varinfo.transp.length, dev->varinfo.transp.offset
        );
        goto fail_free_dev;
    }

    vmem = mmap(
        NULL,
        dev->varinfo.yres_virtual * dev->fixinfo.line_length,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        dev->fd,
        0
    );
    if (vmem == MAP_FAILED) {
        LOG_MODESETTING_ERROR("Couldn't map fbdev. mmap: %s\n", strerror(errno));
        goto fail_free_dev;
    }

    dev->fd = fd;
    dev->width = dev->varinfo.width;
    dev->height = dev->varinfo.height;
    dev->format = format;
    dev->vmem = vmem;

    return dev;


    fail_free_dev:
    free(dev);

    fail_return_null:
    return NULL;
}

struct fbdev *fbdev_new_from_path(const char *path) {
    struct fbdev *dev;
    int fd;

    fd = open(path, O_RDWR);
    if (fd < 0) {
        LOG_MODESETTING_ERROR("Couldn't open fb device. open: %s\n", strerror(errno));
        return NULL;
    }

    dev = fbdev_new_from_fd(fd);
    if (dev == NULL) {
        close(fd);
        return NULL;
    }

    return dev;
}

void fbdev_destroy(struct fbdev *dev) {
    munmap(dev->vmem, dev->size_vmem);
    close(dev->fd);
    free(dev);
}

/*****************************************************************************
 *                                   KMS                                     *
 *****************************************************************************/
static int fetch_connectors(struct kmsdev *dev, struct drm_connector **connectors_out, size_t *n_connectors_out) {
    struct drm_connector *connectors;
    int n_allocated_connectors;
    int ok;

    connectors = calloc(dev->res->count_connectors, sizeof *connectors);
    if (connectors == NULL) {
        *connectors_out = NULL;
        return ENOMEM;
    }

    n_allocated_connectors = 0;
    for (int i = 0; i < dev->res->count_connectors; i++, n_allocated_connectors++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModeConnector *connector;

        connector = drmModeGetConnector(dev->fd, dev->res->connectors[i]);
        if (connector == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device connector. drmModeGetConnector");
            goto fail_free_connectors;
        }

        props = drmModeObjectGetProperties(dev->fd, dev->res->connectors[i], DRM_MODE_OBJECT_CONNECTOR);
        if (props == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device connectors properties. drmModeObjectGetProperties");
            drmModeFreeConnector(connector);
            goto fail_free_connectors;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreeConnector(connector);
            goto fail_free_connectors;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(dev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                perror("[modesetting] Could not get DRM device connector properties' info. drmModeGetProperty");
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreeConnector(connector);
                goto fail_free_connectors;
            }
        }

        connectors[i].connector = connector;
        connectors[i].props = props;
        connectors[i].props_info = props_info;
    }

    *connectors_out = connectors;
    *n_connectors_out = dev->res->count_connectors;

    return 0;

    fail_free_connectors:
    for (int i = 0; i < n_allocated_connectors; i++) {
        for (unsigned int j = 0; j < connectors[i].props->count_props; j++)
            drmModeFreeProperty(connectors[i].props_info[j]);
        free(connectors[i].props_info);
        drmModeFreeObjectProperties(connectors[i].props);
        drmModeFreeConnector(connectors[i].connector);
    }

    fail_free_result:
    free(connectors);

    *connectors_out = NULL;
    *n_connectors_out = 0;
    return ok;
}

static int free_connectors(struct drm_connector *connectors, size_t n_connectors) {
    for (unsigned int i = 0; i < n_connectors; i++) {
        for (unsigned int j = 0; j < connectors[i].props->count_props; j++)
            drmModeFreeProperty(connectors[i].props_info[j]);
        free(connectors[i].props_info);
        drmModeFreeObjectProperties(connectors[i].props);
        drmModeFreeConnector(connectors[i].connector);
    }

    free(connectors);

    return 0;
}

static int fetch_encoders(struct kmsdev *dev, struct drm_encoder **encoders_out, size_t *n_encoders_out) {
    struct drm_encoder *encoders;
    int n_allocated_encoders;
    int ok;

    encoders = calloc(dev->res->count_encoders, sizeof *encoders);
    if (encoders == NULL) {
        *encoders_out = NULL;
        *n_encoders_out = 0;
        return ENOMEM;
    }

    n_allocated_encoders = 0;
    for (int i = 0; i < dev->res->count_encoders; i++, n_allocated_encoders++) {
        drmModeEncoder *encoder;

        encoder = drmModeGetEncoder(dev->fd, dev->res->encoders[i]);
        if (encoder == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device encoder. drmModeGetEncoder");
            goto fail_free_encoders;
        }

        encoders[i].encoder = encoder;
    }

    *encoders_out = encoders;
    *n_encoders_out = dev->res->count_encoders;

    return 0;

    fail_free_encoders:
    for (int i = 0; i < n_allocated_encoders; i++) {
        drmModeFreeEncoder(encoders[i].encoder);
    }

    fail_free_result:
    free(encoders);

    *encoders_out = NULL;
    *n_encoders_out = 0;
    return ok;
}

static int free_encoders(struct drm_encoder *encoders, size_t n_encoders) {
    for (unsigned int i = 0; i < n_encoders; i++) {
        drmModeFreeEncoder(encoders[i].encoder);
    }

    free(encoders);

    return 0;
}

static int fetch_crtcs(struct kmsdev *dev, struct drm_crtc **crtcs_out, size_t *n_crtcs_out) {
    struct drm_crtc *crtcs;
    int n_allocated_crtcs;
    int ok;

    crtcs = calloc(dev->res->count_crtcs, sizeof *crtcs);
    if (crtcs == NULL) {
        *crtcs_out = NULL;
        return ENOMEM;
    }

    n_allocated_crtcs = 0;
    for (int i = 0; i < dev->res->count_crtcs; i++, n_allocated_crtcs++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModeCrtc *crtc;

        crtc = drmModeGetCrtc(dev->fd, dev->res->crtcs[i]);
        if (crtc == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device CRTC. drmModeGetCrtc");
            goto fail_free_crtcs;
        }

        props = drmModeObjectGetProperties(dev->fd, dev->res->crtcs[i], DRM_MODE_OBJECT_CRTC);
        if (props == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device CRTCs properties. drmModeObjectGetProperties");
            drmModeFreeCrtc(crtc);
            goto fail_free_crtcs;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreeCrtc(crtc);
            goto fail_free_crtcs;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(dev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                perror("[modesetting] Could not get DRM device CRTCs properties' info. drmModeGetProperty");
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreeCrtc(crtc);
                goto fail_free_crtcs;
            }
        }
        
        crtcs[i].crtc = crtc;
        crtcs[i].props = props;
        crtcs[i].props_info = props_info;
        
        crtcs[i].index = i;
        crtcs[i].bitmask = 1 << i;

        crtcs[i].selected_connector_index = -1;
        crtcs[i].selected_connector = NULL;
        memset(&crtcs[i].selected_mode, 0, sizeof(drmModeModeInfo));
        crtcs[i].selected_mode_blob_id = 0;
        
        // for the other ones, set some reasonable defaults.
        // these depend on the plane data
        crtcs[i].supports_hardware_cursor = false;

        crtcs[i].supports_zpos = false;
        crtcs[i].min_zpos = 0;
        crtcs[i].max_zpos = 0;
        
        crtcs[i].n_formats = 0;
        crtcs[i].formats = NULL;

        crtcs[i].scanout_cb = NULL;
        crtcs[i].scanout_cb_userdata = NULL;
    }

    *crtcs_out = crtcs;
    *n_crtcs_out = dev->res->count_crtcs;

    return 0;


    fail_free_crtcs:
    for (int i = 0; i < n_allocated_crtcs; i++) {
        for (unsigned int j = 0; j < crtcs[i].props->count_props; j++)
            drmModeFreeProperty(crtcs[i].props_info[j]);
        free(crtcs[i].props_info);
        drmModeFreeObjectProperties(crtcs[i].props);
        drmModeFreeCrtc(crtcs[i].crtc);
    }

    fail_free_result:
    free(crtcs);

    *crtcs_out = NULL;
    *n_crtcs_out = 0;
    return ok;
}

static int free_crtcs(struct drm_crtc *crtcs, size_t n_crtcs) {
    for (unsigned int i = 0; i < n_crtcs; i++) {
        for (unsigned int j = 0; j < crtcs[i].props->count_props; j++)
            drmModeFreeProperty(crtcs[i].props_info[j]);
        free(crtcs[i].props_info);
        drmModeFreeObjectProperties(crtcs[i].props);
        drmModeFreeCrtc(crtcs[i].crtc);
    }

    free(crtcs);

    return 0;
}

static int fetch_planes(struct kmsdev *dev, struct drm_plane **planes_out, size_t *n_planes_out) {
    struct drm_plane *planes;
    int n_allocated_planes;
    int ok;

    planes = calloc(dev->plane_res->count_planes, sizeof *planes);
    if (planes == NULL) {
        *planes_out = NULL;
        return ENOMEM;
    }

    n_allocated_planes = 0;
    for (unsigned int i = 0; i < dev->plane_res->count_planes; i++, n_allocated_planes++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModePlane *plane;

        memset(&planes[i].property_ids, 0xFF, sizeof(planes[i].property_ids));

        plane = drmModeGetPlane(dev->fd, dev->plane_res->planes[i]);
        if (plane == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device plane. drmModeGetPlane");
            goto fail_free_planes;
        }

        props = drmModeObjectGetProperties(dev->fd, dev->plane_res->planes[i], DRM_MODE_OBJECT_PLANE);
        if (props == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device planes' properties. drmModeObjectGetProperties");
            drmModeFreePlane(plane);
            goto fail_free_planes;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreePlane(plane);
            goto fail_free_planes;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(dev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                perror("[modesetting] Could not get DRM device planes' properties' info. drmModeGetProperty");
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreePlane(plane);
                goto fail_free_planes;
            }

            if (strcmp(props_info[j]->name, "type") == 0) {
                planes[i].type = 0;
                for (uint32_t k = 0; k < props->count_props; k++) {
                    if (props->props[k] == props_info[j]->prop_id) {
                        planes[i].type = props->prop_values[k];
                        break;
                    }
                }
            } else if (strcmp(props_info[j]->name, "CRTC_ID") == 0) {
                planes[i].property_ids.crtc_id = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "FB_ID") == 0) {
                planes[i].property_ids.fb_id = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_X") == 0) {
                planes[i].property_ids.src_x = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_Y") == 0) {
                planes[i].property_ids.src_y = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_W") == 0) {
                planes[i].property_ids.src_w = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_H") == 0) {
                planes[i].property_ids.src_h = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_X") == 0) {
                planes[i].property_ids.crtc_x = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_Y") == 0) {
                planes[i].property_ids.crtc_y = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_W") == 0) {
                planes[i].property_ids.crtc_w = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_H") == 0) {
                planes[i].property_ids.crtc_h = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "zpos") == 0) {
                planes[i].property_ids.zpos = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "rotation") == 0) {
                planes[i].property_ids.rotation = props_info[j]->prop_id;
            }
        }

        planes[i].plane = plane;
        planes[i].props = props;
        planes[i].props_info = props_info;
    }

    *planes_out = planes;
    *n_planes_out = dev->plane_res->count_planes;

    return 0;


    fail_free_planes:
    for (int i = 0; i < n_allocated_planes; i++) {
        for (unsigned int j = 0; j < planes[i].props->count_props; j++)
            drmModeFreeProperty(planes[i].props_info[j]);
        free(planes[i].props_info);
        drmModeFreeObjectProperties(planes[i].props);
        drmModeFreePlane(planes[i].plane);
    }

    fail_free_result:
    free(planes);

    *planes_out = NULL;
    *n_planes_out = 0;
    return ok;
}

static int free_planes(struct drm_plane *planes, size_t n_planes) {
    for (size_t i = 0; i < n_planes; i++) {
        for (uint32_t j = 0; j < planes[i].props->count_props; j++)
            drmModeFreeProperty(planes[i].props_info[j]);
        free(planes[i].props_info);
        drmModeFreeObjectProperties(planes[i].props);
        drmModeFreePlane(planes[i].plane);
    }

    free(planes);

    return 0;
}

bool fd_is_kmsfd(int fd) {
    drmModeRes *res;
    int ok;

    res = drmModeGetResources(fd);
    if (res == NULL) {
        return false;
    }

    drmModeFreeResources(res);

    return true;
}

void on_pageflip(
    int fd,
    unsigned int sequence,
    unsigned int tv_sec,
    unsigned int tv_usec,
    unsigned int crtc_id,
    void *userdata
) {
    struct drm_crtc *crtc;
    struct kmsdev *dev;
    int i;
    
    DEBUG_ASSERT(userdata != NULL);

    dev = userdata;

    for (i = 0; i < dev->n_crtcs; i++) {
        if (dev->crtcs[i].crtc->crtc_id == crtc_id) {
            break;
        }
    }

    DEBUG_ASSERT(i < dev->n_crtcs);

    crtc = dev->crtcs + i;

    if (crtc->scanout_cb != NULL) {
        crtc->scanout_cb(i, tv_sec, tv_usec, crtc->scanout_cb_userdata);
        crtc->scanout_cb = NULL;
        crtc->scanout_cb_userdata = NULL;
    }
}

struct kmsdev *kmsdev_new_from_fd(struct event_loop *loop, int fd) {
    struct kmsdev *dev;
    int ok;

    dev = malloc(sizeof *dev);
    if (dev == NULL) {
        return NULL;
    }

    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        ok = errno;
        goto fail_close_fd;
    } else {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client universal planes capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_close_fd;
    }
    
    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        dev->use_atomic_modesetting = false;
    } else if (ok < 0) {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client atomic capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_close_fd;
    } else {
        dev->use_atomic_modesetting = true;
    }

    dev = gbm_create_device(fd);
    if (dev == NULL) {
        ok = errno;
        goto fail_close_fd;
    }

    dev->res = drmModeGetResources(fd);
    if (dev->res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device resources. drmModeGetResources");
        goto fail_close_fd;
    }

    dev->plane_res = drmModeGetPlaneResources(dev->fd);
    if (dev->plane_res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device planes resources. drmModeGetPlaneResources");
        goto fail_free_resources;
    }

    ok = fetch_connectors(dev, &dev->connectors, &dev->n_connectors);
    if (ok != 0) {
        goto fail_free_plane_resources;
    }

    ok = fetch_encoders(dev, &dev->encoders, &dev->n_encoders);
    if (ok != 0) {
        goto fail_free_connectors;
    }

    ok = fetch_crtcs(dev, &dev->crtcs, &dev->n_crtcs);
    if (ok != 0) {
        goto fail_free_encoders;
    }

    ok = fetch_planes(dev, &dev->planes, &dev->n_planes);
    if (ok != 0) {
        goto fail_free_crtcs;
    }

    dev->fd = fd;
    dev->supports_explicit_fencing = false;
    dev->pageflip_evctx = (drmEventContext) {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = NULL,
        .page_flip_handler = NULL,
        .page_flip_handler2 = on_pageflip,
        .sequence_handler = NULL
    };

    return dev;


    fail_free_crtcs:
    free_crtcs(dev->crtcs, dev->n_crtcs);

    fail_free_encoders:
    free_encoders(dev->encoders, dev->n_encoders);

    fail_free_connectors:
    free_connectors(dev->connectors, dev->n_connectors);

    fail_free_plane_resources:
    drmModeFreePlaneResources(dev->plane_res);

    fail_free_resources:
    drmModeFreeResources(dev->res);

    fail_close_fd:
    close(dev->fd);

    fail_free_drmdev:
    free(dev);

    return NULL;
}

void kmsdev_destroy(struct kmsdev *dev) {
    if (dev->fd > 0) {
        close(dev->fd);
    }
    free(dev);
}

int kmsdev_get_n_crtcs(struct kmsdev *dev) {
    DEBUG_ASSERT(dev != NULL);
    return dev->n_crtcs;
}

int kmsdev_get_n_connectors(struct kmsdev *dev) {
    DEBUG_ASSERT(dev != NULL);
    return dev->n_connectors;
}

int kmsdev_configure_crtc(
    struct kmsdev *dev,
    int crtc_index,
    int connector_index,
    drmModeModeInfo *mode
) {
    struct drm_crtc *crtc;
    uint32_t blob_id;
    int ok;

    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);
    DEBUG_ASSERT_VALID_CONNECTOR_INDEX(dev, connector_index);
    DEBUG_ASSERT(mode != NULL);

    crtc = dev->crtcs + crtc_index;

    blob_id = 0;
    if (dev->use_atomic_modesetting) {
        ok = drmModeCreatePropertyBlob(dev->fd, mode, sizeof(mode), &blob_id);
        if (ok < 0) {
            ok = errno;
            LOG_MODESETTING_ERROR("Couldn't upload video mode to KMS. drmModeCreatePropertyBlob: %s\n", strerror(errno));
            goto fail_return_ok;
        }
    } else {
        ok = drmModeSetCrtc(
            dev->fd,
            dev->crtcs[crtc_index].crtc->crtc_id,
            0,
            0, 0,
            &(dev->connectors[connector_index].connector->connector_id),
            1,
            mode
        );
        if (ok < 0) {
            ok = errno;
            LOG_MODESETTING_ERROR("Couldn't set mode on CRTC. drmModeSetCrtc: %s\n", strerror(errno));
            goto fail_return_ok;
        }
    }

    if (crtc->selected_mode_blob_id != 0) {
        ok = drmModeDestroyPropertyBlob(dev->fd, crtc->selected_mode_blob_id);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't delete old video mode. drmModeDestroyPropertyBlob: %s\n", strerror(errno));
            goto fail_maybe_delete_property_blob;
        }
    }

    crtc->selected_connector_index = connector_index;
    crtc->selected_connector = dev->connectors + connector_index;
    crtc->selected_mode = *mode;
    crtc->selected_mode_blob_id = blob_id;

    return 0;

    fail_maybe_delete_property_blob:
    if (blob_id != 0) {
        DEBUG_ASSERT(drmModeDestroyPropertyBlob(dev->fd, blob_id) >= 0);
    }

    fail_return_ok:
    return ok;
}

int kmsdev_configure_crtc_with_preferences(
    struct kmsdev *dev,
    int crtc_index,
    int connector_index,
    const enum kmsdev_mode_preference *preferences
) {
    enum kmsdev_mode_preference *cursor, pref;
    drmModeModeInfo *stored_mode, *mode;

    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);
    DEBUG_ASSERT_VALID_CONNECTOR_INDEX(dev, connector_index);
    DEBUG_ASSERT(preferences != NULL);
    DEBUG_ASSERT(*preferences != kKmsdevModePreferenceNone);

    stored_mode = NULL;
    for (int i = 0; i < dev->connectors[connector_index].connector->count_modes; i++) {
        mode = dev->connectors[connector_index].connector->modes + i;

        if (stored_mode == NULL) {
            continue;
        }

        cursor = preferences;
        do {
            pref = *cursor;

            if (pref == kKmsdevModePreferencePreferred) {
                if (mode_is_preferred(stored_mode) != mode_is_preferred(mode)) {
                    if (mode_is_preferred(mode)) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else if ((pref == kKmsdevModePreferenceHighestResolution) || (pref == kKmsdevModePreferenceLowestResolution)) {
                int area1 = mode_get_display_area(stored_mode);
                int area2 = mode_get_display_area(mode);

                if (area1 != area2) {
                    if ((pref == kKmsdevModePreferenceHighestResolution) && (area2 > area1)) {
                        stored_mode = mode;
                    } else if ((pref == kKmsdevModePreferenceLowestResolution) && (area2 < area1)) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else if ((pref == kKmsdevModePreferenceHighestRefreshrate) || (pref == kKmsdevModePreferenceLowestRefreshrate)) {
                int refreshrate1 = round(mode_get_vrefresh(stored_mode));
                int refreshrate2 = round(mode_get_vrefresh(mode));

                if (refreshrate1 != refreshrate2) {
                    if ((pref == kKmsdevModePreferenceHighestRefreshrate) && (refreshrate2 > refreshrate1)) {
                        stored_mode = mode;
                    } else if ((pref == kKmsdevModePreferenceLowestRefreshrate) && (refreshrate2 < refreshrate1)) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else if ((pref == kKmsdevModePreferenceInterlaced) || (pref == kKmsdevModePreferenceProgressive)) {
                if (mode_is_interlaced(stored_mode) != mode_is_interlaced(mode)) {
                    if ((pref == kKmsdevModePreferenceProgressive) && (!mode_is_interlaced(mode))) {
                        stored_mode = mode;
                    } else if ((pref == kKmsdevModePreferenceInterlaced) && (mode_is_interlaced(mode))) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else {
                // we shouldn't ever reach this point.
                DEBUG_ASSERT(false);
            }
        } while (*(cursor++) != kKmsdevModePreferenceNone);
    }

    return kmsdev_configure_crtc(dev, crtc_index, connector_index, stored_mode);
}

static void destroy_gbm_bo(struct gbm_bo *bo, void *userdata) {
	struct drm_fb *fb = userdata;

    DEBUG_ASSERT(fb != NULL);

    kmsdev_destroy_fb(fb->dev, fb->fb_id);
	free(fb);
}

int kmsdev_add_gbm_bo(
    struct kmsdev *dev,
    struct gbm_bo *bo,
    uint32_t *fb_id_out
) {
    struct drm_fb *fb;
	uint64_t modifiers[4] = {0};
	uint32_t width, height, format;
	uint32_t strides[4] = {0};
	uint32_t handles[4] = {0};
	uint32_t offsets[4] = {0};
	uint32_t flags = 0;
	int num_planes;
	int ok = -1;
	
	fb = gbm_bo_get_user_data(bo);
	/// TODO: Invalidate the drm_fb in the case that the drmdev changed (although that should never happen)
	// If the buffer object already has some userdata associated with it,
	// it's the drm_fb we allocated.
	if (fb != NULL) {
        *fb_id_out = fb->fb_id;
		return 0;
	}

	// If there's no framebuffer for the BO, we need to create one.
	fb = malloc(sizeof *fb);
	if (fb == NULL) {
		return ENOMEM;
	}

	fb->dev = dev;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	format = gbm_bo_get_format(bo);

	modifiers[0] = gbm_bo_get_modifier(bo);
	num_planes = gbm_bo_get_plane_count(bo);

	for (int i = 0; i < num_planes; i++) {
		strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		handles[i] = gbm_bo_get_handle(bo).u32;
		offsets[i] = gbm_bo_get_offset(bo, i);
		modifiers[i] = modifiers[0];
	}

	if (modifiers[0]) {
		flags = DRM_MODE_FB_MODIFIERS;
	}

	ok = kmsdev_add_fb(
		dev,
		width,
		height,
		format,
		handles,
		strides,
		offsets,
		modifiers,
		&fb->fb_id,
		flags
	);
	if (ok != 0) {
		LOG_MODESETTING_ERROR("Couldn't add GBM BO as DRM framebuffer. kmsdev_add_fb: %s\n", strerror(ok));
		free(fb);
		return ok;
	}

	gbm_bo_set_user_data(bo, fb, destroy_gbm_bo);

    *fb_id_out = fb->fb_id;

	return 0;
}

int kmsdev_add_fb(
    struct kmsdev *dev,
    uint32_t width, uint32_t height,
    uint32_t pixel_format,
    const uint32_t bo_handles[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    const uint64_t modifier[4],
    uint32_t *buf_id,
    uint32_t flags
) {
    int ok;
    
    ok = drmModeAddFB2WithModifiers(
        dev->fd,
        width, height,
        pixel_format,
        bo_handles,
        pitches,
        offsets,
        modifier,
        buf_id,
        flags
    );

    return -ok;
}

int kmsdev_destroy_fb(
    struct kmsdev *dev,
    uint32_t buf_id
) {
    return -drmModeRmFB(dev->fd, buf_id);
}

const drmModeModeInfo *kmsdev_get_selected_mode(struct kmsdev *dev, int crtc_index) {
    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);

    return &dev->crtcs[crtc_index].selected_mode;
}

static inline int set_cursor(struct kmsdev *dev, int crtc_index, struct kms_cursor *cursor) {
    int ok;
    
    if (cursor != NULL) {
        ok = drmModeSetCursor2(
            dev->fd,
            dev->crtcs[crtc_index].crtc->crtc_id,
            cursor->handle,
            cursor->width,
            cursor->height,
            cursor->hot_x,
            cursor->hot_y
        );
    } else {
        ok = drmModeSetCursor2(
            dev->fd,
            dev->crtcs[crtc_index].crtc->crtc_id,
            0,
            64, 64,
            0, 0
        );
    }
    
    if (ok < 0) {
        ok = errno;
        LOG_MODESETTING_ERROR("Couldn't set cursor buffer. drmModeSetCursor2: %s\n", strerror(errno));
        return ok;
    }

    return 0;
}

int kmsdev_set_cursor(struct kmsdev *dev, int crtc_index, struct kms_cursor *cursor) {
    struct kms_cursor_state *state;
    int ok;

    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);

    if (state->cursor != cursor) {
        ok = set_cursor(dev, crtc_index, cursor);
        if (ok != 0) {
            return ok;
        }

        state->cursor = cursor;
    }

    return 0;
}

int kmsdev_move_cursor(struct kmsdev *dev, int crtc_index, int x, int y) {
    struct kms_cursor_state *state;
    int ok;

    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);

    state = dev->cursor_state + crtc_index;

    if (state->cursor == NULL) {
        // if the cursor isn't enabled for this CRTC, don't do anything
        return 0;
    }

	ok = drmModeMoveCursor(dev->fd, dev->crtcs[crtc_index].crtc->crtc_id, x - state->cursor->hot_x, y - state->cursor->hot_y);
	if (ok < 0) {
        ok = errno;
		LOG_MODESETTING_ERROR("Could not move cursor. drmModeMoveCursor: %s", strerror(errno));
		return ok;
	}

	state->x = x;
	state->y = y;

	return 0;
}

/**
 * @brief Load raw cursor data into a cursor that can be used by KMS.
 */
struct kms_cursor *kmsdev_load_cursor(
    struct kmsdev *dev,
    int width, int height,
    uint32_t format,
    int hot_x, int hot_y,
    const uint8_t *data
) {
    struct drm_mode_destroy_dumb destroy_req;
    struct drm_mode_create_dumb create_req;
	struct drm_mode_map_dumb map_req;
    struct kms_cursor *cursor;
	uint32_t drm_fb_id;
	uint32_t *buffer;
	uint64_t cap;
	uint8_t depth;
	int ok;
    int ok;

    // ARGB8888 is the format that'll implicitly be used when
    // uploading a cursor BO using drmModeSetCursor2.
    if (format != GBM_FORMAT_ARGB8888) {
        LOG_MODESETTING_ERROR("A format other than ARGB8888 is not supported for cursor images right now.\n");
        goto fail_return_null;
    }

    // first, find out if we can actually use dumb buffers.
    ok = drmGetCap(dev->fd, DRM_CAP_DUMB_BUFFER, &cap);
	if (ok < 0) {
		ok = errno;
        LOG_MODESETTING_ERROR("Could not query GPU driver support for dumb buffers. drmGetCap: %s\n", strerror(ok));
		goto fail_return_null;
	}

	if (cap == 0) {
        ok = ENOTSUP;
        LOG_MODESETTING_ERROR("Kernel / GPU Driver does not support dumb DRM buffers. Mouse cursor will not be displayed.\n");
		goto fail_return_null;
	}

	memset(&create_req, 0, sizeof create_req);
	create_req.width = width;
	create_req.height = height;
	create_req.bpp = 32;
	create_req.flags = 0;

	ok = ioctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
	if (ok < 0) {
		ok = errno;
		LOG_MODESETTING_ERROR("Could not create a dumb buffer for the hardware cursor. ioctl: %s\n", strerror(errno));
		goto fail_return_null;
	}

	memset(&map_req, 0, sizeof map_req);
	map_req.handle = create_req.handle;

	ok = ioctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
	if (ok < 0) {
		ok = errno;
		LOG_MODESETTING_ERROR("Could not prepare dumb buffer mmap for uploading the hardware cursor image. ioctl: %s\n", strerror(errno));
		goto fail_destroy_dumb_buffer;
	}

	buffer = mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, map_req.offset);
	if (buffer == MAP_FAILED) {
		ok = errno;
		LOG_MODESETTING_ERROR("Could not mmap dumb buffer for uploading the hardware cursor icon. mmap: %s\n", strerror(errno));
        goto fail_destroy_dumb_buffer;
	}

    memcpy(buffer, data, create_req.size);

    ok = munmap(buffer, create_req.size);
    assert(ok >= 0); // likely a programming error

    cursor = malloc(sizeof *cursor);
    if (cursor == NULL) {
        goto fail_destroy_dumb_buffer;
    }

    cursor->handle = create_req.handle;
    cursor->width = width;
    cursor->height = height;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;

    return buffer;


	fail_destroy_dumb_buffer:
	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = create_req.handle;
	ioctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

	fail_return_null:
	return NULL;
}

/**
 * @brief Dispose this cursor, freeing all associated resources. Make sure
 * the cursor is now longer used on any crtc before disposing it.
 */
void kmsdev_dispose_cursor(struct kmsdev *dev, struct kms_cursor *cursor) {
    struct drm_mode_destroy_dumb destroy_req;
    int ok;

	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = cursor->handle;

	ok = ioctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    assert(ok >= 0); // likely a programming error
    
    free(cursor);
}


/*****************************************************************************
 *                               PRESENTERS                                  *
 *****************************************************************************/
/**
 * @brief Get the list of fourcc formats supported by this presenter.
 */
int presenter_get_supported_formats(struct presenter *presenter, const uint32_t const **formats_out, size_t *n_formats_out) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(formats_out != NULL);
    DEBUG_ASSERT(n_formats_out != NULL);

    return presenter->get_supported_formats(presenter, formats_out, n_formats_out);
}

static int kms_presenter_get_supported_formats(struct presenter *presenter, const uint32_t const **formats_out, size_t *n_formats_out) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    DEBUG_ASSERT_HAS_ACTIVE_CRTC(private);

    *formats_out = private->active_crtc->formats;
    *n_formats_out = private->active_crtc->n_formats;

    return 0;
}

static int fbdev_presenter_get_supported_formats(struct presenter *presenter, const uint32_t const **formats_out, size_t *n_formats_out) {
    struct fbdev_presenter *private = PRESENTER_PRIVATE_FBDEV(presenter);
    
    *formats_out = &private->fbdev->format;
    *n_formats_out = 1;

    return 0;
}

/**
 * @brief Select the CRTC we're currently pushing layers to.
 */
int presenter_set_active_crtc(struct presenter *presenter, int crtc_index) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->set_active_crtc != NULL);
    return presenter->set_active_crtc(presenter, crtc_index);
}

static int fbdev_presenter_set_active_crtc(struct presenter *presenter, int crtc_index) {
    // for fbdev we only have one CRTC / output.

    if (crtc_index != 0) {
        return EINVAL;
    }

    return 0;
}

static int kms_presenter_set_active_crtc(struct presenter *presenter, int crtc_index) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);
    struct drm_crtc *crtc;

    DEBUG_ASSERT_VALID_CRTC_INDEX(private, crtc_index);

    crtc = private->dev->crtcs + crtc_index;

    private->active_crtc_index = crtc_index;
    private->active_crtc = crtc;
    private->current_zpos = crtc->min_zpos;
    private->has_primary_layer = false;

    return 0;
}

/**
 * @brief Get the index of the active crtc in @param crtc_index_out.
 */
int presenter_get_active_crtc(struct presenter *presenter, int *crtc_index_out) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->get_active_crtc != NULL);
    return presenter->get_active_crtc(presenter, crtc_index_out);
}

static int fbdev_presenter_get_active_crtc(struct presenter *presenter, int *crtc_index_out) {
    *crtc_index_out = 0;
    return 0;
}

static int kms_presenter_get_active_crtc(struct presenter *presenter, int *crtc_index_out) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);
    
    DEBUG_ASSERT_HAS_ACTIVE_CRTC(private);

    *crtc_index_out = private->active_crtc_index;

    return 0;
}

/**
 * @brief Add a callback to all the layer for this CRTC are now present on screen.
 */
int presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->set_scanout_callback != NULL);
    return presenter->set_scanout_callback(presenter, cb, userdata);
}

static int fbdev_presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    struct fbdev_presenter *private = PRESENTER_PRIVATE_FBDEV(presenter);

    private->scanout_cb = cb;
    private->scanout_cb_userdata = userdata;
    
    return 0;
}

static int kms_presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    DEBUG_ASSERT_HAS_ACTIVE_CRTC(private);

    private->scanout_callbacks[private->active_crtc_index].callback = cb;
    private->scanout_callbacks[private->active_crtc_index].userdata = userdata;

    private->active_crtc->scanout_cb = cb;
    private->active_crtc->scanout_cb_userdata = userdata;
    
    return 0;
}

/**
 * @brief Sets the zpos (which will be used for any new pushed planes) to the zpos corresponding to @ref logical_zpos.
 * Only supported for KMS presenters.
 * 
 * Actual hardware zpos ranges are driver-specific, sometimes they range from 0 to 127, sometimes 1 to 256, etc.
 * The logical zpos always starts at 0 and ends at (hw_zpos_max - hw_zpos_min) inclusive.
 */
void presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->set_logical_zpos != NULL);
    return presenter->set_logical_zpos(presenter, logical_zpos);
}

static int kms_presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    DEBUG_ASSERT_HAS_ACTIVE_CRTC(presenter);

    if (logical_zpos >= 0) {
        logical_zpos = logical_zpos + private->active_crtc->min_zpos;
    } else {
        logical_zpos = private->active_crtc->max_zpos - logical_zpos + 1;
    }

    if (!IS_VALID_ZPOS(private, logical_zpos)) {
        return EINVAL;
    }

    private->current_zpos = logical_zpos;

    return 0;
}

static int fbdev_presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    if ((logical_zpos == 0) || (logical_zpos == -1)) {
        return 0;
    } else {
        return EINVAL;
    }
}

/**
 * @brief Gets the current (actual) zpos. Only supported by KMS presenters, and even then not supported all the time.
 */
int presenter_get_zpos(struct presenter *presenter) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->set_logical_zpos != NULL);
    return presenter->get_zpos(presenter);
}

static int kms_presenter_get_zpos(struct presenter *presenter) {
    struct kms_presenter *kms = PRESENTER_PRIVATE_KMS(presenter);

    DEBUG_ASSERT_HAS_ACTIVE_CRTC(presenter);

    return kms->current_zpos;
}

static int fbdev_presenter_get_zpos(struct presenter *presenter) {
    return 0;
}


/**
 * @brief Tries to find an unused DRM plane and returns its index in kmsdev->planes. Otherwise returns -1.
 */
static inline int reserve_plane(struct kms_presenter *presenter, int zpos_hint) {
    for (int i = 0; i < sizeof(presenter->free_planes) * CHAR_BIT; i++) {
        if (presenter->free_planes & (1 << i)) {
            presenter->free_planes &= ~(1 << i);
            return i;
        }
    }
    return -1;
}

/**
 * @brief Unreserves a plane index so it may be reserved using reserve_plane again.
 */
static inline int unreserve_plane(struct kms_presenter *presenter, int plane_index) {
    presenter->free_planes |= (1 << plane_index);
}

/**
 * @brief Presents a DRM framebuffer, can be used only for KMS.
 */
int presenter_push_drm_fb_layer(
    struct presenter *presenter,
    const struct drm_fb_layer *layer
) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT(presenter->push_drm_fb_layer != NULL);

    return presenter->push_drm_fb_layer(presenter, layer);
}

static int kms_presenter_push_drm_fb_layer(struct presenter *presenter, const struct drm_fb_layer *layer) {
    struct kms_presenter *private;
    struct drm_plane *plane;
    uint32_t plane_id, plane_index;
    int ok;
    
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);

    private = PRESENTER_PRIVATE_KMS(presenter);

    DEBUG_ASSERT_HAS_ACTIVE_CRTC(private);
    CHECK_VALID_ZPOS_OR_RETURN_EINVAL(private, private->current_zpos);

    plane_index = reserve_plane(private, private->has_primary_layer? 1 : 0);
    if (plane_index < 0) {
        LOG_MODESETTING_ERROR("Couldn't find unused plane for framebuffer layer.\n");
        return EINVAL;
    }

    plane = &private->dev->planes[plane_index];
    plane_id = plane->plane->plane_id;

    if (layer && (plane->property_ids.rotation == DRM_NO_PROPERTY_ID)) {
        LOG_MODESETTING_ERROR("Rotation was requested but is not supported.\n");
        ok = EINVAL;
        goto fail_unreserve_plane;
    }

    if ((plane->property_ids.zpos == DRM_NO_PROPERTY_ID) && private->has_primary_layer) {
        LOG_MODESETTING_ERROR("Plane doesn't support zpos but that's necessary for overlay planes.\n");
        ok = EINVAL;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.fb_id, layer->fb_id);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_id, private->active_crtc->crtc->crtc_id);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_x, layer->src_x);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_y, layer->src_y);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_w, layer->src_w);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_h, layer->src_h);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_x, layer->crtc_x);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_y, layer->crtc_y);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }
    
    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_w, layer->crtc_w);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_h, layer->crtc_h);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    if (plane->property_ids.zpos != DRM_NO_PROPERTY_ID) {
        ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.zpos, private->current_zpos);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
            ok = -ok;
            goto fail_unreserve_plane;
        }
    }

    if (layer->has_rotation) {
        ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.rotation, layer->rotation);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
            ok = -ok;
            goto fail_unreserve_plane;
        }
    }

    if (private->has_primary_layer == false) {
        memcpy(&private->primary_layer, layer, sizeof(struct drm_fb_layer));
    }

    return 0;


    fail_unreserve_plane:
    unreserve_plane(private, plane_index);

    fail_return_ok:
    return ok;
}

static int fbdev_presenter_push_drm_fb_layer(struct presenter *presenter, const struct drm_fb_layer *layer) {
    return EOPNOTSUPP;
}

/**
 * @brief Presents a GBM buffer object, can be used for both fbdev and KMS.
 */
int presenter_push_gbm_bo_layer(
    struct presenter *presenter,
    const struct gbm_bo_layer *layer
) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT(presenter->push_gbm_bo_layer != NULL);

    return presenter->push_gbm_bo_layer(presenter, layer);
}

static int kms_presenter_push_gbm_bo_layer(struct presenter *presenter, const struct gbm_bo_layer *layer) {
    /// TODO: Implement
    return EINVAL;
}

static int fbdev_presenter_push_gbm_bo_layer(struct presenter *presenter, const struct gbm_bo_layer *layer) {
    /// TODO: Implement
    return EINVAL;
}

/**
 * @brief Presents a software framebuffer (i.e. some malloced memory).
 * 
 * Can only be used for fbdev presenting.
 */
int presenter_push_sw_fb_layer(
    struct presenter *presenter,
    const struct sw_fb_layer *layer
) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT(presenter->push_gbm_bo_layer != NULL);

    return presenter->push_sw_fb_layer(presenter, layer);
}

static int kms_presenter_push_sw_fb_layer(struct presenter *presenter, const struct sw_fb_layer *layer) {
    /// NOTE: We _could_ copy the software framebuffer contents into a real framebuffer here.
    /// But really, the code using this should rather create a GBM BO, map it, and render into that directly.
    return ENOTSUP;
}

static int fbdev_presenter_push_sw_fb_layer(struct presenter *presenter, const struct sw_fb_layer *layer) {
    struct fbdev_presenter *private = PRESENTER_PRIVATE_FBDEV(presenter);

    // formats should probably be handled by the code using this
    if (layer->format != private->fbdev->format) {
        return EINVAL;
    }

    if (layer->width != private->fbdev->width) {
        return EINVAL;
    }

    if (layer->height != private->fbdev->height) {
        return EINVAL;
    }

    // If the pitches don't match, we'd need to copy line by line.
    // We can support that in the future but it's probably unnecessary right now.
    if (private->fbdev->fixinfo.line_length != layer->pitch) {
        LOG_MODESETTING_ERROR("Rendering software framebuffers into fbdev with non-matching buffer pitches is not supported.\n");
        return EINVAL;
    }

    size_t offset = private->fbdev->varinfo.yoffset * private->fbdev->fixinfo.line_length + private->fbdev->varinfo.xoffset;

    memcpy(
        private->fbdev->vmem + offset,
        layer->vmem,
        min(private->fbdev->size_vmem - offset, layer->height * layer->pitch)
    );

    return 0;
}

/**
 * @brief Push a placeholder layer. Increases the zpos by @ref n_reserved_layers for kms presenters.
 * Returns EOVERFLOW if the zpos before incrementing is higher than the maximum supported one by the hardware.
 */
void presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->push_placeholder_layer != NULL);

    return presenter->push_placeholder_layer(presenter, n_reserved_layers);
}

static int kms_presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    if (!IS_VALID_ZPOS(private, private->current_zpos)) {
        return EOVERFLOW;
    }

    private->current_zpos += n_reserved_layers;

    return 0;
}

static int fbdev_presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    return EOPNOTSUPP;
}

/**
 * @brief Makes sure all the output operations are applied. This is NOT the "point of no return", that
 * point is way earlier.
 */
int presenter_flush(struct presenter *presenter) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->flush != NULL);
    return presenter->flush(presenter);
}

static int kms_presenter_flush(struct presenter *presenter) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    /// TODO: hookup callback
    if (private->dev->use_atomic_modesetting) {
        return drmModeAtomicCommit(
            private->dev->fd,
            private->req,
            DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_ALLOW_MODESET,
            NULL
        );
    } else {
        return drmModePageFlip(
            private->dev->fd,
            private->active_crtc->crtc->crtc_id,
            private->primary_layer.fb_id,
            DRM_MODE_PAGE_FLIP_EVENT,
            NULL
        );
    }
}

static int fbdev_presenter_flush(struct presenter *presenter) {
    /// TODO: Implement
    return 0;
}

/**
 * @brief Destroy a presenter, freeing all the allocated resources.
 */
void presenter_destroy(struct presenter *presenter) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->destroy != NULL);

    return presenter->destroy(presenter);
}

static void kms_presenter_destroy(struct presenter *presenter) {
    free(presenter);
}

static void fbdev_presenter_destroy(struct presenter *presenter) {
    free(presenter);
}

/**
 * @brief Creates a new presenter that will present to this KMS device.
 */
struct presenter *kmsdev_create_presenter(struct kmsdev *dev) {
    struct kms_presenter *private;
    struct presenter *presenter;
    drmModeAtomicReq *req;
    uint32_t free_planes;

    presenter = malloc(sizeof *presenter);
    if (presenter == NULL) {
        return NULL;
    }

    private = malloc(sizeof *private);
    if (private == NULL) {
        free(presenter);
        return NULL;
    }

    req = drmModeAtomicAlloc();
    if (req == NULL) {
        free(private);
        free(presenter);
        return NULL;
    }

    for (int i = 0; i < dev->n_planes; i++) {
        if ((dev->planes[i].type == DRM_PLANE_TYPE_PRIMARY) || (dev->planes[i].type == DRM_PLANE_TYPE_OVERLAY)) {
            free_planes |= (1 << i);
        }
    }

    private->dev = dev;
    private->free_planes = free_planes;
    private->active_crtc_index = -1;
    private->active_crtc = NULL;
    private->req = req;
    private->has_primary_layer = false;
    memset(&private->primary_layer, 0, sizeof(private->primary_layer));
    memset(private->scanout_callbacks, 0, sizeof(private->scanout_callbacks));

    presenter->private = private;
    presenter->get_supported_formats = kms_presenter_get_supported_formats;
    presenter->set_active_crtc = kms_presenter_set_active_crtc;
    presenter->get_active_crtc = kms_presenter_get_active_crtc;
    presenter->set_logical_zpos = kms_presenter_set_logical_zpos;
    presenter->get_zpos = kms_presenter_get_zpos;
    presenter->push_drm_fb_layer = kms_presenter_push_drm_fb_layer;
    presenter->push_gbm_bo_layer = kms_presenter_push_gbm_bo_layer;
    presenter->push_sw_fb_layer = kms_presenter_push_sw_fb_layer;
    presenter->push_placeholder_layer = kms_presenter_push_placeholder_layer;
    presenter->flush = kms_presenter_flush;
    presenter->destroy = kms_presenter_destroy;

    
    return presenter;
}

/**
 * @brief Creates a new presenter that will present to this fbdev.
 */
struct presenter *fbdev_create_presenter(struct fbdev *dev) {
    struct fbdev_presenter *private;
    struct presenter *presenter;

    presenter = malloc(sizeof *presenter);
    if (presenter == NULL) {
        return NULL;
    }

    private = malloc(sizeof *private);
    if (private == NULL) {
        free(presenter);
        return NULL;
    }

    private->fbdev = dev;

    presenter->private = private;
    presenter->get_supported_formats = fbdev_presenter_get_supported_formats;
    presenter->set_active_crtc = fbdev_presenter_set_active_crtc;
    presenter->get_active_crtc = fbdev_presenter_get_active_crtc;
    presenter->set_logical_zpos = fbdev_presenter_set_logical_zpos;
    presenter->get_zpos = fbdev_presenter_get_zpos;
    presenter->push_drm_fb_layer = fbdev_presenter_push_drm_fb_layer;
    presenter->push_gbm_bo_layer = fbdev_presenter_push_gbm_bo_layer;
    presenter->push_sw_fb_layer = fbdev_presenter_push_sw_fb_layer;
    presenter->push_placeholder_layer = fbdev_presenter_push_placeholder_layer;
    presenter->flush = fbdev_presenter_flush;
    presenter->destroy = fbdev_presenter_destroy;
    
    return presenter;
}
