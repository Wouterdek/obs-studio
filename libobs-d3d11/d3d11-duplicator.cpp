/******************************************************************************
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "d3d11-subsystem.hpp"

static inline bool get_monitor(gs_device_t *device, int monitor_idx,
		IDXGIOutput **dxgiOutput)
{
	HRESULT hr;

	hr = device->adapter->EnumOutputs(monitor_idx, dxgiOutput);
	if (FAILED(hr)) {
		if (hr == DXGI_ERROR_NOT_FOUND)
			return false;

		throw HRError("Failed to get output", hr);
	}

	return true;
}

void gs_duplicator::Start()
{
	ComPtr<IDXGIOutput1> output1;
	ComPtr<IDXGIOutput> output;
	HRESULT hr;

	if (!get_monitor(device, idx, output.Assign()))
		throw "Invalid monitor index";

	hr = output->QueryInterface(__uuidof(IDXGIOutput1),
			(void**)output1.Assign());
	if (FAILED(hr))
		throw HRError("Failed to query IDXGIOutput1", hr);

	hr = output1->DuplicateOutput(device->device, duplicator.Assign());
	if (FAILED(hr))
		throw HRError("Failed to duplicate output", hr);
}

gs_duplicator::gs_duplicator(gs_device_t *device_, int monitor_idx)
	: gs_obj  (device_, gs_type::gs_duplicator),
	  texture (nullptr),
	  idx     (monitor_idx),
	  nReferences (0)
{
	Start();
}

gs_duplicator::~gs_duplicator()
{
	delete texture;
}

extern "C" {

EXPORT bool device_get_duplicator_monitor_info(gs_device_t *device,
		int monitor_idx, struct gs_monitor_info *info)
{
	DXGI_OUTPUT_DESC desc;
	HRESULT hr;

	try {
		ComPtr<IDXGIOutput> output;

		if (!get_monitor(device, monitor_idx, output.Assign()))
			return false;

		hr = output->GetDesc(&desc);
		if (FAILED(hr))
			throw HRError("GetDesc failed", hr);

	} catch (HRError error) {
		blog(LOG_ERROR, "device_get_duplicator_monitor_info: "
		                "%s (%08lX)", error.str, error.hr);
		return false;
	}

	switch (desc.Rotation) {
	case DXGI_MODE_ROTATION_UNSPECIFIED:
	case DXGI_MODE_ROTATION_IDENTITY:
		info->rotation_degrees = 0;
		break;

	case DXGI_MODE_ROTATION_ROTATE90:
		info->rotation_degrees = 90;
		break;

	case DXGI_MODE_ROTATION_ROTATE180:
		info->rotation_degrees = 180;
		break;

	case DXGI_MODE_ROTATION_ROTATE270:
		info->rotation_degrees = 270;
		break;
	}

	info->x = desc.DesktopCoordinates.left;
	info->y = desc.DesktopCoordinates.top;
	info->cx = desc.DesktopCoordinates.right - info->x;
	info->cy = desc.DesktopCoordinates.bottom - info->y;

	return true;
}

std::map<int, gs_duplicator*> gs_duplicator::instances;

EXPORT gs_duplicator_t *device_duplicator_create(gs_device_t *device,
		int monitor_idx)
{
	gs_duplicator *duplicator;

	//There can only be one gs_duplicator instance per monitor_idx value.
	//Search for a matching instance in the map, or create a new one if no
	//such instance exists yet.
	map<int, gs_duplicator*>::iterator it = gs_duplicator::instances.find(monitor_idx);
	if(it != gs_duplicator::instances.end())
	{
		duplicator = it->second;
	}else
	{
		try {
			duplicator = new gs_duplicator(device, monitor_idx);
			gs_duplicator::instances[monitor_idx] = duplicator;
		}
		catch (const char *error) {
			blog(LOG_DEBUG, "device_duplicator_create: %s",
				error);
			return nullptr;

		}
		catch (HRError error) {
			blog(LOG_DEBUG, "device_duplicator_create: %s (%08lX)",
				error.str, error.hr);
			return nullptr;
		}
	}

	duplicator->nReferences++;
	return duplicator;
}

EXPORT void gs_duplicator_destroy(gs_duplicator_t *duplicator)
{
	duplicator->nReferences--;
	if(duplicator->nReferences == 0)
	{
		gs_duplicator::instances.erase(duplicator->idx);
		delete duplicator;
	}
}

static inline void copy_texture(gs_duplicator_t *d, ID3D11Texture2D *tex)
{
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);

	if (!d->texture ||
	    d->texture->width != desc.Width ||
	    d->texture->height != desc.Height) {

		delete d->texture;
		d->texture = (gs_texture_2d*)gs_texture_create(
				desc.Width, desc.Height,
				ConvertDXGITextureFormat(desc.Format), 1,
				nullptr, 0);
	}

	if (!!d->texture)
		d->device->context->CopyResource(d->texture->texture,
				tex);
}

EXPORT bool gs_duplicator_update_frame(gs_duplicator_t *d)
{
	DXGI_OUTDUPL_FRAME_INFO info;
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<IDXGIResource> res;
	HRESULT hr;

	if (!d->duplicator) {
		return false;
	}

	hr = d->duplicator->AcquireNextFrame(0, &info, res.Assign());
	if (hr == DXGI_ERROR_ACCESS_LOST) {
		return false;

	} else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
		return true;

	} else if (FAILED(hr)) {
		blog(LOG_ERROR, "gs_duplicator_update_frame: Failed to update "
		                "frame (%08lX)", hr);
		return true;
	}

	hr = res->QueryInterface(__uuidof(ID3D11Texture2D),
			(void**)tex.Assign());
	if (FAILED(hr)) {
		blog(LOG_ERROR, "gs_duplicator_update_frame: Failed to query "
		               "ID3D11Texture2D (%08lX)", hr);
		d->duplicator->ReleaseFrame();
		return true;
	}

	copy_texture(d, tex);
	d->duplicator->ReleaseFrame();
	return true;
}

EXPORT gs_texture_t *gs_duplicator_get_texture(gs_duplicator_t *duplicator)
{
	return duplicator->texture;
}

}
