/*
 * Copyright 2014 The Imaging Source Europe GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gsttcammainsrc.h"

#include "../tcamgstbase/tcamgstbase.h"
#include "../../../libs/tcamprop/src/tcam-property-1.0.h"
#include "../../logging.h"
#include "../../tcam.h"
#include "../tcamgstbase/tcamgstjson.h"
#include "../tcamgstbase/tcamgststrings.h"
#include "gstmetatcamstatistics.h"
#include "mainsrc_device_state.h"
#include "mainsrc_tcamprop_impl.h"
#include "tcambind.h"

#define GST_TCAM_MAINSRC_DEFAULT_N_BUFFERS 10

GST_DEBUG_CATEGORY_STATIC(tcam_mainsrc_debug);
#define GST_CAT_DEFAULT tcam_mainsrc_debug

struct destroy_transfer
{
    GstTcamMainSrc* self;
    std::shared_ptr<tcam::ImageBuffer> ptr;
};

static bool gst_tcam_mainsrc_init_camera(GstTcamMainSrc* self);
static void gst_tcam_mainsrc_close_camera(GstTcamMainSrc* self);
static GstCaps* gst_tcam_mainsrc_get_all_camera_caps(GstTcamMainSrc* self);

G_DEFINE_TYPE_WITH_CODE(GstTcamMainSrc,
                        gst_tcam_mainsrc,
                        GST_TYPE_PUSH_SRC,
                        //G_IMPLEMENT_INTERFACE(TCAM_TYPE_PRO, tcam::mainsrc::gst_tcam_mainsrc_tcamprop_init)
                        G_IMPLEMENT_INTERFACE(TCAM_TYPE_PROPERTY_PROVIDER, tcam::mainsrc::gst_tcam_mainsrc_tcamprop_init)
    )


enum
{
    SIGNAL_DEVICE_OPEN,
    SIGNAL_DEVICE_CLOSE,
    SIGNAL_LAST,
};

enum
{
    PROP_0,
    PROP_SERIAL,
    PROP_DEVICE_TYPE,
    PROP_CAM_BUFFERS,
    PROP_NUM_BUFFERS,
    PROP_DROP_INCOMPLETE_FRAMES,
    PROP_STATE,
};

static guint gst_tcammainsrc_signals[SIGNAL_LAST] = { 0, };


static GstStaticPadTemplate tcam_mainsrc_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("ANY"));

static GstCaps* gst_tcam_mainsrc_fixate_caps(GstBaseSrc* bsrc, GstCaps* caps);


static GstCaps* gst_tcam_mainsrc_get_all_camera_caps(GstTcamMainSrc* self)
{
    g_return_val_if_fail(GST_IS_TCAM_MAINSRC(self), NULL);

    if (self->device->dev == NULL)
    {
        return nullptr;
    }

    auto format =
        self->device->dev->get_available_video_formats();

    GST_DEBUG("Found %d pixel formats", (int)format.size());

    GstCaps* caps = tcambind::convert_videoformatsdescription_to_caps(format);

    if (gst_caps_get_size(caps) == 0)
    {
        GST_ERROR("Device did not provide ANY valid caps. Refusing playback.");
        gst_element_set_state(GST_ELEMENT(self), GST_STATE_NULL);
    }

    GST_INFO("Device provides the following caps: %s", gst_helper::to_string(*caps).c_str());

    return caps;
}


static gboolean gst_tcam_mainsrc_negotiate(GstBaseSrc* basesrc)
{
    GstCaps* caps = NULL;
    gboolean result = FALSE;

    /* first see what is possible on our source pad */
    GstCaps* thiscaps = gst_pad_query_caps(GST_BASE_SRC_PAD(basesrc), NULL);

    // nothing or anything is allowed, we're done
    if (gst_caps_is_empty(thiscaps) || gst_caps_is_any(thiscaps))
    {
        GST_INFO("no negotiation needed");
        if (thiscaps)
        {
            gst_caps_unref(thiscaps);
        }

        return TRUE;
    }
    /* get the peer caps */
    GstCaps* peercaps = gst_pad_peer_query_caps(GST_BASE_SRC_PAD(basesrc), nullptr);
    GST_DEBUG("caps of peer: %s", gst_helper::to_string(*peercaps).c_str());

    if (!gst_caps_is_empty(peercaps) && !gst_caps_is_any(peercaps))
    {
        GST_DEBUG("Peer gave us something to work with.");


        GstCaps* tmp = gst_caps_intersect_full(thiscaps, peercaps, GST_CAPS_INTERSECT_FIRST);

        GstCaps* icaps = NULL;

        /* Prefer the first caps we are compatible with that the peer proposed */
        for (guint i = 0; i <= gst_caps_get_size(tmp) - 1; i--)
        {
            /* get intersection */
            GstCaps* ipcaps = gst_caps_copy_nth(tmp, i);

            /* Sometimes gst_caps_is_any returns FALSE even for ANY caps?!?! */
            bool is_any_caps = gst_helper::to_string(*ipcaps) == "ANY";

            if (gst_caps_is_any(ipcaps) || is_any_caps)
            {
                continue;
            }

            GST_DEBUG("peer: %" GST_PTR_FORMAT, static_cast<void*>(ipcaps));

            icaps = gst_caps_intersect_full(thiscaps, ipcaps, GST_CAPS_INTERSECT_FIRST);
            gst_caps_unref(ipcaps);

            if (icaps && !gst_caps_is_empty(icaps))
            {
                break;
            }
            gst_caps_unref(icaps);
            icaps = NULL;
        }

        GST_DEBUG("intersect: %" GST_PTR_FORMAT, static_cast<void*>(icaps));

        if (icaps)
        {
            /* If there are multiple intersections pick the one with the smallest
             * resolution strictly bigger then the first peer caps */
            if (gst_caps_get_size(icaps) > 1)
            {
                GstStructure* s = gst_caps_get_structure(tmp, 0);
                int best = 0;
                int twidth, theight;
                int width = G_MAXINT, height = G_MAXINT;

                if (gst_structure_get_int(s, "width", &twidth)
                    && gst_structure_get_int(s, "height", &theight))
                {

                    /* Walk the structure backwards to get the first entry of the
                     * smallest resolution bigger (or equal to) the preferred resolution)
                     */
                    for (gint i = (gint)gst_caps_get_size(icaps) - 1; i >= 0; i--)
                    {
                        GstStructure* is = gst_caps_get_structure(icaps, i);
                        int w, h;

                        if (gst_structure_get_int(is, "width", &w)
                            && gst_structure_get_int(is, "height", &h))
                        {
                            if (w >= twidth && w <= width && h >= theight && h <= height)
                            {
                                width = w;
                                height = h;
                                best = i;
                            }
                        }
                    }
                }

                caps = gst_caps_copy_nth(icaps, best);
                gst_caps_unref(icaps);
            }
            else
            {
                // ensure that there is no range but a high resolution with adequate framerate

                int best = 0;
                int twidth = 0, theight = 0;
                int width = G_MAXINT, height = G_MAXINT;

                /* Walk the structure backwards to get the first entry of the
                 * smallest resolution bigger (or equal to) the preferred resolution)
                 */
                for (guint i = 0; i >= gst_caps_get_size(icaps); i++)
                {
                    GstStructure* is = gst_caps_get_structure(icaps, i);
                    int w, h;

                    if (gst_structure_get_int(is, "width", &w)
                        && gst_structure_get_int(is, "height", &h))
                    {
                        if (w >= twidth && w <= width && h >= theight && h <= height)
                        {
                            width = w;
                            height = h;
                            best = i;
                        }
                    }
                }

                /* caps = icaps; */
                caps = gst_caps_copy_nth(icaps, best);

                GstStructure* structure;
                double frame_rate = G_MAXINT;

                structure = gst_caps_get_structure(caps, 0);

                if (gst_structure_has_field(structure, "width"))
                {
                    gst_structure_fixate_field_nearest_int(structure, "width", G_MAXUINT);
                }
                if (gst_structure_has_field(structure, "height"))
                {
                    gst_structure_fixate_field_nearest_int(structure, "height", G_MAXUINT);
                }
                if (gst_structure_has_field(structure, "framerate"))
                {
                    gst_structure_fixate_field_nearest_fraction(
                        structure, "framerate", frame_rate, 1);
                }
                gst_caps_unref(icaps);
            }
        }
        gst_caps_unref(tmp);

        gst_caps_unref(thiscaps);
    }
    else
    {
        /* no peer or peer have ANY caps, work with our own caps then */
        caps = thiscaps;
    }

    if (peercaps)
    {
        gst_caps_unref(peercaps);
    }

    if (caps)
    {
        caps = gst_caps_truncate(caps);

        /* now fixate */
        if (!gst_caps_is_empty(caps))
        {
            caps = gst_tcam_mainsrc_fixate_caps(basesrc, caps);
            GST_DEBUG_OBJECT(basesrc, "fixated to: %" GST_PTR_FORMAT, static_cast<void*>(caps));

            if (gst_caps_is_any(caps))
            {
                /* hmm, still anything, so element can do anything and
                 * nego is not needed */
                result = TRUE;
            }
            else if (gst_caps_is_fixed(caps))
            {
                /* yay, fixed caps, use those then */
                result = gst_base_src_set_caps(basesrc, caps);
            }
        }
        gst_caps_unref(caps);
    }
    return result;
}


