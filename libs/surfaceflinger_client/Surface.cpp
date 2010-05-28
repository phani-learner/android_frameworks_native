/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Surface"

#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <utils/Errors.h>
#include <utils/threads.h>
#include <utils/CallStack.h>
#include <utils/Log.h>

#include <binder/IPCThreadState.h>
#include <binder/IMemory.h>

#include <ui/DisplayInfo.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include <surfaceflinger/Surface.h>
#include <surfaceflinger/ISurface.h>
#include <surfaceflinger/ISurfaceComposer.h>
#include <surfaceflinger/SurfaceComposerClient.h>

#include <private/surfaceflinger/SharedBufferStack.h>
#include <private/surfaceflinger/LayerState.h>

namespace android {

// ----------------------------------------------------------------------

static status_t copyBlt(
        const sp<GraphicBuffer>& dst, 
        const sp<GraphicBuffer>& src, 
        const Region& reg)
{
    // src and dst with, height and format must be identical. no verification
    // is done here.
    status_t err;
    uint8_t const * src_bits = NULL;
    err = src->lock(GRALLOC_USAGE_SW_READ_OFTEN, reg.bounds(), (void**)&src_bits);
    LOGE_IF(err, "error locking src buffer %s", strerror(-err));

    uint8_t* dst_bits = NULL;
    err = dst->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, reg.bounds(), (void**)&dst_bits);
    LOGE_IF(err, "error locking dst buffer %s", strerror(-err));

    Region::const_iterator head(reg.begin());
    Region::const_iterator tail(reg.end());
    if (head != tail && src_bits && dst_bits) {
        const size_t bpp = bytesPerPixel(src->format);
        const size_t dbpr = dst->stride * bpp;
        const size_t sbpr = src->stride * bpp;

        while (head != tail) {
            const Rect& r(*head++);
            ssize_t h = r.height();
            if (h <= 0) continue;
            size_t size = r.width() * bpp;
            uint8_t const * s = src_bits + (r.left + src->stride * r.top) * bpp;
            uint8_t       * d = dst_bits + (r.left + dst->stride * r.top) * bpp;
            if (dbpr==sbpr && size==sbpr) {
                size *= h;
                h = 1;
            }
            do {
                memcpy(d, s, size);
                d += dbpr;
                s += sbpr;
            } while (--h > 0);
        }
    }
    
    if (src_bits)
        src->unlock();
    
    if (dst_bits)
        dst->unlock();
    
    return err;
}

// ============================================================================
//  SurfaceControl
// ============================================================================

SurfaceControl::SurfaceControl(
        const sp<SurfaceComposerClient>& client, 
        const sp<ISurface>& surface,
        const ISurfaceFlingerClient::surface_data_t& data,
        uint32_t w, uint32_t h, PixelFormat format, uint32_t flags)
    : mClient(client), mSurface(surface),
      mToken(data.token), mIdentity(data.identity),
      mWidth(data.width), mHeight(data.height), mFormat(data.format),
      mFlags(flags)
{
}
        
SurfaceControl::~SurfaceControl()
{
    destroy();
}

void SurfaceControl::destroy()
{
    if (isValid()) {
        mClient->destroySurface(mToken);
    }

    // clear all references and trigger an IPC now, to make sure things
    // happen without delay, since these resources are quite heavy.
    mClient.clear();
    mSurface.clear();
    IPCThreadState::self()->flushCommands();
}

void SurfaceControl::clear() 
{
    // here, the window manager tells us explicitly that we should destroy
    // the surface's resource. Soon after this call, it will also release
    // its last reference (which will call the dtor); however, it is possible
    // that a client living in the same process still holds references which
    // would delay the call to the dtor -- that is why we need this explicit
    // "clear()" call.
    destroy();
}

bool SurfaceControl::isSameSurface(
        const sp<SurfaceControl>& lhs, const sp<SurfaceControl>& rhs) 
{
    if (lhs == 0 || rhs == 0)
        return false;
    return lhs->mSurface->asBinder() == rhs->mSurface->asBinder();
}

