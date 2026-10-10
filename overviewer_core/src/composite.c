/* 
 * This file is part of the Minecraft Overviewer.
 *
 * Minecraft Overviewer is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * Minecraft Overviewer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with the Overviewer.  If not, see <http://www.gnu.org/licenses/>.
 */

/* 
 * This file implements a custom alpha_over function for (some) PIL
 * images. It's designed to be used through composite.py, which
 * includes a proxy alpha_over function that falls back to the default
 * PIL paste if this extension is not found.
 */

#include <math.h>

#include "overviewer.h"

typedef struct {
    PyObject_HEAD
        Imaging image;
} ImagingObject;

/* interned attribute names, created on first use */
static PyObject* core_image_attr = NULL;     /* "_im" */
static PyObject* image_property_attr = NULL; /* "im" */
/* cleared once an image without '_im' is seen (Pillow before 11) */
static int has_core_image_attr = 1;

static inline int
is_imaging_core(PyObject* im) {
    return strcmp(Py_TYPE(im)->tp_name, "ImagingCore") == 0;
}

inline Imaging
imaging_python_to_c(PyObject* obj) {
    PyObject* im = NULL;
    Imaging image;

    if (!core_image_attr) {
        core_image_attr = PyUnicode_InternFromString("_im");
        image_property_attr = PyUnicode_InternFromString("im");
        if (!core_image_attr || !image_property_attr)
            return NULL;
    }

    /* Pillow 11+ exposes the core image through an 'im' property, which runs
       Python code on every access, and this is called for every block drawn.
       Read the '_im' attribute behind the property directly, falling back to
       'im' for older Pillow versions and for closed images. */
    if (has_core_image_attr) {
        im = PyObject_GetAttr(obj, core_image_attr);
        if (!im) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                return NULL;
            PyErr_Clear();
            has_core_image_attr = 0;
        } else if (!is_imaging_core(im)) {
            Py_DECREF(im);
            im = NULL;
        }
    }

    if (!im) {
        im = PyObject_GetAttr(obj, image_property_attr);
        if (!im)
            return NULL;

        /* make sure 'im' is the right type */
        if (!is_imaging_core(im)) {
            /* it's not -- raise an error and exit */
            Py_DECREF(im);
            PyErr_SetString(PyExc_TypeError,
                            "image attribute 'im' is not a core Imaging type");
            return NULL;
        }
    }

    image = ((ImagingObject*)im)->image;
    Py_DECREF(im);
    return image;
}

/* helper function to setup s{x,y}, d{x,y}, and {x,y}size variables
   in these composite functions -- even handles auto-sizing to src! */
static inline void
setup_source_destination(Imaging src, Imaging dest,
                         int32_t* sx, int32_t* sy,
                         int32_t* dx, int32_t* dy,
                         int32_t* xsize, int32_t* ysize) {
    /* handle negative/zero sizes appropriately */
    if (*xsize <= 0 || *ysize <= 0) {
        *xsize = src->xsize;
        *ysize = src->ysize;
    }

    /* set up the source position, size and destination position */
    /* handle negative dest pos */
    if (*dx < 0) {
        *sx = -(*dx);
        *dx = 0;
    } else {
        *sx = 0;
    }

    if (*dy < 0) {
        *sy = -(*dy);
        *dy = 0;
    } else {
        *sy = 0;
    }

    /* set up source dimensions */
    *xsize -= *sx;
    *ysize -= *sy;

    /* clip dimensions, if needed */
    if (*dx + *xsize > dest->xsize)
        *xsize = dest->xsize - *dx;
    if (*dy + *ysize > dest->ysize)
        *ysize = dest->ysize - *dy;
}