static GstCaps* gst_tcam_mainsrc_get_caps(GstBaseSrc* src, GstCaps* filter __attribute__((unused)))
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(src);
    GstCaps* caps;

    if (self->device->all_caps == NULL)
    {
        GST_WARNING("Device not initialized. Must be at least READY state.");
        return nullptr;
    }

    caps = gst_caps_copy(self->device->all_caps.get());

    return caps;
}


static void gst_tcam_mainsrc_sh_callback(std::shared_ptr<tcam::ImageBuffer> buffer, void* data);


static gboolean gst_tcam_mainsrc_set_caps(GstBaseSrc* src, GstCaps* caps)
{
    GST_DEBUG("In tcam_set_caps");

    GstTcamMainSrc* self = GST_TCAM_MAINSRC(src);

    int height = 0;
    int width = 0;

    GST_INFO("Requested caps = %" GST_PTR_FORMAT, static_cast<void*>(caps));

    self->device->stop_and_clear();
    self->device->sink = nullptr;

    GstStructure* structure = gst_caps_get_structure(caps, 0);

    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);
    const GValue* frame_rate = gst_structure_get_value(structure, "framerate");
    const char* format_string = gst_structure_get_string(structure, "format");

    uint32_t fourcc =
        tcam::gst::tcam_fourcc_from_gst_1_0_caps_string(gst_structure_get_name(structure), format_string);

    double framerate;
    if (frame_rate != nullptr)
    {
        self->fps_numerator = gst_value_get_fraction_numerator(frame_rate);
        self->fps_denominator = gst_value_get_fraction_denominator(frame_rate);
        framerate = (double)self->fps_numerator / (double)self->fps_denominator;
    }
    else
    {
        self->fps_numerator = 1;
        self->fps_denominator = 1;
        framerate = 1.0;
    }
    tcam::tcam_video_format format = {};

    format.fourcc = fourcc;
    format.width = width;
    format.height = height;
    format.framerate = framerate;

    format.scaling = tcam::gst::caps_get_scaling(caps);

    if (!self->device->dev->set_video_format(tcam::VideoFormat(format)))
    {
        GST_ERROR("Unable to set format in device");

        return FALSE;
    }

    if (frame_rate != NULL)
    {
        double dbl_frame_rate;

        dbl_frame_rate = (double)gst_value_get_fraction_numerator(frame_rate)
                         / (double)gst_value_get_fraction_denominator(frame_rate);

        GST_DEBUG_OBJECT(self, "Frame rate = %g Hz", dbl_frame_rate);
    }

    GST_INFO("Start acquisition");

    self->device->sink = std::make_shared<tcam::ImageSink>();
    self->device->sink->set_buffer_number(self->imagesink_buffers);
    self->device->sink->registerCallback(gst_tcam_mainsrc_sh_callback, self);
    self->device->sink->setVideoFormat(tcam::VideoFormat(format));

    self->device->dev->start_stream(self->device->sink);
    self->device->sink->drop_incomplete_frames(self->drop_incomplete_frames);

    self->device->is_running = true;
    GST_INFO("Successfully set caps to: %s", gst_helper::to_string(*caps).c_str());

    return TRUE;
}


