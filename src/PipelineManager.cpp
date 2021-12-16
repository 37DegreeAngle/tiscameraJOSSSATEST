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

#include "PipelineManager.h"

#include "internal.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

using namespace tcam;

PipelineManager::PipelineManager() : status(TCAM_PIPELINE_UNDEFINED), current_ppl_buffer(0) {}


PipelineManager::~PipelineManager()
{
    if (status == TCAM_PIPELINE_PLAYING)
    {
        stop_playing();
    }

    if (m_pipeline_thread.joinable())
    {
        m_pipeline_thread.join();
    }

    available_filter.clear();
    filter_pipeline.clear();
}


std::vector<VideoFormatDescription> PipelineManager::getAvailableVideoFormats() const
{
    return available_output_formats;
}


bool PipelineManager::setVideoFormat(const VideoFormat& f)
{
    this->output_format = f;
    return true;
}


VideoFormat PipelineManager::getVideoFormat() const
{
    return this->output_format;
}


bool PipelineManager::set_status(TCAM_PIPELINE_STATUS s)
{
    if (status == s)
        return true;

    this->status = s;

    if (status == TCAM_PIPELINE_PLAYING)
    {
        if (create_pipeline())
        {
            start_playing();
        }
        else
        {
            status = TCAM_PIPELINE_ERROR;
            return false;
        }
    }
    else if (status == TCAM_PIPELINE_STOPPED)
    {
        stop_playing();
    }

    return true;
}


TCAM_PIPELINE_STATUS PipelineManager::get_status() const
{
    return status;
}


bool PipelineManager::destroyPipeline()
{
    set_status(TCAM_PIPELINE_STOPPED);

    source = nullptr;
    sink = nullptr;

    return true;
}


bool PipelineManager::setSource(std::shared_ptr<DeviceInterface> device)
{
    if (status == TCAM_PIPELINE_PLAYING || status == TCAM_PIPELINE_PAUSED)
    {
        return false;
    }

    available_input_formats = device->get_available_video_formats();

    distributeProperties();

    this->source = std::make_shared<ImageSource>();
    source->setSink(shared_from_this());

    source->setDevice(device);

    property_filter = std::make_shared<tcam::stream::filter::PropertyFilter>(
        device->get_properties(), available_input_formats);

    available_output_formats = available_input_formats;

    if (available_output_formats.empty())
    {
        SPDLOG_ERROR("No output formats available.");
        return false;
    }

    return true;
}


std::shared_ptr<ImageSource> PipelineManager::getSource()
{
    return source;
}


bool PipelineManager::setSink(std::shared_ptr<SinkInterface> s)
{
    if (status == TCAM_PIPELINE_PLAYING || status == TCAM_PIPELINE_PAUSED)
    {
        return false;
    }

    this->sink = s;

    this->sink->set_source(shared_from_this());

    return true;
}


std::shared_ptr<SinkInterface> PipelineManager::getSink()
{
    return sink;
}


void PipelineManager::distributeProperties()
{
    //for (auto& f : available_filter) { f->setDeviceProperties(device_properties); }
}


static bool isFilterApplicable(uint32_t fourcc, const std::vector<uint32_t>& vec)
{
    if (std::find(vec.begin(), vec.end(), fourcc) == vec.end())
    {
        return false;
    }
    return true;
}


void PipelineManager::create_input_format(uint32_t fourcc)
{
    input_format = output_format;
    input_format.set_fourcc(fourcc);
}


std::vector<uint32_t> PipelineManager::getDeviceFourcc()
{
    // for easy usage we create a vector<fourcc> for avail. inputs
    std::vector<uint32_t> device_fourcc;

    for (const auto& v : available_input_formats) { device_fourcc.push_back(v.get_fourcc()); }
    return device_fourcc;
}


bool PipelineManager::set_source_status(TCAM_PIPELINE_STATUS _status)
{
    if (source == nullptr)
    {
        SPDLOG_ERROR("Source is not defined");
        return false;
    }

    if (!source->set_status(_status))
    {
        SPDLOG_ERROR("Source did not accept status change");
        return false;
    }

    return true;
}


bool PipelineManager::set_sink_status(TCAM_PIPELINE_STATUS _status)
{
    if (sink == nullptr)
    {
        if (_status
            != TCAM_PIPELINE_STOPPED) // additional check to prevent warning when pipeline comes up
        {
            SPDLOG_WARN("Sink is not defined.");
        }
        return false;
    }

    if (!sink->set_status(_status))
    {
        SPDLOG_ERROR("Sink spewed error");
        return false;
    }

    return true;
}


