/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2014 - Daniel De Matteis
 *  Copyright (C) 2012-2014 - OV2
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "d3d9.hpp"
#include "render_chain.hpp"
#include "../../file.h"
#include "../context/win32_common.h"

#ifdef _MSC_VER
#ifndef _XBOX
#pragma comment( lib, "d3d9" )
#pragma comment( lib, "d3dx9" )
#pragma comment( lib, "cgd3d9" )
#pragma comment( lib, "dxguid" )
#endif
#endif

static d3d_video_t *curD3D = NULL;
static bool d3d_quit = false;
static void *dinput;

extern bool d3d_restore(void *data);

static void d3d_resize(unsigned new_width, unsigned new_height)
{
   d3d_video_t *d3d = (d3d_video_t*)curD3D;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;
   if (!d3dr)
      return;

   RARCH_LOG("[D3D]: Resize %ux%u.\n", new_width, new_height);

   if (new_width != d3d->video_info.width || new_height != d3d->video_info.height)
   {
      d3d->video_info.width = d3d->screen_width = new_width;
      d3d->video_info.height = d3d->screen_height = new_height;
      d3d_restore(d3d);
   }
}

#ifdef HAVE_WINDOW
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message,
        WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_CREATE:
			LPCREATESTRUCT p_cs;
			p_cs = (LPCREATESTRUCT)lParam;
			curD3D = (d3d_video_t*)p_cs->lpCreateParams;
			break;

        case WM_CHAR:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_SYSKEYDOWN:
			return win32_handle_keyboard_event(hWnd, message, wParam, lParam);

        case WM_DESTROY:
			d3d_quit = true;
			return 0;
        case WM_SIZE:
			unsigned new_width, new_height;
			new_width = LOWORD(lParam);
			new_height = HIWORD(lParam);

			if (new_width && new_height)
				d3d_resize(new_width, new_height);
			return 0;
    }
    if (dinput_handle_message(dinput, message, wParam, lParam))
        return 0;
    return DefWindowProc(hWnd, message, wParam, lParam);
}
#endif

bool d3d_process_shader(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
#ifdef HAVE_FBO
   if (strcmp(path_get_extension(d3d->cg_shader.c_str()), "cgp") == 0)
      return d3d_init_multipass(d3d);
#endif

   return d3d_init_singlepass(d3d);
}

static void gfx_ctx_d3d_swap_buffers(void)
{
   d3d_video_t *d3d = (d3d_video_t*)driver.video_data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;

#ifdef _XBOX
   RD3DDevice_Present(d3dr);
#else
   if (d3dr->Present(NULL, NULL, NULL, NULL) != D3D_OK)
   {
      d3d->needs_restore = true;
      RARCH_ERR("[D3D]: Present() failed.\n");
   }
#endif
}

static void gfx_ctx_d3d_update_title(void)
{
   d3d_video_t *d3d = (d3d_video_t*)driver.video_data;
   char buffer[128], buffer_fps[128];
   bool fps_draw = g_settings.fps_show;

   if (gfx_get_fps(buffer, sizeof(buffer), fps_draw ? buffer_fps : NULL, sizeof(buffer_fps)))
   {
#ifndef _XBOX
      std::string title = buffer;
      title += " || Direct3D";
      SetWindowText(d3d->hWnd, title.c_str());
#endif
   }

   if (fps_draw)
   {
#ifdef _XBOX
      char mem[128];
      MEMORYSTATUS stat;
      GlobalMemoryStatus(&stat);
      snprintf(mem, sizeof(mem), "|| MEM: %.2f/%.2fMB", stat.dwAvailPhys/(1024.0f*1024.0f), stat.dwTotalPhys/(1024.0f*1024.0f));
      strlcat(buffer_fps, mem, sizeof(buffer_fps));
#endif
      msg_queue_push(g_extern.msg_queue, buffer_fps, 1, 1);
   }

   g_extern.frame_count++;
}