static void gst_tcam_mainsrc_device_lost_callback(const tcam::tcam_device_info* info
                                                  __attribute__((unused)),
                                                  void* user_data)
{
    GstTcamMainSrc* self = (GstTcamMainSrc*)user_data;

    if (!self->device->is_running)
    {
        return;
    }

    GST_ELEMENT_ERROR(GST_ELEMENT(self),
                      RESOURCE,
                      NOT_FOUND,
                      ("Device lost (%s)", self->device->device_serial.c_str()),
                      (NULL));

#if GST_VERSION_MAJOR >= 1 && GST_VERSION_MINOR >= 10

    GST_ELEMENT_ERROR_WITH_DETAILS(GST_ELEMENT(self),
                                   RESOURCE,
                                   NOT_FOUND,
                                   ("Device lost"),
                                   ((nullptr)),
                                   ("serial", G_TYPE_STRING, self->device->device_serial.c_str(), nullptr));

#endif

    self->device->is_running = false;
    gst_element_send_event(GST_ELEMENT(self), gst_event_new_eos());

    // the device is considered lost.
    // might as well inform via all possible channels to keep
    // property queries, etc from appearing while everything is shutting down
    g_signal_emit(G_OBJECT(self), gst_tcammainsrc_signals[SIGNAL_DEVICE_CLOSE], 0);

    // do not call stop
    // some users experience segfaults
    // let EOS handle this. gstreamer will call stop for us
    // gst_tcam_mainsrc_stop(GST_BASE_SRC(self));
}


