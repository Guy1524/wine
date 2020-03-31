#include "config.h"

#include <gst/gst.h>

#include "gst_private.h"
#include "gst_cbs.h"

#include <assert.h>
#include <stdarg.h>

#define COBJMACROS
#define NONAMELESSUNION

#include "mfapi.h"
#include "mferror.h"
#include "mfidl.h"
#include "mfobjects.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

static struct source_desc
{
    GstStaticCaps bytestream_caps;
} source_descs[] =
{
    {/*SOURCE_TYPE_MPEG_4*/
        GST_STATIC_CAPS("video/quicktime"),
    }
};

struct media_stream
{
    IMFMediaStream IMFMediaStream_iface;
    LONG ref;
    struct media_source *parent_source;
    IMFMediaEventQueue *event_queue;
    IMFStreamDescriptor *descriptor;
    GstElement *appsink;
    GstPad *their_src, *my_sink;
    /* usually reflects state of source */
    enum
    {
        STREAM_INACTIVE,
        STREAM_ENABLED,
        STREAM_PAUSED,
        STREAM_RUNNING,
        STREAM_SHUTDOWN,
    } state;
    BOOL eos;
};

enum source_async_op
{
    SOURCE_ASYNC_START,
};

struct source_async_command
{
    IUnknown IUnknown_iface;
    LONG refcount;
    enum source_async_op op;
    union
    {
        struct
        {
            IMFPresentationDescriptor *descriptor;
            GUID format;
            PROPVARIANT position;
        } start;
    } u;
};

struct media_source
{
    IMFMediaSource IMFMediaSource_iface;
    IMFAsyncCallback async_commands_callback;
    LONG ref;
    DWORD async_commands_queue;
    enum source_type type;
    IMFMediaEventQueue *event_queue;
    IMFByteStream *byte_stream;
    struct media_stream **streams;
    ULONG stream_count;
    IMFPresentationDescriptor *pres_desc;
    GstBus *bus;
    GstElement *container;
    GstElement *demuxer;
    GstPad *my_src, *their_sink;
    enum
    {
        SOURCE_OPENING,
        SOURCE_STOPPED, /* (READY) */
        SOURCE_PAUSED,
        SOURCE_RUNNING,
        SOURCE_SHUTDOWN,
    } state;
    HANDLE all_streams_event;
};

static inline struct media_stream *impl_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct media_stream, IMFMediaStream_iface);
}

static inline struct media_source *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFMediaSource_iface);
}

static inline struct media_source *impl_from_async_commands_callback_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, async_commands_callback);
}

static inline struct source_async_command *impl_from_async_command_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct source_async_command, IUnknown_iface);
}

static HRESULT WINAPI source_async_command_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI source_async_command_AddRef(IUnknown *iface)
{
    struct source_async_command *command = impl_from_async_command_IUnknown(iface);
    return InterlockedIncrement(&command->refcount);
}

static ULONG WINAPI source_async_command_Release(IUnknown *iface)
{
    struct source_async_command *command = impl_from_async_command_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&command->refcount);

    if (!refcount)
    {
        if (command->op == SOURCE_ASYNC_START)
            PropVariantClear(&command->u.start.position);
        heap_free(command);
    }

    return refcount;
}

static const IUnknownVtbl source_async_command_vtbl =
{
    source_async_command_QueryInterface,
    source_async_command_AddRef,
    source_async_command_Release,
};

static HRESULT source_create_async_op(enum source_async_op op, struct source_async_command **ret)
{
    struct source_async_command *command;

    if (!(command = heap_alloc_zero(sizeof(*command))))
        return E_OUTOFMEMORY;

    command->IUnknown_iface.lpVtbl = &source_async_command_vtbl;
    command->op = op;

    *ret = command;

    return S_OK;
}

