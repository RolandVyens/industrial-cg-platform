/* SPDX-FileCopyrightText: 2005 `Gernot Ziegler <gz@lysator.liu.se>`. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup openexr
 */

#include "BLI_string_ref.hh"
#include "IMB_openexr.hh"

namespace blender {

ExrHandle *IMB_exr_get_handle(bool /*write_multipart*/)
{
  return nullptr;
}
void IMB_exr_add_channels(ExrHandle * /*handle*/,
                          StringRefNull /*layerpassname*/,
                          StringRefNull /*channelnames*/,
                          StringRefNull /*viewname*/,
                          StringRefNull /*colorspace*/,
                          size_t /*xstride*/,
                          size_t /*ystride*/,
                          float * /*rect*/,
                          bool /*use_half_float*/)
{
}

bool IMB_exr_begin_read(ExrHandle * /*handle*/,
                        const char * /*filepath*/,
                        int * /*width*/,
                        int * /*height*/,
                        const bool /*add_channels*/)
{
  return false;
}
bool IMB_exr_begin_write(ExrHandle * /*handle*/,
                         const char * /*filepath*/,
                         int /*width*/,
                         int /*height*/,
                         const double /*ppm*/[2],
                         int /*compress*/,
                         int /*quality*/,
                         const StampData * /*stamp*/)
{
  return false;
}

bool IMB_exr_set_channel(ExrHandle * /*handle*/,
                         StringRefNull /*full_name*/,
                         int /*xstride*/,
                         int /*ystride*/,
                         float * /*rect*/)
{
  return false;
}

void IMB_exr_read_channels(ExrHandle * /*handle*/) {}
void IMB_exr_write_channels(ExrHandle * /*handle*/) {}

void IMB_exr_multilayer_convert(ExrHandle * /*handle*/,
                                void * /*base*/,
                                void *(* /*addview*/)(void *base, const char *str),
                                void *(* /*addlayer*/)(void *base, const char *str),
                                void (* /*addpass*/)(void *base,
                                                     void *lay,
                                                     const char *str,
                                                     float *rect,
                                                     int totchan,
                                                     const char *chan_id,
                                                     const char *view))
{
}

void IMB_exr_close(ExrHandle * /*handle*/) {}

void IMB_exr_add_view(ExrHandle * /*handle*/, const char * /*name*/) {}
bool IMB_exr_has_multilayer(ExrHandle * /*handle*/)
{
  return false;
}

bool IMB_exr_get_ppm(ExrHandle * /*handle*/, double /*ppm*/[2])
{
  return false;
}

void IMB_exr_set_display_window(ExrHandle * /*handle*/,
                                const int /*display_size*/[2],
                                const int /*display_offset*/[2],
                                const int /*data_offset*/[2])
{
}

void IMB_exr_get_display_window(ExrHandle * /*handle*/,
                                int /*display_size*/[2],
                                int /*display_offset*/[2],
                                int /*data_offset*/[2])
{
}

bool IMB_exr_save_deep(const std::vector<std::vector<DeepSample>> & /*deep_data*/,
                       int /*width*/,
                       int /*height*/,
                       const char * /*filepath*/,
                       int /*compression*/,
                       bool /*use_half_float*/,
                       bool /*alpha_only*/,
                       bool /*has_display_window*/,
                       int /*display_width*/,
                       int /*display_height*/,
                       int /*display_offset_x*/,
                       int /*display_offset_y*/,
                       int /*data_offset_x*/,
                       int /*data_offset_y*/)
{
  return false;
}

}  // namespace blender