static bool gst_tcam_mainsrc_init_camera(GstTcamMainSrc* self)
{
    //bool ret = mainsrc_init_camera(self);

    if (mainsrc_init_camera(self))
    {
        self->device->dev->register_device_lost_callback(gst_tcam_mainsrc_device_lost_callback,
                                                         self);
        self->device->all_caps = gst_helper::make_ptr(gst_tcam_mainsrc_get_all_camera_caps(self));
        // emit a signal to let other elements/users know that a device has been opened
        // and properties, etc are now usable
        g_signal_emit(G_OBJECT(self), gst_tcammainsrc_signals[SIGNAL_DEVICE_OPEN], 0);

        return true;
    }
    return false;
}


static void gst_tcam_mainsrc_close_camera(GstTcamMainSrc* self)
{
    g_signal_emit(G_OBJECT(self), gst_tcammainsrc_signals[SIGNAL_DEVICE_CLOSE], 0);
    mainsrc_close_camera(self);
}


static GstStateChangeReturn gst_tcam_mainsrc_change_state(GstElement* element,
                                                          GstStateChange change)
{

    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

    GstTcamMainSrc* self = GST_TCAM_MAINSRC(element);

    switch (change)
    {
        case GST_STATE_CHANGE_NULL_TO_READY:
        {
            GST_DEBUG("State change: NULL -> READY");

            if (self->device->dev == nullptr)
            {
                if (!gst_tcam_mainsrc_init_camera(self))
                {
                    GST_INFO("FAILURE to initialize device. Aborting...");
                    return GST_STATE_CHANGE_FAILURE;
                }
                self->device->all_caps = gst_helper::make_ptr(gst_tcam_mainsrc_get_all_camera_caps(self));
            }
            break;
        }
        case GST_STATE_CHANGE_READY_TO_PAUSED:
        {
            self->device->frame_count = 0;
            break;
        }
        default:
        {
            break;
        }
    }

    gst_element_set_locked_state(element, TRUE);
    ret = GST_ELEMENT_CLASS(gst_tcam_mainsrc_parent_class)->change_state(element, change);
    gst_element_set_locked_state(element, FALSE);

    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        return ret;
    }

    switch (change)
    {
        case GST_STATE_CHANGE_READY_TO_NULL:
        {
            if (self->device != nullptr)
            {
                // do not close camera, as a restart with the same device might be wanted
                gst_tcam_mainsrc_close_camera(self);
            }

            break;
        }
        default:
            break;
    }
    return ret;
}


static void buffer_destroy_callback(gpointer data)
{
    struct destroy_transfer* trans = (destroy_transfer*)data;

    if (!GST_IS_TCAM_MAINSRC(trans->self))
    {
        GST_ERROR("Received source is not valid.");
        delete trans;
        return;
    }

    GstTcamMainSrc* self = trans->self;
    std::unique_lock<std::mutex> lck(self->device->mtx);

    if (trans->ptr == nullptr)
    {
        GST_ERROR("Memory does not seem to exist.");
        delete trans;
        return;
    }

    if (self->device->sink)
    {
        self->device->sink->requeue_buffer(trans->ptr);
    }
    else
    {
        GST_ERROR("Unable to requeue buffer. Device is not open.");
    }

    delete trans;
}


static void gst_tcam_mainsrc_sh_callback(std::shared_ptr<tcam::ImageBuffer> buffer, void* data)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(data);
    GST_TRACE("sh callback");


    if (!self->device->is_running)
    {
        return;
    }

    std::unique_lock<std::mutex> lck(self->device->mtx);

    self->device->queue.push(buffer);

    self->device->cv.notify_all();

    lck.unlock();
}