static HRESULT WINAPI callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI callback_GetParameters(IMFAsyncCallback *iface,
        DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static ULONG WINAPI source_async_commands_callback_AddRef(IMFAsyncCallback *iface)
{
    struct media_source *source = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI source_async_commands_callback_Release(IMFAsyncCallback *iface)
{
    struct media_source *source = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static IMFStreamDescriptor *stream_descriptor_from_id(IMFPresentationDescriptor *pres_desc, DWORD id, BOOL *selected)
{
    ULONG sd_count;
    IMFStreamDescriptor *ret;

    if (FAILED(IMFPresentationDescriptor_GetStreamDescriptorCount(pres_desc, &sd_count)))
        return NULL;

    for (unsigned int i = 0; i < sd_count; i++)
    {
        DWORD stream_id;

        if (FAILED(IMFPresentationDescriptor_GetStreamDescriptorByIndex(pres_desc, i, selected, &ret)))
            return NULL;

        if (SUCCEEDED(IMFStreamDescriptor_GetStreamIdentifier(ret, &stream_id)) && stream_id == id)
            return ret;

        IMFStreamDescriptor_Release(ret);
    }
    return NULL;
}

static HRESULT start_pipeline(struct media_source *source, struct source_async_command *command)
{
    PROPVARIANT *position = &command->u.start.position;
    BOOL seek_message = source->state != SOURCE_STOPPED && position->vt != VT_EMPTY;

    /* we can't seek until the pipeline is in a valid state */
    gst_element_set_state(source->container, GST_STATE_PAUSED);
    assert(gst_element_get_state(source->container, NULL, NULL, -1) == GST_STATE_CHANGE_SUCCESS);
    if (source->state == SOURCE_STOPPED)
    {
        WaitForSingleObject(source->all_streams_event, INFINITE);
    }

    for (unsigned int i = 0; i < source->stream_count; i++)
    {
        struct media_stream *stream;
        IMFStreamDescriptor *sd;
        DWORD stream_id;
        BOOL was_active;
        BOOL selected;

        stream = source->streams[i];

        IMFStreamDescriptor_GetStreamIdentifier(stream->descriptor, &stream_id);

        sd = stream_descriptor_from_id(command->u.start.descriptor, stream_id, &selected);
        IMFStreamDescriptor_Release(sd);

        was_active = stream->state != STREAM_INACTIVE;

        if (position->vt != VT_EMPTY)
        {
            GstEvent *seek_event = gst_event_new_seek(1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                    GST_SEEK_TYPE_SET, position->u.hVal.QuadPart / 100, GST_SEEK_TYPE_NONE, 0);


            gst_pad_push_event(stream->my_sink, seek_event);
            gst_bus_poll(source->bus, GST_MESSAGE_RESET_TIME, -1);

            stream->eos = FALSE;
        }

        stream->state = selected ? STREAM_RUNNING : STREAM_INACTIVE;

        if (selected)
        {
            TRACE("Stream %u (%p) selected\n", i, stream);
            IMFMediaEventQueue_QueueEventParamUnk(source->event_queue,
            was_active ? MEUpdatedStream : MENewStream, &GUID_NULL,
            S_OK, (IUnknown*) &stream->IMFMediaStream_iface);

            IMFMediaEventQueue_QueueEventParamVar(stream->event_queue,
                seek_message ? MEStreamSeeked : MEStreamStarted, &GUID_NULL, S_OK, position);
        }
    }

    IMFMediaEventQueue_QueueEventParamVar(source->event_queue,
        seek_message ? MESourceSeeked : MESourceStarted,
        &GUID_NULL, S_OK, position);

    source->state = SOURCE_RUNNING;

    gst_element_set_state(source->container, GST_STATE_PLAYING);
    gst_element_get_state(source->container, NULL, NULL, -1);

    return S_OK;
}

static HRESULT WINAPI source_async_commands_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct media_source *source = impl_from_async_commands_callback_IMFAsyncCallback(iface);
    struct source_async_command *command;
    IUnknown *state;
    HRESULT hr;

    if (source->state == SOURCE_SHUTDOWN)
        return S_OK;

    if (FAILED(hr = IMFAsyncResult_GetState(result, &state)))
        return hr;

    command = impl_from_async_command_IUnknown(state);
    switch (command->op)
    {
        case SOURCE_ASYNC_START:
            start_pipeline(source, command);
            break;
        default:
            ;
    }

    IUnknown_Release(state);

    return S_OK;
}

static const IMFAsyncCallbackVtbl source_async_commands_callback_vtbl =
{
    callback_QueryInterface,
    source_async_commands_callback_AddRef,
    source_async_commands_callback_Release,
    callback_GetParameters,
    source_async_commands_Invoke,
};

/* inactive stream sample discarder */
static GstFlowReturn stream_new_sample(GstElement *appsink, gpointer user)
{
    struct media_stream *stream = (struct media_stream *) user;
    GstSample *discard_sample;

    TRACE("(%p) got sample\n", stream);

    g_signal_emit_by_name(stream->appsink, "pull-sample", &discard_sample);
    gst_sample_unref(discard_sample);
    return GST_FLOW_OK;
}

GstFlowReturn pull_from_bytestream(GstPad *pad, GstObject *parent, guint64 ofs, guint len,
        GstBuffer **buf)
{
    struct media_source *source = gst_pad_get_element_private(pad);
    IMFByteStream *byte_stream = source->byte_stream;
    ULONG bytes_read;
    GstMapInfo info;
    BOOL is_eof;
    HRESULT hr;

    TRACE("gstreamer requesting %u bytes at %s from source %p into buffer %p\n", len, wine_dbgstr_longlong(ofs), source, buf);

    if (ofs != GST_BUFFER_OFFSET_NONE)
    {
        if (FAILED(IMFByteStream_SetCurrentPosition(byte_stream, ofs)))
            return GST_FLOW_ERROR;
    }

    if (FAILED(IMFByteStream_IsEndOfStream(byte_stream, &is_eof)))
        return GST_FLOW_ERROR;
    if (is_eof)
        return GST_FLOW_EOS;

    if (!(*buf))
        *buf = gst_buffer_new_and_alloc(len);
    gst_buffer_map(*buf, &info, GST_MAP_WRITE);
    hr = IMFByteStream_Read(byte_stream, info.data, len, &bytes_read);
    gst_buffer_unmap(*buf, &info);

    gst_buffer_set_size(*buf, bytes_read);

    if (FAILED(hr))
    {
        return GST_FLOW_ERROR;
    }
    GST_BUFFER_OFFSET(*buf) = ofs;
    return GST_FLOW_OK;
}

static gboolean query_bytestream(GstPad *pad, GstObject *parent, GstQuery *query)
{
    struct media_source *source = gst_pad_get_element_private(pad);
    GstFormat format;
    QWORD bytestream_len;

    TRACE("GStreamer queries source %p for %s\n", source, GST_QUERY_TYPE_NAME(query));

    if (FAILED(IMFByteStream_GetLength(source->byte_stream, &bytestream_len)))
        return FALSE;

    switch (GST_QUERY_TYPE(query))
    {
        case GST_QUERY_DURATION:
        {
            gst_query_parse_duration (query, &format, NULL);
            if (format == GST_FORMAT_PERCENT) {
                gst_query_set_duration (query, GST_FORMAT_PERCENT, GST_FORMAT_PERCENT_MAX);
                return TRUE;
            }
            else if (format == GST_FORMAT_BYTES)
            {
                QWORD length;
                IMFByteStream_GetLength(source->byte_stream, &length);
                gst_query_set_duration (query, GST_FORMAT_BYTES, length);
                return TRUE;
            }
            return FALSE;
        }
        case GST_QUERY_SEEKING:
        {
            gst_query_parse_seeking (query, &format, NULL, NULL, NULL);
            if (format != GST_FORMAT_BYTES)
            {
                WARN("Cannot seek using format \"%s\".\n", gst_format_get_name(format));
                return FALSE;
            }
            gst_query_set_seeking(query, GST_FORMAT_BYTES, 1, 0, bytestream_len);
            return TRUE;
        }
        case GST_QUERY_SCHEDULING:
        {
            gst_query_set_scheduling(query, GST_SCHEDULING_FLAG_SEEKABLE, 1, -1, 0);
            gst_query_add_scheduling_mode(query, GST_PAD_MODE_PULL);
            return TRUE;
        }
        case GST_QUERY_CAPS:
        {
            GstCaps *caps, *filter;

            gst_query_parse_caps(query, &filter);

            caps = gst_static_caps_get(&source_descs[source->type].bytestream_caps);

            if (filter) {
                GstCaps* filtered;
                filtered = gst_caps_intersect_full(
                        filter, caps, GST_CAPS_INTERSECT_FIRST);
                gst_caps_unref(caps);
                caps = filtered;
            }
            gst_query_set_caps_result(query, caps);
            gst_caps_unref(caps);
            return TRUE;
        }
        default:
        {
            WARN("Unhandled query type %s\n", GST_QUERY_TYPE_NAME(query));
            return FALSE;
        }
    }
}

static gboolean activate_bytestream_pad_mode(GstPad *pad, GstObject *parent, GstPadMode mode, gboolean activate)
{
    struct media_source *source = gst_pad_get_element_private(pad);

    TRACE("%s source pad for mediasource %p in %s mode.\n",
            activate ? "Activating" : "Deactivating", source, gst_pad_mode_get_name(mode));

    switch (mode) {
      case GST_PAD_MODE_PULL:
        return TRUE;
      default:
        return FALSE;
    }
    return FALSE;
}

static gboolean process_bytestream_pad_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
    struct media_source *source = gst_pad_get_element_private(pad);

    TRACE("source %p, type \"%s\".\n", source, GST_EVENT_TYPE_NAME(event));

    switch (event->type) {
        /* the seek event should fail in pull mode */
        case GST_EVENT_SEEK:
            return FALSE;
        default:
            WARN("Ignoring \"%s\" event.\n", GST_EVENT_TYPE_NAME(event));
        case GST_EVENT_TAG:
        case GST_EVENT_QOS:
        case GST_EVENT_RECONFIGURE:
            return gst_pad_event_default(pad, parent, event);
    }
    return TRUE;
}

GstBusSyncReply watch_source_bus(GstBus *bus, GstMessage *message, gpointer user)
{
    struct media_source *source = (struct media_source *) user;
    gchar *dbg_info = NULL;
    GError *err = NULL;

    TRACE("source %p message type %s\n", source, GST_MESSAGE_TYPE_NAME(message));

    switch (message->type)
    {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(message, &err, &dbg_info);
            ERR("%s: %s\n", GST_OBJECT_NAME(message->src), err->message);
            ERR("%s\n", dbg_info);
            g_error_free(err);
            g_free(dbg_info);
            break;
        case GST_MESSAGE_WARNING:
            gst_message_parse_warning(message, &err, &dbg_info);
            WARN("%s: %s\n", GST_OBJECT_NAME(message->src), err->message);
            WARN("%s\n", dbg_info);
            g_error_free(err);
            g_free(dbg_info);
            break;
        default:
            break;
    }

    return GST_BUS_PASS;
}

static HRESULT media_stream_constructor(struct media_source *source, GstPad *pad, DWORD stream_id, struct media_stream **out_stream);
static void source_stream_added(GstElement *element, GstPad *pad, gpointer user)
{
    struct media_source *source = (struct media_source *) user;
    struct media_stream **new_stream_array;
    struct media_stream *stream;
    gchar *g_stream_id;
    DWORD stream_id;

    /* Most/All seen randomly calculate the initial part of the stream id, the last three digits are the only deterministic part */
    g_stream_id = gst_pad_get_stream_id(pad);
    sscanf(strstr(g_stream_id, "/"), "/%03u", &stream_id);
    g_free(g_stream_id);

    TRACE("stream-id: %u\n", stream_id);

    /* find existing stream */
    for (unsigned int i = 0; i < source->stream_count; i++)
    {
        DWORD existing_stream_id;
        IMFStreamDescriptor *descriptor = source->streams[i]->descriptor;

        if (FAILED(IMFStreamDescriptor_GetStreamIdentifier(descriptor, &existing_stream_id)))
            goto leave;

        if (existing_stream_id == stream_id)
        {
            struct media_stream *existing_stream = source->streams[i];
            GstPadLinkReturn ret;

            TRACE("Found existing stream %p\n", existing_stream);

            if (!existing_stream->my_sink)
            {
                ERR("Couldn't find our sink\n");
                goto leave;
            }

            existing_stream->their_src = pad;
            gst_pad_set_element_private(pad, existing_stream);

            if ((ret = gst_pad_link(existing_stream->their_src, existing_stream->my_sink)) != GST_PAD_LINK_OK)
            {
                ERR("Error linking demuxer to stream %p, err = %d\n", existing_stream, ret);
            }
            gst_element_sync_state_with_parent(existing_stream->appsink);

            goto leave;
        }
    }

    if (FAILED(media_stream_constructor(source, pad, stream_id, &stream)))
    {
        goto leave;
    }

    if (!(new_stream_array = heap_realloc(source->streams, (source->stream_count + 1) * (sizeof(*new_stream_array)))))
    {
        ERR("Failed to add stream to source\n");
        goto leave;
    }

    source->streams = new_stream_array;
    source->streams[source->stream_count++] = stream;

    leave:
    return;
}

static void source_stream_removed(GstElement *element, GstPad *pad, gpointer user)
{
    struct media_stream *stream;

    if (gst_pad_get_direction(pad) != GST_PAD_SRC)
        return;

    stream = (struct media_stream *) gst_pad_get_element_private(pad);

    if (stream)
    {
        TRACE("Stream %p of Source %p removed\n", stream, stream->parent_source);

        assert (stream->their_src == pad);

        gst_pad_unlink(stream->their_src, stream->my_sink);

        stream->their_src = NULL;
        gst_pad_set_element_private(pad, NULL);
    }
}

static void source_all_streams(GstElement *element, gpointer user)
{
    struct media_source *source = (struct media_source *) user;

    SetEvent(source->all_streams_event);
}

static HRESULT WINAPI media_stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **out)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%s %p)\n", stream, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaStream) ||
        IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *out = &stream->IMFMediaStream_iface;
    }
    else
    {
        FIXME("(%s, %p)\n", debugstr_guid(riid), out);
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*out);
    return S_OK;
}

