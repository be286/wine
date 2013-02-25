/*
 * MACDRV mouse driver
 *
 * Copyright 1998 Ulrich Weigand
 * Copyright 2007 Henri Verbeet
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include "macdrv.h"
#define OEMRESOURCE
#include "winuser.h"
#include "winreg.h"
#include "wine/server.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(cursor);


static CRITICAL_SECTION cursor_cache_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &cursor_cache_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": cursor_cache_section") }
};
static CRITICAL_SECTION cursor_cache_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static CFMutableDictionaryRef cursor_cache;


/***********************************************************************
 *              send_mouse_input
 *
 * Update the various window states on a mouse event.
 */
static void send_mouse_input(HWND hwnd, UINT flags, int x, int y,
                             DWORD mouse_data, unsigned long time)
{
    INPUT input;
    HWND top_level_hwnd;

    top_level_hwnd = GetAncestor(hwnd, GA_ROOT);

    if ((flags & MOUSEEVENTF_MOVE) && (flags & MOUSEEVENTF_ABSOLUTE))
    {
        RECT rect;

        /* update the wine server Z-order */
        SetRect(&rect, x, y, x + 1, y + 1);
        MapWindowPoints(0, top_level_hwnd, (POINT *)&rect, 2);

        SERVER_START_REQ(update_window_zorder)
        {
            req->window      = wine_server_user_handle(top_level_hwnd);
            req->rect.left   = rect.left;
            req->rect.top    = rect.top;
            req->rect.right  = rect.right;
            req->rect.bottom = rect.bottom;
            wine_server_call(req);
        }
        SERVER_END_REQ;
    }

    input.type              = INPUT_MOUSE;
    input.mi.dx             = x;
    input.mi.dy             = y;
    input.mi.mouseData      = mouse_data;
    input.mi.dwFlags        = flags;
    input.mi.time           = time;
    input.mi.dwExtraInfo    = 0;

    __wine_send_input(top_level_hwnd, &input);
}


/***********************************************************************
 *              release_provider_cfdata
 *
 * Helper for create_monochrome_cursor.  A CFData is used by two
 * different CGDataProviders, using different offsets.  One of them is
 * constructed with a pointer to the bytes, not a reference to the
 * CFData object (because of the offset).  So, the CFData is CFRetain'ed
 * on its behalf at creation and released here.
 */
void release_provider_cfdata(void *info, const void *data, size_t size)
{
    CFRelease(info);
}


/***********************************************************************
 *              create_monochrome_cursor
 */