status_t SurfaceControl::setLayer(int32_t layer) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setLayer(mToken, layer);
}
status_t SurfaceControl::setPosition(int32_t x, int32_t y) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setPosition(mToken, x, y);
}
status_t SurfaceControl::setSize(uint32_t w, uint32_t h) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setSize(mToken, w, h);
}
status_t SurfaceControl::hide() {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->hide(mToken);
}
status_t SurfaceControl::show(int32_t layer) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->show(mToken, layer);
}
status_t SurfaceControl::freeze() {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->freeze(mToken);
}
status_t SurfaceControl::unfreeze() {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->unfreeze(mToken);
}
status_t SurfaceControl::setFlags(uint32_t flags, uint32_t mask) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setFlags(mToken, flags, mask);
}
status_t SurfaceControl::setTransparentRegionHint(const Region& transparent) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setTransparentRegionHint(mToken, transparent);
}
status_t SurfaceControl::setAlpha(float alpha) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setAlpha(mToken, alpha);
}
status_t SurfaceControl::setMatrix(float dsdx, float dtdx, float dsdy, float dtdy) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setMatrix(mToken, dsdx, dtdx, dsdy, dtdy);
}
status_t SurfaceControl::setFreezeTint(uint32_t tint) {
    status_t err = validate();
    if (err < 0) return err;
    const sp<SurfaceComposerClient>& client(mClient);
    return client->setFreezeTint(mToken, tint);
}

status_t SurfaceControl::validate() const
{
    if (mToken<0 || mClient==0) {
        LOGE("invalid token (%d, identity=%u) or client (%p)", 
                mToken, mIdentity, mClient.get());
        return NO_INIT;
    }
    return NO_ERROR;
}

status_t SurfaceControl::writeSurfaceToParcel(
        const sp<SurfaceControl>& control, Parcel* parcel)
{
    uint32_t flags = 0;
    uint32_t format = 0;
    SurfaceID token = -1;
    uint32_t identity = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    sp<SurfaceComposerClient> client;
    sp<ISurface> sur;
    if (SurfaceControl::isValid(control)) {
        token    = control->mToken;
        identity = control->mIdentity;
        client   = control->mClient;
        sur      = control->mSurface;
        width    = control->mWidth;
        height   = control->mHeight;
        format   = control->mFormat;
        flags    = control->mFlags;
    }
    parcel->writeStrongBinder(client!=0  ? client->connection() : NULL);
    parcel->writeStrongBinder(sur!=0     ? sur->asBinder()      : NULL);
    parcel->writeInt32(token);
    parcel->writeInt32(identity);
    parcel->writeInt32(width);
    parcel->writeInt32(height);
    parcel->writeInt32(format);
    parcel->writeInt32(flags);
    return NO_ERROR;
}

sp<Surface> SurfaceControl::getSurface() const
{
    Mutex::Autolock _l(mLock);
    if (mSurfaceData == 0) {
        mSurfaceData = new Surface(const_cast<SurfaceControl*>(this));
    }
    return mSurfaceData;
}

// ============================================================================
//  Surface
// ============================================================================


Surface::Surface(const sp<SurfaceControl>& surface)
    : mSurface(surface->mSurface),
      mToken(surface->mToken), mIdentity(surface->mIdentity),
      mFormat(surface->mFormat), mFlags(surface->mFlags),
      mBufferMapper(GraphicBufferMapper::get()), mSharedBufferClient(NULL),
      mInitCheck(NO_INIT),
      mWidth(surface->mWidth), mHeight(surface->mHeight)
{
    mClient = new SurfaceClient(surface->mClient);
    init();
}

Surface::Surface(const Parcel& parcel)
    :  mBufferMapper(GraphicBufferMapper::get()),
       mSharedBufferClient(NULL), mInitCheck(NO_INIT)
{
    sp<IBinder> conn = parcel.readStrongBinder();
    mSurface    = interface_cast<ISurface>(parcel.readStrongBinder());
    mToken      = parcel.readInt32();
    mIdentity   = parcel.readInt32();
    mWidth      = parcel.readInt32();
    mHeight     = parcel.readInt32();
    mFormat     = parcel.readInt32();
    mFlags      = parcel.readInt32();
    mClient = new SurfaceClient(conn);
    init();
}