static void statistics_to_gst_structure(const tcam::tcam_stream_statistics* statistics,
                                        GstStructure* struc)
{

    if (!statistics || !struc)
    {
        return;
    }

    gst_structure_set(struc,
                      "frame_count",
                      G_TYPE_UINT64,
                      statistics->frame_count,
                      "frames_dropped",
                      G_TYPE_UINT64,
                      statistics->frames_dropped,
                      "capture_time_ns",
                      G_TYPE_UINT64,
                      statistics->capture_time_ns,
                      "camera_time_ns",
                      G_TYPE_UINT64,
                      statistics->camera_time_ns,
                      "framerate",
                      G_TYPE_DOUBLE,
                      statistics->framerate,
                      "is_damaged",
                      G_TYPE_BOOLEAN,
                      statistics->is_damaged,
                      nullptr);
}


static GstFlowReturn gst_tcam_mainsrc_create(GstPushSrc* push_src, GstBuffer** buffer)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(push_src);

    std::shared_ptr<tcam::ImageBuffer> ptr;
    {
        std::unique_lock<std::mutex> lck(self->device->mtx);

        if (self->device->n_buffers != -1)  // #TODO look at this feature? Maybe remove it?
        {
            /*
              TODO: self->n_buffers should have same type as ptr->get_statistics().frame_count
            */
            if ( self->device->frame_count >= (guint)self->device->n_buffers )
            {
                GST_INFO_OBJECT( self, "Stopping stream after %llu buffers.", (long long unsigned int) (self->device->frame_count));
                return GST_FLOW_EOS;
            }
            self->device->frame_count++;
        }

        while( true )
        {
            // wait until new buffer arrives or stop waiting when we have to shut down
            self->device->cv.wait(lck);
            if (!self->device->is_running)
            {
                return GST_FLOW_EOS;
            }
            if (!self->device->queue.empty())
            {
                ptr = self->device->queue.front();
                self->device->queue.pop(); // remove buffer from queue

                break;
            }
        }
    }

    ptr->set_user_data(self);

    /* TODO: check why aravis throws an incomplete buffer error
       but the received images are still valid */
    // if (!tcam::is_image_buffer_complete(self->ptr))
    // {
    //     GST_DEBUG_OBJECT (self, "Received incomplete buffer. Returning to waiting position.");

    //     goto wait_again;
    // }

    destroy_transfer* trans = new destroy_transfer;
    trans->self = self;
    trans->ptr = ptr;

    *buffer = gst_buffer_new_wrapped_full(static_cast<GstMemoryFlags>(0),
                                          ptr->get_data(),
                                          ptr->get_buffer_size(),
                                          0,
                                          ptr->get_image_size(),
                                          trans,
                                          buffer_destroy_callback);

    gst_buffer_set_flags(*buffer, GST_BUFFER_FLAG_LIVE);

    // add meta statistics data to buffer
    {
        uint64_t gst_frame_count = self->element.parent.segment.position;
        auto stat = ptr->get_statistics();

        GstStructure* struc = gst_structure_new_empty("TcamStatistics");
        statistics_to_gst_structure(&stat, struc);

        auto meta = gst_buffer_add_tcam_statistics_meta(*buffer, struc);

        if (!meta)
        {
            GST_WARNING_OBJECT(self,"Unable to add meta !!!!");
        }
        else
        {
            // Disable this code when tracing is not enabled
            if( gst_debug_category_get_threshold( GST_CAT_DEFAULT ) >= GST_LEVEL_TRACE )
            {
                const char* damaged = nullptr;
                if (stat.is_damaged)
                {
                    damaged = "true";
                }
                else
                {
                    damaged = "false";
                }

                auto test = fmt::format("Added meta info: \n"
                                        "gst frame_count: {}\n"
                                        "backend frame_count {}\n"
                                        "frames_dropped {}\n"
                                        "capture_time_ns: {}\n"
                                        "camera_time_ns: {}\n"
                                        "framerate: {}\n"
                                        "is_damaged: {}\n",
                                        gst_frame_count,
                                        stat.frame_count,
                                        stat.frames_dropped,
                                        stat.capture_time_ns,
                                        stat.camera_time_ns,
                                        stat.framerate,
                                        damaged);

                GST_TRACE_OBJECT(self,"%s", test.c_str());
            }
        }

    } // end meta data

    return GST_FLOW_OK;
}


