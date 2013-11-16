/*****************************************************************************
 * Copyright (C) 2013 Birunthan Mohanathas
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "cad_sdk.h"

#include <Windows.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_url.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static int Open(vlc_object_t* p_this);
static void Close(vlc_object_t* p_this);

// Module descriptor.
#define MODULE_STRING "cad"
vlc_module_begin()
	set_shortname("CAD NowPlaying")
	set_description("CAD NowPlaying")
	
	set_category(CAT_INTERFACE)
	set_subcategory(SUBCAT_INTERFACE_CONTROL)

	set_capability("interface", 0)
	set_callbacks(Open, Close)
vlc_module_end()

// Module instance data.
struct intf_sys_t
{
	vlc_thread_t thread;
	vlc_mutex_t lock;
	HWND window;
	HWND cad_window;

	int i_id;
	input_thread_t* p_input;
};

const int DATA_MAX_LENGTH = 1024;

static void* Thread(void* p_data);
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static int InputEvent(
	vlc_object_t* p_this, const char* var, vlc_value_t oldval, vlc_value_t val, void* p_data);
static int PlaylistEvent(
	vlc_object_t* p_this, const char* var, vlc_value_t oldval, vlc_value_t val, void* p_data);

// Initialize interface.
static int Open(vlc_object_t* p_this)
{
	intf_thread_t* const p_intf = (intf_thread_t*)p_this;

	// Limit to one instance.
	if (FindWindow(L"CD Art Display IPC Class", L"VLC"))
	{
		p_intf->p_sys = NULL;
		return VLC_SUCCESS;
	}
	
	OutputDebugString(L"VLC Open");
	intf_sys_t* const p_sys = (intf_sys_t*)calloc(1, sizeof(intf_sys_t));
	if (!p_sys)
	{
		return VLC_ENOMEM;
	}
	p_intf->p_sys = p_sys;

	vlc_mutex_init(&p_sys->lock);

	if (vlc_clone(&p_sys->thread, Thread, p_intf, VLC_THREAD_PRIORITY_LOW))
	{
		vlc_mutex_destroy(&p_sys->lock);
		free(p_sys);
		p_intf->p_sys = NULL;
		return VLC_ENOMEM;
	}

	var_AddCallback(pl_Get(p_intf->p_libvlc), "input-current", PlaylistEvent, p_intf);

	return VLC_SUCCESS;
}

// Destroy interface.
static void Close(vlc_object_t* p_this)
{
	intf_thread_t* p_intf = (intf_thread_t*)p_this;
	intf_sys_t* p_sys = p_intf->p_sys;

	if (!p_sys)
	{
		// This instance was not initialized as there was already another instance active.
		return;
	}

	if (p_sys->p_input)
	{
		vlc_object_release(p_sys->p_input);
		p_sys->p_input = NULL;
	}

	var_DelCallback(pl_Get(p_intf->p_libvlc), "input-current", PlaylistEvent, p_intf);

	if (p_sys->cad_window)
	{
		PostMessage(p_sys->cad_window, WM_USER, 0, IPC_SHUTDOWN_NOTIFICATION);
	}

	vlc_mutex_lock(&p_sys->lock);
	if (p_sys->window)
	{
		PostMessage(p_sys->window, WM_CLOSE, 0, 0);
	}
	vlc_mutex_unlock(&p_sys->lock);

	vlc_join(p_sys->thread, NULL);
	vlc_mutex_destroy(&p_sys->lock);

	free(p_intf->p_sys);
}

// Playlist item change callback.
static int InputEvent(
	vlc_object_t* p_this, const char* var, vlc_value_t oldval, vlc_value_t val, void* p_data)
{ 
	input_thread_t* const p_input = (input_thread_t *)p_this;
	intf_thread_t* const p_intf = (intf_thread_t*)p_data;
	intf_sys_t* const p_sys = p_intf->p_sys;

	if (!p_sys->cad_window) return VLC_SUCCESS;

	switch (val.i_int)
	{
	case INPUT_EVENT_ITEM_META:
		{
			input_item_t* p_item = input_GetItem(p_input);
			if (input_item_IsPreparsed(p_item))
			{
				PostMessage(p_sys->cad_window, WM_USER, 0, IPC_TRACK_CHANGED_NOTIFICATION);
			}

			break;
		}

	case INPUT_EVENT_STATE:
		{
			const int input_state = var_GetInteger(p_input, "state");
			const int state =
				input_state == PLAYING_S ? 1 :
				input_state == PAUSE_S ? 2 :
				0;
			PostMessage(p_sys->cad_window, WM_USER, state, IPC_STATE_CHANGED_NOTIFICATION);
			break;
		}
	}

	return VLC_SUCCESS;
}

// Input change callback.
static int PlaylistEvent(
	vlc_object_t* p_this, const char* var, vlc_value_t oldval, vlc_value_t val, void* p_data)
{
	intf_thread_t* const p_intf = (intf_thread_t*)p_data;
	intf_sys_t* const p_sys = p_intf->p_sys;

	if (p_sys->p_input)
	{
		var_DelCallback(p_sys->p_input, "intf-event", InputEvent, p_intf);
		vlc_object_release(p_sys->p_input);
		p_sys->p_input = NULL;
	}

	p_sys->p_input = (input_thread_t*)val.p_address;
	var_AddCallback(p_sys->p_input, "intf-event", InputEvent, p_intf);
	vlc_object_hold(p_sys->p_input);

	return VLC_SUCCESS;
}

static void* Thread(void* p_data)
{
	intf_thread_t* const p_intf = (intf_thread_t*)p_data;
	intf_sys_t* const p_sys = p_intf->p_sys;

	const HMODULE module = (HINSTANCE)&__ImageBase;

	WNDCLASS wc;
	memset(&wc, 0, sizeof(WNDCLASS));
	wc.hInstance = module;
	wc.lpfnWndProc = WindowProc;
	wc.lpszClassName = L"CD Art Display IPC Class";
	RegisterClass(&wc);

	HWND window = CreateWindow(
		L"CD Art Display IPC Class", L"VLC", WS_DISABLED,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, module, NULL);
	if (!window) return NULL;

	vlc_mutex_lock(&p_sys->lock);
	p_sys->window = window;
	vlc_mutex_unlock(&p_sys->lock);

	SetWindowLongPtr(p_sys->window, GWLP_USERDATA, (LONG_PTR)p_intf);

	typedef BOOL (WINAPI * FPCHANGEWINDOWMESSAGEFILTER)(UINT message, DWORD dwFlag);
	typedef BOOL (WINAPI * FPCHANGEWINDOWMESSAGEFILTEREX)(
		HWND hWnd, UINT message, DWORD dwFlag, PCHANGEFILTERSTRUCT pChangeFilterStruct);

	HMODULE user32 = GetModuleHandle(L"user32.dll");
	// WM_USER needs to be removed from the filtered messages. To do so, try
	// ChangeWindowMessageFilterEx first (for Windows 7 and later).
	auto changeWindowMessageFilterEx =
		(FPCHANGEWINDOWMESSAGEFILTEREX)GetProcAddress(user32, "ChangeWindowMessageFilterEx");
	if (changeWindowMessageFilterEx)
	{
		changeWindowMessageFilterEx(p_sys->window, WM_USER, MSGFLT_ALLOW, NULL);
	}
	else
	{
		// Otherwise try ChangeWindowMessageFilter (for Windows Vista).
		FPCHANGEWINDOWMESSAGEFILTER changeWindowMessageFilter =
			(FPCHANGEWINDOWMESSAGEFILTER)GetProcAddress(user32, "ChangeWindowMessageFilter");
		if (changeWindowMessageFilter)
		{
			changeWindowMessageFilter(WM_USER, MSGFLT_ALLOW);
		}
	}

	p_sys->cad_window = FindWindow(NULL, L"CD Art Display 1.x Class");
	if (p_sys->cad_window)
	{
		WCHAR filename[MAX_PATH];
		GetModuleFileName(module, filename, MAX_PATH);

		WCHAR buffer[DATA_MAX_LENGTH];
		const int buffer_len = _snwprintf(
			buffer, ARRAYSIZE(buffer), L"1\tCD Art Display IPC Class\tVLC\t%s\t", filename);

		// Register with CAD.
		COPYDATASTRUCT cds;
		cds.dwData = IPC_REGISTER_NOTIFICATION;
		cds.lpData = buffer;
		cds.cbData = (buffer_len + 1) * sizeof(buffer[0]);
		SendMessage(p_sys->cad_window, WM_COPYDATA, 0, (LPARAM)&cds);
	}

	// Standard message loop.
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		DispatchMessage(&msg);
	}
	
	vlc_mutex_lock(&p_sys->lock);
	DestroyWindow(window);
	UnregisterClass(L"CD Art Display IPC Class", module);
	p_sys->window = NULL;
	vlc_mutex_unlock(&p_sys->lock);

	return 0;
}

static LRESULT HandleCadMessage(intf_thread_t* p_intf, HWND hwnd, WPARAM wParam, LPARAM lParam)
{
	intf_sys_t* const p_sys = p_intf->p_sys;

	switch (lParam)
	{
	case IPC_PLAY:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			playlist_Play(p_playlist);
			return 1;
		}

	case IPC_PLAYPAUSE:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			const bool playing = playlist_Status(p_playlist) == PLAYLIST_RUNNING;
			playlist_Control(p_playlist, playing ? PLAYLIST_PAUSE : PLAYLIST_PLAY, pl_Unlocked);
			return 1;
		}

	case IPC_PAUSE:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			playlist_Pause(p_playlist);
			return 1;
		}

	case IPC_STOP:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			playlist_Stop(p_playlist);
			return 1;
		}
			
	case IPC_NEXT:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			playlist_Next(p_playlist);
			return 1;
		}

	case IPC_PREVIOUS:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			playlist_Prev(p_playlist);
			return 1;
		}

	case IPC_SET_VOLUME:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			playlist_VolumeSet(
				p_playlist,
				(int)wParam / 100.0f);
			return 1;
		}

	case IPC_GET_VOLUME:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			const float volume = playlist_VolumeGet(p_playlist) * 100.0f;
			return (LRESULT)min(volume, 100.0f);
		}

	case IPC_GET_DURATION:
		{
			input_thread_t* p_input = pl_CurrentInput(p_intf->p_libvlc);
			unsigned int duration = 0;
			if (p_input)
			{
				input_item_t* p_item = input_GetItem(p_input);
				duration = (unsigned int)(input_item_GetDuration(p_item) / 1000000);
				vlc_object_release(p_input);
			}
			return duration;
		}

	case IPC_GET_POSITION:
		{
			int pos = 0;
			input_thread_t* p_input = pl_CurrentInput(p_intf->p_libvlc);
			if (p_input)
			{
				pos = (int)(var_GetTime(p_input, "time") / CLOCK_FREQ);
				vlc_object_release(p_input);
			}
			return pos;
		}

	case IPC_SET_POSITION:
		{
			input_thread_t* p_input = pl_CurrentInput(p_intf->p_libvlc);
			if (p_input)
			{
				var_SetTime(p_input, "time", (int64_t)wParam * CLOCK_FREQ);
				vlc_object_release(p_input);
			}
			return 0;
		}

	case IPC_GET_SHUFFLE:
		{
			return (int)var_GetBool(pl_Get(p_intf->p_libvlc), "random");
		}

	case IPC_SET_SHUFFLE:
		{
			return (int)var_SetBool(pl_Get(p_intf->p_libvlc), "random", (bool)wParam);
		}

	case IPC_GET_REPEAT:
		{
			return (int)var_GetBool(pl_Get(p_intf->p_libvlc), "repeat");
		}

	case IPC_SET_REPEAT:
		{
			return (int)var_SetBool(pl_Get(p_intf->p_libvlc), "repeat", (bool)wParam);
		}

	case IPC_SET_RATING:
		{
			// Send back 0.
			PostMessage(p_sys->cad_window, WM_USER, 0, IPC_RATING_CHANGED_NOTIFICATION);
			return 0;
		}

	case IPC_SET_CALLBACK_HWND:
		{
			p_sys->cad_window = (HWND)wParam;
			return 1;
		}

	case IPC_SHOW_WINDOW:
		{
			// TODO.
			return 0;
		}

	case IPC_GET_STATE:
		{
			playlist_t* p_playlist = pl_Get(p_intf->p_libvlc);
			if (p_playlist)
			{
				switch (playlist_Status(p_playlist))
				{
				case PLAYLIST_RUNNING: return 1;
				case PLAYLIST_PAUSED: return 2;
				}
			}

			return 0;
		}

	case IPC_SHUTDOWN_NOTIFICATION:
		{
			p_sys->cad_window = NULL;
			return 1;
		}

	case IPC_CLOSE:
		{
			libvlc_Quit(p_intf->p_libvlc);
			return 1;
		}

	case IPC_GET_CURRENT_TRACK:
		{
			input_thread_t* p_input = pl_CurrentInput(p_intf->p_libvlc);
			if (!p_input) return 0;

			input_item_t* p_item = input_GetItem(p_input);

			char buffer[DATA_MAX_LENGTH];
			int buffer_len = 0;

			char* const file = decode_URI(input_item_GetURI(p_item));
			if (strncmp(file, "file://", 7) == 0)
			{
				char* const title = input_item_GetTitleFbName(p_item);
				char* const artist = input_item_GetArtist(p_item);
				char* const album = input_item_GetAlbum(p_item);
				char* const cover = decode_URI(input_item_GetArtworkURL(p_item));
				const unsigned int duration = input_item_GetDuration(p_item) / 1000000U;

				buffer_len = _snprintf(
					buffer, ARRAYSIZE(buffer), "%s\t%s\t%s\t\t\t\t\t%u\t%s\t\t%s\t\t\t\t\t\t\t",
					title ? title : "",
					artist ? artist : "",
					album ? album : "",
					duration,
					file ? &file[8] : "",
					cover ? &cover[8] : "");  // skip the file://

				free(title);
				free(artist);
				free(album);
				free(cover);
			}
			else if (char* now_playing = input_item_GetNowPlaying(p_item))
			{
				char* artist = NULL;
				char* title = now_playing;
				char* pos = strstr(now_playing, " - ");
				if (pos)
				{
					pos[0] = '\0';
					artist = title;
					title = pos + 3;	// Skip the " - "
				}

				buffer_len = _snprintf(
					buffer, ARRAYSIZE(buffer), "%s\t%s\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t",
					title ? title : "",
					artist ? artist : "");

				free(now_playing);
			}

			free(file);

			if (buffer_len)
			{
				wchar_t buffer_w[DATA_MAX_LENGTH];
				const int buffer_w_len = MultiByteToWideChar(
					CP_UTF8, 0, buffer, buffer_len + 1, buffer_w, ARRAYSIZE(buffer_w));

				COPYDATASTRUCT cds;
				cds.dwData = IPC_CURRENT_TRACK_NOTIFICATION;
				cds.lpData = &buffer_w;
				cds.cbData = buffer_w_len * sizeof(buffer_w[0]);
				SendMessage(
					p_sys->cad_window, WM_COPYDATA, IPC_CURRENT_TRACK_NOTIFICATION, (LPARAM)&cds);
			}

			vlc_object_release(p_input);
			return 1;
		}
	}

	return 0;
}

// Event callback.
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	intf_thread_t* const p_intf = (intf_thread_t*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	if (p_intf)
	{
		switch (msg)
		{
		case WM_USER:
			return HandleCadMessage(p_intf, hwnd, wParam, lParam);

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}