void Surface::init()
{
    android_native_window_t::setSwapInterval  = setSwapInterval;
    android_native_window_t::dequeueBuffer    = dequeueBuffer;
    android_native_window_t::lockBuffer       = lockBuffer;
    android_native_window_t::queueBuffer      = queueBuffer;
    android_native_window_t::query            = query;
    android_native_window_t::perform          = perform;

    DisplayInfo dinfo;
    SurfaceComposerClient::getDisplayInfo(0, &dinfo);
    const_cast<float&>(android_native_window_t::xdpi) = dinfo.xdpi;
    const_cast<float&>(android_native_window_t::ydpi) = dinfo.ydpi;
    // FIXME: set real values here
    const_cast<int&>(android_native_window_t::minSwapInterval) = 1;
    const_cast<int&>(android_native_window_t::maxSwapInterval) = 1;
    const_cast<uint32_t&>(android_native_window_t::flags) = 0;

    mConnected = 0;
    mSwapRectangle.makeInvalid();
    // two buffers by default
    mBuffers.setCapacity(2);
    mBuffers.insertAt(0, 2);

    if (mClient != 0 && mClient->initCheck() == NO_ERROR) {
        mSharedBufferClient = new SharedBufferClient(
                mClient->getSharedClient(), mToken, 2, mIdentity);
    }

    mInitCheck = initCheck();
}

Surface::~Surface()
{
    // this is a client-side operation, the surface is destroyed, unmap
    // its buffers in this process.
    size_t size = mBuffers.size();
    for (size_t i=0 ; i<size ; i++) {
        if (mBuffers[i] != 0 && mBuffers[i]->handle != 0) {
            getBufferMapper().unregisterBuffer(mBuffers[i]->handle);
        }
    }

    // clear all references and trigger an IPC now, to make sure things
    // happen without delay, since these resources are quite heavy.
    mBuffers.clear();
    mClient.clear();
    mSurface.clear();
    delete mSharedBufferClient;
    IPCThreadState::self()->flushCommands();
}

status_t Surface::initCheck() const
{
    if (mToken<0 || mClient==0 || mClient->initCheck() != NO_ERROR) {
        return NO_INIT;
    }
    SharedClient const* cblk = mClient->getSharedClient();
    if (cblk == 0) {
        LOGE("cblk is null (surface id=%d, identity=%u)", mToken, mIdentity);
        return NO_INIT;
    }
    return NO_ERROR;
}

bool Surface::isValid() {
    return mInitCheck == NO_ERROR;
}

status_t Surface::validate() const
{
    // check that we initialized ourself properly
    if (mInitCheck != NO_ERROR) {
        LOGE("invalid token (%d, identity=%u) or client (%p)",
                mToken, mIdentity, mClient.get());
        return mInitCheck;
    }

    // verify the identity of this surface
    SharedClient const* cblk = mClient->getSharedClient();

    uint32_t identity = cblk->getIdentity(mToken);

    // this is a bit of a (temporary) special case, identity==0 means that
    // no operation are allowed from the client (eg: dequeue/queue), this
    // is used with PUSH_BUFFER surfaces for instance
    if (identity == 0) {
        LOGE("[Surface] invalid operation (identity=%u)", mIdentity);
        return INVALID_OPERATION;
    }

    if (mIdentity != identity) {
        LOGE("[Surface] using an invalid surface id=%d, "
                "identity=%u should be %d",
                mToken, mIdentity, identity);
        return NO_INIT;
    }

    // check the surface didn't become invalid
    status_t err = cblk->validate(mToken);
    if (err != NO_ERROR) {
        LOGE("surface (id=%d, identity=%u) is invalid, err=%d (%s)",
                mToken, mIdentity, err, strerror(-err));
        return err;
    }

    return NO_ERROR;
}

bool Surface::isSameSurface(const sp<Surface>& lhs, const sp<Surface>& rhs) {
    if (lhs == 0 || rhs == 0)
        return false;
    return lhs->mSurface->asBinder() == rhs->mSurface->asBinder();
}

sp<ISurface> Surface::getISurface() const {
    return mSurface;
}

// ----------------------------------------------------------------------------