/* blends one source pixel with the given alpha onto an RGBA destination pixel */
static inline void
blend_pixel(UINT8* out, const UINT8* in, UINT8 in_alpha) {
    UINT8* outmask = out + 3;
    int32_t tmp1, tmp2, tmp3;
    uint32_t i;

    /* special cases */
    if (in_alpha == 255 || (*outmask == 0 && in_alpha > 0)) {
        *outmask = in_alpha;
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
    } else if (in_alpha != 0 && *outmask == 255) {
        /* over an opaque pixel the result stays opaque, and the general case
           below reduces exactly to this, without the division */
        out[0] = OV_MULDIV255(in[0], in_alpha, tmp1) + OV_MULDIV255(out[0], 255 - in_alpha, tmp2);
        out[1] = OV_MULDIV255(in[1], in_alpha, tmp1) + OV_MULDIV255(out[1], 255 - in_alpha, tmp2);
        out[2] = OV_MULDIV255(in[2], in_alpha, tmp1) + OV_MULDIV255(out[2], 255 - in_alpha, tmp2);
    } else if (in_alpha != 0) {
        /* general case; a fully transparent source does nothing */
        int32_t alpha = in_alpha + OV_MULDIV255(*outmask, 255 - in_alpha, tmp1);
        for (i = 0; i < 3; i++) {
            out[i] = OV_MULDIV255(in[i], in_alpha, tmp1) +
                     OV_MULDIV255(OV_MULDIV255(out[i], *outmask, tmp2), 255 - in_alpha, tmp3);

            out[i] = (out[i] * 255) / alpha;
        }

        *outmask = alpha;
    }
}

/* convenience alpha_over with 1.0 as overall_alpha */
inline PyObject* alpha_over(PyObject* dest, PyObject* src, PyObject* mask,
                            int32_t dx, int32_t dy, int32_t xsize, int32_t ysize) {
    return alpha_over_full(dest, src, mask, 1.0f, dx, dy, xsize, ysize);
}

/* the full alpha_over function, in a form that can be called from C
 * overall_alpha is multiplied with the whole mask, useful for lighting...
 * if xsize, ysize are negative, they are instead set to the size of the image in src
 * returns NULL on error, dest on success. You do NOT need to decref the return!
 */
