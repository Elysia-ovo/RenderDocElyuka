D3D12 Support
=============

This page documents the support of D3D12 in RenderDoc. This gives an overview of what RenderDoc is capable of, and primarily lists information that is relevant. You might also be interested in the :doc:`full list of features <../getting_started/features>`.

Performance notes
-----------------

D3D12 is intended as to have low CPU overhead and be fully threadable, and RenderDoc strives to maintain that performance as much as possible. While some overhead is inevitable RenderDoc aims to have no locks on the 'hot path' of command buffer recording, minimal or no allocation, and in general to have low performance overhead while not capturing.

Some patterns of access are more or less conducive to good performance on RenderDoc, so if you are having trouble with slow capture, large memory/disk overhead or slow replay you might want to try eliminating use of persistent maps of resources. If you do have persistent maps, ensure that either the memory allocation is small or that you have only a few queue submits during the frame - since RenderDoc must compare the whole allocation at every submit to determine what might have changed and save out the delta.

Likewise try to avoid making very large memory allocations in the range of 1GB and above. By its nature RenderDoc must save one or more copies of memory allocations to enable proper capture, so having allocations limited to only a few 100s of megabytes can help gain granularity of management and limit the memory overhead RenderDoc adds. There may be optimisation of this in future on RenderDoc's side but there are no easy guarantees.

Current support
---------------

RenderDoc has initial support for D3D12, but it contains some caveats. In addition, not all replay features are currently supported, but this will eventually reach parity with other APIs.

* RenderDoc assumes that even if multiple GPUs are present, that only one will be used - i.e. NodeMask is always 0. Multiple queues are supported.
* RenderDoc captures may not be portable between different systems, only currently supporting capture and replay on the same or similar enough machines.

For more information about RenderDoc's raytracing support see :doc:`raytracing`.

Application profile allocation differences
------------------------------------------

Some GPU drivers change texture allocation sizes according to the executable's application profile. A placed resource can therefore fit its captured heap but fail to fit when recreated by the replay application on the same GPU.

The ``D3D12_Capture_NormaliseResourceAllocations`` setting, enabled by default, compares all four ``GetResourceAllocationInfo`` variants with a private helper on the same adapter LUID. The helper runs in the companion command-line executable next to the capture library, outside the game's application profile. Capture queries return the larger size and alignment requirement. Batch queries also recalculate each resource offset so a larger texture cannot overlap the next suballocation. Unchanged queries preserve the driver's original result. This requires no change to the capture format or replay application, and does not edit installed driver profiles or rename the game executable.

Requirements are cached per adapter, API version, visibility mask, complete resource description, and castable format list. Helper startup and communication have timeouts. If the helper is missing, disconnects, or cannot support a resource, the capture log reports the failure and the native query result is retained. Such a capture is not guaranteed to be portable.

An aliasing barrier can refer to resources that overlap only in the extra space reported by these queries. Where either physical layout has no overlap, capture broadens the barrier's before-resource to ``NULL``. This conservatively synchronises all previous aliases of the after-resource and is valid in both layouts. The same broadened barrier is recorded in the capture. Resource ranges are tracked by resource ID and removed when each resource is released.

This addresses allocation decisions made through the wrapped capture device. It cannot repair old captures or allocations whose sizes and offsets bypass these queries. The helper uses the system D3D12 runtime; newer application-specific Agility SDK features may be unavailable. Replay on other GPUs, driver versions, or application profiles can still have different requirements.

RenderDoc extensions
--------------------

On D3D12 there is a RenderDoc extension provided with this interface, queried from an ``ID3D12DescriptorHeap``:

.. highlight:: c++
.. code:: c++

  MIDL_INTERFACE("52528c37-bfd9-4bbb-99ff-fdb7188619ce")
  IRenderDocDescriptorNamer : public IUnknown
  {
  public:
    virtual HRESULT STDMETHODCALLTYPE SetName(UINT DescriptorIndex, LPCSTR Name) = 0;
  };

This interface allows you to set a custom name for descriptors, if using SM6.6 style ``ResourceDescriptorHeap[]`` access for better debugging.

See Also
--------

* :doc:`../getting_started/features`