CFArrayRef create_monochrome_cursor(HDC hdc, const ICONINFOEXW *icon, int width, int height)
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    unsigned int width_bytes = (width + 31) / 32 * 4;
    CGColorSpaceRef colorspace;
    CFMutableDataRef data;
    CGDataProviderRef provider;
    CGImageRef cgimage, cgmask, cgmasked;
    CGPoint hot_spot;
    CFDictionaryRef hot_spot_dict;
    const CFStringRef keys[] = { CFSTR("image"), CFSTR("hotSpot") };
    CFTypeRef values[sizeof(keys) / sizeof(keys[0])];
    CFDictionaryRef frame;
    CFArrayRef frames;

    TRACE("hdc %p icon->hbmMask %p icon->xHotspot %d icon->yHotspot %d width %d height %d\n",
          hdc, icon->hbmMask, icon->xHotspot, icon->yHotspot, width, height);

    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -height * 2;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 1;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = width_bytes * height * 2;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biClrImportant = 0;

    /* This will be owned by the data provider for the mask image and released
       when it is destroyed. */
    data = CFDataCreateMutable(NULL, info->bmiHeader.biSizeImage);
    if (!data)
    {
        WARN("failed to create data\n");
        return NULL;
    }
    CFDataSetLength(data, info->bmiHeader.biSizeImage);

    if (!GetDIBits(hdc, icon->hbmMask, 0, height * 2, CFDataGetMutableBytePtr(data), info, DIB_RGB_COLORS))
    {
        WARN("GetDIBits failed\n");
        CFRelease(data);
        return NULL;
    }

    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceGenericGray);
    if (!colorspace)
    {
        WARN("failed to create colorspace\n");
        CFRelease(data);
        return NULL;
    }

    /* The data object needs to live as long as this provider, so retain it an
       extra time and have the provider's data-release callback release it. */
    provider = CGDataProviderCreateWithData(data, CFDataGetBytePtr(data) + width_bytes * height,
                                            width_bytes * height, release_provider_cfdata);
    if (!provider)
    {
        WARN("failed to create data provider\n");
        CGColorSpaceRelease(colorspace);
        CFRelease(data);
        return NULL;
    }
    CFRetain(data);

    cgimage = CGImageCreate(width, height, 1, 1, width_bytes, colorspace,
                            kCGImageAlphaNone | kCGBitmapByteOrderDefault,
                            provider, NULL, FALSE, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorspace);
    if (!cgimage)
    {
        WARN("failed to create image\n");
        CFRelease(data);
        return NULL;
    }

    provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
    {
        WARN("failed to create data provider\n");
        CGImageRelease(cgimage);
        return NULL;
    }

    cgmask = CGImageMaskCreate(width, height, 1, 1, width_bytes, provider, NULL, FALSE);
    CGDataProviderRelease(provider);
    if (!cgmask)
    {
        WARN("failed to create mask image\n");
        CGImageRelease(cgimage);
        return NULL;
    }

    cgmasked = CGImageCreateWithMask(cgimage, cgmask);
    CGImageRelease(cgimage);
    CGImageRelease(cgmask);
    if (!cgmasked)
    {
        WARN("failed to create masked image\n");
        return NULL;
    }

    hot_spot = CGPointMake(icon->xHotspot, icon->yHotspot);
    hot_spot_dict = CGPointCreateDictionaryRepresentation(hot_spot);
    if (!hot_spot_dict)
    {
        WARN("failed to create hot spot dictionary\n");
        CGImageRelease(cgmasked);
        return NULL;
    }

    values[0] = cgmasked;
    values[1] = hot_spot_dict;
    frame = CFDictionaryCreate(NULL, (const void**)keys, values, sizeof(keys) / sizeof(keys[0]),
                               &kCFCopyStringDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(hot_spot_dict);
    CGImageRelease(cgmasked);
    if (!frame)
    {
        WARN("failed to create frame dictionary\n");
        return NULL;
    }

    frames = CFArrayCreate(NULL, (const void**)&frame, 1, &kCFTypeArrayCallBacks);
    CFRelease(frame);
    if (!frames)
    {
        WARN("failed to create frames array\n");
        return NULL;
    }

    return frames;
}


/***********************************************************************
 *              create_cgimage_from_icon
 */
