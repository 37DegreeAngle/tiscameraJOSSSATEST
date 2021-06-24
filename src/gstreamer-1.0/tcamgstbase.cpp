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

#include "tcamgstbase.h"

#include "base_types.h"
#include <dutils_img/fcc_to_string.h>
#include <dutils_img/image_fourcc_func.h>
#include "logging.h"
#include "public_utils.h"
#include "tcamgststrings.h"

#include <map>
#include <algorithm> //std::find
#include <stddef.h> // NULL
#include <string.h> // strcmp


std::pair<std::string, std::string> separate_serial_and_type(const std::string& input)
{
    auto pos = input.find("-");

    if (pos != std::string::npos)
    {
        std::string tmp1 = input.substr(0, pos);
        std::string tmp2 = input.substr(pos + 1);

        return std::make_pair(tmp1, tmp2);
    }
    return std::make_pair(input, std::string {});
}


bool separate_serial_and_type(const std::string& input, std::string& serial, std::string& type)
{
    auto pos = input.find("-");

    if (pos != std::string::npos)
    {
        // assign to tmp variables
        // input could be self->device_serial
        // overwriting it would ivalidate input for
        // device_type retrieval
        std::string tmp1 = input.substr(0, pos);
        std::string tmp2 = input.substr(pos + 1);

        serial = tmp1;
        type = tmp2;

        return true;
    }
    else
    {
        serial = input;
    }
    return false;
}


typedef struct tcam_src_element_
{
    std::string name;
    // name so because function `g_type_name` exists
    std::string g_type_name_str;
    std::vector<TCAM_DEVICE_TYPE> type;
} tcam_src_element;


std::vector<tcam_src_element> get_possible_sources ()
{
    std::vector<tcam_src_element> ret;

    ret.push_back({"tcammainsrc", "GstTcamMainSrc", {TCAM_DEVICE_TYPE_V4L2, TCAM_DEVICE_TYPE_ARAVIS, TCAM_DEVICE_TYPE_LIBUSB}});
    ret.push_back({"tcamtegrasrc", "GstTcamTegraSrc", {TCAM_DEVICE_TYPE_TEGRA}});
    ret.push_back({"tcampimipisrc", "GstTcamPiMipiSrc", {TCAM_DEVICE_TYPE_PIMIPI}});
    ret.push_back({"tcamsrc", "GstTcamSrc", {TCAM_DEVICE_TYPE_V4L2, TCAM_DEVICE_TYPE_ARAVIS, TCAM_DEVICE_TYPE_LIBUSB, TCAM_DEVICE_TYPE_TEGRA, TCAM_DEVICE_TYPE_PIMIPI}});

    return ret;
}


std::vector<std::string> get_source_element_factory_names()
{
    auto sources = get_possible_sources();

    std::vector<std::string> ret;

    ret.reserve(sources.size());

    for (const auto& s : sources)
    {
        ret.push_back(s.g_type_name_str);
    }

    return ret;
}


GstElement* tcam_gst_find_camera_src_rec (GstElement* element, const std::vector<std::string>& factory_names)
{
    GstPad* orig_pad = gst_element_get_static_pad(element, "sink");

    GstPad* src_pad = gst_pad_get_peer(orig_pad);
    gst_object_unref(orig_pad);

    if (!src_pad)
    {
        // this means we have reached a dead end where no valid tcamsrc exists
        return nullptr;
    }

    GstElement* el = gst_pad_get_parent_element(src_pad);

    gst_object_unref(src_pad);

    std::string name = g_type_name(gst_element_factory_get_element_type(gst_element_get_factory(el)));

    if (std::find(factory_names.begin(), factory_names.end(), name) != factory_names.end())
    {
        return el;
    }

    GstElement* ret = tcam_gst_find_camera_src_rec(el, factory_names);

    gst_object_unref(el);

    return ret;
}


GstElement* tcam_gst_find_camera_src (GstElement* element)
{
    std::vector<std::string> factory_names = get_source_element_factory_names();

    return tcam_gst_find_camera_src_rec(element, factory_names);
}


std::string get_plugin_version (const char* plugin_name)
{
    GstPlugin* plugin = gst_plugin_load_by_name(plugin_name);
    if (plugin == nullptr)
    {
        return {};
    }

    std::string rval;
    const char* version_str = gst_plugin_get_version(plugin);
    if (version_str != nullptr)
    {
        rval = version_str;
    }

    gst_object_unref(plugin);

    return rval;
}


std::vector<std::string> tcam_helper::gst_consume_GSList_to_vector(GSList* lst)
{
    if (lst == nullptr)
    {
        return {};
    }

    std::vector<std::string> rval;
    GSList* iter = lst;
    do {
        char* str = static_cast<char*>(iter->data);

        rval.push_back(str);

        ::g_free(str);

        iter = g_slist_next(iter);
    } while (iter != nullptr);

    g_slist_free(lst);

    return rval;
}


static std::vector<std::string> gst_list_to_vector(const GValue* gst_list)
{
    std::vector<std::string> ret;
    if (!GST_VALUE_HOLDS_LIST(gst_list))
    {
        SPDLOG_ERROR("Given GValue is not a list.");
        return ret;
    }

    for (unsigned int x = 0; x < gst_value_list_get_size(gst_list); ++x)
    {
        const GValue* val = gst_value_list_get_value(gst_list, x);

        if (G_VALUE_TYPE(val) == G_TYPE_STRING)
        {

            ret.push_back(g_value_get_string(val));
        }
        else
        {
            SPDLOG_ERROR("NOT IMPLEMENTED. TYPE CAN NOT BE INTERPRETED");
        }
    }

    return ret;
}