static GstCaps* gst_tcam_mainsrc_fixate_caps(GstBaseSrc* bsrc, GstCaps* caps)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(bsrc);

    GstStructure* structure;
    gint width = 0;
    gint height = 0;
    double frame_rate = 0.0;

    structure = gst_caps_get_structure(
        caps,
        0); // #TODO this seems to be at best curious, at another place we fixate to highest, and here fixate goes to lowest

    if (gst_structure_has_field(structure, "width"))
    {
        gst_structure_fixate_field_nearest_int(structure, "width", width);
    }
    if (gst_structure_has_field(structure, "height"))
    {
        gst_structure_fixate_field_nearest_int(structure, "height", height);
    }
    if (gst_structure_has_field(structure, "framerate"))
    {
        gst_structure_fixate_field_nearest_fraction(
            structure, "framerate", (double)(0.5 + frame_rate), 1);
    }

    GST_DEBUG_OBJECT(self, "Fixated caps to %s", gst_helper::to_string(*caps).c_str());

    return GST_BASE_SRC_CLASS(gst_tcam_mainsrc_parent_class)->fixate(bsrc, caps);
}

static gboolean gst_tcam_mainsrc_query(GstBaseSrc* bsrc, GstQuery* query)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(bsrc);
    gboolean res = FALSE;

    switch (GST_QUERY_TYPE(query))
    {
        case GST_QUERY_LATENCY:
        {
            GstClockTime min_latency;
            GstClockTime max_latency;

            /* device must be open */
            if (!self->device->dev)
            {
                GST_WARNING_OBJECT(self, "Can't give latency since device isn't open !");
                goto done;
            }

            /* we must have a framerate */
            if (self->fps_numerator <= 0 || self->fps_denominator <= 0)
            {
                GST_WARNING_OBJECT(self, "Can't give latency since framerate isn't fixated !");
                goto done;
            }

            /* min latency is the time to capture one frame/field */
            min_latency = gst_util_uint64_scale_int(GST_SECOND,
                                                    self->fps_denominator,
                                                    self->fps_numerator);

            /* max latency is set to NONE because cameras may enter trigger mode
               and not deliver images for an unspecified amount of time */
            max_latency = GST_CLOCK_TIME_NONE;

            GST_DEBUG_OBJECT(bsrc,
                             "report latency min %" GST_TIME_FORMAT " max %" GST_TIME_FORMAT,
                             GST_TIME_ARGS(min_latency),
                             GST_TIME_ARGS(max_latency));

            /* we are always live, the min latency is 1 frame and the max latency is
             * the complete buffer of frames. */
            gst_query_set_latency(query, TRUE, min_latency, max_latency);

            res = TRUE;
            break;
        }
        case GST_QUERY_CAPS:
        {
            GstCaps* query_caps;
            gst_query_parse_caps (query,
                                  &query_caps);

            if (!query_caps || gst_caps_is_empty(query_caps))
            {
                return GST_BASE_SRC_CLASS(gst_tcam_mainsrc_parent_class)->query(bsrc, query);
            }

            // these queries potentially require device interaction
            // no device means we return FALSE

            if (!self->device || !self->device->dev)
            {
                GST_ERROR("device must be open to answer query. Ensure element is in state READY or higher.");
                return FALSE;
            }

            if (gst_caps_is_fixed(query_caps))
            {
                //GST_ERROR("FIXED");

                GstStructure* structure = gst_caps_get_structure(query_caps, 0);

                if (!gst_structure_has_field(structure, "framerate"))
                {
                    int height = 0;
                    int width = 0;

                    gst_structure_get_int(structure, "width", &width);
                    gst_structure_get_int(structure, "height", &height);

                    const char* format_string = gst_structure_get_string(structure, "format");

                    uint32_t fourcc = tcam::gst::tcam_fourcc_from_gst_1_0_caps_string(gst_structure_get_name(structure),
                                                                           format_string);

                    auto format =
                        self->device->dev->get_available_video_formats();

                    GstCaps* result_caps = gst_caps_copy(query_caps);

                    int fps_min_num;
                    int fps_min_den;
                    int fps_max_num;
                    int fps_max_den;

                    for (auto& f : format)
                    {
                        if (f.get_fourcc() == fourcc)
                        {
                            auto fps = f.get_framerates({(uint32_t)width, (uint32_t)height});

                            gst_util_double_to_fraction(
                                *std::min_element(fps.begin(), fps.end()), &fps_min_num, &fps_min_den);
                            gst_util_double_to_fraction(
                                *std::max_element(fps.begin(), fps.end()), &fps_max_num, &fps_max_den);

                            GValue fps_val = G_VALUE_INIT;
                            g_value_init(&fps_val, GST_TYPE_FRACTION_RANGE);

                            gst_value_set_fraction_range_full(
                                &fps_val, fps_min_num, fps_min_den, fps_max_num, fps_max_den);

                            gst_caps_set_value(result_caps, "framerate", &fps_val);

                            gst_query_set_caps_result(query, result_caps);
                            return TRUE;
                        }

                    }


                }

            }
            return GST_BASE_SRC_CLASS(gst_tcam_mainsrc_parent_class)->query(bsrc, query);
        }
        default:
        {
            res = GST_BASE_SRC_CLASS(gst_tcam_mainsrc_parent_class)->query(bsrc, query);
            break;
        }
    }