static CGImageRef create_cgimage_from_icon(HDC hdc, HANDLE icon, HBITMAP hbmColor,
                                           unsigned char *color_bits, int color_size, HBITMAP hbmMask,
                                           unsigned char *mask_bits, int mask_size, int width,
                                           int height, int istep)
{
    int i, has_alpha = FALSE;
    DWORD *ptr;
    CGBitmapInfo alpha_format;
    CGColorSpaceRef colorspace;
    CFDataRef data;
    CGDataProviderRef provider;
    CGImageRef cgimage;

    /* draw the cursor frame to a temporary buffer then create a CGImage from that */
    memset(color_bits, 0x00, color_size);
    SelectObject(hdc, hbmColor);
    if (!DrawIconEx(hdc, 0, 0, icon, width, height, istep, NULL, DI_NORMAL))
    {
        WARN("Could not draw frame %d (walk past end of frames).\n", istep);
        return NULL;
    }

    /* check if the cursor frame was drawn with an alpha channel */
    for (i = 0, ptr = (DWORD*)color_bits; i < width * height; i++, ptr++)
        if ((has_alpha = (*ptr & 0xff000000) != 0)) break;

    if (has_alpha)
        alpha_format = kCGImageAlphaFirst;
    else
        alpha_format = kCGImageAlphaNoneSkipFirst;

    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
    if (!colorspace)
    {
        WARN("failed to create colorspace\n");
        return NULL;
    }

    data = CFDataCreate(NULL, (UInt8*)color_bits, color_size);
    if (!data)
    {
        WARN("failed to create data\n");
        CGColorSpaceRelease(colorspace);
        return NULL;
    }

    provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
    {
        WARN("failed to create data provider\n");
        CGColorSpaceRelease(colorspace);
        return NULL;
    }

    cgimage = CGImageCreate(width, height, 8, 32, width * 4, colorspace,
                            alpha_format | kCGBitmapByteOrder32Little,
                            provider, NULL, FALSE, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorspace);
    if (!cgimage)
    {
        WARN("failed to create image\n");
        return NULL;
    }

    /* if no alpha channel was drawn then generate it from the mask */
    if (!has_alpha)
    {
        unsigned int width_bytes = (width + 31) / 32 * 4;
        CGImageRef cgmask, temp;

        /* draw the cursor mask to a temporary buffer */
        memset(mask_bits, 0xFF, mask_size);
        SelectObject(hdc, hbmMask);
        if (!DrawIconEx(hdc, 0, 0, icon, width, height, istep, NULL, DI_MASK))
        {
            WARN("Failed to draw frame mask %d.\n", istep);
            CGImageRelease(cgimage);
            return NULL;
        }

        data = CFDataCreate(NULL, (UInt8*)mask_bits, mask_size);
        if (!data)
        {
            WARN("failed to create data\n");
            CGImageRelease(cgimage);
            return NULL;
        }

        provider = CGDataProviderCreateWithCFData(data);
        CFRelease(data);
        if (!provider)
        {
            WARN("failed to create data provider\n");
            CGImageRelease(cgimage);
            return NULL;
        }

        cgmask = CGImageMaskCreate(width, height, 1, 1, width_bytes, provider, NULL, FALSE);
        CGDataProviderRelease(provider);
        if (!cgmask)
        {
            WARN("failed to create mask\n");
            CGImageRelease(cgimage);
            return NULL;
        }

        temp = CGImageCreateWithMask(cgimage, cgmask);
        CGImageRelease(cgmask);
        CGImageRelease(cgimage);
        if (!temp)
        {
            WARN("failed to create masked image\n");
            return NULL;
        }
        cgimage = temp;
    }

    return cgimage;
}


/***********************************************************************
 *              create_cursor_frame
 *
 * Create a frame dictionary for a cursor from a Windows icon.
 * Keys:
 *      "image"     a CGImage for the frame
 *      "duration"  a CFNumber for the frame duration in seconds
 *      "hotSpot"   a CFDictionary encoding a CGPoint for the hot spot
 */