int Surface::setSwapInterval(android_native_window_t* window, int interval) {
    return 0;
}

int Surface::dequeueBuffer(android_native_window_t* window, 
        android_native_buffer_t** buffer) {
    Surface* self = getSelf(window);
    return self->dequeueBuffer(buffer);
}

int Surface::lockBuffer(android_native_window_t* window, 
        android_native_buffer_t* buffer) {
    Surface* self = getSelf(window);
    return self->lockBuffer(buffer);
}

int Surface::queueBuffer(android_native_window_t* window, 
        android_native_buffer_t* buffer) {
    Surface* self = getSelf(window);
    return self->queueBuffer(buffer);
}

int Surface::query(android_native_window_t* window, 
        int what, int* value) {
    Surface* self = getSelf(window);
    return self->query(what, value);
}

int Surface::perform(android_native_window_t* window, 
        int operation, ...) {
    va_list args;
    va_start(args, operation);
    Surface* self = getSelf(window);
    int res = self->perform(operation, args);
    va_end(args);
    return res;
}

// ----------------------------------------------------------------------------

bool Surface::needNewBuffer(int bufIdx,
        uint32_t *pWidth, uint32_t *pHeight,
        uint32_t *pFormat, uint32_t *pUsage) const
{
    Mutex::Autolock _l(mSurfaceLock);

    // Always call needNewBuffer(), since it clears the needed buffers flags
    bool needNewBuffer = mSharedBufferClient->needNewBuffer(bufIdx);
    bool validBuffer = mBufferInfo.validateBuffer(mBuffers[bufIdx]);
    bool newNeewBuffer = needNewBuffer || !validBuffer;
    if (newNeewBuffer) {
        mBufferInfo.get(pWidth, pHeight, pFormat, pUsage);
    }
    return newNeewBuffer;
}

int Surface::dequeueBuffer(android_native_buffer_t** buffer)
{
    status_t err = validate();
    if (err != NO_ERROR)
        return err;

    ssize_t bufIdx = mSharedBufferClient->dequeue();
    if (bufIdx < 0) {
        LOGE("error dequeuing a buffer (%s)", strerror(bufIdx));
        return bufIdx;
    }

    // grow the buffer array if needed
    const size_t size = mBuffers.size();
    const size_t needed = bufIdx+1;
    if (size < needed) {
        mBuffers.insertAt(size, needed-size);
    }

    uint32_t w, h, format, usage;
    if (needNewBuffer(bufIdx, &w, &h, &format, &usage)) {
        err = getBufferLocked(bufIdx, w, h, format, usage);
        LOGE_IF(err, "getBufferLocked(%ld, %u, %u, %u, %08x) failed (%s)",
                bufIdx, w, h, format, usage, strerror(-err));
        if (err == NO_ERROR) {
            // reset the width/height with the what we get from the buffer
            const sp<GraphicBuffer>& backBuffer(mBuffers[bufIdx]);
            mWidth  = uint32_t(backBuffer->width);
            mHeight = uint32_t(backBuffer->height);
        }
    }

    // if we still don't have a buffer here, we probably ran out of memory
    const sp<GraphicBuffer>& backBuffer(mBuffers[bufIdx]);
    if (!err && backBuffer==0) {
        err = NO_MEMORY;
    }

    if (err == NO_ERROR) {
        mDirtyRegion.set(backBuffer->width, backBuffer->height);
        *buffer = backBuffer.get();
    } else {
        mSharedBufferClient->undoDequeue(bufIdx);
    }

    return err;
}

int Surface::lockBuffer(android_native_buffer_t* buffer)
{
    status_t err = validate();
    if (err != NO_ERROR)
        return err;

    int32_t bufIdx = getBufferIndex(GraphicBuffer::getSelf(buffer));
    err = mSharedBufferClient->lock(bufIdx);
    LOGE_IF(err, "error locking buffer %d (%s)", bufIdx, strerror(-err));
    return err;
}