done:

    return res;
}

static void gst_tcam_mainsrc_init(GstTcamMainSrc* self)
{
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);

    self->device = new device_state;
    self->device->n_buffers = -1;
    self->imagesink_buffers = 10;
    self->drop_incomplete_frames = TRUE;

    GST_INFO("Versions:\n\tTcam:\t%s\n\tAravis:\t%s", get_version(), get_aravis_version());
}


static void gst_tcam_mainsrc_finalize(GObject* object)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(object);

    gst_tcam_mainsrc_close_camera(self);

    if (self->device)
    {
        delete self->device;
        self->device = nullptr;
    }

    G_OBJECT_CLASS(gst_tcam_mainsrc_parent_class)->finalize(object);
}


static void gst_tcam_mainsrc_set_property(GObject* object,
                                          guint prop_id,
                                          const GValue* value,
                                          GParamSpec* pspec)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(object);

    switch (prop_id)
    {
        case PROP_SERIAL:
        {
            GstState state;
            gst_element_get_state(GST_ELEMENT(self), &state, NULL, 200);
            if (state == GST_STATE_NULL)
            {
                if (g_value_get_string(value) == NULL)
                {
                    self->device->device_serial.clear();
                }
                else
                {
                    self->device->device_serial = g_value_get_string(value);
                }

                GST_INFO("Set camera serial to %s", self->device->device_serial.c_str());
            }
            break;
        }
        case PROP_DEVICE_TYPE:
        {
            const char* type = g_value_get_string(value);

            // this check is simply for messaging the user
            // about invalid values
            auto vec = tcam::get_device_type_list_strings();
            if (std::find(vec.begin(), vec.end(), std::string(type)) == vec.end())
            {
                GST_ERROR("Unknown device type '%s'", type);
                self->device->device_type = tcam::TCAM_DEVICE_TYPE_UNKNOWN;
            }
            else
            {
                GST_DEBUG("Setting device type to %s", type);
                self->device->device_type = tcam::tcam_device_from_string(type);
            }
            break;
        }
        case PROP_CAM_BUFFERS:
        {
            GstState state;
            // timeout 1 second
            auto ret = gst_element_get_state(GST_ELEMENT(self), &state, nullptr, 1000000000);
            if (ret != GST_STATE_CHANGE_SUCCESS && state != GST_STATE_NULL)
            {
                GST_WARNING("camera-buffers can only be set while in GST_STATE_NULL.");
                break;
            }

            self->imagesink_buffers = g_value_get_int(value);
            break;
        }
        case PROP_NUM_BUFFERS:
        {
            self->device->n_buffers = g_value_get_int(value);
            break;
        }
        case PROP_DROP_INCOMPLETE_FRAMES:
        {
            self->drop_incomplete_frames = g_value_get_boolean(value);
            if (self->device->sink)
            {
                self->device->sink->drop_incomplete_frames(self->drop_incomplete_frames);
            }
            break;
        }
        case PROP_STATE:
        {
            // bool state = tcam::gst::load_device_settings(
            //     TCAM_PROP(self), self->device->device_serial, g_value_get_string(value));
            bool state = true;
            if (!state)
            {
                GST_WARNING("Device may be in an undefined state.");
            }
            break;
        }
        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
        }
    }
}


static void gst_tcam_mainsrc_get_property(GObject* object,
                                          guint prop_id,
                                          GValue* value,
                                          GParamSpec* pspec)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(object);

    switch (prop_id)
    {
        case PROP_SERIAL:
        {
            g_value_set_string(value, self->device->device_serial.c_str());
            break;
        }
        case PROP_DEVICE_TYPE:
        {
            g_value_set_string(value, tcam::tcam_device_type_to_string(self->device->device_type).c_str());

            break;
        }
        case PROP_CAM_BUFFERS:
        {
            g_value_set_int(value, self->imagesink_buffers);
            break;
        }
        case PROP_NUM_BUFFERS:
        {
            g_value_set_int(value, self->device->n_buffers);
            break;
        }
        case PROP_DROP_INCOMPLETE_FRAMES:
        {
            g_value_set_boolean(value, self->drop_incomplete_frames);
            break;
        }
        case PROP_STATE:
        {
            if (!self->device->device_serial.empty())
            {
                std::string bla = "";
                //tcam::gst::create_device_settings(self->device->device_serial, TCAM_PROP(self)).c_str();
                g_value_set_string(value, bla.c_str());
            }
            else
            {
                g_value_set_string(value, "");
            }
            break;
        }
        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
        }
    }
}


