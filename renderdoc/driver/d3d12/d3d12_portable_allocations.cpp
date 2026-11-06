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

#include "d3d12_portable_allocations.h"
#include <atomic>
#include "core/settings.h"
#include "strings/string_utils.h"
#include "d3d12_common.h"

RDOC_CONFIG(bool, D3D12_Capture_NormaliseResourceAllocations, true,
            "Compare allocation requirements with a helper on the same adapter outside the "
            "captured application's driver profile, and reserve enough space for both. "
            "Requires renderdoccmd next to the capture library. Other replay GPUs/drivers can "
            "still have different requirements.");

namespace
{
const UINT ProtocolVersion = 1;
const UINT MaxFormats = 256;

struct ProbeInit
{
  UINT protocol;
  LUID adapter;
};

struct ProbeRequest
{
  UINT version;
  UINT mask;
  UINT numFormats;
  D3D12_RESOURCE_DESC1 desc;
};

bool SyncIO(HANDLE pipe, void *data, DWORD size, bool write)
{
  byte *bytes = (byte *)data;
  while(size)
  {
    DWORD done = 0;
    BOOL ok = write ? WriteFile(pipe, bytes, size, &done, NULL)
                    : ReadFile(pipe, bytes, size, &done, NULL);
    if(!ok || !done)
      return false;
    bytes += done;
    size -= done;
  }
  return true;
}

D3D12_RESOURCE_ALLOCATION_INFO Query(ID3D12Device *device, const ProbeRequest &request,
                                     const DXGI_FORMAT *formats)
{
  D3D12_RESOURCE_ALLOCATION_INFO result = {~0ULL, 0};
  if(request.version == 0)
  {
    D3D12_RESOURCE_DESC desc = {};
    memcpy(&desc, &request.desc, sizeof(desc));
    return device->GetResourceAllocationInfo(request.mask, 1, &desc);
  }
  if(request.version == 1)
  {
    ID3D12Device4 *dev = NULL;
    if(SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device4), (void **)&dev)))
    {
      D3D12_RESOURCE_DESC desc = {};
      memcpy(&desc, &request.desc, sizeof(desc));
      result = dev->GetResourceAllocationInfo1(request.mask, 1, &desc, NULL);
      dev->Release();
    }
  }
  else if(request.version == 2)
  {
    ID3D12Device8 *dev = NULL;
    if(SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device8), (void **)&dev)))
    {
      result = dev->GetResourceAllocationInfo2(request.mask, 1, &request.desc, NULL);
      dev->Release();
    }
  }
  else if(request.version == 3)
  {
    ID3D12Device12 *dev = NULL;
    if(SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device12), (void **)&dev)))
    {
      const DXGI_FORMAT *formatArray[] = {formats};
      result = dev->GetResourceAllocationInfo3(request.mask, 1, &request.desc,
                                               request.numFormats ? &request.numFormats : NULL,
                                               request.numFormats ? formatArray : NULL, NULL);
      dev->Release();
    }
  }
  return result;
}

rdcstr CacheKey(const ProbeRequest &r, const DXGI_FORMAT *formats)
{
  // Don't hash padding bytes from caller-owned D3D12 structures.
  rdcstr key;
  // rdcstr ordering is string ordering: encode bytes so embedded NULs cannot collapse keys.
  auto append = [&key](const void *value, size_t size) {
    const char hex[] = "0123456789abcdef";
    const byte *bytes = (const byte *)value;
    for(size_t i = 0; i < size; i++)
    {
      key += hex[bytes[i] >> 4];
      key += hex[bytes[i] & 15];
    }
  };
#define APPEND_KEY(value) append(&(value), sizeof(value))
  APPEND_KEY(r.version);
  APPEND_KEY(r.mask);
  APPEND_KEY(r.numFormats);
  APPEND_KEY(r.desc.Dimension);
  APPEND_KEY(r.desc.Alignment);
  APPEND_KEY(r.desc.Width);
  APPEND_KEY(r.desc.Height);
  APPEND_KEY(r.desc.DepthOrArraySize);
  APPEND_KEY(r.desc.MipLevels);
  APPEND_KEY(r.desc.Format);
  APPEND_KEY(r.desc.SampleDesc.Count);
  APPEND_KEY(r.desc.SampleDesc.Quality);
  APPEND_KEY(r.desc.Layout);
  APPEND_KEY(r.desc.Flags);
  APPEND_KEY(r.desc.SamplerFeedbackMipRegion.Width);
  APPEND_KEY(r.desc.SamplerFeedbackMipRegion.Height);
  APPEND_KEY(r.desc.SamplerFeedbackMipRegion.Depth);
#undef APPEND_KEY
  if(r.numFormats)
    append(formats, r.numFormats * sizeof(DXGI_FORMAT));
  return key;
}