void d3d_set_font_rect(void *data, font_params_t *params)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   float pos_x = g_settings.video.msg_pos_x;
   float pos_y = g_settings.video.msg_pos_y;
   float font_size = g_settings.video.font_size;

   if (params)
   {
      pos_x = params->x;
      pos_y = params->y;
      font_size *= params->scale;
   }

   d3d->font_rect.left = d3d->final_viewport.X + d3d->final_viewport.Width * pos_x;
   d3d->font_rect.right = d3d->final_viewport.X + d3d->final_viewport.Width;
   d3d->font_rect.top = d3d->final_viewport.Y + (1.0f - pos_y) * d3d->final_viewport.Height - font_size; 
   d3d->font_rect.bottom = d3d->final_viewport.Height;

   d3d->font_rect_shifted = d3d->font_rect;
   d3d->font_rect_shifted.left -= 2;
   d3d->font_rect_shifted.right -= 2;
   d3d->font_rect_shifted.top += 2;
   d3d->font_rect_shifted.bottom += 2;
}

void d3d_recompute_pass_sizes(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   LinkInfo link_info = {0};
   link_info.pass = &d3d->shader.pass[0];
   link_info.tex_w = link_info.tex_h = d3d->video_info.input_scale * RARCH_SCALE_BASE;

   unsigned current_width = link_info.tex_w;
   unsigned current_height = link_info.tex_h;
   unsigned out_width = 0;
   unsigned out_height = 0;

   if (!renderchain_set_pass_size(d3d->chain, 0, current_width, current_height))
   {
      RARCH_ERR("[D3D]: Failed to set pass size.\n");
      return;
   }

   for (unsigned i = 1; i < d3d->shader.passes; i++)
   {
      renderchain_convert_geometry(d3d->chain, &link_info,
            out_width, out_height,
            current_width, current_height, &d3d->final_viewport);

      link_info.tex_w = next_pow2(out_width);
      link_info.tex_h = next_pow2(out_height);

      if (!renderchain_set_pass_size(d3d->chain, i, link_info.tex_w, link_info.tex_h))
      {
         RARCH_ERR("[D3D]: Failed to set pass size.\n");
         return;
      }

      current_width = out_width;
      current_height = out_height;

      link_info.pass = &d3d->shader.pass[i];
   }
}

bool d3d_init_singlepass(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   memset(&d3d->shader, 0, sizeof(d3d->shader));
   d3d->shader.passes = 1;
   gfx_shader_pass &pass = d3d->shader.pass[0];
   pass.fbo.valid = true;
   pass.fbo.scale_x = pass.fbo.scale_y = 1.0;
   pass.fbo.type_x = pass.fbo.type_y = RARCH_SCALE_VIEWPORT;
   strlcpy(pass.source.cg, d3d->cg_shader.c_str(), sizeof(pass.source.cg));

   return true;
}

bool d3d_init_imports(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (!d3d->shader.variables)
      return true;

   state_tracker_info tracker_info = {0};

   tracker_info.wram = (uint8_t*)pretro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
   tracker_info.info = d3d->shader.variable;
   tracker_info.info_elem = d3d->shader.variables;

#ifdef HAVE_PYTHON
   if (*d3d->shader.script_path)
   {
      tracker_info.script = d3d->shader.script_path;
      tracker_info.script_is_file = true;
   }

   tracker_info.script_class = *d3d->shader.script_class ? d3d->shader.script_class : NULL;
#endif

   state_tracker_t *state_tracker = state_tracker_init(&tracker_info);
   if (!state_tracker)
   {
      RARCH_ERR("Failed to initialize state tracker.\n");
      return false;
   }

   renderchain_add_state_tracker(d3d->chain, state_tracker);
   return true;
}

bool d3d_init_luts(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   for (unsigned i = 0; i < d3d->shader.luts; i++)
   {
      bool ret = renderchain_add_lut(d3d->chain, d3d->shader.lut[i].id, d3d->shader.lut[i].path,
         d3d->shader.lut[i].filter == RARCH_FILTER_UNSPEC ?
            g_settings.video.smooth :
            (d3d->shader.lut[i].filter == RARCH_FILTER_LINEAR));

      if (!ret)
         return ret;
   }

   return true;
}