static ULONG WINAPI media_stream_AddRef(IMFMediaStream *iface)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);
    ULONG ref = InterlockedIncrement(&stream->ref);

    TRACE("(%p) ref=%u\n", stream, ref);

    return ref;
}

static ULONG WINAPI media_stream_Release(IMFMediaStream *iface)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    ULONG ref = InterlockedDecrement(&stream->ref);

    TRACE("(%p) ref=%u\n", stream, ref);

    if (!ref)
    {
        if (stream->their_src)
            gst_object_unref(GST_OBJECT(stream->their_src));
        if (stream->my_sink)
            gst_object_unref(GST_OBJECT(stream->my_sink));

        if (stream->descriptor)
            IMFStreamDescriptor_Release(stream->descriptor);
        if (stream->event_queue)
            IMFMediaEventQueue_Release(stream->event_queue);
        if (stream->parent_source)
            IMFMediaSource_Release(&stream->parent_source->IMFMediaSource_iface);

        heap_free(stream);
    }

    return ref;
}

static HRESULT WINAPI media_stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%#x, %p)\n", stream, flags, event);

    if (stream->state == STREAM_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_GetEvent(stream->event_queue, flags, event);
}

static HRESULT WINAPI media_stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%p, %p)\n", stream, callback, state);

    if (stream->state == STREAM_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_BeginGetEvent(stream->event_queue, callback, state);
}