bool tcam_gst_raw_only_has_mono(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    auto correct_format = [](const char* str) {
        if (str == nullptr)
        {
            return false;
        }

        static const char* formats[] = { "GRAY8",   "GRAY16_LE", "GRAY16_BE", "GRAY12p",
                                         "GRAY10p", "GRAY12m",   "GRAY10m" };

        return std::any_of(std::begin(formats), std::end(formats), [str](const char* op2) {
            return strcmp(str, op2) == 0;
        });
    };

    for (unsigned int i = 0; i < gst_caps_get_size(caps); ++i)
    {
        GstStructure* struc = gst_caps_get_structure(caps, i);

        if (strcmp("video/x-raw", gst_structure_get_name(struc)) == 0)
        {
            if (gst_structure_has_field(struc, "format"))
            {
                if (gst_structure_get_field_type(struc, "format") == G_TYPE_STRING)
                {
                    if (!correct_format(gst_structure_get_string(struc, "format")))
                    {
                        return false;
                    }
                }
                else if (gst_structure_get_field_type(struc, "format") == GST_TYPE_LIST)
                {
                    auto vec = gst_list_to_vector(gst_structure_get_value(struc, "format"));

                    for (const auto& fmt : vec)
                    {
                        if (!correct_format(fmt.c_str()))
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    SPDLOG_ERROR("Cannot handle format type in GstStructure.");
                }
            }
            else
            {
                // since raw can be anything
                // do not assume it is gray but color
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    return true;
}


static bool tcam_gst_is_fourcc_bayer(const uint32_t fourcc)
{
    if (fourcc == FOURCC_GBRG8 || fourcc == FOURCC_GRBG8 || fourcc == FOURCC_RGGB8
        || fourcc == FOURCC_BGGR8)
    {
        return TRUE;
    }
    return FALSE;
}


static bool tcam_gst_is_bayer10_fourcc (const uint32_t fourcc)
{
   if (fourcc == FOURCC_GBRG10
       || fourcc == FOURCC_GRBG10
       || fourcc == FOURCC_RGGB10
       || fourcc == FOURCC_BGGR10)
   {
       return TRUE;
   }
   return FALSE;
}


static bool tcam_gst_is_bayer10_packed_fourcc (const uint32_t fourcc)
{
   if (fourcc == FOURCC_GBRG10_SPACKED
       || fourcc == FOURCC_GRBG10_SPACKED
       || fourcc == FOURCC_RGGB10_SPACKED
       || fourcc == FOURCC_BGGR10_SPACKED
       || fourcc == FOURCC_GBRG10_MIPI_PACKED
       || fourcc == FOURCC_GRBG10_MIPI_PACKED
       || fourcc == FOURCC_RGGB10_MIPI_PACKED
       || fourcc == FOURCC_BGGR10_MIPI_PACKED)
   {
       return TRUE;
   }
   return FALSE;
}


static bool tcam_gst_is_bayer12_fourcc (const uint32_t fourcc)
{
    if (fourcc == FOURCC_GBRG12
        || fourcc == FOURCC_GRBG12
        || fourcc == FOURCC_RGGB12
        || fourcc == FOURCC_BGGR12)
    {
        return TRUE;
    }
    return FALSE;
}


static bool tcam_gst_is_bayer12_packed_fourcc(const uint32_t fourcc)
{
    if (fourcc == FOURCC_GBRG12_MIPI_PACKED || fourcc == FOURCC_GRBG12_MIPI_PACKED
        || fourcc == FOURCC_RGGB12_MIPI_PACKED || fourcc == FOURCC_BGGR12_MIPI_PACKED
        || fourcc == FOURCC_GBRG12_SPACKED || fourcc == FOURCC_GRBG12_SPACKED
        || fourcc == FOURCC_RGGB12_SPACKED || fourcc == FOURCC_BGGR12_SPACKED
        || fourcc == FOURCC_GBRG12_PACKED || fourcc == FOURCC_GRBG12_PACKED
        || fourcc == FOURCC_RGGB12_PACKED || fourcc == FOURCC_BGGR12_PACKED)
    {
        return TRUE;
    }
    return FALSE;
}


static bool tcam_gst_is_bayer16_fourcc(const uint32_t fourcc)
{
    if (fourcc == FOURCC_GBRG16 || fourcc == FOURCC_GRBG16 || fourcc == FOURCC_RGGB16
        || fourcc == FOURCC_BGGR16)
    {
        return TRUE;
    }
    return FALSE;
}

static bool tcam_gst_is_fourcc_yuv(const uint32_t fourcc)
{
    if (fourcc == FOURCC_YUY2 || fourcc == FOURCC_UYVY
        //|| fourcc == FOURCC_I420
        //|| fourcc == FOURCC_YV16
        || fourcc == FOURCC_IYU1 || fourcc == FOURCC_IYU2
        || fourcc == FOURCC_Y411 || fourcc == FOURCC_NV12)
    {
        return true;
    }
    return false;
}


bool tcam_gst_is_bayer8_string(const char* format_string)
{
    if (format_string == nullptr)
    {
        return false;
    }

    if (strcmp(format_string, "gbrg") == 0 || strcmp(format_string, "grbg") == 0
        || strcmp(format_string, "rggb") == 0 || strcmp(format_string, "bggr") == 0)
    {
        return true;
    }

    return false;
}


bool tcam_gst_is_bayer10_string(const char* format_string)
{
    if (format_string == nullptr)
    {
        return false;
    }

    if (strncmp(format_string, "gbrg10", strlen("gbrg10")) == 0
        || strncmp(format_string, "grbg10", strlen("grbg10")) == 0
        || strncmp(format_string, "rggb10", strlen("rggb10")) == 0
        || strncmp(format_string, "bggr10", strlen("bggr10")) == 0)
    {
        return true;
    }

    return false;
}


bool tcam_gst_is_bayer10_packed_string(const char* format_string)
{
    if (format_string == nullptr)
    {
        return false;
    }

    static const std::array<std::string, 12> format_list = {
        "rggb10p", "grbg10p", "gbrg10p", "bggr10p", "rggb10s", "grbg10s",
        "gbrg10s", "bggr10s", "rggb10m", "grbg10m", "gbrg10m", "bggr10m",
    };

    if (std::find(format_list.begin(), format_list.end(), format_string) != format_list.end())
    {
        return true;
    }

    return false;
}


bool tcam_gst_is_bayer12_string(const char* format_string)
{
    if (format_string == nullptr)
    {
        return false;
    }

    if (strncmp(format_string, "gbrg12", strlen("gbrg12")) == 0
        || strncmp(format_string, "grbg12", strlen("grbg12")) == 0
        || strncmp(format_string, "rggb12", strlen("rggb12")) == 0
        || strncmp(format_string, "bggr12", strlen("bggr12")) == 0)
    {
        return true;
    }

    return false;
}


bool tcam_gst_is_bayer12_packed_string(const char* format_string)
{
    if (format_string == nullptr)
    {
        return false;
    }

    static const std::array<std::string, 12> format_list = {
        "rggb12p", "grbg12p", "gbrg12p", "bggr12p", "rggb12s", "grbg12s",
        "gbrg12s", "bggr12s", "rggb12m", "grbg12m", "gbrg12m", "bggr12m",
    };

    if (std::find(format_list.begin(), format_list.end(), format_string) != format_list.end())
    {
        return true;
    }

    return false;
}


bool tcam_gst_is_bayer16_string(const char* format_string)
{
    if (format_string == nullptr)
    {
        return false;
    }

    if (strcmp(format_string, "gbrg16") == 0 || strcmp(format_string, "grbg16") == 0
        || strcmp(format_string, "rggb16") == 0 || strcmp(format_string, "bggr16") == 0)
    {
        return true;
    }

    return false;
}


bool tcam_gst_is_fourcc_rgb(const unsigned int fourcc)
{
    if (fourcc == GST_MAKE_FOURCC('R', 'G', 'B', 'x')
        || fourcc == GST_MAKE_FOURCC('x', 'R', 'G', 'B')
        || fourcc == GST_MAKE_FOURCC('B', 'G', 'R', 'x')
        || fourcc == GST_MAKE_FOURCC('x', 'B', 'G', 'R')
        || fourcc == GST_MAKE_FOURCC('R', 'G', 'B', 'A')
        || fourcc == GST_MAKE_FOURCC('A', 'R', 'G', 'B')
        || fourcc == GST_MAKE_FOURCC('B', 'G', 'R', 'A')
        || fourcc == GST_MAKE_FOURCC('A', 'B', 'G', 'R') || fourcc == FOURCC_BGR24
        || fourcc == FOURCC_BGRA32 || fourcc == FOURCC_BGRA64)
    {
        return TRUE;
    }

    return FALSE;
}

bool tcam_gst_is_bayerpwl_fourcc(const unsigned int fourcc)
{
    if (fourcc == FOURCC_PWL_RG12_MIPI
        || fourcc == FOURCC_PWL_RG12
        || fourcc == FOURCC_PWL_RG16H12)
    {
        return true;
    }
    return false;
}

bool tcam_gst_is_polarized_mono(const unsigned int fourcc)
{
    if (fourcc == FOURCC_POLARIZATION_MONO8_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_MONO16_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_MONO12_SPACKED_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_MONO12_PACKED_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_ADI_PLANAR_MONO8
        || fourcc == FOURCC_POLARIZATION_ADI_PLANAR_MONO16
        || fourcc == FOURCC_POLARIZATION_ADI_MONO8 || fourcc == FOURCC_POLARIZATION_ADI_MONO16
        || fourcc == FOURCC_POLARIZATION_PACKED8 || fourcc == FOURCC_POLARIZATION_PACKED16)
    {
        return true;
    }

    return false;
}


bool tcam_gst_is_polarized_bayer(const unsigned int fourcc)
{
    if (fourcc == FOURCC_POLARIZATION_BG8_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_BG16_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_BG12_SPACKED_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_BG12_PACKED_90_45_135_0
        || fourcc == FOURCC_POLARIZATION_PACKED8_BAYER_BG
        || fourcc == FOURCC_POLARIZATION_PACKED16_BAYER_BG)
    {
        return true;
    }

    return false;
}


static bool tcam_gst_fixate_caps(GstCaps* caps)
{
    if (caps == nullptr || gst_caps_is_empty(caps) || gst_caps_is_any(caps))
    {
        return FALSE;
    }

    GstStructure* structure = gst_caps_get_structure(caps, 0);

    if (gst_structure_has_field(structure, "width"))
    {
        gst_structure_fixate_field_nearest_int(structure, "width", G_MAXINT);
    }
    if (gst_structure_has_field(structure, "height"))
    {
        gst_structure_fixate_field_nearest_int(structure, "height", G_MAXINT);
    }
    if (gst_structure_has_field(structure, "framerate"))
    {
        gst_structure_fixate_field_nearest_fraction(structure, "framerate", G_MAXINT, 1);
    }

    return TRUE;
}


static void gst_caps_change_name(GstCaps* caps, const char* name)
{
    for (unsigned int i = 0; i < gst_caps_get_size(caps); ++i)
    {
        GstStructure* struc = gst_caps_get_structure(caps, i);

        if (struc != nullptr)
        {
            gst_structure_set_name(struc, name);
            gst_structure_remove_field(struc, "format");
        }
    }
}

bool gst_caps_are_bayer_only(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    for (unsigned int i = 0; i < gst_caps_get_size(caps); ++i)
    {
        GstStructure* struc = gst_caps_get_structure(caps, i);

        if (strcmp(gst_structure_get_name(struc), "video/x-bayer") != 0)
        {
            return false;
        }
    }
    return true;
}

static bool is_really_empty_caps(const GstCaps* caps)
{
    /*
gst_caps_is_empty acts erratic, thus we work around the issue with gst_caps_to_string:

--------
(gdb) print (char*)gst_caps_to_string (caps)
$4 = 0x555555a8f120 "EMPTY"
(gdb) print (int)gst_caps_is_empty (caps)
(process:5873): GStreamer-CRITICAL (recursed) **: gst_caps_is_empty: assertion 'GST_IS_CAPS (caps)' failed
--------

*/

    if (caps == nullptr)
    {
        return true;
    }

    auto tmp_caps_string = gst_helper::to_string(caps);
    if ((tmp_caps_string == "EMPTY") || gst_caps_is_any(caps))
    {
        return true;
    }
    return false;
}


/**
 * Helper function to get a list of all available fourccs in caps
 */
static std::vector<uint32_t> index_format_fourccs(const GstCaps* caps)
{
    if (is_really_empty_caps(caps))
    {
        return {};
    }

    std::vector<uint32_t> ret;


    // only add when fourcc is not 0
    auto add_format = [&ret](const char* name, const char* fmt) {
        uint32_t fourcc = tcam_fourcc_from_gst_1_0_caps_string(name, fmt);
        if (fourcc != 0)
        {
            ret.push_back(fourcc);
        }
    };

    for (guint i = 0; i < gst_caps_get_size(caps); ++i)
    {
        GstStructure* struc = gst_caps_get_structure(caps, i);

        std::vector<std::string> vec;

        if (gst_structure_get_field_type(struc, "format") == GST_TYPE_LIST)
        {
            vec = gst_list_to_vector(gst_structure_get_value(struc, "format"));
        }
        else if (gst_structure_get_field_type(struc, "format") == G_TYPE_STRING)
        {
            vec.push_back(gst_structure_get_string(struc, "format"));
        }


        const char* name = gst_structure_get_name(struc);

        if (!vec.empty())
        {
            for (const auto& fmt : vec) { add_format(name, fmt.c_str()); }
        }
        else
        {
            // this will be the case for things like image/jpeg
            // such caps will have no format field and thus vec.empty() ==
            add_format(name, "");
        }
    }

    // remove duplicate entries
    // probably never enough entries to make switch to std::set a good alternative
    std::sort(ret.begin(), ret.end());
    ret.erase(std::unique(ret.begin(), ret.end()), ret.end());

    return ret;
}

void reset_input_caps_modules(struct input_caps_required_modules& modules)
{
    modules.bayertransform = false;
    modules.bayer2rgb = false;
    modules.videoconvert = false;
    modules.jpegdec = false;
    modules.dutils = false;
}


static uint32_t find_preferred_format(const std::vector<uint32_t>& vec)
{
    /**
     * prefer bayer 8-bit over everything else
     * if bayer 8-bit does not exist order according to the following list:
     * color formats like BGR
     * color formats like YUV
     * formats like MJPEG
     * GRAY16
     * GRAY8
     * pwl bayer
     * bayer12/16
     * polarized bayer
     * polarized mono
     */

    if (vec.empty())
    {
        return 0;
    }

    // maps are ordered
    // assume vec contains only unique values
    // map.begin() thus has the entry with best rank
    // since our key are associated in the way we prefer
    std::map<int, uint32_t> map;

    for (const auto& fourcc : vec)
    {
        if (tcam_gst_is_fourcc_bayer(fourcc))
        {
            //best_result = fourcc;
            map[0] = fourcc;
        }
        else if (tcam_gst_is_fourcc_rgb(fourcc))
        {
            map[10] = fourcc;
        }
        else if (tcam_gst_is_fourcc_yuv(fourcc))
        {
            map[20] = fourcc;
        }
        else if (fourcc == FOURCC_MJPG)
        {
            map[30] = fourcc;
        }
        else if (fourcc == FOURCC_Y800)
        {
            map[40] = fourcc;
        }
        else if (fourcc == FOURCC_Y16)
        {
            map[50] = fourcc;
        }
        else if (tcam_gst_is_bayerpwl_fourcc(fourcc))
        {
            map[60] = fourcc;
        }
        else if (tcam_gst_is_bayer10_fourcc(fourcc) || tcam_gst_is_bayer10_packed_fourcc(fourcc))
        {
            map[65] = fourcc;
        }
        else if (tcam_gst_is_bayer12_fourcc(fourcc) || tcam_gst_is_bayer12_packed_fourcc(fourcc))
        {
            map[70] = fourcc;
        }
        else if (tcam_gst_is_bayer16_fourcc(fourcc))
        {
            map[80] = fourcc;
        }
        else if (tcam_gst_is_polarized_bayer(fourcc))
        {
            map[90] = fourcc;
        }
        else if (tcam_gst_is_polarized_mono(fourcc))
        {
            map[100] = fourcc;
        }
        else
        {
            SPDLOG_ERROR("Could not associate rank with fourcc {:x} {}",
                         fourcc,
                         img::fcc_to_string(fourcc).c_str());
        }
    }
    if (map.empty())
    {
        return 0;
    }

    return map.begin()->second;
}


GstCaps* tcam_gst_find_largest_caps(const GstCaps* incoming)
{
    /**
     * find_largest_caps tries to find the largest caps
     * according to the following rules:
     *
     * 1. determine the preferred format
     *       prefer bayer 8-bit over everything else
     *       if bayer 8-bit does not exist order according to the following list:
     *       color formats like BGR
     *       formats like MJPEG
     *       GRAY8
     *       GRAY16
     *       pwl bayer
     *       bayer12/16
     *
     * 2. find the largest resolution
     * 3. for the format with the largest resolution take the highest framerate
     */
    std::vector<uint32_t> format_fourccs = index_format_fourccs(incoming);

    uint32_t preferred_fourcc = find_preferred_format(format_fourccs);

    if (is_really_empty_caps(incoming))
    {
        return nullptr;
    }

    int largest_index = 0;
    int largest_width = -1;
    int largest_height = -1;

    for (guint i = 0; i < gst_caps_get_size(incoming); ++i)
    {
        GstStructure* struc = gst_caps_get_structure(incoming, i);

        const char* format = gst_structure_get_string(struc, "format");

        uint32_t fourcc =
            tcam_fourcc_from_gst_1_0_caps_string(gst_structure_get_name(struc), format);

        // TODO: what about video/x-raw, format={GRAY8, GRAY16_LE}
        if (fourcc != preferred_fourcc)
        {
            continue;
        }

        int width = -1;
        int height = -1;
        bool new_width = false;
        bool new_height = false;

        // will fail if width is a range so we only handle
        // halfway fixated caps
        if (gst_structure_get_field_type(struc, "width") == G_TYPE_INT)
        {
            if (gst_structure_get_int(struc, "width", &width))
            {
                if (largest_width < width)
                {
                    largest_width = width;
                    new_width = true;
                }
            }
        }
        // else if (gst_structure_get_field_type(struc, "width") == GST_TYPE_INT_RANGE)
        // {
        //     const GValue* int_range = gst_structure_get_value(struc, "width");

        //     width = gst_value_get_int_range_max(int_range);
        //     if (largest_width < width)
        //     {
        //         largest_width = width;
        //         new_width = true;
        //     }
        // }
        else
        {
            SPDLOG_INFO("Field 'width' does not have a supported type. Current type: '{}'",
                        g_type_name(gst_structure_get_field_type(struc, "width")));
        }

        if (gst_structure_get_field_type(struc, "height") == G_TYPE_INT)
        {
            if (gst_structure_get_int(struc, "height", &height))
            {
                if (largest_height <= height)
                {
                    largest_height = height;
                    new_height = true;
                }
            }
        }
        // else if (gst_structure_get_field_type(struc, "height") == GST_TYPE_INT_RANGE)
        // {
        //     const GValue* int_range = gst_structure_get_value(struc, "height");

        //     height = gst_value_get_int_range_max(int_range);
        //     if (largest_height < height)
        //     {
        //         largest_height = height;
        //         new_height = true;
        //     }
        // }
        else
        {
            SPDLOG_INFO("Field 'height' does not have a supported type. Current type: '{}'",
                        g_type_name(gst_structure_get_field_type(struc, "height")));
        }

        if (new_width || new_height)
        {
            largest_index = i;
        }
    }

    GstCaps* largest_caps = gst_caps_copy_nth(incoming, largest_index);

    SPDLOG_INFO("Fixating assumed largest caps: {}", gst_helper::to_string(largest_caps).c_str());

    if (!tcam_gst_fixate_caps(largest_caps))
    {
        gst_caps_unref(largest_caps);

        GST_ERROR("Cannot fixate largest caps. Returning NULL");
        return nullptr;
    }

    GstStructure* s = gst_caps_get_structure(largest_caps, 0);

    int h;
    gst_structure_get_int(s, "height", &h);
    int w;
    gst_structure_get_int(s, "width", &w);

    int num;
    int den;
    gst_structure_get_fraction(s, "framerate", &num, &den);

    GValue vh = G_VALUE_INIT;

    g_value_init(&vh, G_TYPE_INT);
    g_value_set_int(&vh, h); // #TODO this function really looks bad here

    gst_caps_set_value(largest_caps, "height", &vh);

    GstCaps* ret_caps = gst_caps_new_simple(gst_structure_get_name(s),
                                            "framerate",
                                            GST_TYPE_FRACTION,
                                            num,
                                            den,
                                            "width",
                                            G_TYPE_INT,
                                            w,
                                            "height",
                                            G_TYPE_INT,
                                            h,
                                            NULL);

    if (gst_structure_has_field(s, "format"))
    {
        gst_caps_set_value(ret_caps, "format", gst_structure_get_value(s, "format"));
    }

    gst_caps_unref(largest_caps);

    SPDLOG_INFO("Largest caps are: {}", gst_helper::to_string(ret_caps).c_str());

    return ret_caps;
}

bool contains_jpeg(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    for (unsigned int i = 0; i < gst_caps_get_size(caps); ++i)
    {
        if (strcmp("image/jpeg", gst_structure_get_name(gst_caps_get_structure(caps, i))) == 0)
        {
            return true;
        }
    }

    return false;
}

bool contains_bayer(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    for (unsigned int i = 0; i < gst_caps_get_size(caps); ++i)
    {
        if (strcmp("video/x-bayer", gst_structure_get_name(gst_caps_get_structure(caps, i))) == 0)
        {
            return true;
        }
    }

    return false;
}

bool tcam_gst_contains_bayer_10_bit(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    GstCaps* tmp = gst_caps_from_string("video/x-bayer, format={rggb10, bggr10, gbrg10, grbg10,"
                                        "rggb10p, bggr10p, gbrg10p, grbg10p,"
                                        "rggb10s, bggr10s, gbrg10s, grbg10s,"
                                        "rggb10m, bggr10m, gbrg10m, grbg10m}");
    gboolean ret = gst_caps_can_intersect(caps, tmp);
    gst_caps_unref(tmp);

    return ret;
}


bool tcam_gst_contains_bayer_12_bit(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    GstCaps* tmp = gst_caps_from_string("video/x-bayer, format={rggb12, bggr12, gbrg12, grbg12,"
                                        "rggb12p, bggr12p, gbrg12p, grbg12p,"
                                        "rggb12s, bggr12s, gbrg12s, grbg12s,"
                                        "rggb12m, bggr12m, gbrg12m, grbg12m}");
    gboolean ret = gst_caps_can_intersect(caps, tmp);
    gst_caps_unref(tmp);

    return ret;
}

bool tcam_gst_contains_mono_10_bit(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    GstCaps* tmp = gst_caps_from_string("video/x-raw, format={GRAY10, GRAY10, GRAY10, GRAY10,"
                                        "GRAY10p, GRAY10p, GRAY10p, GRAY10p,"
                                        "GRAY10s, GRAY10s, GRAY10s, GRAY10s,"
                                        "GRAY10m, GRAY10m, GRAY10m, GRAY10m}");
    gboolean ret = gst_caps_can_intersect(caps, tmp);
    gst_caps_unref(tmp);

    return ret;
}


bool tcam_gst_contains_mono_12_bit(const GstCaps* caps)
{
    if (caps == nullptr)
    {
        return false;
    }

    GstCaps* tmp = gst_caps_from_string("video/x-raw, format={GRAY12, GRAY12, GRAY12, GRAY12,"
                                        "GRAY12p, GRAY12p, GRAY12p, GRAY12p,"
                                        "GRAY12s, GRAY12s, GRAY12s, GRAY12s,"
                                        "GRAY12m, GRAY12m, GRAY12m, GRAY12m}");
    gboolean ret = gst_caps_can_intersect(caps, tmp);
    gst_caps_unref(tmp);

    return ret;
}


static GstCaps* get_caps_from_element(GstElement* element, const char* padname)
{
    if (!element || !padname)
    {
        return nullptr;
    }

    auto pad = gst_element_get_static_pad(element, padname);
    GstCaps* ret = gst_pad_query_caps(pad, NULL);
    gst_object_unref(pad);

    return ret;
}

GstCaps* get_caps_from_element_name(const char* elementname, const char* padname)
{
    GstElement* ele = gst_element_factory_make(elementname, "tmp-element");
    if (!ele)
    {
        return nullptr;
    }

    auto ret = get_caps_from_element(ele, padname);

    gst_object_unref(ele);

    return ret;
}


std::vector<std::string> index_caps_formats(GstCaps* caps)
{
    // todo missing jpeg

    std::vector<std::string> ret;

    for (guint i = 0; i < gst_caps_get_size(caps); ++i)
    {
        GstStructure* struc = gst_caps_get_structure(caps, i);

        if (gst_structure_get_field_type(struc, "format") == GST_TYPE_LIST)
        {
            auto vec = gst_list_to_vector(gst_structure_get_value(struc, "format"));

            for (const auto& v : vec)
            {
                std::string str = gst_structure_get_name(struc);
                str += ",format=";
                str += v;
                ret.push_back(str);
            }
        }
        else if (gst_structure_get_field_type(struc, "format") == G_TYPE_STRING)
        {
            std::string str = gst_structure_get_name(struc);
            str += ",format=";
            str += gst_structure_get_string(struc, "format");
            ret.push_back(str);
        }
    }

    // make all entries unique
    std::sort(ret.begin(), ret.end());
    ret.erase(std::unique(ret.begin(), ret.end()), ret.end());
    return ret;
}

/**
 * @param formats - GstCaps from which the format and name shall be used
 * @param rest - GstCaps from which the rest i.e. width,height,framerate, shall be used, is_fixed has to be true
 * @return GstCaps containing merged caps, user takes ownership
 */
static GstCaps* create_caps_for_formats(GstCaps* formats, GstCaps* rest)
{
    if (!gst_caps_is_fixed(rest))
    {
        return nullptr;
    }

    auto st = gst_caps_get_structure(rest, 0);
    auto width = gst_structure_get_value(st, "width");
    auto height = gst_structure_get_value(st, "height");
    auto framerate = gst_structure_get_value(st, "framerate");

    auto caps_formats = index_caps_formats(formats);

    if (caps_formats.empty())
    {
        SPDLOG_ERROR("Could not identify formats for caps creation");
        return nullptr;
    }

    GstCaps* ret = gst_caps_new_empty();

    for (const auto& fmt : caps_formats)
    {
        GstCaps* tmp = gst_caps_from_string(fmt.c_str());

        if (width)
        {
            gst_caps_set_value(tmp, "width", width);
        }
        if (height)
        {
            gst_caps_set_value(tmp, "height", height);
        }
        if (framerate)
        {
            gst_caps_set_value(tmp, "framerate", framerate);
        }

        gst_caps_append(ret, tmp);
    }

    return ret;
}


static GstCaps* find_input_caps_dutils(GstCaps* available_caps,
                                       GstCaps* wanted_caps,
                                       bool& /*requires_bayer*/,
                                       bool& requires_vidoeconvert,
                                       bool& /*requires_jpegdec*/,
                                       bool& requires_dutils)
{
    requires_vidoeconvert = true;

    GstElementFactory* dutils = gst_element_factory_find("tcamdutils");

    if (!dutils)
    {
        SPDLOG_ERROR("Could not create dutils.");
        return nullptr;
    }

    // check if only dutils suffice
    if (gst_element_factory_can_src_any_caps(dutils, wanted_caps)
        && gst_element_factory_can_sink_any_caps(dutils, available_caps))
    {
        requires_dutils = true;
        gst_object_unref(dutils);

        GstCaps* ret;
        if (!gst_caps_is_fixed(available_caps))
        {
            if (!gst_caps_is_empty(wanted_caps) && (gst_helper::to_string(wanted_caps) != "NULL"))
            {
                if (!gst_caps_is_fixed(wanted_caps))
                {
                    ret = gst_caps_intersect(available_caps, wanted_caps);

                    if (gst_caps_is_empty(ret))
                    {
                        gst_caps_unref(ret);
                        return gst_caps_copy(available_caps);
                    }
                }
                else
                {
                    GstCaps* possible_matches =
                        create_caps_for_formats(available_caps, wanted_caps);

                    if (!possible_matches || gst_caps_is_empty(possible_matches))
                    {
                        SPDLOG_ERROR("No possible matches for dutils.");
                        return nullptr;
                    }

                    ret = gst_caps_intersect(available_caps, possible_matches);
                    gst_caps_unref(possible_matches);
                }
            }
            else
            {
                ret = tcam_gst_find_largest_caps(available_caps);
            }
        }
        else // is fixed
        {
            ret = gst_caps_copy(available_caps);
        }

        if (!ret)
        {
            // TODO not compatible
            SPDLOG_ERROR("No intersecting caps between dutils and src");
            return nullptr;
        }

        return ret;
    }
    gst_object_unref(dutils);

    SPDLOG_ERROR("Could not negotiate caps");
    return nullptr;
}


/**
 * This function works as follows (please edit when changin)
 * Check if input directly supports wanted caps
 * if yes -> get intersect and be done
 * check if conversions are required.
 * jpegdec
 * debayer
 *
 * generally speaking prefer (for both input/output)
 *                           color over mono
 *                           RGBx over YUV
 *                           anything over jpeg
 */

// TODO: bayer and videoconvert should consider eachother
GstCaps* find_input_caps(GstCaps* available_caps,
                         GstCaps* wanted_caps,
                         struct input_caps_required_modules& modules,
                         struct input_caps_toggles toggles)
{
    reset_input_caps_modules(modules);

    if (!GST_IS_CAPS(available_caps))
    {
        return nullptr;
    }

    if (wanted_caps == nullptr || gst_caps_is_empty(wanted_caps))
    {
        GST_INFO("No sink caps specified. Continuing with caps from source device.");
        wanted_caps = gst_caps_copy(available_caps);
    }

    GstElementFactory* dutils = gst_element_factory_find("tcamdutils");
    if (toggles.use_dutils && dutils)
    {
        gst_object_unref(dutils);
        return find_input_caps_dutils(available_caps,
                                      wanted_caps,
                                      modules.bayer2rgb,
                                      modules.videoconvert,
                                      modules.jpegdec,
                                      modules.dutils);
    }

    if (toggles.use_by1xtransform)
    {
        GstElementFactory* bayer_transform = gst_element_factory_find("tcamby1xtransform");

        if (bayer_transform)
        {
            // check if only bayertransform suffices
            if (gst_element_factory_can_src_any_caps(bayer_transform, wanted_caps)
                && gst_element_factory_can_sink_any_caps(bayer_transform, available_caps))
            {
                modules.bayertransform = true;
                // wanted_caps can be fixed, etc.
                // thus change name to be compatible to bayer2rgb sink pad
                // and create a correct intersection
                GstCaps* temp = gst_caps_copy(wanted_caps);
                gst_caps_change_name(temp, "video/x-bayer");

                GstCaps* ret = gst_caps_intersect(available_caps, temp);
                gst_caps_unref(temp);
                gst_object_unref(bayer_transform);
                return ret;
            }

            GstElementFactory* debayer = gst_element_factory_find("bayer2rgb");

            // check if bayertransform + bayer2rgb works
            if (debayer && !tcam_gst_raw_only_has_mono(available_caps))
            {
                if (gst_element_factory_can_src_any_caps(debayer, wanted_caps)
                    && gst_element_factory_can_sink_any_caps(bayer_transform, available_caps))
                {
                    modules.bayertransform = true;
                    modules.bayer2rgb = true;

                    // wanted_caps can be fixed, etc.
                    // thus change name to be compatible to bayer2rgb sink pad
                    // and create a correct intersection
                    GstCaps* temp = gst_caps_copy(wanted_caps);
                    gst_caps_change_name(temp, "video/x-bayer");

                    GstCaps* ret = gst_caps_intersect(available_caps, temp);
                    gst_caps_unref(temp);
                    gst_object_unref(bayer_transform);
                    gst_object_unref(debayer);

                    return ret;
                }
            }

            GstElementFactory* convert = gst_element_factory_find("videoconvert");

            if (convert)
            {
                auto transform_out_caps = get_caps_from_element_name("tcamby1xtransform", "src");

                if (gst_element_factory_can_src_any_caps(convert, wanted_caps)
                    && gst_element_factory_can_sink_any_caps(convert, transform_out_caps))
                {
                    // this intersection check is to ensure that we can't just pass
                    // the caps through and really need this additional element
                    GstCaps* intersect = gst_caps_intersect(transform_out_caps, wanted_caps);
                    if (!gst_caps_is_empty(intersect))
                    {
                        return intersect;
                    }
                    gst_caps_unref(intersect);

                    modules.bayertransform = true;
                    modules.videoconvert = true;
                    // wanted_caps can be fixed, etc.

                    GstCaps* in = get_caps_from_element_name("tcamby1xtransform", "sink");

                    // this removes things like jpeg
                    GstCaps* ret = gst_caps_intersect(available_caps, in);
                    gst_caps_unref(in);

                    GstCaps* temp = gst_caps_copy(wanted_caps);
                    for (unsigned int i = 0; i < gst_caps_get_size(temp); ++i)
                    {
                        gst_structure_remove_field(gst_caps_get_structure(temp, i), "format");
                    }

                    GstCaps* ret2 = gst_caps_intersect(ret, temp);

                    gst_caps_unref(temp);
                    gst_caps_unref(ret);


                    gst_object_unref(convert);
                    return ret2;
                }
                gst_object_unref(convert);
            }

            gst_object_unref(debayer);
            gst_object_unref(bayer_transform);
        }
    }

    GstElementFactory* debayer = gst_element_factory_find("bayer2rgb");

    if (debayer)
    {
        if (gst_element_factory_can_src_any_caps(debayer, wanted_caps)
            && gst_element_factory_can_sink_any_caps(debayer, available_caps))
        {
            modules.bayer2rgb = true;
            // wanted_caps can be fixed, etc.
            // thus change name to be compatible to bayer2rgb sink pad
            // and create a correct intersection
            GstCaps* temp = gst_caps_copy(wanted_caps);
            gst_caps_change_name(temp, "video/x-bayer");

            GstCaps* ret = gst_caps_intersect(available_caps, temp);
            gst_caps_unref(temp);
            gst_object_unref(debayer);
            if (ret)
            {
                return ret;
            }
        }

        GstElementFactory* convert = gst_element_factory_find("videoconvert");

        if (convert)
        {
            if (gst_element_factory_can_src_any_caps(convert, wanted_caps)
                && gst_element_factory_can_sink_any_caps(debayer, available_caps))
            {
                modules.bayer2rgb = true;
                modules.videoconvert = true;
                // wanted_caps can be fixed, etc.
                // thus change name to be compatible to bayer2rgb sink pad
                // and create a correct intersection
                GstCaps* temp = gst_caps_copy(wanted_caps);
                gst_caps_change_name(temp, "video/x-bayer");

                GstCaps* ret = gst_caps_intersect(available_caps, temp);
                gst_caps_unref(temp);
                gst_object_unref(debayer);
                gst_object_unref(convert);

                if (ret)
                {
                    return ret;
                }
            }
        }

        gst_object_unref(convert);
        gst_object_unref(debayer);

        // fall through so that other conversion can be tested
    }

    GstElementFactory* convert = gst_element_factory_find("videoconvert");

    if (convert)
    {
        if (gst_element_factory_can_src_any_caps(convert, wanted_caps)
            && gst_element_factory_can_sink_any_caps(convert, available_caps))
        {
            // this intersection check is to ensure that we can't just pass
            // the caps through and really need this additional element
            GstCaps* intersect = gst_caps_intersect(available_caps, wanted_caps);
            if (!gst_caps_is_empty(intersect))
            {
                return intersect;
            }
            gst_caps_unref(intersect);

            modules.videoconvert = true;
            // wanted_caps can be fixed, etc.

            GstCaps* in = get_caps_from_element_name("videoconvert", "sink");

            // this removes things like jpeg
            GstCaps* ret = gst_caps_intersect(available_caps, in);
            gst_caps_unref(in);

            GstCaps* temp = gst_caps_copy(wanted_caps);
            for (unsigned int i = 0; i < gst_caps_get_size(temp); ++i)
            {
                gst_structure_remove_field(gst_caps_get_structure(temp, i), "format");
            }

            GstCaps* ret2 = gst_caps_intersect(ret, temp);

            gst_caps_unref(temp);
            gst_caps_unref(ret);


            gst_object_unref(convert);
            return ret2;
        }
        gst_object_unref(convert);
    }

    GstElementFactory* jpegdec = gst_element_factory_find("jpegdec");

    if (jpegdec)
    {
        if (gst_element_factory_can_src_any_caps(jpegdec, wanted_caps)
            && gst_element_factory_can_sink_any_caps(jpegdec, available_caps))
        {
            modules.jpegdec = true;
            modules.videoconvert = true;
            // wanted_caps can be fixed, etc.
            // thus change name to be compatible to jpegdec sink pad
            // and create a correct intersection
            GstCaps* temp = gst_caps_copy(wanted_caps);
            gst_caps_change_name(temp, "image/jpeg");

            for (unsigned int i = 0; i < gst_caps_get_size(temp); ++i)
            {
                gst_structure_remove_field(gst_caps_get_structure(temp, i), "format");
            }

            GstCaps* ret = gst_caps_intersect(available_caps, temp);
            gst_caps_unref(temp);
            gst_object_unref(jpegdec);
            return ret;
        }

        gst_object_unref(jpegdec);
    }

    // no transform elements needed
    // try raw intersection
    GstCaps* intersect = gst_caps_intersect(available_caps, wanted_caps);
    if (!gst_caps_is_empty(intersect))
    {
        return intersect;
    }
    gst_caps_unref(intersect);

    return nullptr;
}


static void fill_structure_fixed_resolution(GstStructure* structure,
                                            const tcam::VideoFormatDescription& format,
                                            const tcam_resolution_description& res)
{
    GValue fps_list = G_VALUE_INIT;
    g_value_init(&fps_list, GST_TYPE_LIST);

    for (auto rate : format.get_frame_rates(res))
    {
        int frame_rate_numerator;
        int frame_rate_denominator;
        gst_util_double_to_fraction(rate, &frame_rate_numerator, &frame_rate_denominator);

        GValue fraction = G_VALUE_INIT;
        g_value_init(&fraction, GST_TYPE_FRACTION);
        gst_value_set_fraction(&fraction, frame_rate_numerator, frame_rate_denominator);
        gst_value_list_append_value(&fps_list, &fraction);
        g_value_unset(&fraction);
    }

    gst_structure_set(structure,
                      "width",
                      G_TYPE_INT,
                      res.max_size.width,
                      "height",
                      G_TYPE_INT,
                      res.max_size.height,
                      NULL);

    gst_structure_take_value(structure, "framerate", &fps_list);
}


GstCaps* convert_videoformatsdescription_to_caps(
    const std::vector<tcam::VideoFormatDescription>& descriptions)
{
    GstCaps* caps = gst_caps_new_empty();

    for (const auto& desc : descriptions)
    {
        if (desc.get_fourcc() == 0)
        {
            SPDLOG_INFO("Format has empty fourcc. Ignoring");
            continue;
        }

        const char* caps_string = tcam_fourcc_to_gst_1_0_caps_string(desc.get_fourcc());

        if (caps_string == nullptr)
        {
            SPDLOG_WARN("Format has empty caps string. Ignoring {}",
                        img::fcc_to_string(desc.get_fourcc()));
            continue;
        }

        std::vector<struct tcam_resolution_description> res = desc.get_resolutions();

        for (const auto& r : res)
        {
            uint32_t min_width = r.min_size.width;
            uint32_t min_height = r.min_size.height;

            uint32_t max_width = r.max_size.width;
            uint32_t max_height = r.max_size.height;

            if (r.type == TCAM_RESOLUTION_TYPE_RANGE)
            {
                std::vector<struct tcam_image_size> framesizes =
                    tcam::get_standard_resolutions(r.min_size, r.max_size);

                // check if min/max are already in the vector.
                // some devices return std resolutions as max
                if (r.min_size != framesizes.front())
                {
                    framesizes.insert(framesizes.begin(), r.min_size);
                }

                if (r.max_size != framesizes.back())
                {
                    framesizes.push_back(r.max_size);
                }

                for (const auto& reso : framesizes)
                {
                    GstStructure* structure = gst_structure_from_string(caps_string, NULL);

                    std::vector<double> framerates = desc.get_framerates(reso);

                    if (framerates.empty())
                    {
                        continue;
                    }

                    GValue fps_list = G_VALUE_INIT;
                    g_value_init(&fps_list, GST_TYPE_LIST);

                    for (const auto& f : framerates)
                    {
                        int frame_rate_numerator;
                        int frame_rate_denominator;
                        gst_util_double_to_fraction(
                            f, &frame_rate_numerator, &frame_rate_denominator);

                        if ((frame_rate_denominator == 0) || (frame_rate_numerator == 0))
                        {
                            continue;
                        }

                        GValue fraction = G_VALUE_INIT;
                        g_value_init(&fraction, GST_TYPE_FRACTION);
                        gst_value_set_fraction(
                            &fraction, frame_rate_numerator, frame_rate_denominator);
                        gst_value_list_append_value(&fps_list, &fraction);
                        g_value_unset(&fraction);
                    }


                    gst_structure_set(structure,
                                      "width",
                                      G_TYPE_INT,
                                      reso.width,
                                      "height",
                                      G_TYPE_INT,
                                      reso.height,
                                      NULL);

                    gst_structure_take_value(structure, "framerate", &fps_list);
                    gst_caps_append_structure(caps, structure);
                }


                std::vector<double> fps = desc.get_frame_rates(r);

                std::vector<double> highest_fps = desc.get_framerates({ min_width, min_height });

                if (fps.empty())
                {
                    // GST_ERROR("Could not find any framerates for format");
                    continue;
                }

                // finally also add the range to allow unusual settings like 1920x96@90fps
                GstStructure* structure = gst_structure_from_string(caps_string, NULL);

                GValue w = G_VALUE_INIT;
                g_value_init(&w, GST_TYPE_INT_RANGE);
                gst_value_set_int_range_step(&w, min_width, max_width, r.width_step_size);

                GValue h = G_VALUE_INIT;
                g_value_init(&h, GST_TYPE_INT_RANGE);
                gst_value_set_int_range_step(&h, min_height, max_height, r.height_step_size);

                int fps_min_num;
                int fps_min_den;
                int fps_max_num;
                int fps_max_den;
                gst_util_double_to_fraction(
                    *std::min_element(fps.begin(), fps.end()), &fps_min_num, &fps_min_den);
                gst_util_double_to_fraction(
                    *std::max_element(highest_fps.begin(), highest_fps.end()),
                    &fps_max_num,
                    &fps_max_den);

                GValue f = G_VALUE_INIT;
                g_value_init(&f, GST_TYPE_FRACTION_RANGE);

                gst_value_set_fraction_range_full(
                    &f, fps_min_num, fps_min_den, fps_max_num, fps_max_den);

                gst_structure_take_value(structure, "width", &w);
                gst_structure_take_value(structure, "height", &h);
                gst_structure_take_value(structure, "framerate", &f);
                gst_caps_append_structure(caps, structure);
            }
            else
            {
                GstStructure* structure = gst_structure_from_string(caps_string, NULL);

                fill_structure_fixed_resolution(structure, desc, r);
                gst_caps_append_structure(caps, structure);
            }
        }
    }

    return caps;
}

bool gst_caps_to_tcam_video_format(GstCaps* caps, struct tcam_video_format* format)
{
    if (!caps || !gst_caps_is_fixed(caps) || !format)
    {
        return false;
    }

    *format = {};

    GstStructure* struc = gst_caps_get_structure(caps, 0);

    format->fourcc = tcam_fourcc_from_gst_1_0_caps_string(
        gst_structure_get_name(struc), gst_structure_get_string(struc, "format"));

    gint tmp_w, tmp_h;
    gst_structure_get_int(struc, "width", &tmp_w);
    gst_structure_get_int(struc, "height", &tmp_h);
    format->width = tmp_w < 0 ? 0 : tmp_w;
    format->height = tmp_h < 0 ? 0 : tmp_h;

    int num;
    int den;
    gst_structure_get_fraction(struc, "framerate", &num, &den);

    format->framerate = den / num;

    return true;
}


bool gst_buffer_to_tcam_image_buffer(GstBuffer* buffer, GstCaps* caps, tcam_image_buffer* image)
{
    if (!buffer || !image)
    {
        return false;
    }

    *image = {};

    GstMapInfo info;

    gst_buffer_map(buffer, &info, GST_MAP_READ);

    image->pData = info.data;
    image->length = info.size;

    if (caps)
    {
        gst_caps_to_tcam_video_format(caps, &image->format);
        image->pitch = img::calc_minimum_pitch(static_cast<img::fourcc>(image->format.fourcc), image->format.width);
    }

    gst_buffer_unmap(buffer, &info);

    return true;
}


int calc_pitch(int fourcc, int width)
{
    return img::calc_minimum_pitch(static_cast<img::fourcc>(fourcc), width);
}