inline PyObject*
alpha_over_full(PyObject* dest, PyObject* src, PyObject* mask, float overall_alpha,
                int32_t dx, int32_t dy, int32_t xsize, int32_t ysize) {
    /* libImaging handles */
    Imaging imDest, imSrc, imMask;
    /* cached blend properties */
    int32_t src_has_alpha, mask_offset, mask_stride;
    /* source position */
    int32_t sx, sy;
    /* iteration variables */
    int32_t x, y;
    /* temporary calculation variable */
    int32_t tmp1;
    /* whether the four-pixel fast path applies, and whether its mask is
       the source image itself */
    int32_t fast_runs, mask_is_src;
    /* the alpha bytes of two RGBA pixels, in a 64-bit word of either byte order */
    static const UINT8 alpha_pattern[8] = {0, 0, 0, 255, 0, 0, 0, 255};
    uint64_t alpha_bits;
    /* integer [0, 255] version of overall_alpha */
    UINT8 overall_alpha_int = 255 * overall_alpha;

    /* short-circuit this whole thing if overall_alpha is zero */
    if (overall_alpha_int == 0)
        return dest;

    /* stop at the first failure; the next lookup can't run with an exception set */
    if (!(imDest = imaging_python_to_c(dest)) ||
        !(imSrc = imaging_python_to_c(src)) ||
        !(imMask = imaging_python_to_c(mask)))
        return NULL;

    /* check the various image modes, make sure they make sense */
    if (!OV_MODE_IS(imDest, RGBA)) {
        PyErr_SetString(PyExc_ValueError,
                        "given destination image does not have mode \"RGBA\"");
        return NULL;
    }

    if (!OV_MODE_IS(imSrc, RGBA) && !OV_MODE_IS(imSrc, RGB)) {
        PyErr_SetString(PyExc_ValueError,
                        "given source image does not have mode \"RGBA\" or \"RGB\"");
        return NULL;
    }

    if (!OV_MODE_IS(imMask, RGBA) && !OV_MODE_IS(imMask, L)) {
        PyErr_SetString(PyExc_ValueError,
                        "given mask image does not have mode \"RGBA\" or \"L\"");
        return NULL;
    }

    /* make sure mask size matches src size */
    if (imSrc->xsize != imMask->xsize || imSrc->ysize != imMask->ysize) {
        PyErr_SetString(PyExc_ValueError,
                        "mask and source image sizes do not match");
        return NULL;
    }

    /* set up flags for the src/mask type */
    src_has_alpha = (imSrc->pixelsize == 4 ? 1 : 0);
    /* how far into image the first alpha byte resides */
    mask_offset = (imMask->pixelsize == 4 ? 3 : 0);
    /* how many bytes to skip to get to the next alpha byte */
    mask_stride = imMask->pixelsize;

    /* setup source & destination vars */
    setup_source_destination(imSrc, imDest, &sx, &sy, &dx, &dy, &xsize, &ysize);

    /* check that there remains any blending to be done */
    if (xsize <= 0 || ysize <= 0) {
        /* nothing to do, return */
        return dest;
    }

    /* runs of four fully opaque or fully transparent pixels are handled
       without the per-pixel blend; that covers most of a block texture */
    fast_runs = (overall_alpha_int == 255 && src_has_alpha && imSrc != imDest);
    /* block textures are their own mask, so the alphas come with the pixels */
    mask_is_src = (fast_runs && imMask == imSrc);
    memcpy(&alpha_bits, alpha_pattern, 8);

    for (y = 0; y < ysize; y++) {
        UINT8* out = (UINT8*)imDest->image[dy + y] + dx * 4;
        UINT8* in = (UINT8*)imSrc->image[sy + y] + sx * (imSrc->pixelsize);
        UINT8* inmask = (UINT8*)imMask->image[sy + y] + sx * mask_stride + mask_offset;

        x = 0;
        if (mask_is_src) {
            for (; x + 4 <= xsize; x += 4) {
                uint64_t w0, w1;

                memcpy(&w0, in, 8);
                memcpy(&w1, in + 8, 8);
                if ((w0 & alpha_bits) == alpha_bits && (w1 & alpha_bits) == alpha_bits) {
                    /* all four opaque, and the copy brings their alpha of 255 */
                    memcpy(out, in, 16);
                } else if (((w0 | w1) & alpha_bits) != 0) {
                    blend_pixel(out, in, in[3]);
                    blend_pixel(out + 4, in + 4, in[7]);
                    blend_pixel(out + 8, in + 8, in[11]);
                    blend_pixel(out + 12, in + 12, in[15]);
                }

                out += 16;
                in += 16;
            }
            inmask += x * mask_stride;
        } else if (fast_runs) {
            for (; x + 4 <= xsize; x += 4) {
                UINT8 a0 = inmask[0];
                UINT8 a1 = inmask[mask_stride];
                UINT8 a2 = inmask[2 * mask_stride];
                UINT8 a3 = inmask[3 * mask_stride];

                if ((a0 & a1 & a2 & a3) == 255) {
                    memcpy(out, in, 16);
                    out[3] = out[7] = out[11] = out[15] = 255;
                } else if ((a0 | a1 | a2 | a3) != 0) {
                    blend_pixel(out, in, a0);
                    blend_pixel(out + 4, in + 4, a1);
                    blend_pixel(out + 8, in + 8, a2);
                    blend_pixel(out + 12, in + 12, a3);
                }

                out += 16;
                in += 16;
                inmask += 4 * mask_stride;
            }
        }

        for (; x < xsize; x++) {
            UINT8 in_alpha;

            /* apply overall_alpha */
            if (overall_alpha_int != 255 && *inmask != 0) {
                in_alpha = OV_MULDIV255(*inmask, overall_alpha_int, tmp1);
            } else {
                in_alpha = *inmask;
            }

            blend_pixel(out, in, in_alpha);

            out += 4;
            in += imSrc->pixelsize;
            inmask += mask_stride;
        }
    }

    return dest;
}

/* wraps alpha_over so it can be called directly from python */
/* properly refs the return value when needed: you DO need to decref the return */
PyObject*
alpha_over_wrap(PyObject* self, PyObject* args) {
    /* raw input python variables */
    PyObject *dest, *src, *pos = NULL, *mask = NULL;
    /* destination position and size */
    int32_t dx, dy, xsize, ysize;
    /* return value: dest image on success */
    PyObject* ret;

    if (!PyArg_ParseTuple(args, "OO|OO", &dest, &src, &pos, &mask))
        return NULL;

    if (mask == NULL)
        mask = src;

    /* destination position read */
    if (pos == NULL) {
        xsize = 0;
        ysize = 0;
        dx = 0;
        dy = 0;
    } else {
        if (!PyArg_ParseTuple(pos, "iiii", &dx, &dy, &xsize, &ysize)) {
            /* try again, but this time try to read a point */
            PyErr_Clear();
            xsize = 0;
            ysize = 0;
            if (!PyArg_ParseTuple(pos, "ii", &dx, &dy)) {
                PyErr_SetString(PyExc_TypeError,
                                "given blend destination rect is not valid");
                return NULL;
            }
        }
    }

    ret = alpha_over(dest, src, mask, dx, dy, xsize, ysize);
    if (ret == dest) {
        /* Python needs us to own our return value */
        Py_INCREF(dest);
    }
    return ret;
}