static CFDictionaryRef create_cursor_frame(HDC hdc, const ICONINFOEXW *iinfo, HANDLE icon,
                                           HBITMAP hbmColor, unsigned char *color_bits, int color_size,
                                           HBITMAP hbmMask, unsigned char *mask_bits, int mask_size,
                                           int width, int height, int istep)
{
    DWORD delay_jiffies, num_steps;
    CFMutableDictionaryRef frame;
    CGPoint hot_spot;
    CFDictionaryRef hot_spot_dict;
    double duration;
    CFNumberRef duration_number;
    CGImageRef cgimage;

    TRACE("hdc %p iinfo->xHotspot %d iinfo->yHotspot %d icon %p hbmColor %p color_bits %p color_size %d"
          " hbmMask %p mask_bits %p mask_size %d width %d height %d istep %d\n",
          hdc, iinfo->xHotspot, iinfo->yHotspot, icon, hbmColor, color_bits, color_size,
          hbmMask, mask_bits, mask_size, width, height, istep);

    frame = CFDictionaryCreateMutable(NULL, 0, &kCFCopyStringDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    if (!frame)
    {
        WARN("failed to allocate dictionary for frame\n");
        return NULL;
    }

    hot_spot = CGPointMake(iinfo->xHotspot, iinfo->yHotspot);
    hot_spot_dict = CGPointCreateDictionaryRepresentation(hot_spot);
    if (!hot_spot_dict)
    {
        WARN("failed to create hot spot dictionary\n");
        CFRelease(frame);
        return NULL;
    }
    CFDictionarySetValue(frame, CFSTR("hotSpot"), hot_spot_dict);
    CFRelease(hot_spot_dict);

    if (GetCursorFrameInfo(icon, 0x0 /* unknown parameter */, istep, &delay_jiffies, &num_steps) != 0)
        duration = delay_jiffies / 60.0; /* convert jiffies (1/60s) to seconds */
    else
    {
        WARN("Failed to retrieve animated cursor frame-rate for frame %d.\n", istep);
        duration = 0.1; /* fallback delay, 100 ms */
    }
    duration_number = CFNumberCreate(NULL, kCFNumberDoubleType, &duration);
    if (!duration_number)
    {
        WARN("failed to create duration number\n");
        CFRelease(frame);
        return NULL;
    }
    CFDictionarySetValue(frame, CFSTR("duration"), duration_number);
    CFRelease(duration_number);

    cgimage = create_cgimage_from_icon(hdc, icon, hbmColor, color_bits, color_size,
                                       hbmMask, mask_bits, mask_size, width, height, istep);
    if (!cgimage)
    {
        CFRelease(frame);
        return NULL;
    }

    CFDictionarySetValue(frame, CFSTR("image"), cgimage);
    CGImageRelease(cgimage);

    return frame;
}


/***********************************************************************
 *              create_color_cursor
 *
 * Create an array of color cursor frames from a Windows cursor.  Each
 * frame is represented in the array by a dictionary.
 * Frame dictionary keys:
 *      "image"     a CGImage for the frame
 *      "duration"  a CFNumber for the frame duration in seconds
 *      "hotSpot"   a CFDictionary encoding a CGPoint for the hot spot
 */
static CFArrayRef create_color_cursor(HDC hdc, const ICONINFOEXW *iinfo, HANDLE icon, int width, int height)
{
    unsigned char *color_bits, *mask_bits;
    HBITMAP hbmColor = 0, hbmMask = 0;
    DWORD nFrames, delay_jiffies, i;
    int color_size, mask_size;
    BITMAPINFO *info = NULL;
    CFMutableArrayRef frames;

    TRACE("hdc %p iinfo %p icon %p width %d height %d\n", hdc, iinfo, icon, width, height);

    /* Retrieve the number of frames to render */
    if (!GetCursorFrameInfo(icon, 0x0 /* unknown parameter */, 0, &delay_jiffies, &nFrames))
    {
        WARN("GetCursorFrameInfo failed\n");
        return NULL;
    }
    if (!(frames = CFArrayCreateMutable(NULL, nFrames, &kCFTypeArrayCallBacks)))
    {
        WARN("failed to allocate frames array\n");
        return NULL;
    }

    /* Allocate all of the resources necessary to obtain a cursor frame */
    if (!(info = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(BITMAPINFO, bmiColors[256])))) goto cleanup;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biClrImportant = 0;
    info->bmiHeader.biBitCount = 32;
    color_size = width * height * 4;
    info->bmiHeader.biSizeImage = color_size;
    hbmColor = CreateDIBSection(hdc, info, DIB_RGB_COLORS, (VOID **) &color_bits, NULL, 0);
    if (!hbmColor)
    {
        WARN("failed to create DIB section for cursor color data\n");
        goto cleanup;
    }
    info->bmiHeader.biBitCount = 1;
    info->bmiColors[0].rgbRed      = 0;
    info->bmiColors[0].rgbGreen    = 0;
    info->bmiColors[0].rgbBlue     = 0;
    info->bmiColors[0].rgbReserved = 0;
    info->bmiColors[1].rgbRed      = 0xff;
    info->bmiColors[1].rgbGreen    = 0xff;
    info->bmiColors[1].rgbBlue     = 0xff;
    info->bmiColors[1].rgbReserved = 0;

    mask_size = ((width + 31) / 32 * 4) * height; /* width_bytes * height */
    info->bmiHeader.biSizeImage = mask_size;
    hbmMask = CreateDIBSection(hdc, info, DIB_RGB_COLORS, (VOID **) &mask_bits, NULL, 0);
    if (!hbmMask)
    {
        WARN("failed to create DIB section for cursor mask data\n");
        goto cleanup;
    }

    /* Create a CFDictionary for each frame of the cursor */
    for (i = 0; i < nFrames; i++)
    {
        CFDictionaryRef frame = create_cursor_frame(hdc, iinfo, icon,
                                                    hbmColor, color_bits, color_size,
                                                    hbmMask, mask_bits, mask_size,
                                                    width, height, i);
        if (!frame) goto cleanup;
        CFArrayAppendValue(frames, frame);
        CFRelease(frame);
    }

cleanup:
    if (CFArrayGetCount(frames) < nFrames)
    {
        CFRelease(frames);
        frames = NULL;
    }
    else
        TRACE("returning cursor with %d frames\n", nFrames);
    /* Cleanup all of the resources used to obtain the frame data */
    if (hbmColor) DeleteObject(hbmColor);
    if (hbmMask) DeleteObject(hbmMask);
    HeapFree(GetProcessHeap(), 0, info);
    return frames;
}