#ifdef HAVE_FBO
bool d3d_init_multipass(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   config_file_t *conf = config_file_new(d3d->cg_shader.c_str());
   if (!conf)
   {
      RARCH_ERR("Failed to load preset.\n");
      return false;
   }

   memset(&d3d->shader, 0, sizeof(d3d->shader));

   if (!gfx_shader_read_conf_cgp(conf, &d3d->shader))
   {
      config_file_free(conf);
      RARCH_ERR("Failed to parse CGP file.\n");
      return false;
   }

   config_file_free(conf);

   gfx_shader_resolve_relative(&d3d->shader, d3d->cg_shader.c_str());

   RARCH_LOG("[D3D9 Meta-Cg] Found %d shaders.\n", d3d->shader.passes);

   for (unsigned i = 0; i < d3d->shader.passes; i++)
   {
      if (!d3d->shader.pass[i].fbo.valid)
      {
         d3d->shader.pass[i].fbo.scale_x = d3d->shader.pass[i].fbo.scale_y = 1.0f;
         d3d->shader.pass[i].fbo.type_x = d3d->shader.pass[i].fbo.type_y = RARCH_SCALE_INPUT;
      }
   }

   bool use_extra_pass = d3d->shader.passes < GFX_MAX_SHADERS && d3d->shader.pass[d3d->shader.passes - 1].fbo.valid;
   if (use_extra_pass)
   {
      d3d->shader.passes++;
      gfx_shader_pass &dummy_pass = d3d->shader.pass[d3d->shader.passes - 1];
      dummy_pass.fbo.scale_x = dummy_pass.fbo.scale_y = 1.0f;
      dummy_pass.fbo.type_x = dummy_pass.fbo.type_y = RARCH_SCALE_VIEWPORT;
      dummy_pass.filter = RARCH_FILTER_UNSPEC;
   }
   else
   {
      gfx_shader_pass &pass = d3d->shader.pass[d3d->shader.passes - 1];
      pass.fbo.scale_x = pass.fbo.scale_y = 1.0f;
      pass.fbo.type_x = pass.fbo.type_y = RARCH_SCALE_VIEWPORT;
   }

   return true;
}
#endif

bool d3d_init_chain(void *data, const video_info_t *video_info)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;
   // Setup information for first pass.
   LinkInfo link_info = {0};

   link_info.pass = &d3d->shader.pass[0];
   link_info.tex_w = link_info.tex_h = video_info->input_scale * RARCH_SCALE_BASE;

   if (d3d->chain)
      free(d3d->chain);
   d3d->chain = (renderchain_t*)calloc(1, sizeof(renderchain_t));
   if (!d3d->chain)
      return false;

   if (!renderchain_init(d3d->chain, &d3d->video_info, d3dr, d3d->cgCtx, &d3d->final_viewport, &link_info,
            d3d->video_info.rgb32 ? ARGB : RGB565))
   {
      RARCH_ERR("[D3D9]: Failed to init render chain.\n");
      return false;
   }

   unsigned current_width = link_info.tex_w;
   unsigned current_height = link_info.tex_h;
   unsigned out_width = 0;
   unsigned out_height = 0;

   for (unsigned i = 1; i < d3d->shader.passes; i++)
   {
      renderchain_convert_geometry(d3d->chain, &link_info,
            out_width, out_height,
            current_width, current_height, &d3d->final_viewport);

      link_info.pass = &d3d->shader.pass[i];
      link_info.tex_w = next_pow2(out_width);
      link_info.tex_h = next_pow2(out_height);

      current_width = out_width;
      current_height = out_height;

      if (!renderchain_add_pass(d3d->chain, &link_info))
      {
         RARCH_ERR("[D3D9]: Failed to add pass.\n");
         return false;
      }
   }

   if (!d3d_init_luts(d3d))
   {
      RARCH_ERR("[D3D9]: Failed to init LUTs.\n");
      return false;
   }

   if (!d3d_init_imports(d3d))
   {
      RARCH_ERR("[D3D9]: Failed to init imports.\n");
      return false;
   }

   return true;
}