/* tints one pixel for tint_with_mask(), for a mask value m that isn't 0 */
static inline void
tint_pixel(UINT8* out, UINT8 m, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
           const bool tint_rgb, const bool tint_alpha) {
    int32_t tmp1, tmp2;

    if (m == 255) {
        if (tint_rgb) {
            out[0] = OV_MULDIV255(out[0], sr, tmp1);
            out[1] = OV_MULDIV255(out[1], sg, tmp1);
            out[2] = OV_MULDIV255(out[2], sb, tmp1);
        }
        if (tint_alpha)
            out[3] = OV_MULDIV255(out[3], sa, tmp1);
    } else {
        /* general case */
        if (tint_rgb) {
            out[0] = OV_MULDIV255(out[0], (255 - m) + OV_MULDIV255(sr, m, tmp1), tmp2);
            out[1] = OV_MULDIV255(out[1], (255 - m) + OV_MULDIV255(sg, m, tmp1), tmp2);
            out[2] = OV_MULDIV255(out[2], (255 - m) + OV_MULDIV255(sb, m, tmp1), tmp2);
        }
        if (tint_alpha)
            out[3] = OV_MULDIV255(out[3], (255 - m) + OV_MULDIV255(sa, m, tmp1), tmp2);
    }
}

/* tints one row of pixels for tint_with_mask(). tint_rgb and tint_alpha are
 * constants at each call (and so is sa where it is 0), so the compiler builds
 * a separate loop for each case without the per-pixel tests; clearing alpha
 * under a fully opaque mask becomes a plain store of 0.
 */
static inline void
tint_row(UINT8* out, const UINT8* inmask, int32_t mask_stride, int32_t xsize,
         uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
         const bool tint_rgb, const bool tint_alpha) {
    int32_t x = 0;

    /* most of a face or block mask is runs of fully transparent or fully
       opaque pixels, so look at four mask pixels at a time */
    for (; x + 4 <= xsize; x += 4, out += 16, inmask += 4 * mask_stride) {
        UINT8 m0 = inmask[0];
        UINT8 m1 = inmask[mask_stride];
        UINT8 m2 = inmask[2 * mask_stride];
        UINT8 m3 = inmask[3 * mask_stride];

        if ((m0 | m1 | m2 | m3) == 0)
            continue;
        if ((m0 & m1 & m2 & m3) == 255) {
            tint_pixel(out, 255, sr, sg, sb, sa, tint_rgb, tint_alpha);
            tint_pixel(out + 4, 255, sr, sg, sb, sa, tint_rgb, tint_alpha);
            tint_pixel(out + 8, 255, sr, sg, sb, sa, tint_rgb, tint_alpha);
            tint_pixel(out + 12, 255, sr, sg, sb, sa, tint_rgb, tint_alpha);
            continue;
        }
        if (m0)
            tint_pixel(out, m0, sr, sg, sb, sa, tint_rgb, tint_alpha);
        if (m1)
            tint_pixel(out + 4, m1, sr, sg, sb, sa, tint_rgb, tint_alpha);
        if (m2)
            tint_pixel(out + 8, m2, sr, sg, sb, sa, tint_rgb, tint_alpha);
        if (m3)
            tint_pixel(out + 12, m3, sr, sg, sb, sa, tint_rgb, tint_alpha);
    }

    for (; x < xsize; x++, out += 4, inmask += mask_stride) {
        if (*inmask)
            tint_pixel(out, *inmask, sr, sg, sb, sa, tint_rgb, tint_alpha);
    }
}