static HRESULT WINAPI media_stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%p, %p)\n", stream, result, event);

    if (stream->state == STREAM_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_EndGetEvent(stream->event_queue, result, event);
}

static HRESULT WINAPI media_stream_QueueEvent(IMFMediaStream *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%d, %s, %#x, %p)\n", stream, event_type, debugstr_guid(ext_type), hr, value);

    if (stream->state == STREAM_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_QueueEventParamVar(stream->event_queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI media_stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **source)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    FIXME("stub (%p)->(%p)\n", stream, source);

    if (stream->state == STREAM_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT WINAPI media_stream_GetStreamDescriptor(IMFMediaStream* iface, IMFStreamDescriptor **descriptor)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%p)\n", stream, descriptor);

    if (stream->state == STREAM_SHUTDOWN)
        return MF_E_SHUTDOWN;

    IMFStreamDescriptor_AddRef(stream->descriptor);
    *descriptor = stream->descriptor;

    return S_OK;
}

static HRESULT WINAPI media_stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct media_stream *stream = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%p)\n", iface, token);

    if (stream->state == STREAM_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static const IMFMediaStreamVtbl media_stream_vtbl =
{
    media_stream_QueryInterface,
    media_stream_AddRef,
    media_stream_Release,
    media_stream_GetEvent,
    media_stream_BeginGetEvent,
    media_stream_EndGetEvent,
    media_stream_QueueEvent,
    media_stream_GetMediaSource,
    media_stream_GetStreamDescriptor,
    media_stream_RequestSample
};

static HRESULT media_stream_constructor(struct media_source *source, GstPad *pad, DWORD stream_id, struct media_stream **out_stream)
{
    struct media_stream *object = heap_alloc_zero(sizeof(*object));
    IMFMediaTypeHandler *type_handler = NULL;
    IMFMediaType *stream_type = NULL;
    GstCaps *caps = NULL;
    HRESULT hr;

    TRACE("(%p %p)->(%p)\n", source, pad, out_stream);

    object->IMFMediaStream_iface.lpVtbl = &media_stream_vtbl;
    object->ref = 1;

    IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
    object->parent_source = source;
    object->their_src = pad;

    object->state = STREAM_INACTIVE;

    if (FAILED(hr = MFCreateEventQueue(&object->event_queue)))
        goto fail;

    if (!(object->appsink = gst_element_factory_make("appsink", NULL)))
    {
        hr = E_OUTOFMEMORY;
        goto fail;
    }
    gst_bin_add(GST_BIN(object->parent_source->container), object->appsink);

    g_object_set(object->appsink, "emit-signals", TRUE, NULL);
    g_object_set(object->appsink, "sync", FALSE, NULL);
    g_object_set(object->appsink, "async", FALSE, NULL); /* <- This allows us interact with the bin w/o prerolling */
    g_signal_connect(object->appsink, "new-sample", G_CALLBACK(stream_new_sample_wrapper), object);

    if (!(caps = gst_pad_query_caps(pad, NULL)))
        goto fail;

    if (TRACE_ON(mfplat))
    {
        gchar *caps_str = gst_caps_to_string(caps);
        TRACE("caps %s\n", debugstr_a(caps_str));
        g_free(caps_str);
    }

    if (!(stream_type = mf_media_type_from_caps(caps)))
        goto fail;

    gst_caps_unref(caps);
    caps = NULL;

    if (FAILED(hr = MFCreateStreamDescriptor(stream_id, 1, &stream_type, &object->descriptor)))
        goto fail;

    if (FAILED(hr = IMFStreamDescriptor_GetMediaTypeHandler(object->descriptor, &type_handler)))
        goto fail;

    if (FAILED(hr = IMFMediaTypeHandler_SetCurrentMediaType(type_handler, stream_type)))
        goto fail;

    IMFMediaTypeHandler_Release(type_handler);
    type_handler = NULL;
    IMFMediaType_Release(stream_type);
    stream_type = NULL;

    object->my_sink = gst_element_get_static_pad(object->appsink, "sink");
    gst_pad_set_element_private(pad, object);

    gst_pad_link(object->their_src, object->my_sink);

    gst_element_sync_state_with_parent(object->appsink);

    TRACE("->(%p)\n", object);

    *out_stream = object;
    return S_OK;

    fail:
    WARN("Failed to construct media stream, hr %#x.\n", hr);

    if (caps)
        gst_caps_unref(caps);
    if (stream_type)
        IMFMediaType_Release(stream_type);
    if (type_handler)
        IMFMediaTypeHandler_Release(type_handler);

    IMFMediaStream_Release(&object->IMFMediaStream_iface);
    return hr;
}

static HRESULT WINAPI media_source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%s %p)\n", source, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaSource) ||
        IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *out = &source->IMFMediaSource_iface;
    }
    else
    {
        FIXME("(%s, %p)\n", debugstr_guid(riid), out);
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*out);
    return S_OK;
}