void d3d_deinit_chain(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (d3d->chain)
      free(d3d->chain);
   d3d->chain = NULL;
}

static void gfx_ctx_d3d_show_mouse(bool state)
{
#ifdef HAVE_WINDOW
   if (state)
      while (ShowCursor(TRUE) < 0);
   else
      while (ShowCursor(FALSE) >= 0);
#endif
}

void d3d_make_d3dpp(void *data, const video_info_t *info, D3DPRESENT_PARAMETERS *d3dpp)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   memset(d3dpp, 0, sizeof(*d3dpp));

   d3dpp->Windowed = g_settings.video.windowed_fullscreen || !info->fullscreen;

   if (info->vsync)
   {
      switch (g_settings.video.swap_interval)
      {
         default:
         case 1: d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_ONE; break;
         case 2: d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_TWO; break;
         case 3: d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_THREE; break;
         case 4: d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_FOUR; break;
      }
   }
   else
      d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

   d3dpp->SwapEffect = D3DSWAPEFFECT_DISCARD;
   d3dpp->hDeviceWindow = d3d->hWnd;
   d3dpp->BackBufferCount = 2;
   d3dpp->BackBufferFormat = !d3dpp->Windowed ? D3DFMT_X8R8G8B8 : D3DFMT_UNKNOWN;

   if (!d3dpp->Windowed)
   {
      d3dpp->BackBufferWidth = d3d->screen_width;
      d3dpp->BackBufferHeight = d3d->screen_height;
   }
}

static void gfx_ctx_d3d_check_window(bool *quit,
   bool *resize, unsigned *width, unsigned *height, unsigned frame_count)
{
   d3d_video_t *d3d = (d3d_video_t*)driver.video_data;
   *quit = false;
   *resize = false;

   if (d3d_quit)
      *quit = true;
#ifdef _XBOX
   if (d3d->should_resize)
      *resize = true;
#else
   MSG msg;

   while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }
#endif
}

static bool gfx_ctx_d3d_has_focus(void)
{
#ifdef _XBOX
   return true;
#else
   d3d_video_t *d3d = (d3d_video_t*)driver.video_data;
   return GetFocus() == d3d->hWnd;
#endif
}

static bool gfx_ctx_d3d_bind_api(enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   (void)major;
   (void)minor;
   (void)api;
#if defined(_XBOX1)
   return api == GFX_CTX_DIRECT3D8_API;
#else /* As long as we don't have a D3D11 implementation, we default to this */
   return api == GFX_CTX_DIRECT3D9_API;
#endif
}

static bool gfx_ctx_d3d_init(void)
{
   return true;
}

static void gfx_ctx_d3d_input_driver(const input_driver_t **input, void **input_data)
{
#ifdef _XBOX
   void *xinput = input_xinput.init();
   *input = xinput ? (const input_driver_t*)&input_xinput : NULL;
   *input_data = xinput;
#else
   dinput = input_dinput.init();
   *input = dinput ? &input_dinput : NULL;
   *input_data = dinput;
#endif
}