/* like alpha_over, but instead of src image it takes a source color
 * also, it multiplies instead of doing an over operation
 */
PyObject*
tint_with_mask(PyObject* dest,
               uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa,
               PyObject* mask,
               int32_t dx, int32_t dy,
               int32_t xsize, int32_t ysize) {
    /* libImaging handles */
    Imaging imDest, imMask;
    /* cached blend properties */
    int32_t mask_offset, mask_stride;
    /* source position */
    int32_t sx, sy;
    /* iteration variables */
    int32_t y;
    /* Multiplying by 255 leaves a channel unchanged, whatever the mask: the
       factor (255 - m) + m is 255. Callers clearing alpha pass 255 for the
       colour, and colour tints pass 255 for alpha. */
    bool tint_rgb = (sr != 255 || sg != 255 || sb != 255);
    bool tint_alpha = (sa != 255);

    if (!(imDest = imaging_python_to_c(dest)) ||
        !(imMask = imaging_python_to_c(mask)))
        return NULL;

    /* check the various image modes, make sure they make sense */
    if (!OV_MODE_IS(imDest, RGBA)) {
        PyErr_SetString(PyExc_ValueError,
                        "given destination image does not have mode \"RGBA\"");
        return NULL;
    }

    if (!OV_MODE_IS(imMask, RGBA) && !OV_MODE_IS(imMask, L)) {
        PyErr_SetString(PyExc_ValueError,
                        "given mask image does not have mode \"RGBA\" or \"L\"");
        return NULL;
    }

    /* how far into image the first alpha byte resides */
    mask_offset = (imMask->pixelsize == 4 ? 3 : 0);
    /* how many bytes to skip to get to the next alpha byte */
    mask_stride = imMask->pixelsize;

    /* setup source & destination vars */
    setup_source_destination(imMask, imDest, &sx, &sy, &dx, &dy, &xsize, &ysize);

    /* check that there remains any blending to be done */
    if (xsize <= 0 || ysize <= 0 || (!tint_rgb && !tint_alpha)) {
        /* nothing to do, return */
        return dest;
    }

    for (y = 0; y < ysize; y++) {
        UINT8* out = (UINT8*)imDest->image[dy + y] + dx * 4;
        UINT8* inmask = (UINT8*)imMask->image[sy + y] + sx * mask_stride + mask_offset;

        if (tint_rgb && tint_alpha)
            tint_row(out, inmask, mask_stride, xsize, sr, sg, sb, sa, true, true);
        else if (tint_rgb)
            tint_row(out, inmask, mask_stride, xsize, sr, sg, sb, sa, true, false);
        else if (sa == 0)
            /* clearing alpha (clear-base) */
            tint_row(out, inmask, mask_stride, xsize, 255, 255, 255, 0, false, true);
        else
            tint_row(out, inmask, mask_stride, xsize, sr, sg, sb, sa, false, true);
    }

    return dest;
}

/* tint_with_mask(dest, (r, g, b, a), mask, (dx, dy)), for tests */
PyObject*
tint_with_mask_wrap(PyObject* self, PyObject* args) {
    PyObject *dest, *mask;
    int32_t sr, sg, sb, sa, dx, dy;
    PyObject* ret;

    if (!PyArg_ParseTuple(args, "O(iiii)O(ii)", &dest, &sr, &sg, &sb, &sa, &mask, &dx, &dy))
        return NULL;

    ret = tint_with_mask(dest, sr, sg, sb, sa, mask, dx, dy, 0, 0);
    if (ret == dest) {
        /* Python needs us to own our return value */
        Py_INCREF(dest);
    }
    return ret;
}

/* draw_triangle(dest, inclusive, (x0, y0, r0, g0, b0), (x1, ...), (x2, ...),
 *               (tux, tuy), [touchup x, touchup y, ...]), for tests */