bool PipelineManager::validate_pipeline()
{
    // check if pipeline is valid
    if (source.get() == nullptr || sink.get() == nullptr)
    {
        return false;
    }

    // check source format
    auto in_format = source->getVideoFormat();

    if (in_format != this->input_format)
    {
        SPDLOG_DEBUG("Video format in source does not match pipeline: '{}' != '{}'",
                     in_format.to_string().c_str(),
                     input_format.to_string().c_str());
        return false;
    }

    VideoFormat in;
    VideoFormat out;
    for (auto f : filter_pipeline)
    {

        f->getVideoFormat(in, out);

        if (in != in_format)
        {
            SPDLOG_ERROR("Ingoing video format for filter {} is not compatible with previous "
                         "element. '{}' != '{}'",
                         f->getDescription().name.c_str(),
                         in_format.to_string().c_str(),
                         in.to_string().c_str());
            return false;
        }
        else
        {
            SPDLOG_DEBUG("Filter {} connected to pipeline -- {}",
                         f->getDescription().name.c_str(),
                         out.to_string().c_str());
            // save output for next comparison
            in_format = out;
        }
    }

    if (in_format != this->output_format)
    {
        SPDLOG_ERROR("Video format in sink does not match pipeline '{}' != '{}'",
                     in_format.to_string().c_str(),
                     output_format.to_string().c_str());
        return false;
    }

    return true;
}


bool PipelineManager::create_conversion_pipeline()
{
    if (source.get() == nullptr || sink.get() == nullptr)
    {
        return false;
    }

    auto device_fourcc = getDeviceFourcc();
    create_input_format(output_format.get_fourcc());

    for (auto f : available_filter)
    {
        std::string s = f->getDescription().name;

        if (f->getDescription().type == FILTER_TYPE_CONVERSION)
        {

            if (isFilterApplicable(output_format.get_fourcc(), f->getDescription().output_fourcc))
            {
                bool filter_valid = false;
                uint32_t fourcc_to_use = 0;
                for (const auto& cc : device_fourcc)
                {
                    if (isFilterApplicable(cc, f->getDescription().input_fourcc))
                    {
                        filter_valid = true;
                        fourcc_to_use = cc;
                        break;
                    }
                }

                // set device format to use correct fourcc
                create_input_format(fourcc_to_use);

                if (filter_valid)
                {
                    if (f->setVideoFormat(input_format, output_format))
                    {
                        SPDLOG_DEBUG("Added filter \"{}\" to pipeline", s.c_str());
                        filter_pipeline.push_back(f);
                    }
                    else
                    {
                        SPDLOG_DEBUG("Filter {} did not accept format settings", s.c_str());
                    }
                }
                else
                {
                    SPDLOG_DEBUG("Filter {} does not use the device output formats.", s.c_str());
                }
            }
            else
            {
                SPDLOG_DEBUG("Filter {} is not applicable", s.c_str());
            }
        }
    }
    return true;
}


bool PipelineManager::add_interpretation_filter()
{

    // if a valid pipeline can be created insert additional filter (e.g. autoexposure)
    // interpretations should be done as early as possible in the pipeline

    for (auto& f : available_filter)
    {
        if (f->getDescription().type == FILTER_TYPE_INTERPRET)
        {
            std::string s = f->getDescription().name;
            // applicable to sink
            bool all_formats = false;
            if (f->getDescription().input_fourcc.size() == 1)
            {
                if (f->getDescription().input_fourcc.at(0) == 0)
                {
                    all_formats = true;
                }
            }

            if (all_formats
                || isFilterApplicable(input_format.get_fourcc(), f->getDescription().input_fourcc))
            {
                SPDLOG_DEBUG("Adding filter '{}' after source", s.c_str());
                f->setVideoFormat(input_format, input_format);
                filter_pipeline.insert(filter_pipeline.begin(), f);
                continue;
            }
            else
            {
                SPDLOG_DEBUG("Filter '{}' not usable after source", s.c_str());
            }

            if (f->setVideoFormat(input_format, input_format))
            {
                continue;
            }
        }
    }
    return true;
}