int Surface::queueBuffer(android_native_buffer_t* buffer)
{   
    status_t err = validate();
    if (err != NO_ERROR)
        return err;

    if (mSwapRectangle.isValid()) {
        mDirtyRegion.set(mSwapRectangle);
    }
    
    int32_t bufIdx = getBufferIndex(GraphicBuffer::getSelf(buffer));
    mSharedBufferClient->setCrop(bufIdx, mNextBufferCrop);
    mSharedBufferClient->setDirtyRegion(bufIdx, mDirtyRegion);
    err = mSharedBufferClient->queue(bufIdx);
    LOGE_IF(err, "error queuing buffer %d (%s)", bufIdx, strerror(-err));

    if (err == NO_ERROR) {
        // FIXME: can we avoid this IPC if we know there is one pending?
        mClient->signalServer();
    }
    return err;
}

int Surface::query(int what, int* value)
{
    switch (what) {
    case NATIVE_WINDOW_WIDTH:
        *value = int(mWidth);
        return NO_ERROR;
    case NATIVE_WINDOW_HEIGHT:
        *value = int(mHeight);
        return NO_ERROR;
    case NATIVE_WINDOW_FORMAT:
        *value = int(mFormat);
        return NO_ERROR;
    }
    return BAD_VALUE;
}

int Surface::perform(int operation, va_list args)
{
    status_t err = validate();
    if (err != NO_ERROR)
        return err;

    int res = NO_ERROR;
    switch (operation) {
    case NATIVE_WINDOW_SET_USAGE:
        dispatch_setUsage( args );
        break;
    case NATIVE_WINDOW_CONNECT:
        res = dispatch_connect( args );
        break;
    case NATIVE_WINDOW_DISCONNECT:
        res = dispatch_disconnect( args );
        break;
    case NATIVE_WINDOW_SET_CROP:
        res = dispatch_crop( args );
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        res = dispatch_set_buffer_count( args );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        res = dispatch_set_buffers_geometry( args );
        break;
    default:
        res = NAME_NOT_FOUND;
        break;
    }
    return res;
}

void Surface::dispatch_setUsage(va_list args) {
    int usage = va_arg(args, int);
    setUsage( usage );
}
int Surface::dispatch_connect(va_list args) {
    int api = va_arg(args, int);
    return connect( api );
}
int Surface::dispatch_disconnect(va_list args) {
    int api = va_arg(args, int);
    return disconnect( api );
}
int Surface::dispatch_crop(va_list args) {
    android_native_rect_t const* rect = va_arg(args, android_native_rect_t*);
    return crop( reinterpret_cast<Rect const*>(rect) );
}
int Surface::dispatch_set_buffer_count(va_list args) {
    size_t bufferCount = va_arg(args, size_t);
    return setBufferCount(bufferCount);
}
int Surface::dispatch_set_buffers_geometry(va_list args) {
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    int f = va_arg(args, int);
    return setBuffersGeometry(w, h, f);
}

void Surface::setUsage(uint32_t reqUsage)
{
    Mutex::Autolock _l(mSurfaceLock);
    mBufferInfo.set(reqUsage);
}

int Surface::connect(int api)
{
    Mutex::Autolock _l(mSurfaceLock);
    int err = NO_ERROR;
    switch (api) {
        case NATIVE_WINDOW_API_EGL:
            if (mConnected) {
                err = -EINVAL;
            } else {
                mConnected = api;
            }
            break;
        default:
            err = -EINVAL;
            break;
    }
    return err;
}

int Surface::disconnect(int api)
{
    Mutex::Autolock _l(mSurfaceLock);
    int err = NO_ERROR;
    switch (api) {
        case NATIVE_WINDOW_API_EGL:
            if (mConnected == api) {
                mConnected = 0;
            } else {
                err = -EINVAL;
            }
            break;
        default:
            err = -EINVAL;
            break;
    }
    return err;
}

int Surface::crop(Rect const* rect)
{
    Mutex::Autolock _l(mSurfaceLock);
    // TODO: validate rect size
    mNextBufferCrop = *rect;
    return NO_ERROR;
}