PyObject*
draw_triangle_wrap(PyObject* self, PyObject* args) {
    PyObject *dest, *touchup_list, *ret;
    int32_t inclusive, x0, y0, r0, g0, b0, x1, y1, r1, g1, b1, x2, y2, r2, g2, b2, tux, tuy;
    int32_t touchups[64];
    Py_ssize_t i, num_touchups;

    if (!PyArg_ParseTuple(args, "Oi(iiiii)(iiiii)(iiiii)(ii)O!", &dest, &inclusive,
                          &x0, &y0, &r0, &g0, &b0, &x1, &y1, &r1, &g1, &b1,
                          &x2, &y2, &r2, &g2, &b2, &tux, &tuy, &PyList_Type, &touchup_list))
        return NULL;
    num_touchups = PyList_GET_SIZE(touchup_list);
    if (num_touchups > 64 || num_touchups % 2) {
        PyErr_SetString(PyExc_ValueError, "touchups must be up to 32 x, y pairs");
        return NULL;
    }
    for (i = 0; i < num_touchups; i++)
        touchups[i] = PyLong_AsLong(PyList_GET_ITEM(touchup_list, i));

    ret = draw_triangle(dest, inclusive, x0, y0, r0, g0, b0, x1, y1, r1, g1, b1,
                        x2, y2, r2, g2, b2, tux, tuy, touchups, num_touchups / 2);
    if (ret == dest) {
        /* Python needs us to own our return value */
        Py_INCREF(dest);
    }
    return ret;
}

/* draws a triangle on the destination image, multiplicatively!
 * used for smooth lighting
 * (excuse the ridiculous number of parameters!)
 *
 * Algorithm adapted from _Fundamentals_of_Computer_Graphics_
 * by Peter Shirley, Michael Ashikhmin
 * (or at least, the version poorly reproduced here:
 *  http://www.gidforums.com/t-20838.html )
 */