/***********************************************************************
 *              DestroyCursorIcon (MACDRV.@)
 */
void CDECL macdrv_DestroyCursorIcon(HCURSOR cursor)
{
    TRACE("cursor %p\n", cursor);

    EnterCriticalSection(&cursor_cache_section);
    if (cursor_cache)
        CFDictionaryRemoveValue(cursor_cache, cursor);
    LeaveCriticalSection(&cursor_cache_section);
}


/***********************************************************************
 *              SetCursor (MACDRV.@)
 */
void CDECL macdrv_SetCursor(HCURSOR cursor)
{
    CFStringRef cursor_name = NULL;
    CFArrayRef cursor_frames = NULL;

    TRACE("%p\n", cursor);

    if (cursor)
    {
        ICONINFOEXW info;
        BITMAP bm;
        HDC hdc;

        EnterCriticalSection(&cursor_cache_section);
        if (cursor_cache)
        {
            CFTypeRef cached_cursor = CFDictionaryGetValue(cursor_cache, cursor);
            if (cached_cursor)
            {
                if (CFGetTypeID(cached_cursor) == CFStringGetTypeID())
                    cursor_name = CFRetain(cached_cursor);
                else
                    cursor_frames = CFRetain(cached_cursor);
            }
        }
        LeaveCriticalSection(&cursor_cache_section);
        if (cursor_name || cursor_frames)
            goto done;

        info.cbSize = sizeof(info);
        if (!GetIconInfoExW(cursor, &info))
        {
            WARN("GetIconInfoExW failed\n");
            return;
        }

        GetObjectW(info.hbmMask, sizeof(bm), &bm);
        if (!info.hbmColor) bm.bmHeight = max(1, bm.bmHeight / 2);

        /* make sure hotspot is valid */
        if (info.xHotspot >= bm.bmWidth || info.yHotspot >= bm.bmHeight)
        {
            info.xHotspot = bm.bmWidth / 2;
            info.yHotspot = bm.bmHeight / 2;
        }

        hdc = CreateCompatibleDC(0);

        if (info.hbmColor)
        {
            cursor_frames = create_color_cursor(hdc, &info, cursor, bm.bmWidth, bm.bmHeight);
            DeleteObject(info.hbmColor);
        }
        else
            cursor_frames = create_monochrome_cursor(hdc, &info, bm.bmWidth, bm.bmHeight);

        DeleteObject(info.hbmMask);
        DeleteDC(hdc);

        if (cursor_name || cursor_frames)
        {
            EnterCriticalSection(&cursor_cache_section);
            if (!cursor_cache)
                cursor_cache = CFDictionaryCreateMutable(NULL, 0, NULL,
                                                         &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(cursor_cache, cursor,
                                 cursor_name ? (CFTypeRef)cursor_name : (CFTypeRef)cursor_frames);
            LeaveCriticalSection(&cursor_cache_section);
        }
        else
            cursor_name = CFRetain(CFSTR("arrowCursor"));
    }

done:
    TRACE("setting cursor with cursor_name %s cursor_frames %p\n", debugstr_cf(cursor_name), cursor_frames);
    macdrv_set_cursor(cursor_name, cursor_frames);
    if (cursor_name) CFRelease(cursor_name);
    if (cursor_frames) CFRelease(cursor_frames);
}


/***********************************************************************
 *              macdrv_mouse_button
 *
 * Handler for MOUSE_BUTTON events.
 */
void macdrv_mouse_button(HWND hwnd, const macdrv_event *event)
{
    UINT flags = 0;
    WORD data = 0;

    TRACE("win %p button %d %s at (%d,%d) time %lu (%lu ticks ago)\n", hwnd, event->mouse_button.button,
          (event->mouse_button.pressed ? "pressed" : "released"),
          event->mouse_button.x, event->mouse_button.y,
          event->mouse_button.time_ms, (GetTickCount() - event->mouse_button.time_ms));

    if (event->mouse_button.pressed)
    {
        switch (event->mouse_button.button)
        {
        case 0: flags |= MOUSEEVENTF_LEFTDOWN; break;
        case 1: flags |= MOUSEEVENTF_RIGHTDOWN; break;
        case 2: flags |= MOUSEEVENTF_MIDDLEDOWN; break;
        default:
            flags |= MOUSEEVENTF_XDOWN;
            data = 1 << (event->mouse_button.button - 3);
            break;
        }
    }
    else
    {
        switch (event->mouse_button.button)
        {
        case 0: flags |= MOUSEEVENTF_LEFTUP; break;
        case 1: flags |= MOUSEEVENTF_RIGHTUP; break;
        case 2: flags |= MOUSEEVENTF_MIDDLEUP; break;
        default:
            flags |= MOUSEEVENTF_XUP;
            data = 1 << (event->mouse_button.button - 3);
            break;
        }
    }

    send_mouse_input(hwnd, flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE,
                     event->mouse_button.x, event->mouse_button.y,
                     data, event->mouse_button.time_ms);
}


/***********************************************************************
 *              macdrv_mouse_moved
 *
 * Handler for MOUSE_MOVED and MOUSE_MOVED_ABSOLUTE events.
 */
void macdrv_mouse_moved(HWND hwnd, const macdrv_event *event)
{
    UINT flags = MOUSEEVENTF_MOVE;

    TRACE("win %p/%p %s (%d,%d) time %lu (%lu ticks ago)\n", hwnd, event->window,
          (event->type == MOUSE_MOVED) ? "relative" : "absolute",
          event->mouse_moved.x, event->mouse_moved.y,
          event->mouse_moved.time_ms, (GetTickCount() - event->mouse_moved.time_ms));

    if (event->type == MOUSE_MOVED_ABSOLUTE)
        flags |= MOUSEEVENTF_ABSOLUTE;

    send_mouse_input(hwnd, flags, event->mouse_moved.x, event->mouse_moved.y,
                     0, event->mouse_moved.time_ms);
}


/***********************************************************************
 *              macdrv_mouse_scroll
 *
 * Handler for MOUSE_SCROLL events.
 */
void macdrv_mouse_scroll(HWND hwnd, const macdrv_event *event)
{
    TRACE("win %p/%p scroll (%d,%d) at (%d,%d) time %lu (%lu ticks ago)\n", hwnd,
          event->window, event->mouse_scroll.x_scroll, event->mouse_scroll.y_scroll,
          event->mouse_scroll.x, event->mouse_scroll.y,
          event->mouse_scroll.time_ms, (GetTickCount() - event->mouse_scroll.time_ms));

    send_mouse_input(hwnd, MOUSEEVENTF_WHEEL | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE,
                     event->mouse_scroll.x, event->mouse_scroll.y,
                     event->mouse_scroll.y_scroll, event->mouse_scroll.time_ms);
    send_mouse_input(hwnd, MOUSEEVENTF_HWHEEL | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE,
                     event->mouse_scroll.x, event->mouse_scroll.y,
                     event->mouse_scroll.x_scroll, event->mouse_scroll.time_ms);
}
