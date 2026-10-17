// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <webgpu/webgpu.h>

#include <unordered_map>

// Caches of immutable WebGPU objects (docs/web-port/WEBGPU-DESIGN.md §6.7).
//
// Samplers are keyed by GSHWDrawConfig::SamplerSelector, as in GSDeviceVK::GetSampler(); the
// utility passes use its Point() and Linear() entries. Utility bind groups (texture + sampler) are
// cached on each GSTextureWebGPU instead, so a texture's destructor drops them; the TFX bind-group
// cache keyed by view tuples comes with the HW renderer (step 2).
class WebGPUSamplerCache
{
public:
	WebGPUSamplerCache();
	~WebGPUSamplerCache();

	void SetDevice(WGPUDevice device);
	WGPUSampler Get(GSHWDrawConfig::SamplerSelector ss);
	/// A non-filtering point sampler, for unfilterable-float sources (depth, r32float).
	WGPUSampler GetNonFiltering();
	void Clear();

private:
	WGPUDevice m_device = nullptr;
	std::unordered_map<u8, WGPUSampler> m_samplers;
	WGPUSampler m_non_filtering = nullptr;
};