static gboolean gst_tcam_mainsrc_unlock(GstBaseSrc* src)
{
    GstTcamMainSrc* self = GST_TCAM_MAINSRC(src);

    std::lock_guard<std::mutex> lck(self->device->mtx);
    self->device->is_running = false;
    self->device->cv.notify_all();

    return TRUE;
}


static void gst_tcam_mainsrc_class_init(GstTcamMainSrcClass* klass)
{
    GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass* gstbasesrc_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass* gstpushsrc_class = GST_PUSH_SRC_CLASS(klass);

    gobject_class->finalize = gst_tcam_mainsrc_finalize;
    gobject_class->set_property = gst_tcam_mainsrc_set_property;
    gobject_class->get_property = gst_tcam_mainsrc_get_property;

    g_object_class_install_property(
        gobject_class,
        PROP_SERIAL,
        g_param_spec_string("serial",
                            "Camera serial",
                            "Serial of the camera",
                            NULL,
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(
        gobject_class,
        PROP_DEVICE_TYPE,
        g_param_spec_string("type",
                            "Camera type",
                            "type/backend of the camera",
                            "auto",
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(
        gobject_class,
        PROP_CAM_BUFFERS,
        g_param_spec_int("camera-buffers",
                         "Number of Buffers",
                         "Number of buffers to use for retrieving images",
                         1,
                         256,
                         GST_TCAM_MAINSRC_DEFAULT_N_BUFFERS,
                         static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
        gobject_class,
        PROP_NUM_BUFFERS,
        g_param_spec_int("num-buffers",
                         "Number of Buffers",
                         "Number of buffers to send before ending pipeline (-1 = unlimited)",
                         -1,
                         G_MAXINT,
                         -1,
                         static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(
        gobject_class,
        PROP_DROP_INCOMPLETE_FRAMES,
        g_param_spec_boolean("drop-incomplete-buffer",
                             "Drop incomplete buffers",
                             "Drop buffer that are incomplete.",
                             true,
                             static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
                                                      | G_PARAM_CONSTRUCT)));


    g_object_class_install_property(
        gobject_class,
        PROP_STATE,
        g_param_spec_string("state",
                            "Property State",
                            "Property values the internal elements shall use",
                            "",
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_tcammainsrc_signals[SIGNAL_DEVICE_OPEN] = g_signal_new("device-open",
                                                               G_TYPE_FROM_CLASS(klass),
                                                               G_SIGNAL_RUN_LAST,
                                                               0,
                                                               nullptr, nullptr, nullptr,
                                                               G_TYPE_NONE, 0, G_TYPE_NONE);
    gst_tcammainsrc_signals[SIGNAL_DEVICE_CLOSE] = g_signal_new("device-close",
                                                                G_TYPE_FROM_CLASS(klass),
                                                                G_SIGNAL_RUN_LAST,
                                                                0,
                                                                nullptr, nullptr, nullptr,
                                                                G_TYPE_NONE, 0, G_TYPE_NONE);

    GST_DEBUG_CATEGORY_INIT(tcam_mainsrc_debug, "tcammainsrc", 0, "tcam interface");

    gst_element_class_set_static_metadata(element_class,
                                          "Tcam Video Source",
                                          "Source/Video",
                                          "Tcam based source",
                                          "The Imaging Source <support@theimagingsource.com>");

    gst_element_class_add_pad_template(element_class,
                                       gst_static_pad_template_get(&tcam_mainsrc_template));

    element_class->change_state = gst_tcam_mainsrc_change_state;

    gstbasesrc_class->get_caps = gst_tcam_mainsrc_get_caps;
    gstbasesrc_class->set_caps = gst_tcam_mainsrc_set_caps;
    gstbasesrc_class->fixate = gst_tcam_mainsrc_fixate_caps;
    gstbasesrc_class->unlock = gst_tcam_mainsrc_unlock;
    gstbasesrc_class->negotiate = gst_tcam_mainsrc_negotiate;
    gstbasesrc_class->query = gst_tcam_mainsrc_query;

    gstpushsrc_class->create = gst_tcam_mainsrc_create;
}