static ULONG WINAPI media_source_AddRef(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedIncrement(&source->ref);

    TRACE("(%p) ref=%u\n", source, ref);

    return ref;
}

static ULONG WINAPI media_source_Release(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedDecrement(&source->ref);

    TRACE("(%p) ref=%u\n", source, ref);

    if (!ref)
    {
        heap_free(source);
    }

    return ref;
}

static HRESULT WINAPI media_source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%#x, %p)\n", source, flags, event);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_GetEvent(source->event_queue, flags, event);
}

static HRESULT WINAPI media_source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%p, %p)\n", source, callback, state);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_BeginGetEvent(source->event_queue, callback, state);
}

static HRESULT WINAPI media_source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%p, %p)\n", source, result, event);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_EndGetEvent(source->event_queue, result, event);
}

static HRESULT WINAPI media_source_QueueEvent(IMFMediaSource *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%d, %s, %#x, %p)\n", source, event_type, debugstr_guid(ext_type), hr, value);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_QueueEventParamVar(source->event_queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI media_source_GetCharacteristics(IMFMediaSource *iface, DWORD *characteristics)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p)->(%p): stub\n", source, characteristics);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **descriptor)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%p)\n", source, descriptor);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    if (!(source->pres_desc))
    {
        return MF_E_NOT_INITIALIZED;
    }

    IMFPresentationDescriptor_Clone(source->pres_desc, descriptor);

    return S_OK;
}

static HRESULT WINAPI media_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor,
                                     const GUID *time_format, const PROPVARIANT *position)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    struct source_async_command *command;
    HRESULT hr;

    TRACE("(%p)->(%p, %p, %p)\n", source, descriptor, time_format, position);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    if (!(IsEqualIID(time_format, &GUID_NULL)))
    {
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    if (SUCCEEDED(hr = source_create_async_op(SOURCE_ASYNC_START, &command)))
    {
        command->u.start.descriptor = descriptor;
        command->u.start.format = *time_format;
        PropVariantCopy(&command->u.start.position, position);

        hr = MFPutWorkItem(source->async_commands_queue, &source->async_commands_callback, &command->IUnknown_iface);
    }

    return hr;
}

static HRESULT WINAPI media_source_Stop(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p): stub\n", source);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_Pause(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p): stub\n", source);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT media_source_teardown(struct media_source *source)
{
    if (source->my_src)
        gst_object_unref(GST_OBJECT(source->my_src));
    if (source->their_sink)
        gst_object_unref(GST_OBJECT(source->their_sink));
    if (source->container)
    {
        gst_element_set_state(source->container, GST_STATE_NULL);
        gst_object_unref(GST_OBJECT(source->container));
    }
    if (source->pres_desc)
        IMFPresentationDescriptor_Release(source->pres_desc);
    if (source->event_queue)
        IMFMediaEventQueue_Release(source->event_queue);
    if (source->byte_stream)
        IMFByteStream_Release(source->byte_stream);

    for (unsigned int i = 0; i < source->stream_count; i++)
    {
        source->streams[i]->state = STREAM_SHUTDOWN;
        IMFMediaStream_Release(&source->streams[i]->IMFMediaStream_iface);
    }

    if (source->stream_count)
        heap_free(source->streams);

    if (source->all_streams_event)
        CloseHandle(source->all_streams_event);

    if (source->async_commands_queue)
        MFUnlockWorkQueue(source->async_commands_queue);

    return S_OK;
}

static HRESULT WINAPI media_source_Shutdown(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)\n", source);

    source->state = SOURCE_SHUTDOWN;
    return media_source_teardown(source);
}

static const IMFMediaSourceVtbl IMFMediaSource_vtbl =
{
    media_source_QueryInterface,
    media_source_AddRef,
    media_source_Release,
    media_source_GetEvent,
    media_source_BeginGetEvent,
    media_source_EndGetEvent,
    media_source_QueueEvent,
    media_source_GetCharacteristics,
    media_source_CreatePresentationDescriptor,
    media_source_Start,
    media_source_Stop,
    media_source_Pause,
    media_source_Shutdown,
};

