/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 RenderDoc contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include "driver/dx/official/d3d12.h"

struct ResourceId;

inline bool D3D12AllocationRangesOverlap(UINT64 a, UINT64 aSize, UINT64 b, UINT64 bSize)
{
  return aSize && bSize && aSize <= ~0ULL - a && bSize <= ~0ULL - b && a < b + bSize && b < a + aSize;
}

inline bool D3D12NeedsWildcardAlias(UINT64 a, UINT64 aNative, UINT64 aNeutral, UINT64 b,
                                    UINT64 bNative, UINT64 bNeutral, UINT64 aReported = 0,
                                    UINT64 bReported = 0)
{
  const UINT64 aLogical = aReported ? aReported : (aNative > aNeutral ? aNative : aNeutral);
  const UINT64 bLogical = bReported ? bReported : (bNative > bNeutral ? bNative : bNeutral);
  return D3D12AllocationRangesOverlap(a, aLogical, b, bLogical) &&
         (!D3D12AllocationRangesOverlap(a, aNative, b, bNative) ||
          !D3D12AllocationRangesOverlap(a, aNeutral, b, bNeutral));
}

// Combine requirements before the application suballocates its heap. Growing a heap at replay
// cannot fix overlaps with the next resource when a driver's application profile changes sizes.
inline bool D3D12MergeAllocationLayout(UINT count, const D3D12_RESOURCE_ALLOCATION_INFO *requirements,
                                       D3D12_RESOURCE_ALLOCATION_INFO nativeTotal,
                                       D3D12_RESOURCE_ALLOCATION_INFO &total,
                                       D3D12_RESOURCE_ALLOCATION_INFO1 *placements)
{
  UINT64 end = 0, alignment = nativeTotal.Alignment;
  if(!alignment || (alignment & (alignment - 1)) || nativeTotal.SizeInBytes == ~0ULL)
    return false;

  for(UINT i = 0; i < count; i++)
  {
    const UINT64 a = requirements[i].Alignment, size = requirements[i].SizeInBytes;
    if(!a || (a & (a - 1)) || size == ~0ULL || end > ~0ULL - (a - 1))
      return false;
    const UINT64 offset = (end + a - 1) & ~(a - 1);
    if(size > ~0ULL - offset)
      return false;
    if(placements)
      placements[i] = {offset, a, size};
    end = offset + size;
    if(a > alignment)
      alignment = a;
  }

  if(end < nativeTotal.SizeInBytes)
    end = nativeTotal.SizeInBytes;
  if(end > ~0ULL - (alignment - 1))
    return false;
  total = {(end + alignment - 1) & ~(alignment - 1), alignment};
  return total.SizeInBytes != ~0ULL;
}

D3D12_RESOURCE_ALLOCATION_INFO D3D12PortableAllocationInfo(
    ID3D12Device *device, UINT version, UINT visibleMask, UINT count,
    const D3D12_RESOURCE_DESC1 *descs, const UINT32 *numFormats, const DXGI_FORMAT *const *formats,
    D3D12_RESOURCE_ALLOCATION_INFO nativeTotal, D3D12_RESOURCE_ALLOCATION_INFO1 *placements);

void D3D12RecordPortableResource(ID3D12Device *device, ResourceId resource, ResourceId heap,
                                 UINT64 offset, UINT version, const D3D12_RESOURCE_DESC1 &desc,
                                 UINT numFormats, const DXGI_FORMAT *formats);
void D3D12ForgetPortableResource(ResourceId resource);
bool D3D12PortableAliasNeedsWildcard(ResourceId before, ResourceId after);