struct AllocationProbe
{
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HANDLE process = NULL;
  bool attempted = false;
  bool warnedUnsupported = false;
  std::map<rdcstr, D3D12_RESOURCE_ALLOCATION_INFO> cache;

  ~AllocationProbe() { Close(); }

  void Close()
  {
    if(pipe != INVALID_HANDLE_VALUE)
      CloseHandle(pipe);
    pipe = INVALID_HANDLE_VALUE;
    if(process)
    {
      // This is our private helper, never the captured application.
      if(WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
        TerminateProcess(process, 1);
      CloseHandle(process);
    }
    process = NULL;
  }

  bool Complete(OVERLAPPED &overlapped, DWORD timeout)
  {
    HANDLE waits[] = {overlapped.hEvent, process};
    const DWORD wait = WaitForMultipleObjects(process ? 2 : 1, waits, FALSE, timeout);
    if(wait == WAIT_OBJECT_0)
      return true;
    CancelIoEx(pipe, &overlapped);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
    return false;
  }

  bool IO(void *data, DWORD size, bool write, DWORD timeout = 5000)
  {
    byte *bytes = (byte *)data;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!ov.hEvent)
      return false;
    bool success = true;
    while(size)
    {
      ResetEvent(ov.hEvent);
      DWORD done = 0;
      BOOL ok = write ? WriteFile(pipe, bytes, size, &done, &ov)
                      : ReadFile(pipe, bytes, size, &done, &ov);
      if(!ok && GetLastError() == ERROR_IO_PENDING)
        ok = Complete(ov, timeout) && GetOverlappedResult(pipe, &ov, &done, FALSE);
      if(!ok || !done)
      {
        success = false;
        break;
      }
      bytes += done;
      size -= done;
    }
    CloseHandle(ov.hEvent);
    return success;
  }

  bool Start(LUID adapter)
  {
    attempted = true;
    rdcstr library;
    FileIO::GetLibraryFilename(library);
    // Driver projects are static libraries and don't receive the main DLL project's
    // RDOC_BASE_NAME definition. Derive the companion name from the loaded library instead.
    const rdcstr exePath = strip_extension(library) + "cmd.exe";
    const rdcwstr exe = StringFormat::UTF82Wide(exePath);
    const rdcstr pipePath =
        StringFormat::Fmt("\\\\.\\pipe\\RenderDocAllocation-%lu-%llu", GetCurrentProcessId(),
                          (unsigned long long)(uintptr_t)this);
    const rdcwstr pipeName = StringFormat::UTF82Wide(pipePath);
    pipe = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096,
        0, NULL);
    if(pipe == INVALID_HANDLE_VALUE)
      return false;