static HRESULT media_source_constructor(IMFByteStream *bytestream, enum source_type type, struct media_source **out_media_source)
{
    GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
        "mf_src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        source_descs[type].bytestream_caps);

    struct media_source *object = heap_alloc_zero(sizeof(*object));
    GList *demuxer_list_one, *demuxer_list_two;
    GstElementFactory *demuxer_factory = NULL;
    IMFStreamDescriptor **descriptors = NULL;
    HRESULT hr;
    int ret;

    if (!object)
        return E_OUTOFMEMORY;

    object->IMFMediaSource_iface.lpVtbl = &IMFMediaSource_vtbl;
    object->async_commands_callback.lpVtbl = &source_async_commands_callback_vtbl;
    object->ref = 1;
    object->type = type;
    object->byte_stream = bytestream;
    IMFByteStream_AddRef(bytestream);
    object->all_streams_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    if (FAILED(hr = MFCreateEventQueue(&object->event_queue)))
        goto fail;

    if (FAILED(hr = MFAllocateWorkQueue(&object->async_commands_queue)))
        goto fail;

    object->container = gst_bin_new(NULL);
    object->bus = gst_bus_new();
    gst_bus_set_sync_handler(object->bus, watch_source_bus_wrapper, object, NULL);
    gst_element_set_bus(object->container, object->bus);

    object->my_src = gst_pad_new_from_static_template(&src_template, "mf-src");
    gst_pad_set_element_private(object->my_src, object);
    gst_pad_set_getrange_function(object->my_src, pull_from_bytestream_wrapper);
    gst_pad_set_query_function(object->my_src, query_bytestream_wrapper);
    gst_pad_set_activatemode_function(object->my_src, activate_bytestream_pad_mode_wrapper);
    gst_pad_set_event_function(object->my_src, process_bytestream_pad_event_wrapper);

    /* Find demuxer */
    demuxer_list_one = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DEMUXER, 1);

    demuxer_list_two = gst_element_factory_list_filter(demuxer_list_one, gst_static_caps_get(&source_descs[type].bytestream_caps), GST_PAD_SINK, 0);
    gst_plugin_feature_list_free(demuxer_list_one);

    if (!(g_list_length(demuxer_list_two)))
    {
        ERR("Failed to find demuxer for source.\n");
        gst_plugin_feature_list_free(demuxer_list_two);
        hr = E_FAIL;
        goto fail;
    }

    demuxer_factory = g_list_first(demuxer_list_two)->data;
    gst_object_ref(demuxer_factory);
    gst_plugin_feature_list_free(demuxer_list_two);

    TRACE("Found demuxer %s.\n", GST_ELEMENT_NAME(demuxer_factory));

    object->demuxer = gst_element_factory_create(demuxer_factory, NULL);
    if (!(object->demuxer))
    {
        WARN("Failed to create demuxer for source\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }
    gst_bin_add(GST_BIN(object->container), object->demuxer);

    g_signal_connect(object->demuxer, "pad-added", G_CALLBACK(source_stream_added_wrapper), object);
    g_signal_connect(object->demuxer, "pad-removed", G_CALLBACK(source_stream_removed_wrapper), object);
    g_signal_connect(object->demuxer, "no-more-pads", G_CALLBACK(source_all_streams_wrapper), object);

    object->their_sink = gst_element_get_static_pad(object->demuxer, "sink");

    if ((ret = gst_pad_link(object->my_src, object->their_sink)) < 0)
    {
        WARN("Failed to link our bytestream pad to the demuxer input\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    object->state = SOURCE_OPENING;

    gst_element_set_state(object->container, GST_STATE_PAUSED);
    ret = gst_element_get_state(object->container, NULL, NULL, -1);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        ERR("Failed to play source.\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    WaitForSingleObject(object->all_streams_event, INFINITE);

    /* init presentation descriptor */

    descriptors = heap_alloc(object->stream_count * sizeof(IMFStreamDescriptor*));
    for (unsigned int i = 0; i < object->stream_count; i++)
    {
        IMFMediaStream_GetStreamDescriptor(&object->streams[i]->IMFMediaStream_iface, &descriptors[i]);
    }

    if (FAILED(MFCreatePresentationDescriptor(object->stream_count, descriptors, &object->pres_desc)))
        goto fail;

    for (unsigned int i = 0; i < object->stream_count; i++)
    {
        IMFPresentationDescriptor_SelectStream(object->pres_desc, i);
        IMFStreamDescriptor_Release(descriptors[i]);
    }
    heap_free(descriptors);
    descriptors = NULL;

    gst_element_set_state(object->container, GST_STATE_READY);

    object->state = SOURCE_STOPPED;

    *out_media_source = object;
    return S_OK;

    fail:
    WARN("Failed to construct MFMediaSource, hr %#x.\n", hr);

    if (demuxer_factory)
        gst_object_unref(demuxer_factory);
    if (descriptors)
        heap_free(descriptors);
    media_source_teardown(object);
    heap_free(object);
    *out_media_source = NULL;
    return hr;
}

/* IMFByteStreamHandler */

struct container_stream_handler_result
{
    struct list entry;
    IMFAsyncResult *result;
    MF_OBJECT_TYPE obj_type;
    IUnknown *object;
};

struct container_stream_handler
{
    IMFByteStreamHandler IMFByteStreamHandler_iface;
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    enum source_type type;
    struct list results;
    CRITICAL_SECTION cs;
};

static struct container_stream_handler *impl_from_IMFByteStreamHandler(IMFByteStreamHandler *iface)
{
    return CONTAINING_RECORD(iface, struct container_stream_handler, IMFByteStreamHandler_iface);
}

static struct container_stream_handler *impl_from_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct container_stream_handler, IMFAsyncCallback_iface);
}

static HRESULT WINAPI container_stream_handler_QueryInterface(IMFByteStreamHandler *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFByteStreamHandler) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFByteStreamHandler_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI container_stream_handler_AddRef(IMFByteStreamHandler *iface)
{
    struct container_stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedIncrement(&handler->refcount);

    TRACE("%p, refcount %u.\n", handler, refcount);

    return refcount;
}

static ULONG WINAPI container_stream_handler_Release(IMFByteStreamHandler *iface)
{
    struct container_stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedDecrement(&handler->refcount);
    struct container_stream_handler_result *result, *next;

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        LIST_FOR_EACH_ENTRY_SAFE(result, next, &handler->results, struct container_stream_handler_result, entry)
        {
            list_remove(&result->entry);
            IMFAsyncResult_Release(result->result);
            if (result->object)
                IUnknown_Release(result->object);
            heap_free(result);
        }
        DeleteCriticalSection(&handler->cs);
        heap_free(handler);
    }

    return refcount;
}

struct create_object_context
{
    IUnknown IUnknown_iface;
    LONG refcount;

    IPropertyStore *props;
    IMFByteStream *stream;
    WCHAR *url;
    DWORD flags;
};

static struct create_object_context *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct create_object_context, IUnknown_iface);
}

static HRESULT WINAPI create_object_context_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI create_object_context_AddRef(IUnknown *iface)
{
    struct create_object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&context->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI create_object_context_Release(IUnknown *iface)
{
    struct create_object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&context->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        if (context->props)
            IPropertyStore_Release(context->props);
        if (context->stream)
            IMFByteStream_Release(context->stream);
        if (context->url)
            heap_free(context->url);
        heap_free(context);
    }

    return refcount;
}