bool PipelineManager::create_pipeline()
{
    if (source.get() == nullptr || sink.get() == nullptr)
    {
        return false;
    }

    // assure everything is in a defined state
    filter_pipeline.clear();

    if (!create_conversion_pipeline())
    {
        SPDLOG_ERROR("Unable to determine conversion pipeline.");
        return false;
    }

    if (!source->setVideoFormat(input_format))
    {
        SPDLOG_ERROR("Unable to set video format in source.");
        return false;
    }

    if (!sink->setVideoFormat(output_format))
    {
        SPDLOG_ERROR("Unable to set video format in sink.");
        return false;
    }

    if (!source->set_buffer_collection(sink->get_buffer_collection()))
    {
        SPDLOG_ERROR("Unable to set buffer collection.");
        return false;
    }

    SPDLOG_INFO("Pipeline creation successful.");

    std::string ppl = "source -> ";
    property_filter->setVideoFormat(output_format, output_format);
    for (const auto& f : filter_pipeline)
    {
        ppl += f->getDescription().name;
        ppl += " -> ";
    }
    ppl += " sink";
    SPDLOG_INFO("{}", ppl.c_str());

    return true;
}


bool PipelineManager::start_playing()
{

    if (!set_sink_status(TCAM_PIPELINE_PLAYING))
    {
        SPDLOG_ERROR("Sink refused to change to state PLAYING");
        goto error;
    }

    if (!set_source_status(TCAM_PIPELINE_PLAYING))
    {
        SPDLOG_ERROR("Source refused to change to state PLAYING");
        goto error;
    }

    property_filter->setStatus(TCAM_PIPELINE_PLAYING);
    status = TCAM_PIPELINE_PLAYING;

    m_pipeline_thread = std::thread(&PipelineManager::run_pipeline, this);

    return true;

error:
    stop_playing();
    return false;
}


bool PipelineManager::stop_playing()
{
    status = TCAM_PIPELINE_STOPPED;
    m_cv.notify_all();

    if (!set_source_status(TCAM_PIPELINE_STOPPED))
    {
        SPDLOG_ERROR("Source refused to change to state STOP");
        return false;
    }

    for (auto& f : filter_pipeline)
    {
        if (!f->setStatus(TCAM_PIPELINE_STOPPED))
        {
            SPDLOG_ERROR("Filter {} refused to change to state STOP",
                         f->getDescription().name.c_str());
            return false;
        }
    }

    set_sink_status(TCAM_PIPELINE_STOPPED);
    property_filter->setStatus(TCAM_PIPELINE_STOPPED);

    if (m_pipeline_thread.joinable())
    {
        m_pipeline_thread.join();
    }
    //destroyPipeline();

    return true;
}


void PipelineManager::push_image(std::shared_ptr<ImageBuffer> buffer)
{
    if (status == TCAM_PIPELINE_STOPPED)
    {
        return;
    }

    std::scoped_lock lock(m_mtx);

    m_entry_queue.push(buffer);

    m_cv.notify_all();
}


void PipelineManager::requeue_buffer(std::shared_ptr<ImageBuffer> buffer)
{
    if (source)
    {
        source->requeue_buffer(buffer);
    }
}


std::vector<std::shared_ptr<ImageBuffer>> PipelineManager::get_buffer_collection()
{
    return std::vector<std::shared_ptr<ImageBuffer>>();
}


void PipelineManager::drop_incomplete_frames(bool drop_them)
{
    if (source)
    {
        source->drop_incomplete_frames(drop_them);
    }
}


bool PipelineManager::should_incomplete_frames_be_dropped() const
{
    if (source)
    {
        return source->should_incomplete_frames_be_dropped();
    }

    SPDLOG_ERROR("No shource to ask if incomplete frames should be dropped.");
    return true;
}


std::vector<std::shared_ptr<tcam::property::IPropertyBase>> PipelineManager::get_properties()
{
    return property_filter->getProperties();
}


void PipelineManager::run_pipeline()
{
    tcam::set_thread_name("tcam_pipeline");
    while (true)
    {
        std::shared_ptr<tcam::ImageBuffer> current_buffer;

        {
            std::unique_lock lock(m_mtx);

            while (status == TCAM_PIPELINE_PLAYING && m_entry_queue.empty())
            {
                m_cv.wait_for(lock, std::chrono::milliseconds(500));
            }

            if (status != TCAM_PIPELINE_PLAYING)
            {
                SPDLOG_INFO("Pipeline not in playing. Stopping pipeline thread.");
                return;
            }

            if (m_entry_queue.empty())
            {
                SPDLOG_ERROR("Buffer queue is empty. Returning to waiting position.");

                continue;
            }

            // SPDLOG_DEBUG("Working on new image");
            current_buffer = m_entry_queue.front();
            m_entry_queue.pop(); // remove buffer from queue
        }

        property_filter->apply(current_buffer);

        if (sink != nullptr)
        {
            sink->push_image(current_buffer);
        }
        else
        {
            SPDLOG_ERROR("Sink is NULL");
        }
    }
}