PyObject*
draw_triangle(PyObject* dest, int32_t inclusive,
              int32_t x0, int32_t y0,
              uint8_t r0, uint8_t g0, uint8_t b0,
              int32_t x1, int32_t y1,
              uint8_t r1, uint8_t g1, uint8_t b1,
              int32_t x2, int32_t y2,
              uint8_t r2, uint8_t g2, uint8_t b2,
              int32_t tux, int32_t tuy,
              int32_t* touchups, uint32_t num_touchups) {

    /* destination image */
    Imaging imDest;
    /* ranges of pixels that are affected */
    int32_t xmin, xmax, ymin, ymax;
    /* constant coefficients for alpha, beta, gamma */
    int32_t a12, a20, a01;
    int32_t b12, b20, b01;
    int32_t c12, c20, c01;
    /* constant normalizers for alpha, beta, gamma */
    float alpha_norm, beta_norm, gamma_norm;
    /* temporary variables */
    int32_t tmp;
    /* iteration variables */
    int32_t x, y;

    imDest = imaging_python_to_c(dest);
    if (!imDest)
        return NULL;

    /* check the various image modes, make sure they make sense */
    if (!OV_MODE_IS(imDest, RGBA)) {
        PyErr_SetString(PyExc_ValueError,
                        "given destination image does not have mode \"RGBA\"");
        return NULL;
    }

    /* set up draw ranges */
    xmin = OV_MIN(x0, OV_MIN(x1, x2));
    ymin = OV_MIN(y0, OV_MIN(y1, y2));
    xmax = OV_MAX(x0, OV_MAX(x1, x2)) + 1;
    ymax = OV_MAX(y0, OV_MAX(y1, y2)) + 1;

    xmin = OV_MAX(xmin, 0);
    ymin = OV_MAX(ymin, 0);
    xmax = OV_MIN(xmax, imDest->xsize);
    ymax = OV_MIN(ymax, imDest->ysize);

    /* setup coefficients */
    a12 = y1 - y2;
    b12 = x2 - x1;
    c12 = (x1 * y2) - (x2 * y1);
    a20 = y2 - y0;
    b20 = x0 - x2;
    c20 = (x2 * y0) - (x0 * y2);
    a01 = y0 - y1;
    b01 = x1 - x0;
    c01 = (x0 * y1) - (x1 * y0);

    /* setup normalizers */
    alpha_norm = 1.0f / ((a12 * x0) + (b12 * y0) + c12);
    beta_norm = 1.0f / ((a20 * x1) + (b20 * y1) + c20);
    gamma_norm = 1.0f / ((a01 * x2) + (b01 * y2) + c01);

    /* Which pixels are inside depends only on the signs of the edge values
       (a * x + b * y + c), which are integers: alpha >= 0 when the edge
       value has the sign of its normalizer, or is 0. Step the edge values
       along each row in integers, and only work out the colour of pixels
       inside. A degenerate triangle has an infinite normalizer, so it keeps
       the floating point test. */
    if (isfinite(alpha_norm) && isfinite(beta_norm) && isfinite(gamma_norm)) {
        int32_t s12 = alpha_norm < 0 ? -1 : 1;
        int32_t s20 = beta_norm < 0 ? -1 : 1;
        int32_t s01 = gamma_norm < 0 ? -1 : 1;
        /* inside needs every signed edge value >= 0, or > 0 if not inclusive */
        int32_t threshold = inclusive ? 0 : 1;

        for (y = ymin; y < ymax; y++) {
            UINT8* out = (UINT8*)imDest->image[y] + xmin * 4;
            int32_t e12 = (a12 * xmin) + (b12 * y) + c12;
            int32_t e20 = (a20 * xmin) + (b20 * y) + c20;
            int32_t e01 = (a01 * xmin) + (b01 * y) + c01;
            bool entered = false;

            for (x = xmin; x < xmax; x++, out += 4, e12 += a12, e20 += a20, e01 += a01) {
                float alpha, beta, gamma;
                uint32_t r, g, b;

                if (e12 * s12 < threshold || e20 * s20 < threshold || e01 * s01 < threshold) {
                    /* a triangle is convex, so once a row leaves it, it's done */
                    if (entered)
                        break;
                    continue;
                }
                entered = true;

                alpha = alpha_norm * e12;
                beta = beta_norm * e20;
                gamma = gamma_norm * e01;
                r = alpha * r0 + beta * r1 + gamma * r2;
                g = alpha * g0 + beta * g1 + gamma * g2;
                b = alpha * b0 + beta * b1 + gamma * b2;

                out[0] = OV_MULDIV255(out[0], r, tmp);
                out[1] = OV_MULDIV255(out[1], g, tmp);
                out[2] = OV_MULDIV255(out[2], b, tmp);
                /* keep alpha the same */
            }
        }
    } else {
        /* iterate over the destination rect */
        for (y = ymin; y < ymax; y++) {
            UINT8* out = (UINT8*)imDest->image[y] + xmin * 4;

            for (x = xmin; x < xmax; x++) {
                float alpha, beta, gamma;
                alpha = alpha_norm * ((a12 * x) + (b12 * y) + c12);
                beta = beta_norm * ((a20 * x) + (b20 * y) + c20);
                gamma = gamma_norm * ((a01 * x) + (b01 * y) + c01);

                if (alpha >= 0 && beta >= 0 && gamma >= 0 &&
                    (inclusive || (alpha * beta * gamma > 0))) {
                    uint32_t r = alpha * r0 + beta * r1 + gamma * r2;
                    uint32_t g = alpha * g0 + beta * g1 + gamma * g2;
                    uint32_t b = alpha * b0 + beta * b1 + gamma * b2;

                    *out = OV_MULDIV255(*out, r, tmp);
                    out++;
                    *out = OV_MULDIV255(*out, g, tmp);
                    out++;
                    *out = OV_MULDIV255(*out, b, tmp);
                    out++;

                    /* keep alpha the same */
                    out++;
                } else {
                    /* skip */
                    out += 4;
                }
            }
        }
    }

    while (num_touchups > 0) {
        float alpha, beta, gamma;
        uint32_t r, g, b;
        UINT8* out;

        x = touchups[0] + tux;
        y = touchups[1] + tuy;
        touchups += 2;
        num_touchups--;

        if (x < 0 || x >= imDest->xsize || y < 0 || y >= imDest->ysize)
            continue;

        out = (UINT8*)imDest->image[y] + x * 4;

        alpha = alpha_norm * ((a12 * x) + (b12 * y) + c12);
        beta = beta_norm * ((a20 * x) + (b20 * y) + c20);
        gamma = gamma_norm * ((a01 * x) + (b01 * y) + c01);

        r = alpha * r0 + beta * r1 + gamma * r2;
        g = alpha * g0 + beta * g1 + gamma * g2;
        b = alpha * b0 + beta * b1 + gamma * b2;

        *out = OV_MULDIV255(*out, r, tmp);
        out++;
        *out = OV_MULDIV255(*out, g, tmp);
        out++;
        *out = OV_MULDIV255(*out, b, tmp);
        out++;
    }

    return dest;
}