static const IUnknownVtbl create_object_context_vtbl =
{
    create_object_context_QueryInterface,
    create_object_context_AddRef,
    create_object_context_Release,
};

static WCHAR *heap_strdupW(const WCHAR *str)
{
    WCHAR *ret = NULL;

    if (str)
    {
        unsigned int size;

        size = (lstrlenW(str) + 1) * sizeof(WCHAR);
        ret = heap_alloc(size);
        if (ret)
            memcpy(ret, str, size);
    }

    return ret;
}

static HRESULT WINAPI container_stream_handler_BeginCreateObject(IMFByteStreamHandler *iface, IMFByteStream *stream, const WCHAR *url, DWORD flags,
        IPropertyStore *props, IUnknown **cancel_cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct container_stream_handler *this = impl_from_IMFByteStreamHandler(iface);
    struct create_object_context *context;
    IMFAsyncResult *caller, *item;
    HRESULT hr;

    TRACE("%p, %s, %#x, %p, %p, %p, %p.\n", iface, debugstr_w(url), flags, props, cancel_cookie, callback, state);

    if (cancel_cookie)
        *cancel_cookie = NULL;

    if (FAILED(hr = MFCreateAsyncResult(NULL, callback, state, &caller)))
        return hr;

    context = heap_alloc(sizeof(*context));
    if (!context)
    {
        IMFAsyncResult_Release(caller);
        return E_OUTOFMEMORY;
    }

    context->IUnknown_iface.lpVtbl = &create_object_context_vtbl;
    context->refcount = 1;
    context->props = props;
    if (context->props)
        IPropertyStore_AddRef(context->props);
    context->flags = flags;
    context->stream = stream;
    if (context->stream)
        IMFByteStream_AddRef(context->stream);
    if (url)
        context->url = heap_strdupW(url);
    if (!context->stream)
    {
        IMFAsyncResult_Release(caller);
        IUnknown_Release(&context->IUnknown_iface);
        return E_OUTOFMEMORY;
    }

    hr = MFCreateAsyncResult(&context->IUnknown_iface, &this->IMFAsyncCallback_iface, (IUnknown *)caller, &item);
    IUnknown_Release(&context->IUnknown_iface);
    if (SUCCEEDED(hr))
    {
        if (SUCCEEDED(hr = MFPutWorkItemEx(MFASYNC_CALLBACK_QUEUE_IO, item)))
        {
            if (cancel_cookie)
            {
                *cancel_cookie = (IUnknown *)caller;
                IUnknown_AddRef(*cancel_cookie);
            }
        }

        IMFAsyncResult_Release(item);
    }
    IMFAsyncResult_Release(caller);

    return hr;
}

static HRESULT WINAPI container_stream_handler_EndCreateObject(IMFByteStreamHandler *iface, IMFAsyncResult *result,
        MF_OBJECT_TYPE *obj_type, IUnknown **object)
{
    struct container_stream_handler *this = impl_from_IMFByteStreamHandler(iface);
    struct container_stream_handler_result *found = NULL, *cur;
    HRESULT hr;

    TRACE("%p, %p, %p, %p.\n", iface, result, obj_type, object);

    EnterCriticalSection(&this->cs);

    LIST_FOR_EACH_ENTRY(cur, &this->results, struct container_stream_handler_result, entry)
    {
        if (result == cur->result)
        {
            list_remove(&cur->entry);
            found = cur;
            break;
        }
    }

    LeaveCriticalSection(&this->cs);

    if (found)
    {
        *obj_type = found->obj_type;
        *object = found->object;
        hr = IMFAsyncResult_GetStatus(found->result);
        IMFAsyncResult_Release(found->result);
        heap_free(found);
    }
    else
    {
        *obj_type = MF_OBJECT_INVALID;
        *object = NULL;
        hr = MF_E_UNEXPECTED;
    }

    return hr;
}

static HRESULT WINAPI container_stream_handler_CancelObjectCreation(IMFByteStreamHandler *iface, IUnknown *cancel_cookie)
{
    struct container_stream_handler *this = impl_from_IMFByteStreamHandler(iface);
    struct container_stream_handler_result *found = NULL, *cur;

    TRACE("%p, %p.\n", iface, cancel_cookie);

    EnterCriticalSection(&this->cs);

    LIST_FOR_EACH_ENTRY(cur, &this->results, struct container_stream_handler_result, entry)
    {
        if (cancel_cookie == (IUnknown *)cur->result)
        {
            list_remove(&cur->entry);
            found = cur;
            break;
        }
    }

    LeaveCriticalSection(&this->cs);

    if (found)
    {
        IMFAsyncResult_Release(found->result);
        if (found->object)
            IUnknown_Release(found->object);
        heap_free(found);
    }

    return found ? S_OK : MF_E_UNEXPECTED;
}

static HRESULT WINAPI container_stream_handler_GetMaxNumberOfBytesRequiredForResolution(IMFByteStreamHandler *iface, QWORD *bytes)
{
    FIXME("stub (%p %p)\n", iface, bytes);
    return E_NOTIMPL;
}

static const IMFByteStreamHandlerVtbl container_stream_handler_vtbl =
{
    container_stream_handler_QueryInterface,
    container_stream_handler_AddRef,
    container_stream_handler_Release,
    container_stream_handler_BeginCreateObject,
    container_stream_handler_EndCreateObject,
    container_stream_handler_CancelObjectCreation,
    container_stream_handler_GetMaxNumberOfBytesRequiredForResolution,
};

static HRESULT WINAPI container_stream_handler_callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI container_stream_handler_callback_AddRef(IMFAsyncCallback *iface)
{
    struct container_stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFByteStreamHandler_AddRef(&handler->IMFByteStreamHandler_iface);
}

static ULONG WINAPI container_stream_handler_callback_Release(IMFAsyncCallback *iface)
{
    struct container_stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFByteStreamHandler_Release(&handler->IMFByteStreamHandler_iface);
}

