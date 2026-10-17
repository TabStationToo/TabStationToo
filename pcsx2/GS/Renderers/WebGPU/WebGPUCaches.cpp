// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/WebGPUCaches.h"

WebGPUSamplerCache::WebGPUSamplerCache() = default;

WebGPUSamplerCache::~WebGPUSamplerCache()
{
	Clear();
}

void WebGPUSamplerCache::SetDevice(WGPUDevice device)
{
	Clear();
	m_device = device;
}

WGPUSampler WebGPUSamplerCache::Get(GSHWDrawConfig::SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
		return it->second;

	// The same mapping as GSDeviceVK::GetSampler(); anisotropic filtering is done in the shader.
	WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
	desc.magFilter = ss.IsMagFilterLinear() ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.minFilter = ss.IsMinFilterLinear() ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.mipmapFilter = ss.IsMipFilterLinear() ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
	desc.addressModeU = ss.tau ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
	desc.addressModeV = ss.tav ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
	desc.addressModeW = WGPUAddressMode_ClampToEdge;
	desc.lodMinClamp = 0.0f;
	desc.lodMaxClamp = (ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : 32.0f;
	desc.maxAnisotropy = 1;
	WGPUSampler sampler = wgpuDeviceCreateSampler(m_device, &desc);
	m_samplers.emplace(ss.key, sampler);
	return sampler;
}

WGPUSampler WebGPUSamplerCache::GetNonFiltering()
{
	if (!m_non_filtering)
	{
		WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
		desc.magFilter = WGPUFilterMode_Nearest;
		desc.minFilter = WGPUFilterMode_Nearest;
		desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		desc.addressModeU = WGPUAddressMode_ClampToEdge;
		desc.addressModeV = WGPUAddressMode_ClampToEdge;
		desc.addressModeW = WGPUAddressMode_ClampToEdge;
		desc.lodMaxClamp = 0.25f;
		m_non_filtering = wgpuDeviceCreateSampler(m_device, &desc);
	}

	return m_non_filtering;
}

void WebGPUSamplerCache::Clear()
{
	for (const auto& it : m_samplers)
	{
		if (it.second)
			wgpuSamplerRelease(it.second);
	}
	m_samplers.clear();

	if (m_non_filtering)
	{
		wgpuSamplerRelease(m_non_filtering);
		m_non_filtering = nullptr;
	}
}