int Surface::setBufferCount(int bufferCount)
{
    sp<ISurface> s(mSurface);
    if (s == 0) return NO_INIT;

    class SetBufferCountIPC : public SharedBufferClient::SetBufferCountCallback {
        sp<ISurface> surface;
        virtual status_t operator()(int bufferCount) const {
            return surface->setBufferCount(bufferCount);
        }
    public:
        SetBufferCountIPC(const sp<ISurface>& surface) : surface(surface) { }
    } ipc(s);

    status_t err = mSharedBufferClient->setBufferCount(bufferCount, ipc);
    LOGE_IF(err, "ISurface::setBufferCount(%d) returned %s",
            bufferCount, strerror(-err));
    return err;
}

int Surface::setBuffersGeometry(int w, int h, int format)
{
    if (w<0 || h<0 || format<0)
        return BAD_VALUE;

    if ((w && !h) || (!w && h))
        return BAD_VALUE;

    Mutex::Autolock _l(mSurfaceLock);
    mBufferInfo.set(w, h, format);
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

int Surface::getConnectedApi() const
{
    Mutex::Autolock _l(mSurfaceLock);
    return mConnected;
}

// ----------------------------------------------------------------------------

status_t Surface::lock(SurfaceInfo* info, bool blocking) {
    return Surface::lock(info, NULL, blocking);
}

status_t Surface::lock(SurfaceInfo* other, Region* dirtyIn, bool blocking) 
{
    if (getConnectedApi()) {
        LOGE("Surface::lock(%p) failed. Already connected to another API",
                (android_native_window_t*)this);
        CallStack stack;
        stack.update();
        stack.dump("");
        return INVALID_OPERATION;
    }

    if (mApiLock.tryLock() != NO_ERROR) {
        LOGE("calling Surface::lock from different threads!");
        CallStack stack;
        stack.update();
        stack.dump("");
        return WOULD_BLOCK;
    }

    /* Here we're holding mApiLock */
    
    if (mLockedBuffer != 0) {
        LOGE("Surface::lock failed, already locked");
        mApiLock.unlock();
        return INVALID_OPERATION;
    }

    // we're intending to do software rendering from this point
    setUsage(GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);

    android_native_buffer_t* out;
    status_t err = dequeueBuffer(&out);
    LOGE_IF(err, "dequeueBuffer failed (%s)", strerror(-err));
    if (err == NO_ERROR) {
        sp<GraphicBuffer> backBuffer(GraphicBuffer::getSelf(out));
        err = lockBuffer(backBuffer.get());
        LOGE_IF(err, "lockBuffer (idx=%d) failed (%s)",
                getBufferIndex(backBuffer), strerror(-err));
        if (err == NO_ERROR) {
            const Rect bounds(backBuffer->width, backBuffer->height);
            const Region boundsRegion(bounds);
            Region scratch(boundsRegion);
            Region& newDirtyRegion(dirtyIn ? *dirtyIn : scratch);
            newDirtyRegion &= boundsRegion;

            // figure out if we can copy the frontbuffer back
            const sp<GraphicBuffer>& frontBuffer(mPostedBuffer);
            const bool canCopyBack = (frontBuffer != 0 &&
                    backBuffer->width  == frontBuffer->width &&
                    backBuffer->height == frontBuffer->height &&
                    backBuffer->format == frontBuffer->format &&
                    !(mFlags & ISurfaceComposer::eDestroyBackbuffer));

            // the dirty region we report to surfaceflinger is the one
            // given by the user (as opposed to the one *we* return to the
            // user).
            mDirtyRegion = newDirtyRegion;

            if (canCopyBack) {
                // copy the area that is invalid and not repainted this round
                const Region copyback(mOldDirtyRegion.subtract(newDirtyRegion));
                if (!copyback.isEmpty())
                    copyBlt(backBuffer, frontBuffer, copyback);
            } else {
                // if we can't copy-back anything, modify the user's dirty
                // region to make sure they redraw the whole buffer
                newDirtyRegion = boundsRegion;
            }

            // keep track of the are of the buffer that is "clean"
            // (ie: that will be redrawn)
            mOldDirtyRegion = newDirtyRegion;

            void* vaddr;
            status_t res = backBuffer->lock(
                    GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                    newDirtyRegion.bounds(), &vaddr);
            
            LOGW_IF(res, "failed locking buffer (handle = %p)", 
                    backBuffer->handle);

            mLockedBuffer = backBuffer;
            other->w      = backBuffer->width;
            other->h      = backBuffer->height;
            other->s      = backBuffer->stride;
            other->usage  = backBuffer->usage;
            other->format = backBuffer->format;
            other->bits   = vaddr;
        }
    }
    mApiLock.unlock();
    return err;
}
    
status_t Surface::unlockAndPost() 
{
    if (mLockedBuffer == 0) {
        LOGE("Surface::unlockAndPost failed, no locked buffer");
        return INVALID_OPERATION;
    }

    status_t err = mLockedBuffer->unlock();
    LOGE_IF(err, "failed unlocking buffer (%p)", mLockedBuffer->handle);
    
    err = queueBuffer(mLockedBuffer.get());
    LOGE_IF(err, "queueBuffer (idx=%d) failed (%s)",
            getBufferIndex(mLockedBuffer), strerror(-err));

    mPostedBuffer = mLockedBuffer;
    mLockedBuffer = 0;
    return err;
}

void Surface::setSwapRectangle(const Rect& r) {
    Mutex::Autolock _l(mSurfaceLock);
    mSwapRectangle = r;
}

int Surface::getBufferIndex(const sp<GraphicBuffer>& buffer) const
{
    return buffer->getIndex();
}

status_t Surface::getBufferLocked(int index,
        uint32_t w, uint32_t h, uint32_t format, uint32_t usage)
{
    sp<ISurface> s(mSurface);
    if (s == 0) return NO_INIT;

    status_t err = NO_MEMORY;

    // free the current buffer
    sp<GraphicBuffer>& currentBuffer(mBuffers.editItemAt(index));
    if (currentBuffer != 0) {
        getBufferMapper().unregisterBuffer(currentBuffer->handle);
        currentBuffer.clear();
    }

    sp<GraphicBuffer> buffer = s->requestBuffer(index, w, h, format, usage);
    LOGE_IF(buffer==0,
            "ISurface::getBuffer(%d, %08x) returned NULL",
            index, usage);
    if (buffer != 0) { // this should never happen by construction
        LOGE_IF(buffer->handle == NULL, 
                "Surface (identity=%d) requestBuffer(%d, %u, %u, %u, %08x) "
                "returned a buffer with a null handle",
                mIdentity, index, w, h, format, usage);
        err = mSharedBufferClient->getStatus();
        LOGE_IF(err,  "Surface (identity=%d) state = %d", mIdentity, err);
        if (!err && buffer->handle != NULL) {
            err = getBufferMapper().registerBuffer(buffer->handle);
            LOGW_IF(err, "registerBuffer(...) failed %d (%s)",
                    err, strerror(-err));
            if (err == NO_ERROR) {
                currentBuffer = buffer;
                currentBuffer->setIndex(index);
            }
        } else {
            err = err<0 ? err : status_t(NO_MEMORY);
        }
    }
    return err; 
}

// ----------------------------------------------------------------------------
Surface::BufferInfo::BufferInfo()
    : mWidth(0), mHeight(0), mFormat(0),
      mUsage(GRALLOC_USAGE_HW_RENDER), mDirty(0)
{
}

void Surface::BufferInfo::set(uint32_t w, uint32_t h, uint32_t format) {
    if ((mWidth != w) || (mHeight != h) || (mFormat != format)) {
        mWidth = w;
        mHeight = h;
        mFormat = format;
        mDirty |= GEOMETRY;
    }
}

void Surface::BufferInfo::set(uint32_t usage) {
    mUsage = usage;
}

void Surface::BufferInfo::get(uint32_t *pWidth, uint32_t *pHeight,
        uint32_t *pFormat, uint32_t *pUsage) const {
    *pWidth  = mWidth;
    *pHeight = mHeight;
    *pFormat = mFormat;
    *pUsage  = mUsage;
}

bool Surface::BufferInfo::validateBuffer(const sp<GraphicBuffer>& buffer) const {
    // make sure we AT LEAST have the usage flags we want
    if (mDirty || buffer==0 ||
            ((buffer->usage & mUsage) != mUsage)) {
        mDirty = 0;
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
}; // namespace android