static HRESULT WINAPI container_stream_handler_callback_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT container_stream_handler_create_object(struct container_stream_handler *This, WCHAR *url, IMFByteStream *stream, DWORD flags,
                                            IPropertyStore *props, IUnknown **out_object, MF_OBJECT_TYPE *out_obj_type)
{
    TRACE("(%p %s %p %u %p %p %p)\n", This, debugstr_w(url), stream, flags, props, out_object, out_obj_type);

    if (flags & MF_RESOLUTION_MEDIASOURCE)
    {
        HRESULT hr;
        struct media_source *new_source;

        if (FAILED(hr = media_source_constructor(stream, This->type, &new_source)))
            return hr;

        TRACE("->(%p)\n", new_source);

        *out_object = (IUnknown*)&new_source->IMFMediaSource_iface;
        *out_obj_type = MF_OBJECT_MEDIASOURCE;

        return S_OK;
    }
    else
    {
        FIXME("flags = %08x\n", flags);
        return E_NOTIMPL;
    }
}

static HRESULT WINAPI container_stream_handler_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct container_stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    struct container_stream_handler_result *handler_result;
    MF_OBJECT_TYPE obj_type = MF_OBJECT_INVALID;
    IUnknown *object = NULL, *context_object;
    struct create_object_context *context;
    IMFAsyncResult *caller;
    HRESULT hr;

    caller = (IMFAsyncResult *)IMFAsyncResult_GetStateNoAddRef(result);

    if (FAILED(hr = IMFAsyncResult_GetObject(result, &context_object)))
    {
        WARN("Expected context set for callee result.\n");
        return hr;
    }

    context = impl_from_IUnknown(context_object);

    hr = container_stream_handler_create_object(handler, context->url, context->stream, context->flags, context->props, &object, &obj_type);

    handler_result = heap_alloc(sizeof(*handler_result));
    if (handler_result)
    {
        handler_result->result = caller;
        IMFAsyncResult_AddRef(handler_result->result);
        handler_result->obj_type = obj_type;
        handler_result->object = object;

        EnterCriticalSection(&handler->cs);
        list_add_tail(&handler->results, &handler_result->entry);
        LeaveCriticalSection(&handler->cs);
    }
    else
    {
        if (object)
            IUnknown_Release(object);
        hr = E_OUTOFMEMORY;
    }

    IUnknown_Release(&context->IUnknown_iface);

    IMFAsyncResult_SetStatus(caller, hr);
    MFInvokeCallback(caller);

    return S_OK;
}

static const IMFAsyncCallbackVtbl container_stream_handler_callback_vtbl =
{
    container_stream_handler_callback_QueryInterface,
    container_stream_handler_callback_AddRef,
    container_stream_handler_callback_Release,
    container_stream_handler_callback_GetParameters,
    container_stream_handler_callback_Invoke,
};

HRESULT container_stream_handler_construct(REFIID riid, void **obj, enum source_type type)
{
    struct container_stream_handler *this;
    HRESULT hr;

    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    this = heap_alloc_zero(sizeof(*this));
    if (!this)
        return E_OUTOFMEMORY;

    list_init(&this->results);
    InitializeCriticalSection(&this->cs);

    this->type = type;
    this->IMFByteStreamHandler_iface.lpVtbl = &container_stream_handler_vtbl;
    this->IMFAsyncCallback_iface.lpVtbl = &container_stream_handler_callback_vtbl;
    this->refcount = 1;

    hr = IMFByteStreamHandler_QueryInterface(&this->IMFByteStreamHandler_iface, riid, obj);
    IMFByteStreamHandler_Release(&this->IMFByteStreamHandler_iface);

    return hr;
}

/* helper for callback forwarding */
void perform_cb_media_source(struct cb_data *cbdata)
{
    switch(cbdata->type)
    {
    case PULL_FROM_BYTESTREAM:
        {
            struct getrange_data *data = &cbdata->u.getrange_data;
            cbdata->u.getrange_data.ret = pull_from_bytestream(data->pad, data->parent,
                    data->ofs, data->len, data->buf);
            break;
        }
    case QUERY_BYTESTREAM:
        {
            struct query_function_data *data = &cbdata->u.query_function_data;
            cbdata->u.query_function_data.ret = query_bytestream(data->pad, data->parent, data->query);
            break;
        }
    case ACTIVATE_BYTESTREAM_PAD_MODE:
        {
            struct activate_mode_data *data = &cbdata->u.activate_mode_data;
            cbdata->u.activate_mode_data.ret = activate_bytestream_pad_mode(data->pad, data->parent, data->mode, data->activate);
            break;
        }
    case PROCESS_BYTESTREAM_PAD_EVENT:
        {
            struct event_src_data *data = &cbdata->u.event_src_data;
            cbdata->u.event_src_data.ret = process_bytestream_pad_event(data->pad, data->parent, data->event);
            break;
        }
    case WATCH_SOURCE_BUS:
        {
            struct watch_bus_data *data = &cbdata->u.watch_bus_data;
            cbdata->u.watch_bus_data.ret = watch_source_bus(data->bus, data->msg, data->user);
            break;
        }
    case SOURCE_STREAM_ADDED:
        {
            struct pad_added_data *data = &cbdata->u.pad_added_data;
            source_stream_added(data->element, data->pad, data->user);
            break;
        }
    case SOURCE_STREAM_REMOVED:
        {
            struct pad_removed_data *data = &cbdata->u.pad_removed_data;
            source_stream_removed(data->element, data->pad, data->user);
            break;
        }
    case SOURCE_ALL_STREAMS:
        {
            struct no_more_pads_data *data = &cbdata->u.no_more_pads_data;
            source_all_streams(data->element, data->user);
            break;
        }
    case STREAM_NEW_SAMPLE:
        {
            struct new_sample_data *data = &cbdata->u.new_sample_data;
            cbdata->u.new_sample_data.ret = stream_new_sample(data->appsink, data->user);
            break;
        }
    default:
        {
            ERR("Wrong callback forwarder called\n");
            return;
        }
    }
}