    rdcwstr command = StringFormat::UTF82Wide(StringFormat::Fmt(
        "\"%s\" d3d12alloc \"%s\" %lu", exePath.c_str(), pipePath.c_str(), GetCurrentProcessId()));
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child = {};
    if(!CreateProcessW(exe.c_str(), &command[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                       &startup, &child))
      return false;
    process = child.hProcess;
    CloseHandle(child.hThread);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!ov.hEvent)
      return false;
    BOOL connected = ConnectNamedPipe(pipe, &ov);
    if(!connected)
    {
      const DWORD error = GetLastError();
      connected = error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING && Complete(ov, 30000));
    }
    CloseHandle(ov.hEvent);
    ULONG client = 0;
    if(!connected || !GetNamedPipeClientProcessId(pipe, &client) || client != child.dwProcessId)
      return false;

    ProbeInit init = {ProtocolVersion, adapter};
    HRESULT status = E_FAIL;
    return IO(&init, sizeof(init), true) && IO(&status, sizeof(status), false, 30000) &&
           SUCCEEDED(status);
  }

  bool Get(LUID adapter, ProbeRequest &request, const DXGI_FORMAT *formats,
           D3D12_RESOURCE_ALLOCATION_INFO &result)
  {
    rdcstr key = CacheKey(request, formats);
    auto it = cache.find(key);
    if(it != cache.end())
    {
      result = it->second;
      return true;
    }
    if(!attempted && !Start(adapter))
    {
      RDCWARN(
          "Could not start allocation helper (error %lu); capture allocation portability "
          "is unavailable. Keep the matching renderdoccmd next to the capture library.",
          GetLastError());
      Close();
    }
    if(pipe == INVALID_HANDLE_VALUE)
      return false;
    if(!IO(&request, sizeof(request), true) ||
       (request.numFormats && !IO((void *)formats, request.numFormats * sizeof(DXGI_FORMAT), true)) ||
       !IO(&result, sizeof(result), false))
    {
      RDCWARN(
          "Allocation helper disconnected or timed out; retaining native allocation "
          "requirements. Capture allocation portability is no longer guaranteed.");
      Close();
      return false;
    }
    if(result.SizeInBytes == ~0ULL || !result.Alignment)
    {
      if(!warnedUnsupported)
        RDCWARN(
            "Allocation helper cannot query this resource (API version %u); retaining native "
            "requirements. The helper runtime may not support the application's features.",
            request.version);
      warnedUnsupported = true;
      return false;
    }
    cache[key] = result;
    return true;
  }
};

struct ProbeRegistry;
std::atomic<ProbeRegistry *> liveRegistry(NULL);

struct ProbeRegistry
{
  struct Resource
  {
    ResourceId heap;
    UINT64 offset, nativeSize, neutralSize, reportedSize;
  };
  Threading::CriticalSection lock;
  std::map<uint64_t, AllocationProbe *> probes;
  std::map<ResourceId, Resource> resources;
  bool loggedAlias = false;
  ~ProbeRegistry()
  {
    liveRegistry.store(NULL, std::memory_order_release);
    for(auto &probe : probes)
      delete probe.second;
  }
};

ProbeRegistry &Registry()
{
  static ProbeRegistry registry;
  liveRegistry.store(&registry, std::memory_order_release);
  return registry;
}
};    // namespace