/* scales the image to half size
 */
inline PyObject*
resize_half(PyObject* dest, PyObject* src) {
    /* libImaging handles */
    Imaging imDest, imSrc;
    /* alpha properties */
    int32_t src_has_alpha, dest_has_alpha;
    /* iteration variables */
    uint32_t x, y;
    /* temp color variables */
    uint32_t r, g, b, a;
    /* size values for source and destination */
    uint32_t src_width, src_height, dest_width, dest_height;

    if (!(imDest = imaging_python_to_c(dest)) ||
        !(imSrc = imaging_python_to_c(src)))
        return NULL;

    /* check the various image modes, make sure they make sense */
    if (!OV_MODE_IS(imDest, RGBA)) {
        PyErr_SetString(PyExc_ValueError,
                        "given destination image does not have mode \"RGBA\"");
        return NULL;
    }

    if (!OV_MODE_IS(imSrc, RGBA) && !OV_MODE_IS(imSrc, RGB)) {
        PyErr_SetString(PyExc_ValueError,
                        "given source image does not have mode \"RGBA\" or \"RGB\"");
        return NULL;
    }

    src_width = imSrc->xsize;
    src_height = imSrc->ysize;
    dest_width = imDest->xsize;
    dest_height = imDest->ysize;

    /* make sure destination size is 1/2 src size */
    if (src_width / 2 != dest_width || src_height / 2 != dest_height) {
        PyErr_SetString(PyExc_ValueError,
                        "destination image size is not one-half source image size");
        return NULL;
    }

    /* set up flags for the src/mask type */
    src_has_alpha = (imSrc->pixelsize == 4 ? 1 : 0);
    dest_has_alpha = (imDest->pixelsize == 4 ? 1 : 0);

    /* check that there remains anything to resize */
    if (dest_width <= 0 || dest_height <= 0) {
        /* nothing to do, return */
        return dest;
    }

    /* set to fully opaque if source has no alpha channel */
    if (!src_has_alpha)
        a = 0xFF << 2;

    for (y = 0; y < dest_height; y++) {

        UINT8* out = (UINT8*)imDest->image[y];
        UINT8* in_row1 = (UINT8*)imSrc->image[y * 2];
        UINT8* in_row2 = (UINT8*)imSrc->image[y * 2 + 1];

        for (x = 0; x < dest_width; x++) {

            // read first column
            r = *in_row1;
            r += *in_row2;
            in_row1++;
            in_row2++;
            g = *in_row1;
            g += *in_row2;
            in_row1++;
            in_row2++;
            b = *in_row1;
            b += *in_row2;
            in_row1++;
            in_row2++;

            if (src_has_alpha) {
                a = *in_row1;
                a += *in_row2;
                in_row1++;
                in_row2++;
            }

            // read second column
            r += *in_row1;
            r += *in_row2;
            in_row1++;
            in_row2++;
            g += *in_row1;
            g += *in_row2;
            in_row1++;
            in_row2++;
            b += *in_row1;
            b += *in_row2;
            in_row1++;
            in_row2++;

            if (src_has_alpha) {
                a += *in_row1;
                a += *in_row2;
                in_row1++;
                in_row2++;
            }

            // write blended color
            *out = (UINT8)(r >> 2);
            out++;
            *out = (UINT8)(g >> 2);
            out++;
            *out = (UINT8)(b >> 2);
            out++;

            if (dest_has_alpha) {
                *out = (UINT8)(a >> 2);
                out++;
            }
        }
    }

    return dest;
}

/* wraps resize_half so it can be called directly from python */
PyObject*
resize_half_wrap(PyObject* self, PyObject* args) {
    /* raw input python variables */
    PyObject *dest, *src;
    /* return value: dest image on success */
    PyObject* ret;

    if (!PyArg_ParseTuple(args, "OO", &dest, &src))
        return NULL;

    ret = resize_half(dest, src);
    if (ret == dest) {
        /* Python needs us to own our return value */
        Py_INCREF(dest);
    }
    return ret;
}