static void gfx_ctx_d3d_get_video_size(unsigned *width, unsigned *height)
{
#ifdef _XBOX
   (void)width;
   (void)height;
#if defined(_XBOX360)
   XVIDEO_MODE video_mode;
   XGetVideoMode(&video_mode);

   *width  = video_mode.dwDisplayWidth;
   *height = video_mode.dwDisplayHeight;

   if(video_mode.fIsHiDef)
   {
      *width = 1280;
      *height = 720;
      g_extern.lifecycle_state |= (1ULL << MODE_MENU_HD);
   }
   else
   {
	   *width = 640;
	   *height = 480;
      g_extern.lifecycle_state &= ~(1ULL << MODE_MENU_HD);
   }

   if(video_mode.fIsWideScreen)
	   g_extern.lifecycle_state |= (1ULL << MODE_MENU_WIDESCREEN);
   else
      g_extern.lifecycle_state &= ~(1ULL << MODE_MENU_WIDESCREEN);
#elif defined(_XBOX1)
   DWORD video_mode = XGetVideoFlags();

    *width  = 640;
    *height = 480;

   // Only valid in PAL mode, not valid for HDTV modes!
   if(XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I)
   {
      // Check for 16:9 mode (PAL REGION)
      if(video_mode & XC_VIDEO_FLAGS_WIDESCREEN)
      {
         if(video_mode & XC_VIDEO_FLAGS_PAL_60Hz)
         {	//60 Hz, 720x480i
            *width = 720;
            *height = 480;
         }
         else
         {	//50 Hz, 720x576i
            *width = 720;
            *height = 576;
         }
         g_extern.lifecycle_state |= (1ULL << MODE_MENU_WIDESCREEN);
      }
      else
         g_extern.lifecycle_state &= ~(1ULL << MODE_MENU_WIDESCREEN);
   }
   else
   {
      // Check for 16:9 mode (NTSC REGIONS)
      if(video_mode & XC_VIDEO_FLAGS_WIDESCREEN)
      {
         *width = 720;
         *height = 480;
         g_extern.lifecycle_state |= (1ULL << MODE_MENU_WIDESCREEN);
      }
	  else
       g_extern.lifecycle_state &= ~(1ULL << MODE_MENU_WIDESCREEN);
   }

   if(XGetAVPack() == XC_AV_PACK_HDTV)
   {
      if(video_mode & XC_VIDEO_FLAGS_HDTV_480p)
      {
         *width	= 640;
         *height  = 480;
         g_extern.lifecycle_state &= ~(1ULL << MODE_MENU_WIDESCREEN);
         g_extern.lifecycle_state |= (1ULL << MODE_MENU_HD);
      }
	   else if(video_mode & XC_VIDEO_FLAGS_HDTV_720p)
      {
         *width	= 1280;
         *height  = 720;
         g_extern.lifecycle_state |= (1ULL << MODE_MENU_WIDESCREEN);
         g_extern.lifecycle_state |= (1ULL << MODE_MENU_HD);
      }
	   else if(video_mode & XC_VIDEO_FLAGS_HDTV_1080i)
      {
         *width	= 1920;
         *height  = 1080;
         g_extern.lifecycle_state |= (1ULL << MODE_MENU_WIDESCREEN);
         g_extern.lifecycle_state |= (1ULL << MODE_MENU_HD);
      }
   }
#endif
#endif
}

static void gfx_ctx_d3d_swap_interval(unsigned interval)
{
#ifdef _XBOX
   d3d_video_t *d3d = (d3d_video_t*)driver.video_data;
   LPDIRECT3DDEVICE d3dr = d3d->dev;

   if (interval)
      d3dr->SetRenderState(XBOX_PRESENTATIONINTERVAL, D3DPRESENT_INTERVAL_ONE);
   else
      d3dr->SetRenderState(XBOX_PRESENTATIONINTERVAL, D3DPRESENT_INTERVAL_IMMEDIATE);
#endif
}

const gfx_ctx_driver_t gfx_ctx_d3d9 = {
   gfx_ctx_d3d_init,
   NULL,							// gfx_ctx_destroy
   gfx_ctx_d3d_bind_api,
   gfx_ctx_d3d_swap_interval,
   NULL,							// gfx_ctx_set_video_mode
   gfx_ctx_d3d_get_video_size,
   NULL,							
   gfx_ctx_d3d_update_title,
   gfx_ctx_d3d_check_window,
   d3d_resize,
   gfx_ctx_d3d_has_focus,
   gfx_ctx_d3d_swap_buffers,
   gfx_ctx_d3d_input_driver,
   NULL,
   gfx_ctx_d3d_show_mouse,
   "d3d",
};