// Internal renderdoccmd entry point. Its replay marker prevents recursive capture of this device.
extern "C" RENDERDOC_API int RENDERDOC_CC INTERNAL_D3D12AllocationServer(const char *name,
                                                                         uint32_t parentPID)
{
  rdcwstr pipeName = StringFormat::UTF82Wide(name);
  const char prefix[] = "\\\\.\\pipe\\RenderDocAllocation-";
  if(strncmp(name, prefix, sizeof(prefix) - 1) != 0)
    return 1;
  HANDLE pipe =
      CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  ULONG server = 0;
  if(pipe == INVALID_HANDLE_VALUE)
    return 1;
  if(!GetNamedPipeServerProcessId(pipe, &server) || server != parentPID)
  {
    CloseHandle(pipe);
    return 1;
  }
  ProbeInit init = {};
  if(!SyncIO(pipe, &init, sizeof(init), false) || init.protocol != ProtocolVersion)
  {
    CloseHandle(pipe);
    return 1;
  }

  HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  typedef HRESULT(WINAPI * CreateFactory)(REFIID, void **);
  CreateFactory createFactory =
      dxgi ? (CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
  PFN_D3D12_CREATE_DEVICE createDevice =
      d3d12 ? (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
  IDXGIFactory4 *factory = NULL;
  IDXGIAdapter *adapter = NULL;
  ID3D12Device *device = NULL;
  HRESULT status = E_NOINTERFACE;
  if(createFactory && createDevice)
  {
    status = createFactory(__uuidof(IDXGIFactory4), (void **)&factory);
    if(SUCCEEDED(status))
      status = factory->EnumAdapterByLuid(init.adapter, __uuidof(IDXGIAdapter), (void **)&adapter);
    if(SUCCEEDED(status))
      status =
          createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&device);
  }
  bool connected = SyncIO(pipe, &status, sizeof(status), true) && SUCCEEDED(status);
  while(connected)
  {
    ProbeRequest request = {};
    DXGI_FORMAT formats[MaxFormats] = {};
    if(!SyncIO(pipe, &request, sizeof(request), false) || request.version > 3 ||
       request.numFormats > MaxFormats)
      break;
    if(request.numFormats && !SyncIO(pipe, formats, request.numFormats * sizeof(DXGI_FORMAT), false))
      break;
    // The unmodified replay path permits SRVs so depth resources can be inspected.
    request.desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_RESOURCE_ALLOCATION_INFO result = Query(device, request, formats);
    connected = SyncIO(pipe, &result, sizeof(result), true);
  }
  SAFE_RELEASE(device);
  SAFE_RELEASE(adapter);
  SAFE_RELEASE(factory);
  if(d3d12)
    FreeLibrary(d3d12);
  if(dxgi)
    FreeLibrary(dxgi);
  CloseHandle(pipe);
  return SUCCEEDED(status) ? 0 : 1;
}

D3D12_RESOURCE_ALLOCATION_INFO D3D12PortableAllocationInfo(
    ID3D12Device *device, UINT version, UINT visibleMask, UINT count,
    const D3D12_RESOURCE_DESC1 *descs, const UINT32 *numFormats, const DXGI_FORMAT *const *formats,
    D3D12_RESOURCE_ALLOCATION_INFO nativeTotal, D3D12_RESOURCE_ALLOCATION_INFO1 *placements)
{
  if(!D3D12_Capture_NormaliseResourceAllocations() || !count || !descs ||
     nativeTotal.SizeInBytes == ~0ULL || !nativeTotal.Alignment)
    return nativeTotal;

  ProbeRegistry &registry = Registry();
  SCOPED_LOCK(registry.lock);
  const LUID adapter = device->GetAdapterLuid();
  const uint64_t key = (uint64_t(UINT32(adapter.HighPart)) << 32) | adapter.LowPart;
  AllocationProbe *&probe = registry.probes[key];
  rdcarray<D3D12_RESOURCE_ALLOCATION_INFO> requirements;
  requirements.resize(count);
  bool changed = false;
  for(UINT i = 0; i < count; i++)
  {
    ProbeRequest request = {};
    request.version = version;
    request.mask = visibleMask;
    request.desc = descs[i];
    request.numFormats = numFormats ? numFormats[i] : 0;
    const DXGI_FORMAT *castable = formats ? formats[i] : NULL;
    if(request.numFormats > MaxFormats || (request.numFormats && !castable))
      return nativeTotal;

    D3D12_RESOURCE_ALLOCATION_INFO local = nativeTotal;
    if(count > 1)
    {
      if(placements)
        local = {placements[i].SizeInBytes, placements[i].Alignment};
      else
        local = Query(device, request, castable);
    }
    if(local.SizeInBytes == ~0ULL || !local.Alignment)
      return nativeTotal;
    requirements[i] = local;
    // Buffer sizes don't depend on texture metadata/compression optimisations.
    if(descs[i].Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      continue;
    if(!probe)
      probe = new AllocationProbe;
    D3D12_RESOURCE_ALLOCATION_INFO neutral = {};
    if(!probe->Get(adapter, request, castable, neutral))
      return nativeTotal;
    requirements[i].SizeInBytes = RDCMAX(local.SizeInBytes, neutral.SizeInBytes);
    requirements[i].Alignment = RDCMAX(local.Alignment, neutral.Alignment);
    if(local.SizeInBytes != requirements[i].SizeInBytes ||
       local.Alignment != requirements[i].Alignment)
    {
      changed = true;
      // Log only once per descriptor, independently from the IPC cache.
      static std::set<rdcstr> logged;
      if(logged.insert(CacheKey(request, castable)).second)
        RDCLOG(
            "Normalising capture allocation: %llux%u format=%u native=%llu/%llu "
            "replay-profile=%llu/%llu",
            descs[i].Width, descs[i].Height, (UINT)descs[i].Format, local.SizeInBytes,
            local.Alignment, neutral.SizeInBytes, neutral.Alignment);
    }
  }
  if(!changed)
    return nativeTotal;

  // Do not alter caller outputs until the entire replacement layout is known to be valid.
  D3D12_RESOURCE_ALLOCATION_INFO total = {};
  rdcarray<D3D12_RESOURCE_ALLOCATION_INFO1> offsets;
  offsets.resize(count);
  if(!D3D12MergeAllocationLayout(count, requirements.data(), nativeTotal, total, offsets.data()))
  {
    RDCWARN("Portable allocation layout overflow; retaining native requirements");
    return nativeTotal;
  }
  if(placements)
    memcpy(placements, offsets.data(), count * sizeof(*placements));
  return total;
}

void D3D12RecordPortableResource(ID3D12Device *device, ResourceId resource, ResourceId heap,
                                 UINT64 offset, UINT version, const D3D12_RESOURCE_DESC1 &desc,
                                 UINT numFormats, const DXGI_FORMAT *formats)
{
  if(!D3D12_Capture_NormaliseResourceAllocations() || numFormats > MaxFormats)
    return;
  ProbeRequest request = {};
  request.version = version;
  request.desc = desc;
  request.numFormats = numFormats;
  D3D12_RESOURCE_ALLOCATION_INFO native = Query(device, request, formats);
  if(native.SizeInBytes == ~0ULL || !native.Alignment)
    return;
  D3D12_RESOURCE_ALLOCATION_INFO neutral = native;
  ProbeRegistry &registry = Registry();
  SCOPED_LOCK(registry.lock);
  if(desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
  {
    const LUID adapter = device->GetAdapterLuid();
    const uint64_t key = (uint64_t(UINT32(adapter.HighPart)) << 32) | adapter.LowPart;
    AllocationProbe *&probe = registry.probes[key];
    if(!probe)
      probe = new AllocationProbe;
    if(!probe->Get(adapter, request, formats, neutral))
      neutral = native;
  }
  D3D12_RESOURCE_ALLOCATION_INFO requirement = {RDCMAX(native.SizeInBytes, neutral.SizeInBytes),
                                                RDCMAX(native.Alignment, neutral.Alignment)};
  D3D12_RESOURCE_ALLOCATION_INFO reported = native;
  if(!D3D12MergeAllocationLayout(1, &requirement, native, reported, NULL))
    reported = native;
  registry.resources[resource] = {heap, offset, native.SizeInBytes, neutral.SizeInBytes,
                                  reported.SizeInBytes};
}

void D3D12ForgetPortableResource(ResourceId resource)
{
  // Avoid constructing the registry for disabled captures or after process teardown.
  ProbeRegistry *registry = liveRegistry.load(std::memory_order_acquire);
  if(!registry)
    return;
  SCOPED_LOCK(registry->lock);
  registry->resources.erase(resource);
}

bool D3D12PortableAliasNeedsWildcard(ResourceId before, ResourceId after)
{
  ProbeRegistry &registry = Registry();
  SCOPED_LOCK(registry.lock);
  auto a = registry.resources.find(before), b = registry.resources.find(after);
  if(a == registry.resources.end() || b == registry.resources.end() ||
     a->second.heap != b->second.heap)
    return false;
  const ProbeRegistry::Resource &first = a->second, &second = b->second;
  const bool wildcard = D3D12NeedsWildcardAlias(first.offset, first.nativeSize, first.neutralSize,
                                                second.offset, second.nativeSize, second.neutralSize,
                                                first.reportedSize, second.reportedSize);
  if(wildcard && !registry.loggedAlias)
  {
    RDCLOG(
        "Normalising padded alias barrier: before offset=%llu size=%llu/%llu, "
        "after offset=%llu size=%llu/%llu",
        first.offset, first.nativeSize, first.neutralSize, second.offset, second.nativeSize,
        second.neutralSize);
    registry.loggedAlias = true;
  }
  return wildcard;
}

#if ENABLED(ENABLE_UNIT_TESTS)
#include "catch/catch.hpp"

TEST_CASE("Portable D3D12 allocation layout", "[d3d12]")
{
  D3D12_RESOURCE_ALLOCATION_INFO native = {8847360, 65536}, total = {};
  D3D12_RESOURCE_ALLOCATION_INFO requirements[] = {{11075584, 65536}, {8847360, 65536}};
  D3D12_RESOURCE_ALLOCATION_INFO1 slots[2] = {};
  REQUIRE(D3D12MergeAllocationLayout(2, requirements, native, total, slots));
  CHECK(slots[1].Offset == 11075584);
  CHECK(total.SizeInBytes == 19922944);

  // Mix small resources with MSAA alignment; all suballocations must remain in bounds.
  requirements[0] = {4096, 4096};
  requirements[1] = {4194304, 4194304};
  REQUIRE(D3D12MergeAllocationLayout(2, requirements, native, total, slots));
  CHECK(slots[1].Offset % 4194304 == 0);
  CHECK(slots[1].Offset >= slots[0].Offset + slots[0].SizeInBytes);
  CHECK(total.SizeInBytes >= slots[1].Offset + slots[1].SizeInBytes);
  CHECK(total.SizeInBytes >= native.SizeInBytes);

  requirements[0] = {~0ULL, 65536};
  CHECK_FALSE(D3D12MergeAllocationLayout(2, requirements, native, total, slots));
  requirements[0] = {1024, 3};
  CHECK_FALSE(D3D12MergeAllocationLayout(2, requirements, native, total, slots));
  requirements[0] = {~0ULL - 65535, 65536};
  CHECK_FALSE(D3D12MergeAllocationLayout(2, requirements, native, total, slots));
}

TEST_CASE("D3D12 allocation cache distinguishes descriptors", "[d3d12]")
{
  ProbeRequest a = {}, b = {};
  a.desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  b = a;
  b.desc.Format = DXGI_FORMAT_D32_FLOAT;
  CHECK(CacheKey(a, NULL) != CacheKey(b, NULL));
  CHECK((CacheKey(a, NULL) < CacheKey(b, NULL) || CacheKey(b, NULL) < CacheKey(a, NULL)));
  b = a;
  b.mask = 1;
  CHECK(CacheKey(a, NULL) != CacheKey(b, NULL));
  b = a;
  b.version = 3;
  CHECK(CacheKey(a, NULL) != CacheKey(b, NULL));
  a.numFormats = b.numFormats = 1;
  const DXGI_FORMAT first = DXGI_FORMAT_R8G8B8A8_UNORM, second = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  CHECK(CacheKey(a, &first) != CacheKey(a, &second));
}
TEST_CASE("Portable D3D12 alias padding", "[d3d12]")
{
  CHECK(D3D12NeedsWildcardAlias(0, 8847360, 11075584, 9437184, 65536, 65536));
  CHECK(D3D12NeedsWildcardAlias(0, 11075584, 8847360, 9437184, 65536, 65536));
  CHECK_FALSE(D3D12NeedsWildcardAlias(0, 8847360, 11075584, 0, 65536, 65536));
  CHECK_FALSE(D3D12NeedsWildcardAlias(0, 8847360, 11075584, 12582912, 65536, 65536));
  CHECK_FALSE(D3D12AllocationRangesOverlap(~0ULL - 10, 32, 0, 64));
  CHECK_FALSE(D3D12AllocationRangesOverlap(0, 64, 64, 64));
  // Different largest sizes and largest alignments can introduce additional end padding.
  CHECK(D3D12NeedsWildcardAlias(0, 6291456, 4194304, 7340032, 65536, 65536, 8388608, 65536));
  CHECK_FALSE(D3D12NeedsWildcardAlias(0, 6291456, 4194304, 9437184, 65536, 65536, 8388608, 65536));
}
#endif
