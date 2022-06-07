#if defined(CONF_BACKEND_VULKAN)

#include <engine/client/backend/vulkan/backend_vulkan.h>

#include <engine/client/backend/backend_base.h>
#include <engine/client/backend_sdl.h>
#include <engine/client/graphics_threaded.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <base/log.h>
#include <base/math.h>
#include <base/system.h>

#include <array>
#include <map>
#include <set>
#include <vector>

#include <algorithm>

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string>

#include <condition_variable>
#include <mutex>
#include <thread>

#include <cstdlib>

#include <unordered_map>

#include <SDL_video.h>
#include <SDL_vulkan.h>

#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>

#ifndef VK_API_VERSION_MAJOR
#define VK_API_VERSION_MAJOR VK_VERSION_MAJOR
#define VK_API_VERSION_MINOR VK_VERSION_MINOR
#define VK_API_VERSION_PATCH VK_VERSION_PATCH
#endif

// for msvc
#ifndef PRIu64
#define PRIu64 "I64u"
#endif

using namespace std::chrono_literals;

class CCommandProcessorFragment_Vulkan : public CCommandProcessorFragment_GLBase
{
	enum EMemoryBlockUsage
	{
		MEMORY_BLOCK_USAGE_TEXTURE = 0,
		MEMORY_BLOCK_USAGE_BUFFER,
		MEMORY_BLOCK_USAGE_STREAM,
		MEMORY_BLOCK_USAGE_STAGING,

		// whenever dummy is used, make sure to deallocate all memory
		MEMORY_BLOCK_USAGE_DUMMY,
	};

	bool IsVerbose()
	{
		return g_Config.m_DbgGfx == DEBUG_GFX_MODE_VERBOSE || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL;
	}

	void VerboseAllocatedMemory(VkDeviceSize Size, size_t FrameImageIndex, EMemoryBlockUsage MemUsage)
	{
		const char *pUsage = "unknown";
		switch(MemUsage)
		{
		case MEMORY_BLOCK_USAGE_TEXTURE:
			pUsage = "texture";
			break;
		case MEMORY_BLOCK_USAGE_BUFFER:
			pUsage = "buffer";
			break;
		case MEMORY_BLOCK_USAGE_STREAM:
			pUsage = "stream";
			break;
		case MEMORY_BLOCK_USAGE_STAGING:
			pUsage = "staging buffer";
			break;
		default: break;
		}
		dbg_msg("vulkan", "allocated chunk of memory with size: %" PRIu64 " for frame %" PRIu64 " (%s)", (size_t)Size, (size_t)m_CurImageIndex, pUsage);
	}

	void VerboseDeallocatedMemory(VkDeviceSize Size, size_t FrameImageIndex, EMemoryBlockUsage MemUsage)
	{
		const char *pUsage = "unknown";
		switch(MemUsage)
		{
		case MEMORY_BLOCK_USAGE_TEXTURE:
			pUsage = "texture";
			break;
		case MEMORY_BLOCK_USAGE_BUFFER:
			pUsage = "buffer";
			break;
		case MEMORY_BLOCK_USAGE_STREAM:
			pUsage = "stream";
			break;
		case MEMORY_BLOCK_USAGE_STAGING:
			pUsage = "staging buffer";
			break;
		default: break;
		}
		dbg_msg("vulkan", "deallocated chunk of memory with size: %" PRIu64 " for frame %" PRIu64 " (%s)", (size_t)Size, (size_t)m_CurImageIndex, pUsage);
	}

	/************************
	* STRUCT DEFINITIONS
	************************/

	static constexpr size_t s_StagingBufferCacheID = 0;
	static constexpr size_t s_StagingBufferImageCacheID = 1;
	static constexpr size_t s_VertexBufferCacheID = 2;
	static constexpr size_t s_ImageBufferCacheID = 3;

	struct SDeviceMemoryBlock
	{
		VkDeviceMemory m_Mem = VK_NULL_HANDLE;
		VkDeviceSize m_Size = 0;
		EMemoryBlockUsage m_UsageType = MEMORY_BLOCK_USAGE_TEXTURE;
	};

	struct SDeviceDescriptorPools;

	struct SDeviceDescriptorSet
	{
		VkDescriptorSet m_Descriptor = VK_NULL_HANDLE;
		SDeviceDescriptorPools *m_pPools = nullptr;
		size_t m_PoolIndex = std::numeric_limits<size_t>::max();
	};

	struct SDeviceDescriptorPool
	{
		VkDescriptorPool m_Pool;
		VkDeviceSize m_Size = 0;
		VkDeviceSize m_CurSize = 0;
	};

	struct SDeviceDescriptorPools
	{
		std::vector<SDeviceDescriptorPool> m_vPools;
		VkDeviceSize m_DefaultAllocSize = 0;
		bool m_IsUniformPool = false;
	};

	// some mix of queue and binary tree
	struct SMemoryHeap
	{
		struct SMemoryHeapElement;
		struct SMemoryHeapQueueElement
		{
			size_t m_AllocationSize = 0;
			// only useful information for the heap
			size_t m_OffsetInHeap = 0;
			// useful for the user of this element
			size_t m_OffsetToAlign = 0;
			SMemoryHeapElement *m_pElementInHeap = nullptr;
			bool operator>(const SMemoryHeapQueueElement &Other) const { return m_AllocationSize > Other.m_AllocationSize; }
		};

		typedef std::multiset<SMemoryHeapQueueElement, std::greater<>> TMemoryHeapQueue;

		struct SMemoryHeapElement
		{
			size_t m_AllocationSize = 0;
			size_t m_Offset = 0;
			SMemoryHeapElement *m_pParent = nullptr;
			std::unique_ptr<SMemoryHeapElement> m_pLeft = nullptr;
			std::unique_ptr<SMemoryHeapElement> m_pRight = nullptr;

			bool m_InUse = false;
			TMemoryHeapQueue::iterator m_InQueue;
		};

		SMemoryHeapElement m_Root;
		TMemoryHeapQueue m_Elements;

		void Init(size_t Size, size_t Offset)
		{
			m_Root.m_AllocationSize = Size;
			m_Root.m_Offset = Offset;
			m_Root.m_pParent = nullptr;
			m_Root.m_InUse = false;

			SMemoryHeapQueueElement QueueEl;
			QueueEl.m_AllocationSize = Size;
			QueueEl.m_OffsetInHeap = Offset;
			QueueEl.m_OffsetToAlign = Offset;
			QueueEl.m_pElementInHeap = &m_Root;
			m_Root.m_InQueue = m_Elements.insert(QueueEl);
		}

		bool Allocate(size_t AllocSize, size_t AllocAlignment, SMemoryHeapQueueElement &AllocatedMemory)
		{
			if(m_Elements.empty())
			{
				return false;
			}
			else
			{
				// calculate the alignment
				size_t ExtraSizeAlign = m_Elements.begin()->m_OffsetInHeap % AllocAlignment;
				if(ExtraSizeAlign != 0)
					ExtraSizeAlign = AllocAlignment - ExtraSizeAlign;
				size_t RealAllocSize = AllocSize + ExtraSizeAlign;

				// check if there is enough space in this instance
				if(m_Elements.begin()->m_AllocationSize < RealAllocSize)
				{
					return false;
				}
				else
				{
					auto TopEl = *m_Elements.begin();
					m_Elements.erase(TopEl.m_pElementInHeap->m_InQueue);

					TopEl.m_pElementInHeap->m_InUse = true;

					// the heap element gets children
					TopEl.m_pElementInHeap->m_pLeft = std::make_unique<SMemoryHeapElement>();
					TopEl.m_pElementInHeap->m_pLeft->m_AllocationSize = RealAllocSize;
					TopEl.m_pElementInHeap->m_pLeft->m_Offset = TopEl.m_OffsetInHeap;
					TopEl.m_pElementInHeap->m_pLeft->m_pParent = TopEl.m_pElementInHeap;
					TopEl.m_pElementInHeap->m_pLeft->m_InUse = true;

					if(RealAllocSize < TopEl.m_AllocationSize)
					{
						SMemoryHeapQueueElement RemainingEl;
						RemainingEl.m_OffsetInHeap = TopEl.m_OffsetInHeap + RealAllocSize;
						RemainingEl.m_AllocationSize = TopEl.m_AllocationSize - RealAllocSize;

						TopEl.m_pElementInHeap->m_pRight = std::make_unique<SMemoryHeapElement>();
						TopEl.m_pElementInHeap->m_pRight->m_AllocationSize = RemainingEl.m_AllocationSize;
						TopEl.m_pElementInHeap->m_pRight->m_Offset = RemainingEl.m_OffsetInHeap;
						TopEl.m_pElementInHeap->m_pRight->m_pParent = TopEl.m_pElementInHeap;
						TopEl.m_pElementInHeap->m_pRight->m_InUse = false;

						RemainingEl.m_pElementInHeap = TopEl.m_pElementInHeap->m_pRight.get();
						RemainingEl.m_pElementInHeap->m_InQueue = m_Elements.insert(RemainingEl);
					}

					AllocatedMemory.m_pElementInHeap = TopEl.m_pElementInHeap->m_pLeft.get();
					AllocatedMemory.m_AllocationSize = RealAllocSize;
					AllocatedMemory.m_OffsetInHeap = TopEl.m_OffsetInHeap;
					AllocatedMemory.m_OffsetToAlign = TopEl.m_OffsetInHeap + ExtraSizeAlign;
					return true;
				}
			}
		}

		void Free(SMemoryHeapQueueElement &AllocatedMemory)
		{
			bool ContinueFree = true;
			SMemoryHeapQueueElement ThisEl = AllocatedMemory;
			while(ContinueFree)
			{
				// first check if the other block is in use, if not merge them again
				SMemoryHeapElement *pThisHeapObj = ThisEl.m_pElementInHeap;
				SMemoryHeapElement *pThisParent = pThisHeapObj->m_pParent;
				pThisHeapObj->m_InUse = false;
				SMemoryHeapElement *pOtherHeapObj = nullptr;
				if(pThisParent != nullptr && pThisHeapObj == pThisParent->m_pLeft.get())
					pOtherHeapObj = pThisHeapObj->m_pParent->m_pRight.get();
				else if(pThisParent != nullptr)
					pOtherHeapObj = pThisHeapObj->m_pParent->m_pLeft.get();

				if((pThisParent != nullptr && pOtherHeapObj == nullptr) || (pOtherHeapObj != nullptr && !pOtherHeapObj->m_InUse))
				{
					// merge them
					if(pOtherHeapObj != nullptr)
					{
						m_Elements.erase(pOtherHeapObj->m_InQueue);
						pOtherHeapObj->m_InUse = false;
					}

					SMemoryHeapQueueElement ParentEl;
					ParentEl.m_OffsetInHeap = pThisParent->m_Offset;
					ParentEl.m_AllocationSize = pThisParent->m_AllocationSize;
					ParentEl.m_pElementInHeap = pThisParent;

					pThisParent->m_pLeft = nullptr;
					pThisParent->m_pRight = nullptr;

					ThisEl = ParentEl;
				}
				else
				{
					// else just put this back into queue
					ThisEl.m_pElementInHeap->m_InQueue = m_Elements.insert(ThisEl);
					ContinueFree = false;
				}
			}
		}

		bool IsUnused()
		{
			return !m_Root.m_InUse;
		}
	};

	template<size_t ID>
	struct SMemoryBlock
	{
		SMemoryHeap::SMemoryHeapQueueElement m_HeapData;

		VkDeviceSize m_UsedSize = 0;

		// optional
		VkBuffer m_Buffer;

		SDeviceMemoryBlock m_BufferMem;
		void *m_pMappedBuffer = nullptr;

		bool m_IsCached = false;
		SMemoryHeap *m_pHeap = nullptr;
	};

	template<size_t ID>
	struct SMemoryImageBlock : public SMemoryBlock<ID>
	{
		uint32_t m_ImageMemoryBits = 0;
	};

	template<size_t ID>
	struct SMemoryBlockCache
	{
		struct SMemoryCacheType
		{
			struct SMemoryCacheHeap
			{
				SMemoryHeap m_Heap;
				VkBuffer m_Buffer;

				SDeviceMemoryBlock m_BufferMem;
				void *m_pMappedBuffer = nullptr;
			};
			std::vector<SMemoryCacheHeap *> m_vpMemoryHeaps;
		};
		SMemoryCacheType m_MemoryCaches;
		std::vector<std::vector<SMemoryBlock<ID>>> m_vvFrameDelayedCachedBufferCleanup;

		bool m_CanShrink = false;

		void Init(size_t SwapChainImageCount)
		{
			m_vvFrameDelayedCachedBufferCleanup.resize(SwapChainImageCount);
		}

		void DestroyFrameData(size_t ImageCount)
		{
			for(size_t i = 0; i < ImageCount; ++i)
				Cleanup(i);
			m_vvFrameDelayedCachedBufferCleanup.clear();
		}

		void Destroy(VkDevice &Device)
		{
			for(auto it = m_MemoryCaches.m_vpMemoryHeaps.begin(); it != m_MemoryCaches.m_vpMemoryHeaps.end();)
			{
				auto *pHeap = *it;
				if(pHeap->m_pMappedBuffer != nullptr)
					vkUnmapMemory(Device, pHeap->m_BufferMem.m_Mem);
				if(pHeap->m_Buffer != VK_NULL_HANDLE)
					vkDestroyBuffer(Device, pHeap->m_Buffer, nullptr);
				vkFreeMemory(Device, pHeap->m_BufferMem.m_Mem, nullptr);

				delete pHeap;
				it = m_MemoryCaches.m_vpMemoryHeaps.erase(it);
			}

			m_MemoryCaches.m_vpMemoryHeaps.clear();
			m_vvFrameDelayedCachedBufferCleanup.clear();
		}

		void Cleanup(size_t ImgIndex)
		{
			for(auto &MemBlock : m_vvFrameDelayedCachedBufferCleanup[ImgIndex])
			{
				MemBlock.m_UsedSize = 0;
				MemBlock.m_pHeap->Free(MemBlock.m_HeapData);

				m_CanShrink = true;
			}
			m_vvFrameDelayedCachedBufferCleanup[ImgIndex].clear();
		}

		void FreeMemBlock(SMemoryBlock<ID> &Block, size_t ImgIndex)
		{
			m_vvFrameDelayedCachedBufferCleanup[ImgIndex].push_back(Block);
		}

		// returns the total free'd memory
		size_t Shrink(VkDevice &Device)
		{
			size_t FreeedMemory = 0;
			if(m_CanShrink)
			{
				m_CanShrink = false;
				if(m_MemoryCaches.m_vpMemoryHeaps.size() > 1)
				{
					for(auto it = m_MemoryCaches.m_vpMemoryHeaps.begin(); it != m_MemoryCaches.m_vpMemoryHeaps.end();)
					{
						auto *pHeap = *it;
						if(pHeap->m_Heap.IsUnused())
						{
							if(pHeap->m_pMappedBuffer != nullptr)
								vkUnmapMemory(Device, pHeap->m_BufferMem.m_Mem);
							if(pHeap->m_Buffer != VK_NULL_HANDLE)
								vkDestroyBuffer(Device, pHeap->m_Buffer, nullptr);
							vkFreeMemory(Device, pHeap->m_BufferMem.m_Mem, nullptr);
							FreeedMemory += pHeap->m_BufferMem.m_Size;

							delete pHeap;
							it = m_MemoryCaches.m_vpMemoryHeaps.erase(it);
							if(m_MemoryCaches.m_vpMemoryHeaps.size() == 1)
								break;
						}
						else
							++it;
					}
				}
			}

			return FreeedMemory;
		}
	};

	struct CTexture
	{
		VkImage m_Img = VK_NULL_HANDLE;
		SMemoryImageBlock<s_ImageBufferCacheID> m_ImgMem;
		VkImageView m_ImgView = VK_NULL_HANDLE;
		VkSampler m_aSamplers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

		VkImage m_Img3D = VK_NULL_HANDLE;
		SMemoryImageBlock<s_ImageBufferCacheID> m_Img3DMem;
		VkImageView m_Img3DView = VK_NULL_HANDLE;
		VkSampler m_Sampler3D = VK_NULL_HANDLE;

		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_RescaleCount = 0;

		uint32_t m_MipMapCount = 1;

		std::array<SDeviceDescriptorSet, 2> m_aVKStandardTexturedDescrSets;
		SDeviceDescriptorSet m_VKStandard3DTexturedDescrSet;
		SDeviceDescriptorSet m_VKTextDescrSet;
	};

	struct SBufferObject
	{
		SMemoryBlock<s_VertexBufferCacheID> m_Mem;
	};

	struct SBufferObjectFrame
	{
		SBufferObject m_BufferObject;

		// since stream buffers can be used the cur buffer should always be used for rendering
		bool m_IsStreamedBuffer = false;
		VkBuffer m_CurBuffer = VK_NULL_HANDLE;
		size_t m_CurBufferOffset = 0;
	};

	struct SBufferContainer
	{
		int m_BufferObjectIndex = 0;
	};

	struct SFrameBuffers
	{
		VkBuffer m_Buffer;
		SDeviceMemoryBlock m_BufferMem;
		size_t m_OffsetInBuffer = 0;
		size_t m_Size = 0;
		size_t m_UsedSize = 0;
		void *m_pMappedBufferData = nullptr;

		SFrameBuffers(VkBuffer Buffer, SDeviceMemoryBlock BufferMem, size_t OffsetInBuffer, size_t Size, size_t UsedSize, void *pMappedBufferData) :
			m_Buffer(Buffer), m_BufferMem(BufferMem), m_OffsetInBuffer(OffsetInBuffer), m_Size(Size), m_UsedSize(UsedSize), m_pMappedBufferData(pMappedBufferData)
		{
		}
	};

	struct SFrameUniformBuffers : public SFrameBuffers
	{
		std::array<SDeviceDescriptorSet, 2> m_aUniformSets;

		SFrameUniformBuffers(VkBuffer Buffer, SDeviceMemoryBlock BufferMem, size_t OffsetInBuffer, size_t Size, size_t UsedSize, void *pMappedBufferData) :
			SFrameBuffers(Buffer, BufferMem, OffsetInBuffer, Size, UsedSize, pMappedBufferData) {}
	};

	template<typename TName>
	struct SStreamMemory
	{
		typedef std::vector<std::vector<TName>> TBufferObjectsOfFrame;
		typedef std::vector<std::vector<VkMappedMemoryRange>> TMemoryMapRangesOfFrame;
		typedef std::vector<size_t> TStreamUseCount;
		TBufferObjectsOfFrame m_vvBufferObjectsOfFrame;
		TMemoryMapRangesOfFrame m_vvBufferObjectsOfFrameRangeData;
		TStreamUseCount m_vCurrentUsedCount;

		std::vector<TName> &GetBuffers(size_t FrameImageIndex)
		{
			return m_vvBufferObjectsOfFrame[FrameImageIndex];
		}

		std::vector<VkMappedMemoryRange> &GetRanges(size_t FrameImageIndex)
		{
			return m_vvBufferObjectsOfFrameRangeData[FrameImageIndex];
		}

		size_t GetUsedCount(size_t FrameImageIndex)
		{
			return m_vCurrentUsedCount[FrameImageIndex];
		}

		void IncreaseUsedCount(size_t FrameImageIndex)
		{
			++m_vCurrentUsedCount[FrameImageIndex];
		}

		bool IsUsed(size_t FrameImageIndex)
		{
			return GetUsedCount(FrameImageIndex) > 0;
		}

		void ResetFrame(size_t FrameImageIndex)
		{
			m_vCurrentUsedCount[FrameImageIndex] = 0;
		}

		void Init(size_t FrameImageCount)
		{
			m_vvBufferObjectsOfFrame.resize(FrameImageCount);
			m_vvBufferObjectsOfFrameRangeData.resize(FrameImageCount);
			m_vCurrentUsedCount.resize(FrameImageCount);
		}

		typedef std::function<void(size_t, TName &)> TDestroyBufferFunc;

		void Destroy(TDestroyBufferFunc &&DestroyBuffer)
		{
			size_t ImageIndex = 0;
			for(auto &vBuffersOfFrame : m_vvBufferObjectsOfFrame)
			{
				for(auto &BufferOfFrame : vBuffersOfFrame)
				{
					VkDeviceMemory BufferMem = BufferOfFrame.m_BufferMem.m_Mem;
					DestroyBuffer(ImageIndex, BufferOfFrame);

					// delete similar buffers
					for(auto &BufferOfFrameDel : vBuffersOfFrame)
					{
						if(BufferOfFrameDel.m_BufferMem.m_Mem == BufferMem)
						{
							BufferOfFrameDel.m_Buffer = VK_NULL_HANDLE;
							BufferOfFrameDel.m_BufferMem.m_Mem = VK_NULL_HANDLE;
						}
					}
				}
				++ImageIndex;
			}
			m_vvBufferObjectsOfFrame.clear();
			m_vvBufferObjectsOfFrameRangeData.clear();
			m_vCurrentUsedCount.clear();
		}
	};

	struct SShaderModule
	{
		VkShaderModule m_VertShaderModule = VK_NULL_HANDLE;
		VkShaderModule m_FragShaderModule = VK_NULL_HANDLE;

		VkDevice m_VKDevice = VK_NULL_HANDLE;

		~SShaderModule()
		{
			if(m_VKDevice != VK_NULL_HANDLE)
			{
				if(m_VertShaderModule != VK_NULL_HANDLE)
					vkDestroyShaderModule(m_VKDevice, m_VertShaderModule, nullptr);

				if(m_FragShaderModule != VK_NULL_HANDLE)
					vkDestroyShaderModule(m_VKDevice, m_FragShaderModule, nullptr);
			}
		}
	};

	enum EVulkanBackendAddressModes
	{
		VULKAN_BACKEND_ADDRESS_MODE_REPEAT = 0,
		VULKAN_BACKEND_ADDRESS_MODE_CLAMP_EDGES,

		VULKAN_BACKEND_ADDRESS_MODE_COUNT,
	};

	enum EVulkanBackendBlendModes
	{
		VULKAN_BACKEND_BLEND_MODE_ALPHA = 0,
		VULKAN_BACKEND_BLEND_MODE_NONE,
		VULKAN_BACKEND_BLEND_MODE_ADDITATIVE,

		VULKAN_BACKEND_BLEND_MODE_COUNT,
	};

	enum EVulkanBackendClipModes
	{
		VULKAN_BACKEND_CLIP_MODE_NONE = 0,
		VULKAN_BACKEND_CLIP_MODE_DYNAMIC_SCISSOR_AND_VIEWPORT,

		VULKAN_BACKEND_CLIP_MODE_COUNT,
	};

	enum EVulkanBackendTextureModes
	{
		VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED = 0,
		VULKAN_BACKEND_TEXTURE_MODE_TEXTURED,

		VULKAN_BACKEND_TEXTURE_MODE_COUNT,
	};

	struct SPipelineContainer
	{
		// 3 blend modes - 2 viewport & scissor modes - 2 texture modes
		std::array<std::array<std::array<VkPipelineLayout, VULKAN_BACKEND_TEXTURE_MODE_COUNT>, VULKAN_BACKEND_CLIP_MODE_COUNT>, VULKAN_BACKEND_BLEND_MODE_COUNT> m_aaaPipelineLayouts;
		std::array<std::array<std::array<VkPipeline, VULKAN_BACKEND_TEXTURE_MODE_COUNT>, VULKAN_BACKEND_CLIP_MODE_COUNT>, VULKAN_BACKEND_BLEND_MODE_COUNT> m_aaaPipelines;

		SPipelineContainer()
		{
			for(auto &aaPipeLayouts : m_aaaPipelineLayouts)
			{
				for(auto &aPipeLayouts : aaPipeLayouts)
				{
					for(auto &PipeLayout : aPipeLayouts)
					{
						PipeLayout = VK_NULL_HANDLE;
					}
				}
			}
			for(auto &aaPipe : m_aaaPipelines)
			{
				for(auto &aPipe : aaPipe)
				{
					for(auto &Pipe : aPipe)
					{
						Pipe = VK_NULL_HANDLE;
					}
				}
			}
		}

		void Destroy(VkDevice &Device)
		{
			for(auto &aaPipeLayouts : m_aaaPipelineLayouts)
			{
				for(auto &aPipeLayouts : aaPipeLayouts)
				{
					for(auto &PipeLayout : aPipeLayouts)
					{
						if(PipeLayout != VK_NULL_HANDLE)
							vkDestroyPipelineLayout(Device, PipeLayout, nullptr);
						PipeLayout = VK_NULL_HANDLE;
					}
				}
			}
			for(auto &aaPipe : m_aaaPipelines)
			{
				for(auto &aPipe : aaPipe)
				{
					for(auto &Pipe : aPipe)
					{
						if(Pipe != VK_NULL_HANDLE)
							vkDestroyPipeline(Device, Pipe, nullptr);
						Pipe = VK_NULL_HANDLE;
					}
				}
			}
		}
	};

	/*******************************
	* UNIFORM PUSH CONSTANT LAYOUTS
	********************************/

	struct SUniformGPos
	{
		float m_aPos[4 * 2] = {0};
	};

	struct SUniformGTextPos
	{
		float m_aPos[4 * 2] = {0};
		float m_TextureSize = 0;
	};

	typedef vec3 SUniformTextGFragmentOffset;

	struct SUniformTextGFragmentConstants
	{
		ColorRGBA m_TextColor;
		ColorRGBA m_TextOutlineColor;
	};

	struct SUniformTextFragment
	{
		SUniformTextGFragmentConstants m_Constants;
	};

	struct SUniformTileGPos
	{
		float m_aPos[4 * 2] = {0};
	};

	struct SUniformTileGPosBorderLine : public SUniformTileGPos
	{
		vec2 m_Dir;
		vec2 m_Offset;
	};

	struct SUniformTileGPosBorder : public SUniformTileGPosBorderLine
	{
		int32_t m_JumpIndex = 0;
	};

	typedef ColorRGBA SUniformTileGVertColor;

	struct SUniformTileGVertColorAlign
	{
		float m_aPad[(64 - 52) / 4] = {0};
	};

	struct SUniformPrimExGPosRotationless
	{
		float m_aPos[4 * 2] = {0};
	};

	struct SUniformPrimExGPos : public SUniformPrimExGPosRotationless
	{
		vec2 m_Center;
		float m_Rotation = 0;
	};

	typedef ColorRGBA SUniformPrimExGVertColor;

	struct SUniformPrimExGVertColorAlign
	{
		float m_aPad[(48 - 44) / 4] = {0};
	};

	struct SUniformSpriteMultiGPos
	{
		float m_aPos[4 * 2] = {0};
		vec2 m_Center;
	};

	typedef ColorRGBA SUniformSpriteMultiGVertColor;

	struct SUniformSpriteMultiGVertColorAlign
	{
		float m_aPad[(48 - 40) / 4] = {0};
	};

	struct SUniformSpriteMultiPushGPosBase
	{
		float m_aPos[4 * 2] = {0};
		vec2 m_Center;
		vec2 m_Padding;
	};

	struct SUniformSpriteMultiPushGPos : public SUniformSpriteMultiPushGPosBase
	{
		vec4 m_aPSR[1];
	};

	typedef ColorRGBA SUniformSpriteMultiPushGVertColor;

	struct SUniformQuadGPosBase
	{
		float m_aPos[4 * 2] = {0};
		int32_t m_QuadOffset = 0;
	};

	struct SUniformQuadPushGBufferObject
	{
		vec4 m_VertColor;
		vec2 m_Offset;
		float m_Rotation = 0;
		float m_Padding = 0;

		SUniformQuadPushGBufferObject& operator=(const SQuadRenderInfo &QuadInfo)
		{
			m_VertColor = QuadInfo.m_Color;
			m_Offset = QuadInfo.m_Offsets;
			m_Rotation = QuadInfo.m_Rotation;
			m_Padding = QuadInfo.m_Padding;

			return *this;
		}
	};

	struct SUniformQuadPushGPos
	{
		float m_aPos[4 * 2] = {0};
		SUniformQuadPushGBufferObject m_BOPush;
		int32_t m_QuadOffset = 0;
	};

	struct SUniformQuadGPos
	{
		float m_aPos[4 * 2] = {0};
		int32_t m_QuadOffset = 0;
	};

	enum ESupportedSamplerTypes
	{
		SUPPORTED_SAMPLER_TYPE_REPEAT = 0,
		SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE,
		SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY,

		SUPPORTED_SAMPLER_TYPE_COUNT,
	};

	struct SShaderFileCache
	{
		std::vector<uint8_t> m_vBinary;
	};

	struct SSwapImgViewportExtent
	{
		VkExtent2D m_SwapImageViewport;
		bool m_HasForcedViewport = false;
		VkExtent2D m_ForcedViewport;

		// the viewport of the resulting presented image on the screen
		// if there is a forced viewport the resulting image is smaller
		// than the full swap image size
		VkExtent2D GetPresentedImageViewport()
		{
			uint32_t ViewportWidth = m_SwapImageViewport.width;
			uint32_t ViewportHeight = m_SwapImageViewport.height;
			if(m_HasForcedViewport)
			{
				ViewportWidth = m_ForcedViewport.width;
				ViewportHeight = m_ForcedViewport.height;
			}

			return {ViewportWidth, ViewportHeight};
		}
	};

	struct SSwapChainMultiSampleImage
	{
		VkImage m_Image = VK_NULL_HANDLE;
		SMemoryImageBlock<s_ImageBufferCacheID> m_ImgMem;
		VkImageView m_ImgView = VK_NULL_HANDLE;
	};

	/************************
	* MEMBER VARIABLES
	************************/

	std::unordered_map<std::string, SShaderFileCache> m_ShaderFiles;

	SMemoryBlockCache<s_StagingBufferCacheID> m_StagingBufferCache;
	SMemoryBlockCache<s_StagingBufferImageCacheID> m_StagingBufferCacheImage;
	SMemoryBlockCache<s_VertexBufferCacheID> m_VertexBufferCache;
	std::map<uint32_t, SMemoryBlockCache<s_ImageBufferCacheID>> m_ImageBufferCaches;

	std::vector<VkMappedMemoryRange> m_vNonFlushedStagingBufferRange;

	std::vector<CTexture> m_vTextures;

	std::atomic<uint64_t> *m_pTextureMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pBufferMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pStreamMemoryUsage = nullptr;
	std::atomic<uint64_t> *m_pStagingMemoryUsage = nullptr;

	TTWGraphicsGPUList *m_pGPUList = nullptr;

	int m_GlobalTextureLodBIAS = 0;
	uint32_t m_MultiSamplingCount = 1;

	uint32_t m_NextMultiSamplingCount = std::numeric_limits<uint32_t>::max();

	bool m_RecreateSwapChain = false;
	bool m_SwapchainCreated = false;
	bool m_RenderingPaused = false;
	bool m_HasDynamicViewport = false;
	VkOffset2D m_DynamicViewportOffset;
	VkExtent2D m_DynamicViewportSize;

	bool m_AllowsLinearBlitting = false;
	bool m_OptimalSwapChainImageBlitting = false;
	bool m_OptimalRGBAImageBlitting = false;
	bool m_LinearRGBAImageBlitting = false;

	VkBuffer m_IndexBuffer;
	SDeviceMemoryBlock m_IndexBufferMemory;

	VkBuffer m_RenderIndexBuffer;
	SDeviceMemoryBlock m_RenderIndexBufferMemory;
	size_t m_CurRenderIndexPrimitiveCount = 0;

	VkDeviceSize m_NonCoherentMemAlignment = 0;
	VkDeviceSize m_OptimalImageCopyMemAlignment = 0;
	uint32_t m_MaxTextureSize = 0;
	uint32_t m_MaxSamplerAnisotropy = 0;
	VkSampleCountFlags m_MaxMultiSample;

	uint32_t m_MinUniformAlign = 0;

	std::vector<uint8_t> m_vScreenshotHelper;

	SDeviceMemoryBlock m_GetPresentedImgDataHelperMem;
	VkImage m_GetPresentedImgDataHelperImage = VK_NULL_HANDLE;
	uint8_t *m_pGetPresentedImgDataHelperMappedMemory = nullptr;
	VkDeviceSize m_GetPresentedImgDataHelperMappedLayoutOffset = 0;
	VkDeviceSize m_GetPresentedImgDataHelperMappedLayoutPitch = 0;
	uint32_t m_GetPresentedImgDataHelperWidth = 0;
	uint32_t m_GetPresentedImgDataHelperHeight = 0;
	VkFence m_GetPresentedImgDataHelperFence = VK_NULL_HANDLE;

	std::array<VkSampler, SUPPORTED_SAMPLER_TYPE_COUNT> m_aSamplers;

	class IStorage *m_pStorage = nullptr;

	struct SDelayedBufferCleanupItem
	{
		VkBuffer m_Buffer;
		SDeviceMemoryBlock m_Mem;
		void *m_pMappedData = nullptr;
	};

	std::vector<std::vector<SDelayedBufferCleanupItem>> m_vvFrameDelayedBufferCleanup;
	std::vector<std::vector<CTexture>> m_vvFrameDelayedTextureCleanup;
	std::vector<std::vector<std::pair<CTexture, CTexture>>> m_vvFrameDelayedTextTexturesCleanup;

	size_t m_ThreadCount = 1;
	static constexpr size_t ms_MainThreadIndex = 0;
	size_t m_CurCommandInPipe = 0;
	size_t m_CurRenderCallCountInPipe = 0;
	size_t m_CommandsInPipe = 0;
	size_t m_RenderCallsInPipe = 0;
	size_t m_LastCommandsInPipeThreadIndex = 0;

	struct SRenderThread
	{
		bool m_IsRendering = false;
		std::thread m_Thread;
		std::mutex m_Mutex;
		std::condition_variable m_Cond;
		bool m_Finished = false;
		bool m_Started = false;
	};
	std::vector<std::unique_ptr<SRenderThread>> m_vpRenderThreads;

private:
	std::vector<VkImageView> m_vSwapChainImageViewList;
	std::vector<SSwapChainMultiSampleImage> m_vSwapChainMultiSamplingImages;
	std::vector<VkFramebuffer> m_vFramebufferList;
	std::vector<VkCommandBuffer> m_vMainDrawCommandBuffers;

	std::vector<std::vector<VkCommandBuffer>> m_vvThreadDrawCommandBuffers;
	std::vector<VkCommandBuffer> m_vHelperThreadDrawCommandBuffers;
	std::vector<std::vector<bool>> m_vvUsedThreadDrawCommandBuffer;

	std::vector<VkCommandBuffer> m_vMemoryCommandBuffers;
	std::vector<bool> m_vUsedMemoryCommandBuffer;

	// swapped by use case
	std::vector<VkSemaphore> m_vWaitSemaphores;
	std::vector<VkSemaphore> m_vSigSemaphores;

	std::vector<VkSemaphore> m_vMemorySemaphores;

	std::vector<VkFence> m_vFrameFences;
	std::vector<VkFence> m_vImagesFences;

	uint64_t m_CurFrame = 0;
	std::vector<uint64_t> m_vImageLastFrameCheck;

	uint32_t m_LastPresentedSwapChainImageIndex = 0;

	std::vector<SBufferObjectFrame> m_vBufferObjects;

	std::vector<SBufferContainer> m_vBufferContainers;

	VkInstance m_VKInstance;
	VkPhysicalDevice m_VKGPU;
	uint32_t m_VKGraphicsQueueIndex = std::numeric_limits<uint32_t>::max();
	VkDevice m_VKDevice;
	VkQueue m_VKGraphicsQueue, m_VKPresentQueue;
	VkSurfaceKHR m_VKPresentSurface;
	SSwapImgViewportExtent m_VKSwapImgAndViewportExtent;

#ifdef VK_EXT_debug_utils
	VkDebugUtilsMessengerEXT m_DebugMessenger;
#endif

	VkDescriptorSetLayout m_StandardTexturedDescriptorSetLayout;
	VkDescriptorSetLayout m_Standard3DTexturedDescriptorSetLayout;

	VkDescriptorSetLayout m_TextDescriptorSetLayout;

	VkDescriptorSetLayout m_SpriteMultiUniformDescriptorSetLayout;
	VkDescriptorSetLayout m_QuadUniformDescriptorSetLayout;

	SPipelineContainer m_StandardPipeline;
	SPipelineContainer m_StandardLinePipeline;
	SPipelineContainer m_Standard3DPipeline;
	SPipelineContainer m_TextPipeline;
	SPipelineContainer m_TilePipeline;
	SPipelineContainer m_TileBorderPipeline;
	SPipelineContainer m_TileBorderLinePipeline;
	SPipelineContainer m_PrimExPipeline;
	SPipelineContainer m_PrimExRotationlessPipeline;
	SPipelineContainer m_SpriteMultiPipeline;
	SPipelineContainer m_SpriteMultiPushPipeline;
	SPipelineContainer m_QuadPipeline;
	SPipelineContainer m_QuadPushPipeline;

	std::vector<VkPipeline> m_vLastPipeline;

	std::vector<VkCommandPool> m_vCommandPools;

	VkRenderPass m_VKRenderPass;

	VkSurfaceFormatKHR m_VKSurfFormat;

	SDeviceDescriptorPools m_StandardTextureDescrPool;
	SDeviceDescriptorPools m_TextTextureDescrPool;

	std::vector<SDeviceDescriptorPools> m_vUniformBufferDescrPools;

	VkSwapchainKHR m_VKSwapChain = VK_NULL_HANDLE;
	std::vector<VkImage> m_vSwapChainImages;
	uint32_t m_SwapChainImageCount = 0;

	std::vector<SStreamMemory<SFrameBuffers>> m_vStreamedVertexBuffers;
	std::vector<SStreamMemory<SFrameUniformBuffers>> m_vStreamedUniformBuffers;

	uint32_t m_CurFrames = 0;
	uint32_t m_CurImageIndex = 0;

	uint32_t m_CanvasWidth = 0;
	uint32_t m_CanvasHeight = 0;

	SDL_Window *m_pWindow = nullptr;

	std::array<float, 4> m_aClearColor = {0, 0, 0, 0};

	struct SRenderCommandExecuteBuffer
	{
		CCommandBuffer::ECommandBufferCMD m_Command = CCommandBuffer::ECommandBufferCMD::CMDGROUP_CORE;
		const CCommandBuffer::SCommand *m_pRawCommand = nullptr;
		uint32_t m_ThreadIndex = 0;

		// must be calculated when the buffer gets filled
		size_t m_EstimatedRenderCallCount = 0;

		// usefull data
		VkBuffer m_Buffer;
		size_t m_BufferOff = 0;
		std::array<SDeviceDescriptorSet, 2> m_aDescriptors;

		VkBuffer m_IndexBuffer;

		bool m_ClearColorInRenderThread = false;

		bool m_HasDynamicState = false;
		VkViewport m_Viewport;
		VkRect2D m_Scissor;
	};

	typedef std::vector<SRenderCommandExecuteBuffer> TCommandList;
	typedef std::vector<TCommandList> TThreadCommandList;

	TThreadCommandList m_vvThreadCommandLists;
	std::vector<bool> m_vThreadHelperHadCommands;

	typedef std::function<bool(const CCommandBuffer::SCommand *, SRenderCommandExecuteBuffer &)> TCommandBufferCommandCallback;
	typedef std::function<void(SRenderCommandExecuteBuffer &, const CCommandBuffer::SCommand *)> TCommandBufferFillExecuteBufferFunc;

	struct SCommandCallback
	{
		bool m_IsRenderCommand = false;
		TCommandBufferFillExecuteBufferFunc m_FillExecuteBuffer;
		TCommandBufferCommandCallback m_CommandCB;
	};
	std::array<SCommandCallback, CCommandBuffer::CMD_COUNT - CCommandBuffer::CMD_FIRST> m_aCommandCallbacks;

protected:
	/************************
	* ERROR MANAGMENT
	************************/

	char m_aError[1024] = {0};
	bool m_HasError = false;
	bool m_CanAssert = false;

	void SetError(const char *pErr, const char *pErrStrExtra = nullptr)
	{
		char aError[1024];
		if(pErrStrExtra == nullptr)
			str_format(aError, std::size(aError), "%s", pErr);
		else
			str_format(aError, std::size(aError), "%s: %s", pErr, pErrStrExtra);
		dbg_msg("vulkan", "vulkan error: %s", aError);
		m_HasError = true;
		dbg_assert(!m_CanAssert, aError);
	}

	void SetWarning(const char *pErr)
	{
		dbg_msg("vulkan", "vulkan warning: %s", pErr);
	}

	const char *CheckVulkanCriticalError(VkResult CallResult)
	{
		const char *pCriticalError = nullptr;
		switch(CallResult)
		{
		case VK_ERROR_OUT_OF_HOST_MEMORY:
			pCriticalError = "host ran out of memory";
			dbg_msg("vulkan", "%s", pCriticalError);
			break;
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			pCriticalError = "device ran out of memory";
			dbg_msg("vulkan", "%s", pCriticalError);
			break;
		case VK_ERROR_DEVICE_LOST:
			pCriticalError = "device lost";
			dbg_msg("vulkan", "%s", pCriticalError);
			break;
		case VK_ERROR_OUT_OF_DATE_KHR:
		{
			if(IsVerbose())
			{
				dbg_msg("vulkan", "queueing swap chain recreation because the current is out of date");
			}
			m_RecreateSwapChain = true;
			break;
		}
		case VK_ERROR_SURFACE_LOST_KHR:
			dbg_msg("vulkan", "surface lost");
			break;
		/*case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
			dbg_msg("vulkan", "fullscreen exlusive mode lost");
			break;*/
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			pCriticalError = "no compatible driver found. Vulkan 1.1 is required.";
			dbg_msg("vulkan", "%s", pCriticalError);
			break;
		case VK_ERROR_INITIALIZATION_FAILED:
			pCriticalError = "initialization failed for unknown reason.";
			dbg_msg("vulkan", "%s", pCriticalError);
			break;
		case VK_ERROR_LAYER_NOT_PRESENT:
			SetWarning("One Vulkan layer was not present. (try to disable them)");
			break;
		case VK_ERROR_EXTENSION_NOT_PRESENT:
			SetWarning("One Vulkan extension was not present. (try to disable them)");
			break;
		case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
			dbg_msg("vulkan", "native window in use");
			break;
		case VK_SUCCESS:
			break;
		case VK_SUBOPTIMAL_KHR:
			if(IsVerbose())
			{
				dbg_msg("vulkan", "queueing swap chain recreation because the current is sub optimal");
			}
			m_RecreateSwapChain = true;
			break;
		default:
			str_format(m_aError, std::size(m_aError), "unknown error %u", (uint32_t)CallResult);
			pCriticalError = m_aError;
			break;
		}

		return pCriticalError;
	}

	/************************
	* COMMAND CALLBACKS
	************************/

	size_t CommandBufferCMDOff(CCommandBuffer::ECommandBufferCMD CommandBufferCMD)
	{
		return (size_t)CommandBufferCMD - CCommandBuffer::ECommandBufferCMD::CMD_FIRST;
	}

	void RegisterCommands()
	{
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_TEXTURE_CREATE)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Texture_Create(static_cast<const CCommandBuffer::SCommand_Texture_Create *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_TEXTURE_DESTROY)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Texture_Destroy(static_cast<const CCommandBuffer::SCommand_Texture_Destroy *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_TEXTURE_UPDATE)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Texture_Update(static_cast<const CCommandBuffer::SCommand_Texture_Update *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_TEXT_TEXTURES_CREATE)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_TextTextures_Create(static_cast<const CCommandBuffer::SCommand_TextTextures_Create *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_TEXT_TEXTURES_DESTROY)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_TextTextures_Destroy(static_cast<const CCommandBuffer::SCommand_TextTextures_Destroy *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_TEXT_TEXTURE_UPDATE)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_TextTexture_Update(static_cast<const CCommandBuffer::SCommand_TextTexture_Update *>(pBaseCommand)); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_CLEAR)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_Clear_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_Clear *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Clear(ExecBuffer, static_cast<const CCommandBuffer::SCommand_Clear *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_Render_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_Render *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) {	Cmd_Render(static_cast<const CCommandBuffer::SCommand_Render *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_TEX3D)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderTex3D_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderTex3D *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderTex3D(static_cast<const CCommandBuffer::SCommand_RenderTex3D *>(pBaseCommand), ExecBuffer); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_CREATE_BUFFER_OBJECT)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) {	Cmd_CreateBufferObject(static_cast<const CCommandBuffer::SCommand_CreateBufferObject *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RECREATE_BUFFER_OBJECT)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RecreateBufferObject(static_cast<const CCommandBuffer::SCommand_RecreateBufferObject *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_UPDATE_BUFFER_OBJECT)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_UpdateBufferObject(static_cast<const CCommandBuffer::SCommand_UpdateBufferObject *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_COPY_BUFFER_OBJECT)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_CopyBufferObject(static_cast<const CCommandBuffer::SCommand_CopyBufferObject *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_DELETE_BUFFER_OBJECT)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_DeleteBufferObject(static_cast<const CCommandBuffer::SCommand_DeleteBufferObject *>(pBaseCommand)); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_CREATE_BUFFER_CONTAINER)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_CreateBufferContainer(static_cast<const CCommandBuffer::SCommand_CreateBufferContainer *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_DELETE_BUFFER_CONTAINER)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_DeleteBufferContainer(static_cast<const CCommandBuffer::SCommand_DeleteBufferContainer *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_UPDATE_BUFFER_CONTAINER)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_UpdateBufferContainer(static_cast<const CCommandBuffer::SCommand_UpdateBufferContainer *>(pBaseCommand)); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_INDICES_REQUIRED_NUM_NOTIFY)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_IndicesRequiredNumNotify(static_cast<const CCommandBuffer::SCommand_IndicesRequiredNumNotify *>(pBaseCommand)); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_TILE_LAYER)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderTileLayer_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderTileLayer *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderTileLayer(static_cast<const CCommandBuffer::SCommand_RenderTileLayer *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_BORDER_TILE)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderBorderTile_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderBorderTile *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderBorderTile(static_cast<const CCommandBuffer::SCommand_RenderBorderTile *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_BORDER_TILE_LINE)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderBorderTileLine_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderBorderTileLine *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderBorderTileLine(static_cast<const CCommandBuffer::SCommand_RenderBorderTileLine *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_QUAD_LAYER)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderQuadLayer_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderQuadLayer *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderQuadLayer(static_cast<const CCommandBuffer::SCommand_RenderQuadLayer *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_TEXT)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderText_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderText *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderText(static_cast<const CCommandBuffer::SCommand_RenderText *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_QUAD_CONTAINER)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderQuadContainer_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderQuadContainer *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderQuadContainer(static_cast<const CCommandBuffer::SCommand_RenderQuadContainer *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_QUAD_CONTAINER_EX)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderQuadContainerEx_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderQuadContainerEx *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderQuadContainerEx(static_cast<const CCommandBuffer::SCommand_RenderQuadContainerEx *>(pBaseCommand), ExecBuffer); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_RENDER_QUAD_CONTAINER_SPRITE_MULTIPLE)] = {true, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_RenderQuadContainerAsSpriteMultiple_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_RenderQuadContainerAsSpriteMultiple *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_RenderQuadContainerAsSpriteMultiple(static_cast<const CCommandBuffer::SCommand_RenderQuadContainerAsSpriteMultiple *>(pBaseCommand), ExecBuffer); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_SWAP)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Swap(static_cast<const CCommandBuffer::SCommand_Swap *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_FINISH)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Finish(static_cast<const CCommandBuffer::SCommand_Finish *>(pBaseCommand)); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_VSYNC)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_VSync(static_cast<const CCommandBuffer::SCommand_VSync *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_MULTISAMPLING)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_MultiSampling(static_cast<const CCommandBuffer::SCommand_MultiSampling *>(pBaseCommand)); return true; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_TRY_SWAP_AND_SCREENSHOT)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Screenshot(static_cast<const CCommandBuffer::SCommand_TrySwapAndScreenshot *>(pBaseCommand)); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_UPDATE_VIEWPORT)] = {false, [this](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) { Cmd_Update_Viewport_FillExecuteBuffer(ExecBuffer, static_cast<const CCommandBuffer::SCommand_Update_Viewport *>(pBaseCommand)); }, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_Update_Viewport(static_cast<const CCommandBuffer::SCommand_Update_Viewport *>(pBaseCommand)); return true; }};

		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_WINDOW_CREATE_NTF)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_WindowCreateNtf(static_cast<const CCommandBuffer::SCommand_WindowCreateNtf *>(pBaseCommand)); return false; }};
		m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::CMD_WINDOW_DESTROY_NTF)] = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [this](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { Cmd_WindowDestroyNtf(static_cast<const CCommandBuffer::SCommand_WindowDestroyNtf *>(pBaseCommand)); return false; }};

		for(auto &Callback : m_aCommandCallbacks)
		{
			if(!(bool)Callback.m_CommandCB)
				Callback = {false, [](SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand *pBaseCommand) {}, [](const CCommandBuffer::SCommand *pBaseCommand, SRenderCommandExecuteBuffer &ExecBuffer) { return true; }};
		}
	}

	/*****************************
	* VIDEO AND SCREENSHOT HELPER
	******************************/

	uint8_t *PreparePresentedImageDataImage(uint32_t Width, uint32_t Height)
	{
		bool NeedsNewImg = Width != m_GetPresentedImgDataHelperWidth || Height != m_GetPresentedImgDataHelperHeight;
		if(m_GetPresentedImgDataHelperImage == VK_NULL_HANDLE || NeedsNewImg)
		{
			if(m_GetPresentedImgDataHelperImage != VK_NULL_HANDLE)
			{
				DeletePresentedImageDataImage();
			}
			m_GetPresentedImgDataHelperWidth = Width;
			m_GetPresentedImgDataHelperHeight = Height;

			VkImageCreateInfo ImageInfo{};
			ImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			ImageInfo.imageType = VK_IMAGE_TYPE_2D;
			ImageInfo.extent.width = Width;
			ImageInfo.extent.height = Height;
			ImageInfo.extent.depth = 1;
			ImageInfo.mipLevels = 1;
			ImageInfo.arrayLayers = 1;
			ImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			ImageInfo.tiling = VK_IMAGE_TILING_LINEAR;
			ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			ImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			ImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			ImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			vkCreateImage(m_VKDevice, &ImageInfo, nullptr, &m_GetPresentedImgDataHelperImage);
			// Create memory to back up the image
			VkMemoryRequirements MemRequirements;
			vkGetImageMemoryRequirements(m_VKDevice, m_GetPresentedImgDataHelperImage, &MemRequirements);

			VkMemoryAllocateInfo MemAllocInfo{};
			MemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			MemAllocInfo.allocationSize = MemRequirements.size;
			MemAllocInfo.memoryTypeIndex = FindMemoryType(m_VKGPU, MemRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

			vkAllocateMemory(m_VKDevice, &MemAllocInfo, nullptr, &m_GetPresentedImgDataHelperMem.m_Mem);
			vkBindImageMemory(m_VKDevice, m_GetPresentedImgDataHelperImage, m_GetPresentedImgDataHelperMem.m_Mem, 0);

			ImageBarrier(m_GetPresentedImgDataHelperImage, 0, 1, 0, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

			VkImageSubresource SubResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
			VkSubresourceLayout SubResourceLayout;
			vkGetImageSubresourceLayout(m_VKDevice, m_GetPresentedImgDataHelperImage, &SubResource, &SubResourceLayout);

			vkMapMemory(m_VKDevice, m_GetPresentedImgDataHelperMem.m_Mem, 0, VK_WHOLE_SIZE, 0, (void **)&m_pGetPresentedImgDataHelperMappedMemory);
			m_GetPresentedImgDataHelperMappedLayoutOffset = SubResourceLayout.offset;
			m_GetPresentedImgDataHelperMappedLayoutPitch = SubResourceLayout.rowPitch;
			m_pGetPresentedImgDataHelperMappedMemory += m_GetPresentedImgDataHelperMappedLayoutOffset;

			VkFenceCreateInfo FenceInfo{};
			FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			vkCreateFence(m_VKDevice, &FenceInfo, nullptr, &m_GetPresentedImgDataHelperFence);
		}
		return m_pGetPresentedImgDataHelperMappedMemory;
	}

	void DeletePresentedImageDataImage()
	{
		if(m_GetPresentedImgDataHelperImage != VK_NULL_HANDLE)
		{
			vkDestroyFence(m_VKDevice, m_GetPresentedImgDataHelperFence, nullptr);

			m_GetPresentedImgDataHelperFence = VK_NULL_HANDLE;

			vkDestroyImage(m_VKDevice, m_GetPresentedImgDataHelperImage, nullptr);
			vkUnmapMemory(m_VKDevice, m_GetPresentedImgDataHelperMem.m_Mem);
			vkFreeMemory(m_VKDevice, m_GetPresentedImgDataHelperMem.m_Mem, nullptr);

			m_GetPresentedImgDataHelperImage = VK_NULL_HANDLE;
			m_GetPresentedImgDataHelperMem = {};
			m_pGetPresentedImgDataHelperMappedMemory = nullptr;

			m_GetPresentedImgDataHelperWidth = 0;
			m_GetPresentedImgDataHelperHeight = 0;
		}
	}

	bool GetPresentedImageDataImpl(uint32_t &Width, uint32_t &Height, uint32_t &Format, std::vector<uint8_t> &vDstData, bool FlipImgData, bool ResetAlpha)
	{
		bool IsB8G8R8A8 = m_VKSurfFormat.format == VK_FORMAT_B8G8R8A8_UNORM;
		bool UsesRGBALikeFormat = m_VKSurfFormat.format == VK_FORMAT_R8G8B8A8_UNORM || IsB8G8R8A8;
		if(UsesRGBALikeFormat && m_LastPresentedSwapChainImageIndex != std::numeric_limits<decltype(m_LastPresentedSwapChainImageIndex)>::max())
		{
			auto Viewport = m_VKSwapImgAndViewportExtent.GetPresentedImageViewport();
			Width = Viewport.width;
			Height = Viewport.height;
			Format = CImageInfo::FORMAT_RGBA;

			size_t ImageTotalSize = (size_t)Width * Height * 4;

			PreparePresentedImageDataImage(Width, Height);

			VkCommandBuffer CommandBuffer = GetMemoryCommandBuffer();

			VkBufferImageCopy Region{};
			Region.bufferOffset = 0;
			Region.bufferRowLength = 0;
			Region.bufferImageHeight = 0;
			Region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			Region.imageSubresource.mipLevel = 0;
			Region.imageSubresource.baseArrayLayer = 0;
			Region.imageSubresource.layerCount = 1;
			Region.imageOffset = {0, 0, 0};
			Region.imageExtent = {Viewport.width, Viewport.height, 1};

			auto &SwapImg = m_vSwapChainImages[m_LastPresentedSwapChainImageIndex];

			ImageBarrier(m_GetPresentedImgDataHelperImage, 0, 1, 0, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			ImageBarrier(SwapImg, 0, 1, 0, 1, m_VKSurfFormat.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

			// If source and destination support blit we'll blit as this also does automatic format conversion (e.g. from BGR to RGB)
			if(m_OptimalSwapChainImageBlitting && m_LinearRGBAImageBlitting)
			{
				VkOffset3D BlitSize;
				BlitSize.x = Width;
				BlitSize.y = Height;
				BlitSize.z = 1;
				VkImageBlit ImageBlitRegion{};
				ImageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				ImageBlitRegion.srcSubresource.layerCount = 1;
				ImageBlitRegion.srcOffsets[1] = BlitSize;
				ImageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				ImageBlitRegion.dstSubresource.layerCount = 1;
				ImageBlitRegion.dstOffsets[1] = BlitSize;

				// Issue the blit command
				vkCmdBlitImage(CommandBuffer, SwapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					m_GetPresentedImgDataHelperImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1, &ImageBlitRegion, VK_FILTER_NEAREST);

				// transformed to RGBA
				IsB8G8R8A8 = false;
			}
			else
			{
				// Otherwise use image copy (requires us to manually flip components)
				VkImageCopy ImageCopyRegion{};
				ImageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				ImageCopyRegion.srcSubresource.layerCount = 1;
				ImageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				ImageCopyRegion.dstSubresource.layerCount = 1;
				ImageCopyRegion.extent.width = Width;
				ImageCopyRegion.extent.height = Height;
				ImageCopyRegion.extent.depth = 1;

				// Issue the copy command
				vkCmdCopyImage(CommandBuffer, SwapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					m_GetPresentedImgDataHelperImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1, &ImageCopyRegion);
			}

			ImageBarrier(m_GetPresentedImgDataHelperImage, 0, 1, 0, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
			ImageBarrier(SwapImg, 0, 1, 0, 1, m_VKSurfFormat.format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

			vkEndCommandBuffer(CommandBuffer);
			m_vUsedMemoryCommandBuffer[m_CurImageIndex] = false;

			VkSubmitInfo SubmitInfo{};
			SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

			SubmitInfo.commandBufferCount = 1;
			SubmitInfo.pCommandBuffers = &CommandBuffer;

			vkResetFences(m_VKDevice, 1, &m_GetPresentedImgDataHelperFence);
			vkQueueSubmit(m_VKGraphicsQueue, 1, &SubmitInfo, m_GetPresentedImgDataHelperFence);
			vkWaitForFences(m_VKDevice, 1, &m_GetPresentedImgDataHelperFence, VK_TRUE, std::numeric_limits<uint64_t>::max());

			VkMappedMemoryRange MemRange{};
			MemRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			MemRange.memory = m_GetPresentedImgDataHelperMem.m_Mem;
			MemRange.offset = m_GetPresentedImgDataHelperMappedLayoutOffset;
			MemRange.size = VK_WHOLE_SIZE;
			vkInvalidateMappedMemoryRanges(m_VKDevice, 1, &MemRange);

			size_t RealFullImageSize = maximum(ImageTotalSize, (size_t)(Height * m_GetPresentedImgDataHelperMappedLayoutPitch));
			if(vDstData.size() < RealFullImageSize + (Width * 4))
				vDstData.resize(RealFullImageSize + (Width * 4)); // extra space for flipping

			mem_copy(vDstData.data(), m_pGetPresentedImgDataHelperMappedMemory, RealFullImageSize);

			// pack image data together without any offset that the driver might require
			if(Width * 4 < m_GetPresentedImgDataHelperMappedLayoutPitch)
			{
				for(uint32_t Y = 0; Y < Height; ++Y)
				{
					size_t OffsetImagePacked = (Y * Width * 4);
					size_t OffsetImageUnpacked = (Y * m_GetPresentedImgDataHelperMappedLayoutPitch);
					mem_copy(vDstData.data() + OffsetImagePacked, vDstData.data() + OffsetImageUnpacked, Width * 4);
				}
			}

			if(IsB8G8R8A8 || ResetAlpha)
			{
				// swizzle
				for(uint32_t Y = 0; Y < Height; ++Y)
				{
					for(uint32_t X = 0; X < Width; ++X)
					{
						size_t ImgOff = (Y * Width * 4) + (X * 4);
						if(IsB8G8R8A8)
						{
							std::swap(vDstData[ImgOff], vDstData[ImgOff + 2]);
						}
						vDstData[ImgOff + 3] = 255;
					}
				}
			}

			if(FlipImgData)
			{
				uint8_t *pTempRow = vDstData.data() + Width * Height * 4;
				for(uint32_t Y = 0; Y < Height / 2; ++Y)
				{
					mem_copy(pTempRow, vDstData.data() + Y * Width * 4, Width * 4);
					mem_copy(vDstData.data() + Y * Width * 4, vDstData.data() + ((Height - Y) - 1) * Width * 4, Width * 4);
					mem_copy(vDstData.data() + ((Height - Y) - 1) * Width * 4, pTempRow, Width * 4);
				}
			}

			return true;
		}
		else
		{
			if(!UsesRGBALikeFormat)
			{
				dbg_msg("vulkan", "swap chain image was not in a RGBA like format.");
			}
			else
			{
				dbg_msg("vulkan", "swap chain image was not ready to be copied.");
			}
			return false;
		}
	}

	bool GetPresentedImageData(uint32_t &Width, uint32_t &Height, uint32_t &Format, std::vector<uint8_t> &vDstData) override
	{
		return GetPresentedImageDataImpl(Width, Height, Format, vDstData, false, false);
	}

	/************************
	* MEMORY MANAGMENT
	************************/

	bool AllocateVulkanMemory(const VkMemoryAllocateInfo *pAllocateInfo, VkDeviceMemory *pMemory)
	{
		VkResult Res = vkAllocateMemory(m_VKDevice, pAllocateInfo, nullptr, pMemory);
		if(Res != VK_SUCCESS)
		{
			dbg_msg("vulkan", "vulkan memory allocation failed, trying to recover.");
			if(Res == VK_ERROR_OUT_OF_HOST_MEMORY || Res == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			{
				// aggressivly try to get more memory
				vkDeviceWaitIdle(m_VKDevice);
				for(size_t i = 0; i < m_SwapChainImageCount + 1; ++i)
					NextFrame();
				Res = vkAllocateMemory(m_VKDevice, pAllocateInfo, nullptr, pMemory);
			}
			if(Res != VK_SUCCESS)
			{
				dbg_msg("vulkan", "vulkan memory allocation failed.");
				return false;
			}
		}
		return true;
	}

	void GetBufferImpl(VkDeviceSize RequiredSize, EMemoryBlockUsage MemUsage, VkBuffer &Buffer, SDeviceMemoryBlock &BufferMemory, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags BufferProperties)
	{
		CreateBuffer(RequiredSize, MemUsage, BufferUsage, BufferProperties, Buffer, BufferMemory);
	}

	template<size_t ID,
		int64_t MemoryBlockSize, size_t BlockCount,
		bool RequiresMapping>
	SMemoryBlock<ID> GetBufferBlockImpl(SMemoryBlockCache<ID> &MemoryCache, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags BufferProperties, const void *pBufferData, VkDeviceSize RequiredSize, VkDeviceSize TargetAlignment)
	{
		SMemoryBlock<ID> RetBlock;

		auto &&CreateCacheBlock = [&]() {
			bool FoundAllocation = false;
			SMemoryHeap::SMemoryHeapQueueElement AllocatedMem;
			SDeviceMemoryBlock TmpBufferMemory;
			typename SMemoryBlockCache<ID>::SMemoryCacheType::SMemoryCacheHeap *pCacheHeap = nullptr;
			auto &Heaps = MemoryCache.m_MemoryCaches.m_vpMemoryHeaps;
			for(size_t i = 0; i < Heaps.size(); ++i)
			{
				auto *pHeap = Heaps[i];
				if(pHeap->m_Heap.Allocate(RequiredSize, TargetAlignment, AllocatedMem))
				{
					TmpBufferMemory = pHeap->m_BufferMem;
					FoundAllocation = true;
					pCacheHeap = pHeap;
					break;
				}
			}
			if(!FoundAllocation)
			{
				typename SMemoryBlockCache<ID>::SMemoryCacheType::SMemoryCacheHeap *pNewHeap = new typename SMemoryBlockCache<ID>::SMemoryCacheType::SMemoryCacheHeap();

				VkBuffer TmpBuffer;
				GetBufferImpl(MemoryBlockSize * BlockCount, RequiresMapping ? MEMORY_BLOCK_USAGE_STAGING : MEMORY_BLOCK_USAGE_BUFFER, TmpBuffer, TmpBufferMemory, BufferUsage, BufferProperties);

				void *pMapData = nullptr;

				if(RequiresMapping)
				{
					if(vkMapMemory(m_VKDevice, TmpBufferMemory.m_Mem, 0, VK_WHOLE_SIZE, 0, &pMapData) != VK_SUCCESS)
					{
						SetError("Failed to map buffer block memory.");
					}
				}

				pNewHeap->m_Buffer = TmpBuffer;

				pNewHeap->m_BufferMem = TmpBufferMemory;
				pNewHeap->m_pMappedBuffer = pMapData;

				pCacheHeap = pNewHeap;
				Heaps.emplace_back(pNewHeap);
				Heaps.back()->m_Heap.Init(MemoryBlockSize * BlockCount, 0);
				if(!Heaps.back()->m_Heap.Allocate(RequiredSize, TargetAlignment, AllocatedMem))
				{
					dbg_assert(false, "Heap allocation failed directly after creating fresh heap");
				}
			}

			RetBlock.m_Buffer = pCacheHeap->m_Buffer;
			RetBlock.m_BufferMem = TmpBufferMemory;
			if(RequiresMapping)
				RetBlock.m_pMappedBuffer = ((uint8_t *)pCacheHeap->m_pMappedBuffer) + AllocatedMem.m_OffsetToAlign;
			else
				RetBlock.m_pMappedBuffer = nullptr;
			RetBlock.m_IsCached = true;
			RetBlock.m_pHeap = &pCacheHeap->m_Heap;
			RetBlock.m_HeapData = AllocatedMem;
			RetBlock.m_UsedSize = RequiredSize;

			if(RequiresMapping)
				mem_copy(RetBlock.m_pMappedBuffer, pBufferData, RequiredSize);
		};

		if(RequiredSize < (VkDeviceSize)MemoryBlockSize)
		{
			CreateCacheBlock();
		}
		else
		{
			VkBuffer TmpBuffer;
			SDeviceMemoryBlock TmpBufferMemory;
			GetBufferImpl(RequiredSize, RequiresMapping ? MEMORY_BLOCK_USAGE_STAGING : MEMORY_BLOCK_USAGE_BUFFER, TmpBuffer, TmpBufferMemory, BufferUsage, BufferProperties);

			void *pMapData = nullptr;
			if(RequiresMapping)
			{
				vkMapMemory(m_VKDevice, TmpBufferMemory.m_Mem, 0, VK_WHOLE_SIZE, 0, &pMapData);
				mem_copy(pMapData, pBufferData, static_cast<size_t>(RequiredSize));
			}

			RetBlock.m_Buffer = TmpBuffer;
			RetBlock.m_BufferMem = TmpBufferMemory;
			RetBlock.m_pMappedBuffer = pMapData;
			RetBlock.m_pHeap = nullptr;
			RetBlock.m_IsCached = false;
			RetBlock.m_HeapData.m_OffsetToAlign = 0;
			RetBlock.m_HeapData.m_AllocationSize = RequiredSize;
			RetBlock.m_UsedSize = RequiredSize;
		}

		return RetBlock;
	}

	SMemoryBlock<s_StagingBufferCacheID> GetStagingBuffer(const void *pBufferData, VkDeviceSize RequiredSize)
	{
		return GetBufferBlockImpl<s_StagingBufferCacheID, 8 * 1024 * 1024, 3, true>(m_StagingBufferCache, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, pBufferData, RequiredSize, maximum<VkDeviceSize>(m_NonCoherentMemAlignment, 16));
	}

	SMemoryBlock<s_StagingBufferImageCacheID> GetStagingBufferImage(const void *pBufferData, VkDeviceSize RequiredSize)
	{
		return GetBufferBlockImpl<s_StagingBufferImageCacheID, 8 * 1024 * 1024, 3, true>(m_StagingBufferCacheImage, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, pBufferData, RequiredSize, maximum<VkDeviceSize>(m_OptimalImageCopyMemAlignment, maximum<VkDeviceSize>(m_NonCoherentMemAlignment, 16)));
	}

	template<size_t ID>
	void PrepareStagingMemRange(SMemoryBlock<ID> &Block)
	{
		VkMappedMemoryRange UploadRange{};
		UploadRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		UploadRange.memory = Block.m_BufferMem.m_Mem;
		UploadRange.offset = Block.m_HeapData.m_OffsetToAlign;

		auto AlignmentMod = ((VkDeviceSize)Block.m_HeapData.m_AllocationSize % m_NonCoherentMemAlignment);
		auto AlignmentReq = (m_NonCoherentMemAlignment - AlignmentMod);
		if(AlignmentMod == 0)
			AlignmentReq = 0;
		UploadRange.size = Block.m_HeapData.m_AllocationSize + AlignmentReq;

		if(UploadRange.offset + UploadRange.size > Block.m_BufferMem.m_Size)
			UploadRange.size = VK_WHOLE_SIZE;

		m_vNonFlushedStagingBufferRange.push_back(UploadRange);
	}

	void UploadAndFreeStagingMemBlock(SMemoryBlock<s_StagingBufferCacheID> &Block)
	{
		PrepareStagingMemRange(Block);
		if(!Block.m_IsCached)
		{
			m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({Block.m_Buffer, Block.m_BufferMem, Block.m_pMappedBuffer});
		}
		else
		{
			m_StagingBufferCache.FreeMemBlock(Block, m_CurImageIndex);
		}
	}

	void UploadAndFreeStagingImageMemBlock(SMemoryBlock<s_StagingBufferImageCacheID> &Block)
	{
		PrepareStagingMemRange(Block);
		if(!Block.m_IsCached)
		{
			m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({Block.m_Buffer, Block.m_BufferMem, Block.m_pMappedBuffer});
		}
		else
		{
			m_StagingBufferCacheImage.FreeMemBlock(Block, m_CurImageIndex);
		}
	}

	SMemoryBlock<s_VertexBufferCacheID> GetVertexBuffer(VkDeviceSize RequiredSize)
	{
		return GetBufferBlockImpl<s_VertexBufferCacheID, 8 * 1024 * 1024, 3, false>(m_VertexBufferCache, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, RequiredSize, 16);
	}

	void FreeVertexMemBlock(SMemoryBlock<s_VertexBufferCacheID> &Block)
	{
		if(!Block.m_IsCached)
		{
			m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({Block.m_Buffer, Block.m_BufferMem, nullptr});
		}
		else
		{
			m_VertexBufferCache.FreeMemBlock(Block, m_CurImageIndex);
		}
	}

	static size_t ImageMipLevelCount(size_t Width, size_t Height, size_t Depth)
	{
		return floor(log2(maximum(Width, maximum(Height, Depth)))) + 1;
	}

	static size_t ImageMipLevelCount(VkExtent3D &ImgExtent)
	{
		return ImageMipLevelCount(ImgExtent.width, ImgExtent.height, ImgExtent.depth);
	}

	// good approximation of 1024x1024 image with mipmaps
	static constexpr int64_t s_1024x1024ImgSize = (1024 * 1024 * 4) * 2;

	bool GetImageMemoryImpl(VkDeviceSize RequiredSize, uint32_t RequiredMemoryTypeBits, SDeviceMemoryBlock &BufferMemory, VkMemoryPropertyFlags BufferProperties)
	{
		VkMemoryAllocateInfo MemAllocInfo{};
		MemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		MemAllocInfo.allocationSize = RequiredSize;
		MemAllocInfo.memoryTypeIndex = FindMemoryType(m_VKGPU, RequiredMemoryTypeBits, BufferProperties);

		BufferMemory.m_Size = RequiredSize;
		m_pTextureMemoryUsage->store(m_pTextureMemoryUsage->load(std::memory_order_relaxed) + RequiredSize, std::memory_order_relaxed);

		if(IsVerbose())
		{
			VerboseAllocatedMemory(RequiredSize, m_CurImageIndex, MEMORY_BLOCK_USAGE_TEXTURE);
		}

		if(!AllocateVulkanMemory(&MemAllocInfo, &BufferMemory.m_Mem))
		{
			SetError("Allocation for image memory failed.");
			return false;
		}

		BufferMemory.m_UsageType = MEMORY_BLOCK_USAGE_TEXTURE;

		return true;
	}

	template<size_t ID,
		int64_t MemoryBlockSize, size_t BlockCount>
	SMemoryImageBlock<ID> GetImageMemoryBlockImpl(SMemoryBlockCache<ID> &MemoryCache, VkMemoryPropertyFlags BufferProperties, VkDeviceSize RequiredSize, VkDeviceSize RequiredAlignment, uint32_t RequiredMemoryTypeBits)
	{
		SMemoryImageBlock<ID> RetBlock;

		auto &&CreateCacheBlock = [&]() {
			bool FoundAllocation = false;
			SMemoryHeap::SMemoryHeapQueueElement AllocatedMem;
			SDeviceMemoryBlock TmpBufferMemory;
			typename SMemoryBlockCache<ID>::SMemoryCacheType::SMemoryCacheHeap *pCacheHeap = nullptr;
			for(size_t i = 0; i < MemoryCache.m_MemoryCaches.m_vpMemoryHeaps.size(); ++i)
			{
				auto *pHeap = MemoryCache.m_MemoryCaches.m_vpMemoryHeaps[i];
				if(pHeap->m_Heap.Allocate(RequiredSize, RequiredAlignment, AllocatedMem))
				{
					TmpBufferMemory = pHeap->m_BufferMem;
					FoundAllocation = true;
					pCacheHeap = pHeap;
					break;
				}
			}
			if(!FoundAllocation)
			{
				typename SMemoryBlockCache<ID>::SMemoryCacheType::SMemoryCacheHeap *pNewHeap = new typename SMemoryBlockCache<ID>::SMemoryCacheType::SMemoryCacheHeap();

				GetImageMemoryImpl(MemoryBlockSize * BlockCount, RequiredMemoryTypeBits, TmpBufferMemory, BufferProperties);

				pNewHeap->m_Buffer = VK_NULL_HANDLE;

				pNewHeap->m_BufferMem = TmpBufferMemory;
				pNewHeap->m_pMappedBuffer = nullptr;

				auto &Heaps = MemoryCache.m_MemoryCaches.m_vpMemoryHeaps;
				pCacheHeap = pNewHeap;
				Heaps.emplace_back(pNewHeap);
				Heaps.back()->m_Heap.Init(MemoryBlockSize * BlockCount, 0);
				if(!Heaps.back()->m_Heap.Allocate(RequiredSize, RequiredAlignment, AllocatedMem))
				{
					dbg_assert(false, "Heap allocation failed directly after creating fresh heap for image");
				}
			}

			RetBlock.m_Buffer = VK_NULL_HANDLE;
			RetBlock.m_BufferMem = TmpBufferMemory;
			RetBlock.m_pMappedBuffer = nullptr;
			RetBlock.m_IsCached = true;
			RetBlock.m_pHeap = &pCacheHeap->m_Heap;
			RetBlock.m_HeapData = AllocatedMem;
			RetBlock.m_UsedSize = RequiredSize;
		};

		if(RequiredSize < (VkDeviceSize)MemoryBlockSize)
		{
			CreateCacheBlock();
		}
		else
		{
			SDeviceMemoryBlock TmpBufferMemory;
			GetImageMemoryImpl(RequiredSize, RequiredMemoryTypeBits, TmpBufferMemory, BufferProperties);

			RetBlock.m_Buffer = VK_NULL_HANDLE;
			RetBlock.m_BufferMem = TmpBufferMemory;
			RetBlock.m_pMappedBuffer = nullptr;
			RetBlock.m_IsCached = false;
			RetBlock.m_pHeap = nullptr;
			RetBlock.m_HeapData.m_OffsetToAlign = 0;
			RetBlock.m_HeapData.m_AllocationSize = RequiredSize;
			RetBlock.m_UsedSize = RequiredSize;
		}

		RetBlock.m_ImageMemoryBits = RequiredMemoryTypeBits;

		return RetBlock;
	}

	SMemoryImageBlock<s_ImageBufferCacheID> GetImageMemory(VkDeviceSize RequiredSize, VkDeviceSize RequiredAlignment, uint32_t RequiredMemoryTypeBits)
	{
		auto it = m_ImageBufferCaches.find(RequiredMemoryTypeBits);
		if(it == m_ImageBufferCaches.end())
		{
			it = m_ImageBufferCaches.insert({RequiredMemoryTypeBits, {}}).first;

			it->second.Init(m_SwapChainImageCount);
		}
		return GetImageMemoryBlockImpl<s_ImageBufferCacheID, s_1024x1024ImgSize, 2>(it->second, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, RequiredSize, RequiredAlignment, RequiredMemoryTypeBits);
	}

	void FreeImageMemBlock(SMemoryImageBlock<s_ImageBufferCacheID> &Block)
	{
		if(!Block.m_IsCached)
		{
			m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({Block.m_Buffer, Block.m_BufferMem, nullptr});
		}
		else
		{
			m_ImageBufferCaches[Block.m_ImageMemoryBits].FreeMemBlock(Block, m_CurImageIndex);
		}
	}

	template<bool FlushForRendering, typename TName>
	void UploadStreamedBuffer(SStreamMemory<TName> &StreamedBuffer)
	{
		size_t RangeUpdateCount = 0;
		if(StreamedBuffer.IsUsed(m_CurImageIndex))
		{
			for(size_t i = 0; i < StreamedBuffer.GetUsedCount(m_CurImageIndex); ++i)
			{
				auto &BufferOfFrame = StreamedBuffer.GetBuffers(m_CurImageIndex)[i];
				auto &MemRange = StreamedBuffer.GetRanges(m_CurImageIndex)[RangeUpdateCount++];
				MemRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
				MemRange.memory = BufferOfFrame.m_BufferMem.m_Mem;
				MemRange.offset = BufferOfFrame.m_OffsetInBuffer;
				auto AlignmentMod = ((VkDeviceSize)BufferOfFrame.m_UsedSize % m_NonCoherentMemAlignment);
				auto AlignmentReq = (m_NonCoherentMemAlignment - AlignmentMod);
				if(AlignmentMod == 0)
					AlignmentReq = 0;
				MemRange.size = BufferOfFrame.m_UsedSize + AlignmentReq;

				if(MemRange.offset + MemRange.size > BufferOfFrame.m_BufferMem.m_Size)
					MemRange.size = VK_WHOLE_SIZE;

				BufferOfFrame.m_UsedSize = 0;
			}
			if(RangeUpdateCount > 0 && FlushForRendering)
			{
				vkFlushMappedMemoryRanges(m_VKDevice, RangeUpdateCount, StreamedBuffer.GetRanges(m_CurImageIndex).data());
			}
		}
		StreamedBuffer.ResetFrame(m_CurImageIndex);
	}

	void CleanBufferPair(size_t ImageIndex, VkBuffer &Buffer, SDeviceMemoryBlock &BufferMem)
	{
		bool IsBuffer = Buffer != VK_NULL_HANDLE;
		if(IsBuffer)
		{
			vkDestroyBuffer(m_VKDevice, Buffer, nullptr);

			Buffer = VK_NULL_HANDLE;
		}
		if(BufferMem.m_Mem != VK_NULL_HANDLE)
		{
			vkFreeMemory(m_VKDevice, BufferMem.m_Mem, nullptr);
			if(BufferMem.m_UsageType == MEMORY_BLOCK_USAGE_BUFFER)
				m_pBufferMemoryUsage->store(m_pBufferMemoryUsage->load(std::memory_order_relaxed) - BufferMem.m_Size, std::memory_order_relaxed);
			else if(BufferMem.m_UsageType == MEMORY_BLOCK_USAGE_TEXTURE)
				m_pTextureMemoryUsage->store(m_pTextureMemoryUsage->load(std::memory_order_relaxed) - BufferMem.m_Size, std::memory_order_relaxed);
			else if(BufferMem.m_UsageType == MEMORY_BLOCK_USAGE_STREAM)
				m_pStreamMemoryUsage->store(m_pStreamMemoryUsage->load(std::memory_order_relaxed) - BufferMem.m_Size, std::memory_order_relaxed);
			else if(BufferMem.m_UsageType == MEMORY_BLOCK_USAGE_STAGING)
				m_pStagingMemoryUsage->store(m_pStagingMemoryUsage->load(std::memory_order_relaxed) - BufferMem.m_Size, std::memory_order_relaxed);

			if(IsVerbose())
			{
				VerboseDeallocatedMemory(BufferMem.m_Size, (size_t)ImageIndex, BufferMem.m_UsageType);
			}

			BufferMem.m_Mem = VK_NULL_HANDLE;
		}
	}

	void DestroyTexture(CTexture &Texture)
	{
		if(Texture.m_Img != VK_NULL_HANDLE)
		{
			FreeImageMemBlock(Texture.m_ImgMem);
			vkDestroyImage(m_VKDevice, Texture.m_Img, nullptr);

			vkDestroyImageView(m_VKDevice, Texture.m_ImgView, nullptr);
		}

		if(Texture.m_Img3D != VK_NULL_HANDLE)
		{
			FreeImageMemBlock(Texture.m_Img3DMem);
			vkDestroyImage(m_VKDevice, Texture.m_Img3D, nullptr);

			vkDestroyImageView(m_VKDevice, Texture.m_Img3DView, nullptr);
		}

		DestroyTexturedStandardDescriptorSets(Texture, 0);
		DestroyTexturedStandardDescriptorSets(Texture, 1);

		DestroyTextured3DStandardDescriptorSets(Texture);
	}

	void DestroyTextTexture(CTexture &Texture, CTexture &TextureOutline)
	{
		if(Texture.m_Img != VK_NULL_HANDLE)
		{
			FreeImageMemBlock(Texture.m_ImgMem);
			vkDestroyImage(m_VKDevice, Texture.m_Img, nullptr);

			vkDestroyImageView(m_VKDevice, Texture.m_ImgView, nullptr);
		}

		if(TextureOutline.m_Img != VK_NULL_HANDLE)
		{
			FreeImageMemBlock(TextureOutline.m_ImgMem);
			vkDestroyImage(m_VKDevice, TextureOutline.m_Img, nullptr);

			vkDestroyImageView(m_VKDevice, TextureOutline.m_ImgView, nullptr);
		}

		DestroyTextDescriptorSets(Texture, TextureOutline);
	}

	void ClearFrameData(size_t FrameImageIndex)
	{
		UploadStagingBuffers();

		// clear pending buffers, that require deletion
		for(auto &BufferPair : m_vvFrameDelayedBufferCleanup[FrameImageIndex])
		{
			if(BufferPair.m_pMappedData != nullptr)
			{
				vkUnmapMemory(m_VKDevice, BufferPair.m_Mem.m_Mem);
			}
			CleanBufferPair(FrameImageIndex, BufferPair.m_Buffer, BufferPair.m_Mem);
		}
		m_vvFrameDelayedBufferCleanup[FrameImageIndex].clear();

		// clear pending textures, that require deletion
		for(auto &Texture : m_vvFrameDelayedTextureCleanup[FrameImageIndex])
		{
			DestroyTexture(Texture);
		}
		m_vvFrameDelayedTextureCleanup[FrameImageIndex].clear();

		for(auto &TexturePair : m_vvFrameDelayedTextTexturesCleanup[FrameImageIndex])
		{
			DestroyTextTexture(TexturePair.first, TexturePair.second);
		}
		m_vvFrameDelayedTextTexturesCleanup[FrameImageIndex].clear();

		m_StagingBufferCache.Cleanup(FrameImageIndex);
		m_StagingBufferCacheImage.Cleanup(FrameImageIndex);
		m_VertexBufferCache.Cleanup(FrameImageIndex);
		for(auto &ImageBufferCache : m_ImageBufferCaches)
			ImageBufferCache.second.Cleanup(FrameImageIndex);
	}

	void ShrinkUnusedCaches()
	{
		size_t FreeedMemory = 0;
		FreeedMemory += m_StagingBufferCache.Shrink(m_VKDevice);
		FreeedMemory += m_StagingBufferCacheImage.Shrink(m_VKDevice);
		if(FreeedMemory > 0)
		{
			m_pStagingMemoryUsage->store(m_pStagingMemoryUsage->load(std::memory_order_relaxed) - FreeedMemory, std::memory_order_relaxed);
			if(IsVerbose())
			{
				dbg_msg("vulkan", "deallocated chunks of memory with size: %" PRIu64 " from all frames (staging buffer)", (size_t)FreeedMemory);
			}
		}
		FreeedMemory = 0;
		FreeedMemory += m_VertexBufferCache.Shrink(m_VKDevice);
		if(FreeedMemory > 0)
		{
			m_pBufferMemoryUsage->store(m_pBufferMemoryUsage->load(std::memory_order_relaxed) - FreeedMemory, std::memory_order_relaxed);
			if(IsVerbose())
			{
				dbg_msg("vulkan", "deallocated chunks of memory with size: %" PRIu64 " from all frames (buffer)", (size_t)FreeedMemory);
			}
		}
		FreeedMemory = 0;
		for(auto &ImageBufferCache : m_ImageBufferCaches)
			FreeedMemory += ImageBufferCache.second.Shrink(m_VKDevice);
		if(FreeedMemory > 0)
		{
			m_pTextureMemoryUsage->store(m_pTextureMemoryUsage->load(std::memory_order_relaxed) - FreeedMemory, std::memory_order_relaxed);
			if(IsVerbose())
			{
				dbg_msg("vulkan", "deallocated chunks of memory with size: %" PRIu64 " from all frames (texture)", (size_t)FreeedMemory);
			}
		}
	}

	void MemoryBarrier(VkBuffer Buffer, VkDeviceSize Offset, VkDeviceSize Size, VkAccessFlags BufferAccessType, bool BeforeCommand)
	{
		VkCommandBuffer MemCommandBuffer = GetMemoryCommandBuffer();

		VkBufferMemoryBarrier Barrier{};
		Barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.buffer = Buffer;
		Barrier.offset = Offset;
		Barrier.size = Size;

		VkPipelineStageFlags SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkPipelineStageFlags DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

		if(BeforeCommand)
		{
			Barrier.srcAccessMask = BufferAccessType;
			Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			SourceStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
			DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else
		{
			Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			Barrier.dstAccessMask = BufferAccessType;

			SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			DestinationStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
		}

		vkCmdPipelineBarrier(
			MemCommandBuffer,
			SourceStage, DestinationStage,
			0,
			0, nullptr,
			1, &Barrier,
			0, nullptr);
	}

	/************************
	* SWAPPING MECHANISM
	************************/

	void StartRenderThread(size_t ThreadIndex)
	{
		auto &List = m_vvThreadCommandLists[ThreadIndex];
		if(!List.empty())
		{
			m_vThreadHelperHadCommands[ThreadIndex] = true;
			auto *pThread = m_vpRenderThreads[ThreadIndex].get();
			std::unique_lock<std::mutex> Lock(pThread->m_Mutex);
			pThread->m_IsRendering = true;
			pThread->m_Cond.notify_one();
		}
	}

	void FinishRenderThreads()
	{
		if(m_ThreadCount > 1)
		{
			// execute threads

			for(size_t ThreadIndex = 0; ThreadIndex < m_ThreadCount - 1; ++ThreadIndex)
			{
				if(!m_vThreadHelperHadCommands[ThreadIndex])
				{
					StartRenderThread(ThreadIndex);
				}
			}

			for(size_t ThreadIndex = 0; ThreadIndex < m_ThreadCount - 1; ++ThreadIndex)
			{
				if(m_vThreadHelperHadCommands[ThreadIndex])
				{
					auto &pRenderThread = m_vpRenderThreads[ThreadIndex];
					m_vThreadHelperHadCommands[ThreadIndex] = false;
					std::unique_lock<std::mutex> Lock(pRenderThread->m_Mutex);
					pRenderThread->m_Cond.wait(Lock, [&pRenderThread] { return !pRenderThread->m_IsRendering; });
					m_vLastPipeline[ThreadIndex + 1] = VK_NULL_HANDLE;
				}
			}
		}
	}

	void ExecuteMemoryCommandBuffer()
	{
		if(m_vUsedMemoryCommandBuffer[m_CurImageIndex])
		{
			auto &MemoryCommandBuffer = m_vMemoryCommandBuffers[m_CurImageIndex];
			vkEndCommandBuffer(MemoryCommandBuffer);

			VkSubmitInfo SubmitInfo{};
			SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

			SubmitInfo.commandBufferCount = 1;
			SubmitInfo.pCommandBuffers = &MemoryCommandBuffer;
			vkQueueSubmit(m_VKGraphicsQueue, 1, &SubmitInfo, VK_NULL_HANDLE);
			vkQueueWaitIdle(m_VKGraphicsQueue);

			m_vUsedMemoryCommandBuffer[m_CurImageIndex] = false;
		}
	}

	void ClearFrameMemoryUsage()
	{
		ClearFrameData(m_CurImageIndex);
		ShrinkUnusedCaches();
	}

	void WaitFrame()
	{
		FinishRenderThreads();
		m_LastCommandsInPipeThreadIndex = 0;

		UploadNonFlushedBuffers<true>();

		auto &CommandBuffer = GetMainGraphicCommandBuffer();

		// render threads
		if(m_ThreadCount > 1)
		{
			size_t ThreadedCommandsUsedCount = 0;
			size_t RenderThreadCount = m_ThreadCount - 1;
			for(size_t i = 0; i < RenderThreadCount; ++i)
			{
				if(m_vvUsedThreadDrawCommandBuffer[i + 1][m_CurImageIndex])
				{
					auto &GraphicThreadCommandBuffer = m_vvThreadDrawCommandBuffers[i + 1][m_CurImageIndex];
					m_vHelperThreadDrawCommandBuffers[ThreadedCommandsUsedCount++] = GraphicThreadCommandBuffer;

					m_vvUsedThreadDrawCommandBuffer[i + 1][m_CurImageIndex] = false;
				}
			}
			if(ThreadedCommandsUsedCount > 0)
			{
				vkCmdExecuteCommands(CommandBuffer, ThreadedCommandsUsedCount, m_vHelperThreadDrawCommandBuffers.data());
			}

			// special case if swap chain was not completed in one runbuffer call

			if(m_vvUsedThreadDrawCommandBuffer[0][m_CurImageIndex])
			{
				auto &GraphicThreadCommandBuffer = m_vvThreadDrawCommandBuffers[0][m_CurImageIndex];
				vkEndCommandBuffer(GraphicThreadCommandBuffer);

				vkCmdExecuteCommands(CommandBuffer, 1, &GraphicThreadCommandBuffer);

				m_vvUsedThreadDrawCommandBuffer[0][m_CurImageIndex] = false;
			}
		}

		vkCmdEndRenderPass(CommandBuffer);

		if(vkEndCommandBuffer(CommandBuffer) != VK_SUCCESS)
		{
			SetError("Command buffer cannot be ended anymore.");
		}

		VkSemaphore WaitSemaphore = m_vWaitSemaphores[m_CurFrames];

		VkSubmitInfo SubmitInfo{};
		SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		SubmitInfo.commandBufferCount = 1;
		SubmitInfo.pCommandBuffers = &CommandBuffer;

		std::array<VkCommandBuffer, 2> aCommandBuffers = {};

		if(m_vUsedMemoryCommandBuffer[m_CurImageIndex])
		{
			auto &MemoryCommandBuffer = m_vMemoryCommandBuffers[m_CurImageIndex];
			vkEndCommandBuffer(MemoryCommandBuffer);

			aCommandBuffers[0] = MemoryCommandBuffer;
			aCommandBuffers[1] = CommandBuffer;
			SubmitInfo.commandBufferCount = 2;
			SubmitInfo.pCommandBuffers = aCommandBuffers.data();

			m_vUsedMemoryCommandBuffer[m_CurImageIndex] = false;
		}

		std::array<VkSemaphore, 1> aWaitSemaphores = {WaitSemaphore};
		std::array<VkPipelineStageFlags, 1> aWaitStages = {(VkPipelineStageFlags)VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
		SubmitInfo.waitSemaphoreCount = aWaitSemaphores.size();
		SubmitInfo.pWaitSemaphores = aWaitSemaphores.data();
		SubmitInfo.pWaitDstStageMask = aWaitStages.data();

		std::array<VkSemaphore, 1> aSignalSemaphores = {m_vSigSemaphores[m_CurFrames]};
		SubmitInfo.signalSemaphoreCount = aSignalSemaphores.size();
		SubmitInfo.pSignalSemaphores = aSignalSemaphores.data();

		vkResetFences(m_VKDevice, 1, &m_vFrameFences[m_CurFrames]);

		VkResult QueueSubmitRes = vkQueueSubmit(m_VKGraphicsQueue, 1, &SubmitInfo, m_vFrameFences[m_CurFrames]);
		if(QueueSubmitRes != VK_SUCCESS)
		{
			const char *pCritErrorMsg = CheckVulkanCriticalError(QueueSubmitRes);
			if(pCritErrorMsg != nullptr)
				SetError("Submitting to graphics queue failed.", pCritErrorMsg);
		}

		std::swap(m_vWaitSemaphores[m_CurFrames], m_vSigSemaphores[m_CurFrames]);

		VkPresentInfoKHR PresentInfo{};
		PresentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

		PresentInfo.waitSemaphoreCount = aSignalSemaphores.size();
		PresentInfo.pWaitSemaphores = aSignalSemaphores.data();

		std::array<VkSwapchainKHR, 1> aSwapChains = {m_VKSwapChain};
		PresentInfo.swapchainCount = aSwapChains.size();
		PresentInfo.pSwapchains = aSwapChains.data();

		PresentInfo.pImageIndices = &m_CurImageIndex;

		m_LastPresentedSwapChainImageIndex = m_CurImageIndex;

		VkResult QueuePresentRes = vkQueuePresentKHR(m_VKPresentQueue, &PresentInfo);
		if(QueuePresentRes != VK_SUCCESS && QueuePresentRes != VK_SUBOPTIMAL_KHR)
		{
			const char *pCritErrorMsg = CheckVulkanCriticalError(QueuePresentRes);
			if(pCritErrorMsg != nullptr)
				SetError("Presenting graphics queue failed.", pCritErrorMsg);
		}

		m_CurFrames = (m_CurFrames + 1) % m_SwapChainImageCount;
	}

	void PrepareFrame()
	{
		if(m_RecreateSwapChain)
		{
			m_RecreateSwapChain = false;
			if(IsVerbose())
			{
				dbg_msg("vulkan", "recreating swap chain requested by user (prepare frame).");
			}
			RecreateSwapChain();
		}

		auto AcqResult = vkAcquireNextImageKHR(m_VKDevice, m_VKSwapChain, std::numeric_limits<uint64_t>::max(), m_vSigSemaphores[m_CurFrames], VK_NULL_HANDLE, &m_CurImageIndex);
		if(AcqResult != VK_SUCCESS)
		{
			if(AcqResult == VK_ERROR_OUT_OF_DATE_KHR || m_RecreateSwapChain)
			{
				m_RecreateSwapChain = false;
				if(IsVerbose())
				{
					dbg_msg("vulkan", "recreating swap chain requested by acquire next image (prepare frame).");
				}
				RecreateSwapChain();
				PrepareFrame();
				return;
			}
			else
			{
				if(AcqResult != VK_SUBOPTIMAL_KHR)
					dbg_msg("vulkan", "acquire next image failed %d", (int)AcqResult);

				const char *pCritErrorMsg = CheckVulkanCriticalError(AcqResult);
				if(pCritErrorMsg != nullptr)
					SetError("Acquiring next image failed.", pCritErrorMsg);
				else if(AcqResult == VK_ERROR_SURFACE_LOST_KHR)
				{
					m_RenderingPaused = true;
					return;
				}
			}
		}
		std::swap(m_vWaitSemaphores[m_CurFrames], m_vSigSemaphores[m_CurFrames]);

		if(m_vImagesFences[m_CurImageIndex] != VK_NULL_HANDLE)
		{
			vkWaitForFences(m_VKDevice, 1, &m_vImagesFences[m_CurImageIndex], VK_TRUE, std::numeric_limits<uint64_t>::max());
		}
		m_vImagesFences[m_CurImageIndex] = m_vFrameFences[m_CurFrames];

		// next frame
		m_CurFrame++;
		m_vImageLastFrameCheck[m_CurImageIndex] = m_CurFrame;

		// check if older frames weren't used in a long time
		for(size_t FrameImageIndex = 0; FrameImageIndex < m_vImageLastFrameCheck.size(); ++FrameImageIndex)
		{
			auto LastFrame = m_vImageLastFrameCheck[FrameImageIndex];
			if(m_CurFrame - LastFrame > (uint64_t)m_SwapChainImageCount)
			{
				if(m_vImagesFences[FrameImageIndex] != VK_NULL_HANDLE)
				{
					vkWaitForFences(m_VKDevice, 1, &m_vImagesFences[FrameImageIndex], VK_TRUE, std::numeric_limits<uint64_t>::max());
					ClearFrameData(FrameImageIndex);
					m_vImagesFences[FrameImageIndex] = VK_NULL_HANDLE;
				}
				m_vImageLastFrameCheck[FrameImageIndex] = m_CurFrame;
			}
		}

		// clear frame's memory data
		ClearFrameMemoryUsage();

		// clear frame
		vkResetCommandBuffer(GetMainGraphicCommandBuffer(), VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

		auto &CommandBuffer = GetMainGraphicCommandBuffer();
		VkCommandBufferBeginInfo BeginInfo{};
		BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		if(vkBeginCommandBuffer(CommandBuffer, &BeginInfo) != VK_SUCCESS)
		{
			SetError("Command buffer cannot be filled anymore.");
		}

		VkRenderPassBeginInfo RenderPassInfo{};
		RenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		RenderPassInfo.renderPass = m_VKRenderPass;
		RenderPassInfo.framebuffer = m_vFramebufferList[m_CurImageIndex];
		RenderPassInfo.renderArea.offset = {0, 0};
		RenderPassInfo.renderArea.extent = m_VKSwapImgAndViewportExtent.m_SwapImageViewport;

		VkClearValue ClearColorVal = {{{m_aClearColor[0], m_aClearColor[1], m_aClearColor[2], m_aClearColor[3]}}};
		RenderPassInfo.clearValueCount = 1;
		RenderPassInfo.pClearValues = &ClearColorVal;

		vkCmdBeginRenderPass(CommandBuffer, &RenderPassInfo, m_ThreadCount > 1 ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);

		for(auto &LastPipe : m_vLastPipeline)
			LastPipe = VK_NULL_HANDLE;
	}

	void UploadStagingBuffers()
	{
		if(!m_vNonFlushedStagingBufferRange.empty())
		{
			vkFlushMappedMemoryRanges(m_VKDevice, m_vNonFlushedStagingBufferRange.size(), m_vNonFlushedStagingBufferRange.data());

			m_vNonFlushedStagingBufferRange.clear();
		}
	}

	template<bool FlushForRendering>
	void UploadNonFlushedBuffers()
	{
		// streamed vertices
		for(auto &StreamVertexBuffer : m_vStreamedVertexBuffers)
			UploadStreamedBuffer<FlushForRendering>(StreamVertexBuffer);
		// now the buffer objects
		for(auto &StreamUniformBuffer : m_vStreamedUniformBuffers)
			UploadStreamedBuffer<FlushForRendering>(StreamUniformBuffer);

		UploadStagingBuffers();
	}

	void PureMemoryFrame()
	{
		ExecuteMemoryCommandBuffer();

		// reset streamed data
		UploadNonFlushedBuffers<false>();

		ClearFrameMemoryUsage();
	}

	void NextFrame()
	{
		if(!m_RenderingPaused)
		{
			WaitFrame();
			PrepareFrame();
		}
		// else only execute the memory command buffer
		else
		{
			PureMemoryFrame();
		}
	}

	/************************
	* TEXTURES
	************************/

	size_t VulkanFormatToImageColorChannelCount(VkFormat Format)
	{
		if(Format == VK_FORMAT_R8G8B8_UNORM)
			return 3;
		else if(Format == VK_FORMAT_R8G8B8A8_UNORM)
			return 4;
		else if(Format == VK_FORMAT_R8_UNORM)
			return 1;
		return 4;
	}

	void UpdateTexture(size_t TextureSlot, VkFormat Format, void *&pData, int64_t XOff, int64_t YOff, size_t Width, size_t Height, size_t ColorChannelCount)
	{
		size_t ImageSize = Width * Height * ColorChannelCount;
		auto StagingBuffer = GetStagingBufferImage(pData, ImageSize);

		auto &Tex = m_vTextures[TextureSlot];

		if(Tex.m_RescaleCount > 0)
		{
			for(uint32_t i = 0; i < Tex.m_RescaleCount; ++i)
			{
				Width >>= 1;
				Height >>= 1;

				XOff /= 2;
				YOff /= 2;
			}

			void *pTmpData = Resize((const uint8_t *)pData, Width, Height, Width, Height, VulkanFormatToImageColorChannelCount(Format));
			free(pData);
			pData = pTmpData;
		}

		ImageBarrier(Tex.m_Img, 0, Tex.m_MipMapCount, 0, 1, Format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(StagingBuffer.m_Buffer, StagingBuffer.m_HeapData.m_OffsetToAlign, Tex.m_Img, XOff, YOff, Width, Height, 1);

		if(Tex.m_MipMapCount > 1)
			BuildMipmaps(Tex.m_Img, Format, Width, Height, 1, Tex.m_MipMapCount);
		else
			ImageBarrier(Tex.m_Img, 0, 1, 0, 1, Format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		UploadAndFreeStagingImageMemBlock(StagingBuffer);
	}

	void CreateTextureCMD(
		int Slot,
		int Width,
		int Height,
		int PixelSize,
		VkFormat Format,
		VkFormat StoreFormat,
		int Flags,
		void *&pData)
	{
		size_t ImageIndex = (size_t)Slot;
		int ImageColorChannels = VulkanFormatToImageColorChannelCount(Format);

		while(ImageIndex >= m_vTextures.size())
		{
			m_vTextures.resize((m_vTextures.size() * 2) + 1);
		}

		// resample if needed
		uint32_t RescaleCount = 0;
		if((size_t)Width > m_MaxTextureSize || (size_t)Height > m_MaxTextureSize)
		{
			do
			{
				Width >>= 1;
				Height >>= 1;
				++RescaleCount;
			} while((size_t)Width > m_MaxTextureSize || (size_t)Height > m_MaxTextureSize);

			void *pTmpData = Resize((const uint8_t *)(pData), Width, Height, Width, Height, ImageColorChannels);
			free(pData);
			pData = pTmpData;
		}

		bool Requires2DTexture = (Flags & CCommandBuffer::TEXFLAG_NO_2D_TEXTURE) == 0;
		bool Requires2DTextureArray = (Flags & (CCommandBuffer::TEXFLAG_TO_2D_ARRAY_TEXTURE | CCommandBuffer::TEXFLAG_TO_2D_ARRAY_TEXTURE_SINGLE_LAYER)) != 0;
		bool Is2DTextureSingleLayer = (Flags & CCommandBuffer::TEXFLAG_TO_2D_ARRAY_TEXTURE_SINGLE_LAYER) != 0;
		bool RequiresMipMaps = (Flags & CCommandBuffer::TEXFLAG_NOMIPMAPS) == 0;
		size_t MipMapLevelCount = 1;
		if(RequiresMipMaps)
		{
			VkExtent3D ImgSize{(uint32_t)Width, (uint32_t)Height, 1};
			MipMapLevelCount = ImageMipLevelCount(ImgSize);
			if(!m_OptimalRGBAImageBlitting)
				MipMapLevelCount = 1;
		}

		CTexture &Texture = m_vTextures[ImageIndex];

		Texture.m_Width = Width;
		Texture.m_Height = Height;
		Texture.m_RescaleCount = RescaleCount;
		Texture.m_MipMapCount = MipMapLevelCount;

		if(Requires2DTexture)
		{
			CreateTextureImage(ImageIndex, Texture.m_Img, Texture.m_ImgMem, pData, Format, Width, Height, 1, PixelSize, MipMapLevelCount);
			VkFormat ImgFormat = Format;
			VkImageView ImgView = CreateTextureImageView(Texture.m_Img, ImgFormat, VK_IMAGE_VIEW_TYPE_2D, 1, MipMapLevelCount);
			Texture.m_ImgView = ImgView;
			VkSampler ImgSampler = GetTextureSampler(SUPPORTED_SAMPLER_TYPE_REPEAT);
			Texture.m_aSamplers[0] = ImgSampler;
			ImgSampler = GetTextureSampler(SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE);
			Texture.m_aSamplers[1] = ImgSampler;

			CreateNewTexturedStandardDescriptorSets(ImageIndex, 0);
			CreateNewTexturedStandardDescriptorSets(ImageIndex, 1);
		}

		if(Requires2DTextureArray)
		{
			int Image3DWidth = Width;
			int Image3DHeight = Height;

			int ConvertWidth = Width;
			int ConvertHeight = Height;

			if(!Is2DTextureSingleLayer)
			{
				if(ConvertWidth == 0 || (ConvertWidth % 16) != 0 || ConvertHeight == 0 || (ConvertHeight % 16) != 0)
				{
					dbg_msg("vulkan", "3D/2D array texture was resized");
					int NewWidth = maximum<int>(HighestBit(ConvertWidth), 16);
					int NewHeight = maximum<int>(HighestBit(ConvertHeight), 16);
					uint8_t *pNewTexData = (uint8_t *)Resize((const uint8_t *)pData, ConvertWidth, ConvertHeight, NewWidth, NewHeight, ImageColorChannels);

					ConvertWidth = NewWidth;
					ConvertHeight = NewHeight;

					free(pData);
					pData = pNewTexData;
				}
			}

			void *p3DTexData = pData;
			bool Needs3DTexDel = false;
			if(!Is2DTextureSingleLayer)
			{
				p3DTexData = malloc((size_t)ImageColorChannels * ConvertWidth * ConvertHeight);
				if(!Texture2DTo3D(pData, ConvertWidth, ConvertHeight, ImageColorChannels, 16, 16, p3DTexData, Image3DWidth, Image3DHeight))
				{
					free(p3DTexData);
					p3DTexData = nullptr;
				}
				Needs3DTexDel = true;
			}
			else
			{
				Image3DWidth = ConvertWidth;
				Image3DHeight = ConvertHeight;
			}

			if(p3DTexData != nullptr)
			{
				const size_t ImageDepth2DArray = Is2DTextureSingleLayer ? 1 : ((size_t)16 * 16);
				VkExtent3D ImgSize{(uint32_t)Image3DWidth, (uint32_t)Image3DHeight, 1};
				if(RequiresMipMaps)
				{
					MipMapLevelCount = ImageMipLevelCount(ImgSize);
					if(!m_OptimalRGBAImageBlitting)
						MipMapLevelCount = 1;
				}

				CreateTextureImage(ImageIndex, Texture.m_Img3D, Texture.m_Img3DMem, p3DTexData, Format, Image3DWidth, Image3DHeight, ImageDepth2DArray, PixelSize, MipMapLevelCount);
				VkFormat ImgFormat = Format;
				VkImageView ImgView = CreateTextureImageView(Texture.m_Img3D, ImgFormat, VK_IMAGE_VIEW_TYPE_2D_ARRAY, ImageDepth2DArray, MipMapLevelCount);
				Texture.m_Img3DView = ImgView;
				VkSampler ImgSampler = GetTextureSampler(SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY);
				Texture.m_Sampler3D = ImgSampler;

				CreateNew3DTexturedStandardDescriptorSets(ImageIndex);

				if(Needs3DTexDel)
					free(p3DTexData);
			}
		}
	}

	VkFormat TextureFormatToVulkanFormat(int TexFormat)
	{
		if(TexFormat == CCommandBuffer::TEXFORMAT_RGBA)
			return VK_FORMAT_R8G8B8A8_UNORM;
		return VK_FORMAT_R8G8B8A8_UNORM;
	}

	void BuildMipmaps(VkImage Image, VkFormat ImageFormat, size_t Width, size_t Height, size_t Depth, size_t MipMapLevelCount)
	{
		VkCommandBuffer MemCommandBuffer = GetMemoryCommandBuffer();

		VkImageMemoryBarrier Barrier{};
		Barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		Barrier.image = Image;
		Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Barrier.subresourceRange.levelCount = 1;
		Barrier.subresourceRange.baseArrayLayer = 0;
		Barrier.subresourceRange.layerCount = Depth;

		int32_t TmpMipWidth = (int32_t)Width;
		int32_t TmpMipHeight = (int32_t)Height;

		for(size_t i = 1; i < MipMapLevelCount; ++i)
		{
			Barrier.subresourceRange.baseMipLevel = i - 1;
			Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			Barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			Barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

			vkCmdPipelineBarrier(MemCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &Barrier);

			VkImageBlit Blit{};
			Blit.srcOffsets[0] = {0, 0, 0};
			Blit.srcOffsets[1] = {TmpMipWidth, TmpMipHeight, 1};
			Blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			Blit.srcSubresource.mipLevel = i - 1;
			Blit.srcSubresource.baseArrayLayer = 0;
			Blit.srcSubresource.layerCount = Depth;
			Blit.dstOffsets[0] = {0, 0, 0};
			Blit.dstOffsets[1] = {TmpMipWidth > 1 ? TmpMipWidth / 2 : 1, TmpMipHeight > 1 ? TmpMipHeight / 2 : 1, 1};
			Blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			Blit.dstSubresource.mipLevel = i;
			Blit.dstSubresource.baseArrayLayer = 0;
			Blit.dstSubresource.layerCount = Depth;

			vkCmdBlitImage(MemCommandBuffer,
				Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &Blit,
				m_AllowsLinearBlitting ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

			Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			Barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			Barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			vkCmdPipelineBarrier(MemCommandBuffer,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
				0, nullptr,
				0, nullptr,
				1, &Barrier);

			if(TmpMipWidth > 1)
				TmpMipWidth /= 2;
			if(TmpMipHeight > 1)
				TmpMipHeight /= 2;
		}

		Barrier.subresourceRange.baseMipLevel = MipMapLevelCount - 1;
		Barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		Barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(MemCommandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
			0, nullptr,
			0, nullptr,
			1, &Barrier);
	}

	bool CreateTextureImage(size_t ImageIndex, VkImage &NewImage, SMemoryImageBlock<s_ImageBufferCacheID> &NewImgMem, const void *pData, VkFormat Format, size_t Width, size_t Height, size_t Depth, size_t PixelSize, size_t MipMapLevelCount)
	{
		int ImageSize = Width * Height * Depth * PixelSize;

		auto StagingBuffer = GetStagingBufferImage(pData, ImageSize);

		VkFormat ImgFormat = Format;

		CreateImage(Width, Height, Depth, MipMapLevelCount, ImgFormat, VK_IMAGE_TILING_OPTIMAL, NewImage, NewImgMem);

		ImageBarrier(NewImage, 0, MipMapLevelCount, 0, Depth, ImgFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(StagingBuffer.m_Buffer, StagingBuffer.m_HeapData.m_OffsetToAlign, NewImage, 0, 0, static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), Depth);
		//ImageBarrier(NewImage, 0, 1, 0, Depth, ImgFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		UploadAndFreeStagingImageMemBlock(StagingBuffer);

		if(MipMapLevelCount > 1)
			BuildMipmaps(NewImage, ImgFormat, Width, Height, Depth, MipMapLevelCount);
		else
			ImageBarrier(NewImage, 0, 1, 0, Depth, ImgFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		return true;
	}

	VkImageView CreateTextureImageView(VkImage TexImage, VkFormat ImgFormat, VkImageViewType ViewType, size_t Depth, size_t MipMapLevelCount)
	{
		return CreateImageView(TexImage, ImgFormat, ViewType, Depth, MipMapLevelCount);
	}

	bool CreateTextureSamplersImpl(VkSampler &CreatedSampler, VkSamplerAddressMode AddrModeU, VkSamplerAddressMode AddrModeV, VkSamplerAddressMode AddrModeW)
	{
		VkSamplerCreateInfo SamplerInfo{};
		SamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		SamplerInfo.magFilter = VK_FILTER_LINEAR;
		SamplerInfo.minFilter = VK_FILTER_LINEAR;
		SamplerInfo.addressModeU = AddrModeU;
		SamplerInfo.addressModeV = AddrModeV;
		SamplerInfo.addressModeW = AddrModeW;
		SamplerInfo.anisotropyEnable = VK_FALSE;
		SamplerInfo.maxAnisotropy = m_MaxSamplerAnisotropy;
		SamplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		SamplerInfo.unnormalizedCoordinates = VK_FALSE;
		SamplerInfo.compareEnable = VK_FALSE;
		SamplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		SamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		SamplerInfo.mipLodBias = (m_GlobalTextureLodBIAS / 1000.0f);
		SamplerInfo.minLod = -1000;
		SamplerInfo.maxLod = 1000;

		if(vkCreateSampler(m_VKDevice, &SamplerInfo, nullptr, &CreatedSampler) != VK_SUCCESS)
		{
			dbg_msg("vulkan", "failed to create texture sampler!");
			return false;
		}
		return true;
	}

	bool CreateTextureSamplers()
	{
		bool Ret = true;
		Ret &= CreateTextureSamplersImpl(m_aSamplers[SUPPORTED_SAMPLER_TYPE_REPEAT], VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT);
		Ret &= CreateTextureSamplersImpl(m_aSamplers[SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE], VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
		Ret &= CreateTextureSamplersImpl(m_aSamplers[SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY], VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
		return Ret;
	}

	void DestroyTextureSamplers()
	{
		vkDestroySampler(m_VKDevice, m_aSamplers[SUPPORTED_SAMPLER_TYPE_REPEAT], nullptr);
		vkDestroySampler(m_VKDevice, m_aSamplers[SUPPORTED_SAMPLER_TYPE_CLAMP_TO_EDGE], nullptr);
		vkDestroySampler(m_VKDevice, m_aSamplers[SUPPORTED_SAMPLER_TYPE_2D_TEXTURE_ARRAY], nullptr);
	}

	VkSampler GetTextureSampler(ESupportedSamplerTypes SamplerType)
	{
		return m_aSamplers[SamplerType];
	}

	VkImageView CreateImageView(VkImage Image, VkFormat Format, VkImageViewType ViewType, size_t Depth, size_t MipMapLevelCount)
	{
		VkImageViewCreateInfo ViewCreateInfo{};
		ViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ViewCreateInfo.image = Image;
		ViewCreateInfo.viewType = ViewType;
		ViewCreateInfo.format = Format;
		ViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ViewCreateInfo.subresourceRange.baseMipLevel = 0;
		ViewCreateInfo.subresourceRange.levelCount = MipMapLevelCount;
		ViewCreateInfo.subresourceRange.baseArrayLayer = 0;
		ViewCreateInfo.subresourceRange.layerCount = Depth;

		VkImageView ImageView;
		if(vkCreateImageView(m_VKDevice, &ViewCreateInfo, nullptr, &ImageView) != VK_SUCCESS)
		{
			return VK_NULL_HANDLE;
		}

		return ImageView;
	}

	void CreateImage(uint32_t Width, uint32_t Height, uint32_t Depth, size_t MipMapLevelCount, VkFormat Format, VkImageTiling Tiling, VkImage &Image, SMemoryImageBlock<s_ImageBufferCacheID> &ImageMemory, VkImageUsageFlags ImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
	{
		VkImageCreateInfo ImageInfo{};
		ImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ImageInfo.imageType = VK_IMAGE_TYPE_2D;
		ImageInfo.extent.width = Width;
		ImageInfo.extent.height = Height;
		ImageInfo.extent.depth = 1;
		ImageInfo.mipLevels = MipMapLevelCount;
		ImageInfo.arrayLayers = Depth;
		ImageInfo.format = Format;
		ImageInfo.tiling = Tiling;
		ImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		ImageInfo.usage = ImageUsage;
		ImageInfo.samples = (ImageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ? VK_SAMPLE_COUNT_1_BIT : GetSampleCount();
		ImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if(vkCreateImage(m_VKDevice, &ImageInfo, nullptr, &Image) != VK_SUCCESS)
		{
			dbg_msg("vulkan", "failed to create image!");
		}

		VkMemoryRequirements MemRequirements;
		vkGetImageMemoryRequirements(m_VKDevice, Image, &MemRequirements);

		auto ImageMem = GetImageMemory(MemRequirements.size, MemRequirements.alignment, MemRequirements.memoryTypeBits);

		ImageMemory = ImageMem;
		vkBindImageMemory(m_VKDevice, Image, ImageMem.m_BufferMem.m_Mem, ImageMem.m_HeapData.m_OffsetToAlign);
	}

	void ImageBarrier(VkImage &Image, size_t MipMapBase, size_t MipMapCount, size_t LayerBase, size_t LayerCount, VkFormat Format, VkImageLayout OldLayout, VkImageLayout NewLayout)
	{
		VkCommandBuffer MemCommandBuffer = GetMemoryCommandBuffer();

		VkImageMemoryBarrier Barrier{};
		Barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		Barrier.oldLayout = OldLayout;
		Barrier.newLayout = NewLayout;
		Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.image = Image;
		Barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Barrier.subresourceRange.baseMipLevel = MipMapBase;
		Barrier.subresourceRange.levelCount = MipMapCount;
		Barrier.subresourceRange.baseArrayLayer = LayerBase;
		Barrier.subresourceRange.layerCount = LayerCount;

		VkPipelineStageFlags SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkPipelineStageFlags DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

		if(OldLayout == VK_IMAGE_LAYOUT_UNDEFINED && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			Barrier.srcAccessMask = 0;
			Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if(OldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			DestinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		}
		else if(OldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			Barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			SourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if(OldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
		{
			Barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			Barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

			SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			DestinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		}
		else if(OldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
		{
			Barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			Barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

			SourceStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if(OldLayout == VK_IMAGE_LAYOUT_UNDEFINED && NewLayout == VK_IMAGE_LAYOUT_GENERAL)
		{
			Barrier.srcAccessMask = 0;
			Barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

			SourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if(OldLayout == VK_IMAGE_LAYOUT_GENERAL && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		{
			Barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			Barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else if(OldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_GENERAL)
		{
			Barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			Barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

			SourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			DestinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
		else
		{
			dbg_msg("vulkan", "unsupported layout transition!");
		}

		vkCmdPipelineBarrier(
			MemCommandBuffer,
			SourceStage, DestinationStage,
			0,
			0, nullptr,
			0, nullptr,
			1, &Barrier);
	}

	void CopyBufferToImage(VkBuffer Buffer, VkDeviceSize BufferOffset, VkImage Image, int32_t X, int32_t Y, uint32_t Width, uint32_t Height, size_t Depth)
	{
		VkCommandBuffer CommandBuffer = GetMemoryCommandBuffer();

		VkBufferImageCopy Region{};
		Region.bufferOffset = BufferOffset;
		Region.bufferRowLength = 0;
		Region.bufferImageHeight = 0;
		Region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Region.imageSubresource.mipLevel = 0;
		Region.imageSubresource.baseArrayLayer = 0;
		Region.imageSubresource.layerCount = Depth;
		Region.imageOffset = {X, Y, 0};
		Region.imageExtent = {
			Width,
			Height,
			1};

		vkCmdCopyBufferToImage(CommandBuffer, Buffer, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);
	}

	/************************
	* BUFFERS
	************************/

	void CreateBufferObject(size_t BufferIndex, const void *pUploadData, VkDeviceSize BufferDataSize, bool IsOneFrameBuffer)
	{
		void *pUploadDataTmp = nullptr;
		if(pUploadData == nullptr)
		{
			pUploadDataTmp = malloc(BufferDataSize);
			pUploadData = pUploadDataTmp;
		}

		while(BufferIndex >= m_vBufferObjects.size())
		{
			m_vBufferObjects.resize((m_vBufferObjects.size() * 2) + 1);
		}
		auto &BufferObject = m_vBufferObjects[BufferIndex];

		VkBuffer VertexBuffer;
		size_t BufferOffset = 0;
		if(!IsOneFrameBuffer)
		{
			auto StagingBuffer = GetStagingBuffer(pUploadData, BufferDataSize);

			auto Mem = GetVertexBuffer(BufferDataSize);

			BufferObject.m_BufferObject.m_Mem = Mem;
			VertexBuffer = Mem.m_Buffer;
			BufferOffset = Mem.m_HeapData.m_OffsetToAlign;

			MemoryBarrier(VertexBuffer, Mem.m_HeapData.m_OffsetToAlign, BufferDataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, true);
			CopyBuffer(StagingBuffer.m_Buffer, VertexBuffer, StagingBuffer.m_HeapData.m_OffsetToAlign, Mem.m_HeapData.m_OffsetToAlign, BufferDataSize);
			MemoryBarrier(VertexBuffer, Mem.m_HeapData.m_OffsetToAlign, BufferDataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, false);
			UploadAndFreeStagingMemBlock(StagingBuffer);
		}
		else
		{
			SDeviceMemoryBlock VertexBufferMemory;
			CreateStreamVertexBuffer(ms_MainThreadIndex, VertexBuffer, VertexBufferMemory, BufferOffset, pUploadData, BufferDataSize);
		}
		BufferObject.m_IsStreamedBuffer = IsOneFrameBuffer;
		BufferObject.m_CurBuffer = VertexBuffer;
		BufferObject.m_CurBufferOffset = BufferOffset;

		if(pUploadDataTmp != nullptr)
			free(pUploadDataTmp);
	}

	void DeleteBufferObject(size_t BufferIndex)
	{
		auto &BufferObject = m_vBufferObjects[BufferIndex];
		if(!BufferObject.m_IsStreamedBuffer)
		{
			FreeVertexMemBlock(BufferObject.m_BufferObject.m_Mem);
		}
		BufferObject = {};
	}

	void CopyBuffer(VkBuffer SrcBuffer, VkBuffer DstBuffer, VkDeviceSize SrcOffset, VkDeviceSize DstOffset, VkDeviceSize CopySize)
	{
		VkCommandBuffer CommandBuffer = GetMemoryCommandBuffer();
		VkBufferCopy CopyRegion{};
		CopyRegion.srcOffset = SrcOffset;
		CopyRegion.dstOffset = DstOffset;
		CopyRegion.size = CopySize;
		vkCmdCopyBuffer(CommandBuffer, SrcBuffer, DstBuffer, 1, &CopyRegion);
	}

	/************************
	* RENDER STATES
	************************/

	void GetStateMatrix(const CCommandBuffer::SState &State, std::array<float, (size_t)4 * 2> &Matrix)
	{
		Matrix = {
			// column 1
			2.f / (State.m_ScreenBR.x - State.m_ScreenTL.x),
			0,
			// column 2
			0,
			2.f / (State.m_ScreenBR.y - State.m_ScreenTL.y),
			// column 3
			0,
			0,
			// column 4
			-((State.m_ScreenTL.x + State.m_ScreenBR.x) / (State.m_ScreenBR.x - State.m_ScreenTL.x)),
			-((State.m_ScreenTL.y + State.m_ScreenBR.y) / (State.m_ScreenBR.y - State.m_ScreenTL.y)),
		};
	}

	bool GetIsTextured(const CCommandBuffer::SState &State)
	{
		return State.m_Texture != -1;
	}

	size_t GetAddressModeIndex(const CCommandBuffer::SState &State)
	{
		return State.m_WrapMode == CCommandBuffer::WRAP_REPEAT ? VULKAN_BACKEND_ADDRESS_MODE_REPEAT : VULKAN_BACKEND_ADDRESS_MODE_CLAMP_EDGES;
	}

	size_t GetBlendModeIndex(const CCommandBuffer::SState &State)
	{
		return State.m_BlendMode == CCommandBuffer::BLEND_ADDITIVE ? VULKAN_BACKEND_BLEND_MODE_ADDITATIVE : (State.m_BlendMode == CCommandBuffer::BLEND_NONE ? VULKAN_BACKEND_BLEND_MODE_NONE : VULKAN_BACKEND_BLEND_MODE_ALPHA);
	}

	size_t GetDynamicModeIndexFromState(const CCommandBuffer::SState &State)
	{
		return (State.m_ClipEnable || m_HasDynamicViewport || m_VKSwapImgAndViewportExtent.m_HasForcedViewport) ? VULKAN_BACKEND_CLIP_MODE_DYNAMIC_SCISSOR_AND_VIEWPORT : VULKAN_BACKEND_CLIP_MODE_NONE;
	}

	size_t GetDynamicModeIndexFromExecBuffer(const SRenderCommandExecuteBuffer &ExecBuffer)
	{
		return (ExecBuffer.m_HasDynamicState) ? VULKAN_BACKEND_CLIP_MODE_DYNAMIC_SCISSOR_AND_VIEWPORT : VULKAN_BACKEND_CLIP_MODE_NONE;
	}

	VkPipeline &GetPipeline(SPipelineContainer &Container, bool IsTextured, size_t BlendModeIndex, size_t DynamicIndex)
	{
		return Container.m_aaaPipelines[BlendModeIndex][DynamicIndex][(size_t)IsTextured];
	}

	VkPipelineLayout &GetPipeLayout(SPipelineContainer &Container, bool IsTextured, size_t BlendModeIndex, size_t DynamicIndex)
	{
		return Container.m_aaaPipelineLayouts[BlendModeIndex][DynamicIndex][(size_t)IsTextured];
	}

	VkPipelineLayout &GetStandardPipeLayout(bool IsLineGeometry, bool IsTextured, size_t BlendModeIndex, size_t DynamicIndex)
	{
		if(IsLineGeometry)
			return GetPipeLayout(m_StandardLinePipeline, IsTextured, BlendModeIndex, DynamicIndex);
		else
			return GetPipeLayout(m_StandardPipeline, IsTextured, BlendModeIndex, DynamicIndex);
	}

	VkPipeline &GetStandardPipe(bool IsLineGeometry, bool IsTextured, size_t BlendModeIndex, size_t DynamicIndex)
	{
		if(IsLineGeometry)
			return GetPipeline(m_StandardLinePipeline, IsTextured, BlendModeIndex, DynamicIndex);
		else
			return GetPipeline(m_StandardPipeline, IsTextured, BlendModeIndex, DynamicIndex);
	}

	VkPipelineLayout &GetTileLayerPipeLayout(int Type, bool IsTextured, size_t BlendModeIndex, size_t DynamicIndex)
	{
		if(Type == 0)
			return GetPipeLayout(m_TilePipeline, IsTextured, BlendModeIndex, DynamicIndex);
		else if(Type == 1)
			return GetPipeLayout(m_TileBorderPipeline, IsTextured, BlendModeIndex, DynamicIndex);
		else
			return GetPipeLayout(m_TileBorderLinePipeline, IsTextured, BlendModeIndex, DynamicIndex);
	}

	VkPipeline &GetTileLayerPipe(int Type, bool IsTextured, size_t BlendModeIndex, size_t DynamicIndex)
	{
		if(Type == 0)
			return GetPipeline(m_TilePipeline, IsTextured, BlendModeIndex, DynamicIndex);
		else if(Type == 1)
			return GetPipeline(m_TileBorderPipeline, IsTextured, BlendModeIndex, DynamicIndex);
		else
			return GetPipeline(m_TileBorderLinePipeline, IsTextured, BlendModeIndex, DynamicIndex);
	}

	void GetStateIndices(const SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, bool &IsTextured, size_t &BlendModeIndex, size_t &DynamicIndex, size_t &AddressModeIndex)
	{
		IsTextured = GetIsTextured(State);
		AddressModeIndex = GetAddressModeIndex(State);
		BlendModeIndex = GetBlendModeIndex(State);
		DynamicIndex = GetDynamicModeIndexFromExecBuffer(ExecBuffer);
	}

	void ExecBufferFillDynamicStates(const CCommandBuffer::SState &State, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		size_t DynamicStateIndex = GetDynamicModeIndexFromState(State);
		if(DynamicStateIndex == VULKAN_BACKEND_CLIP_MODE_DYNAMIC_SCISSOR_AND_VIEWPORT)
		{
			VkViewport Viewport;
			if(m_HasDynamicViewport)
			{
				Viewport.x = (float)m_DynamicViewportOffset.x;
				Viewport.y = (float)m_DynamicViewportOffset.y;
				Viewport.width = (float)m_DynamicViewportSize.width;
				Viewport.height = (float)m_DynamicViewportSize.height;
				Viewport.minDepth = 0.0f;
				Viewport.maxDepth = 1.0f;
			}
			// else check if there is a forced viewport
			else if(m_VKSwapImgAndViewportExtent.m_HasForcedViewport)
			{
				Viewport.x = 0.0f;
				Viewport.y = 0.0f;
				Viewport.width = (float)m_VKSwapImgAndViewportExtent.m_ForcedViewport.width;
				Viewport.height = (float)m_VKSwapImgAndViewportExtent.m_ForcedViewport.height;
				Viewport.minDepth = 0.0f;
				Viewport.maxDepth = 1.0f;
			}
			else
			{
				Viewport.x = 0.0f;
				Viewport.y = 0.0f;
				Viewport.width = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width;
				Viewport.height = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height;
				Viewport.minDepth = 0.0f;
				Viewport.maxDepth = 1.0f;
			}

			VkRect2D Scissor;
			// convert from OGL to vulkan clip

			// the scissor always assumes the presented viewport, because the front-end keeps the calculation
			// for the forced viewport in sync
			auto ScissorViewport = m_VKSwapImgAndViewportExtent.GetPresentedImageViewport();
			if(State.m_ClipEnable)
			{
				int32_t ScissorY = (int32_t)ScissorViewport.height - ((int32_t)State.m_ClipY + (int32_t)State.m_ClipH);
				uint32_t ScissorH = (int32_t)State.m_ClipH;
				Scissor.offset = {(int32_t)State.m_ClipX, ScissorY};
				Scissor.extent = {(uint32_t)State.m_ClipW, ScissorH};
			}
			else
			{
				Scissor.offset = {0, 0};
				Scissor.extent = {(uint32_t)ScissorViewport.width, (uint32_t)ScissorViewport.height};
			}

			// if there is a dynamic viewport make sure the scissor data is scaled down to that
			if(m_HasDynamicViewport)
			{
				Scissor.offset.x = (int32_t)(((float)Scissor.offset.x / (float)ScissorViewport.width) * (float)m_DynamicViewportSize.width) + m_DynamicViewportOffset.x;
				Scissor.offset.y = (int32_t)(((float)Scissor.offset.y / (float)ScissorViewport.height) * (float)m_DynamicViewportSize.height) + m_DynamicViewportOffset.y;
				Scissor.extent.width = (uint32_t)(((float)Scissor.extent.width / (float)ScissorViewport.width) * (float)m_DynamicViewportSize.width);
				Scissor.extent.height = (uint32_t)(((float)Scissor.extent.height / (float)ScissorViewport.height) * (float)m_DynamicViewportSize.height);
			}

			Viewport.x = clamp(Viewport.x, 0.0f, std::numeric_limits<decltype(Viewport.x)>::max());
			Viewport.y = clamp(Viewport.y, 0.0f, std::numeric_limits<decltype(Viewport.y)>::max());

			Scissor.offset.x = clamp(Scissor.offset.x, 0, std::numeric_limits<decltype(Scissor.offset.x)>::max());
			Scissor.offset.y = clamp(Scissor.offset.y, 0, std::numeric_limits<decltype(Scissor.offset.y)>::max());

			ExecBuffer.m_HasDynamicState = true;
			ExecBuffer.m_Viewport = Viewport;
			ExecBuffer.m_Scissor = Scissor;
		}
		else
		{
			ExecBuffer.m_HasDynamicState = false;
		}
	}

	void BindPipeline(size_t RenderThreadIndex, VkCommandBuffer &CommandBuffer, SRenderCommandExecuteBuffer &ExecBuffer, VkPipeline &BindingPipe, const CCommandBuffer::SState &State)
	{
		if(m_vLastPipeline[RenderThreadIndex] != BindingPipe)
		{
			vkCmdBindPipeline(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, BindingPipe);
			m_vLastPipeline[RenderThreadIndex] = BindingPipe;
		}

		size_t DynamicStateIndex = GetDynamicModeIndexFromExecBuffer(ExecBuffer);
		if(DynamicStateIndex == VULKAN_BACKEND_CLIP_MODE_DYNAMIC_SCISSOR_AND_VIEWPORT)
		{
			vkCmdSetViewport(CommandBuffer, 0, 1, &ExecBuffer.m_Viewport);
			vkCmdSetScissor(CommandBuffer, 0, 1, &ExecBuffer.m_Scissor);
		}
	}

	/**************************
	* RENDERING IMPLEMENTATION
	***************************/

	void RenderTileLayer_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, size_t DrawCalls, const CCommandBuffer::SState &State, size_t BufferContainerIndex)
	{
		size_t BufferObjectIndex = (size_t)m_vBufferContainers[BufferContainerIndex].m_BufferObjectIndex;
		auto &BufferObject = m_vBufferObjects[BufferObjectIndex];

		ExecBuffer.m_Buffer = BufferObject.m_CurBuffer;
		ExecBuffer.m_BufferOff = BufferObject.m_CurBufferOffset;

		bool IsTextured = GetIsTextured(State);
		if(IsTextured)
		{
			auto &DescrSet = m_vTextures[State.m_Texture].m_VKStandard3DTexturedDescrSet;
			ExecBuffer.m_aDescriptors[0] = DescrSet;
		}

		ExecBuffer.m_IndexBuffer = m_RenderIndexBuffer;

		ExecBuffer.m_EstimatedRenderCallCount = DrawCalls;

		ExecBufferFillDynamicStates(State, ExecBuffer);
	}

	void RenderTileLayer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, int Type, const GL_SColorf &Color, const vec2 &Dir, const vec2 &Off, int32_t JumpIndex, size_t IndicesDrawNum, char *const *pIndicesOffsets, const unsigned int *pDrawCount, size_t InstanceCount)
	{
		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(State, m);

		bool IsTextured;
		size_t BlendModeIndex;
		size_t DynamicIndex;
		size_t AddressModeIndex;
		GetStateIndices(ExecBuffer, State, IsTextured, BlendModeIndex, DynamicIndex, AddressModeIndex);
		auto &PipeLayout = GetTileLayerPipeLayout(Type, IsTextured, BlendModeIndex, DynamicIndex);
		auto &PipeLine = GetTileLayerPipe(Type, IsTextured, BlendModeIndex, DynamicIndex);

		auto &CommandBuffer = GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex);

		BindPipeline(ExecBuffer.m_ThreadIndex, CommandBuffer, ExecBuffer, PipeLine, State);

		std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		if(IsTextured)
		{
			vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 0, 1, &ExecBuffer.m_aDescriptors[0].m_Descriptor, 0, nullptr);
		}

		SUniformTileGPosBorder VertexPushConstants;
		size_t VertexPushConstantSize = sizeof(SUniformTileGPos);
		SUniformTileGVertColor FragPushConstants;
		size_t FragPushConstantSize = sizeof(SUniformTileGVertColor);

		mem_copy(VertexPushConstants.m_aPos, m.data(), m.size() * sizeof(float));
		FragPushConstants = Color;

		if(Type == 1)
		{
			VertexPushConstants.m_Dir = Dir;
			VertexPushConstants.m_Offset = Off;
			VertexPushConstants.m_JumpIndex = JumpIndex;
			VertexPushConstantSize = sizeof(SUniformTileGPosBorder);
		}
		else if(Type == 2)
		{
			VertexPushConstants.m_Dir = Dir;
			VertexPushConstants.m_Offset = Off;
			VertexPushConstantSize = sizeof(SUniformTileGPosBorderLine);
		}

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, VertexPushConstantSize, &VertexPushConstants);
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformTileGPosBorder) + sizeof(SUniformTileGVertColorAlign), FragPushConstantSize, &FragPushConstants);

		size_t DrawCount = (size_t)IndicesDrawNum;
		vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, 0, VK_INDEX_TYPE_UINT32);
		for(size_t i = 0; i < DrawCount; ++i)
		{
			VkDeviceSize IndexOffset = (VkDeviceSize)((ptrdiff_t)pIndicesOffsets[i] / sizeof(uint32_t));

			vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(pDrawCount[i]), InstanceCount, IndexOffset, 0, 0);
		}
	}

	template<typename TName, bool Is3DTextured>
	void RenderStandard(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, int PrimType, const TName *pVertices, int PrimitiveCount)
	{
		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(State, m);

		bool IsLineGeometry = PrimType == CCommandBuffer::PRIMTYPE_LINES;

		bool IsTextured;
		size_t BlendModeIndex;
		size_t DynamicIndex;
		size_t AddressModeIndex;
		GetStateIndices(ExecBuffer, State, IsTextured, BlendModeIndex, DynamicIndex, AddressModeIndex);
		auto &PipeLayout = Is3DTextured ? GetPipeLayout(m_Standard3DPipeline, IsTextured, BlendModeIndex, DynamicIndex) : GetStandardPipeLayout(IsLineGeometry, IsTextured, BlendModeIndex, DynamicIndex);
		auto &PipeLine = Is3DTextured ? GetPipeline(m_Standard3DPipeline, IsTextured, BlendModeIndex, DynamicIndex) : GetStandardPipe(IsLineGeometry, IsTextured, BlendModeIndex, DynamicIndex);

		auto &CommandBuffer = GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex);

		BindPipeline(ExecBuffer.m_ThreadIndex, CommandBuffer, ExecBuffer, PipeLine, State);

		size_t VertPerPrim = 2;
		bool IsIndexed = false;
		if(PrimType == CCommandBuffer::PRIMTYPE_QUADS)
		{
			VertPerPrim = 4;
			IsIndexed = true;
		}
		else if(PrimType == CCommandBuffer::PRIMTYPE_TRIANGLES)
		{
			VertPerPrim = 3;
		}

		VkBuffer VKBuffer;
		SDeviceMemoryBlock VKBufferMem;
		size_t BufferOff = 0;
		CreateStreamVertexBuffer(ExecBuffer.m_ThreadIndex, VKBuffer, VKBufferMem, BufferOff, pVertices, VertPerPrim * sizeof(TName) * PrimitiveCount);

		std::array<VkBuffer, 1> aVertexBuffers = {VKBuffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		if(IsIndexed)
			vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, 0, VK_INDEX_TYPE_UINT32);

		if(IsTextured)
		{
			vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 0, 1, &ExecBuffer.m_aDescriptors[0].m_Descriptor, 0, nullptr);
		}

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos), m.data());

		if(IsIndexed)
			vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(PrimitiveCount * 6), 1, 0, 0, 0);
		else
			vkCmdDraw(CommandBuffer, static_cast<uint32_t>(PrimitiveCount * VertPerPrim), 1, 0, 0);
	}

public:
	CCommandProcessorFragment_Vulkan()
	{
		m_vTextures.reserve(CCommandBuffer::MAX_TEXTURES);
	}

	/************************
	* VULKAN SETUP CODE
	************************/

	bool GetVulkanExtensions(SDL_Window *pWindow, std::vector<std::string> &vVKExtensions)
	{
		unsigned int ExtCount = 0;
		if(!SDL_Vulkan_GetInstanceExtensions(pWindow, &ExtCount, nullptr))
		{
			SetError("Could not get instance extensions from SDL.");
			return false;
		}

		std::vector<const char *> vExtensionList(ExtCount);
		if(!SDL_Vulkan_GetInstanceExtensions(pWindow, &ExtCount, vExtensionList.data()))
		{
			SetError("Could not get instance extensions from SDL.");
			return false;
		}

		for(uint32_t i = 0; i < ExtCount; i++)
		{
			vVKExtensions.emplace_back(vExtensionList[i]);
		}

		return true;
	}

	std::set<std::string> OurVKLayers()
	{
		std::set<std::string> OurLayers;

		if(g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL)
		{
			OurLayers.emplace("VK_LAYER_KHRONOS_validation");
			// deprecated, but VK_LAYER_KHRONOS_validation was released after vulkan 1.1
			OurLayers.emplace("VK_LAYER_LUNARG_standard_validation");
		}

		return OurLayers;
	}

	std::set<std::string> OurDeviceExtensions()
	{
		std::set<std::string> OurExt;
		OurExt.emplace(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		return OurExt;
	}

	std::vector<VkImageUsageFlags> OurImageUsages()
	{
		std::vector<VkImageUsageFlags> vImgUsages;

		vImgUsages.emplace_back(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		vImgUsages.emplace_back(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

		return vImgUsages;
	}

	bool GetVulkanLayers(std::vector<std::string> &vVKLayers)
	{
		uint32_t LayerCount = 0;
		VkResult Res = vkEnumerateInstanceLayerProperties(&LayerCount, NULL);
		if(Res != VK_SUCCESS)
		{
			SetError("Could not get vulkan layers.");
			return false;
		}

		std::vector<VkLayerProperties> vVKInstanceLayers(LayerCount);
		Res = vkEnumerateInstanceLayerProperties(&LayerCount, vVKInstanceLayers.data());
		if(Res != VK_SUCCESS)
		{
			SetError("Could not get vulkan layers.");
			return false;
		}

		std::set<std::string> ReqLayerNames = OurVKLayers();
		vVKLayers.clear();
		for(const auto &LayerName : vVKInstanceLayers)
		{
			auto it = ReqLayerNames.find(std::string(LayerName.layerName));
			if(it != ReqLayerNames.end())
			{
				vVKLayers.emplace_back(LayerName.layerName);
			}
		}

		return true;
	}

	bool CreateVulkanInstance(const std::vector<std::string> &vVKLayers, const std::vector<std::string> &vVKExtensions, bool TryDebugExtensions)
	{
		std::vector<const char *> vLayersCStr;
		vLayersCStr.reserve(vVKLayers.size());
		for(const auto &Layer : vVKLayers)
			vLayersCStr.emplace_back(Layer.c_str());

		std::vector<const char *> vExtCStr;
		vExtCStr.reserve(vVKExtensions.size() + 1);
		for(const auto &Ext : vVKExtensions)
			vExtCStr.emplace_back(Ext.c_str());

#ifdef VK_EXT_debug_utils
		if(TryDebugExtensions && (g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL))
		{
			// debug message support
			vExtCStr.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
#endif

		VkApplicationInfo VKAppInfo = {};
		VKAppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		VKAppInfo.pNext = NULL;
		VKAppInfo.pApplicationName = "DDNet";
		VKAppInfo.applicationVersion = 1;
		VKAppInfo.pEngineName = "DDNet-Vulkan";
		VKAppInfo.engineVersion = 1;
		VKAppInfo.apiVersion = VK_API_VERSION_1_0;

		void *pExt = nullptr;
#if defined(VK_EXT_validation_features) && VK_EXT_VALIDATION_FEATURES_SPEC_VERSION >= 5
		VkValidationFeaturesEXT Features = {};
		std::array<VkValidationFeatureEnableEXT, 2> aEnables = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT, VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
		if(TryDebugExtensions && (g_Config.m_DbgGfx == DEBUG_GFX_MODE_AFFECTS_PERFORMANCE || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL))
		{
			Features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
			Features.enabledValidationFeatureCount = aEnables.size();
			Features.pEnabledValidationFeatures = aEnables.data();

			pExt = &Features;
		}
#endif

		VkInstanceCreateInfo VKInstanceInfo = {};
		VKInstanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		VKInstanceInfo.pNext = pExt;
		VKInstanceInfo.flags = 0;
		VKInstanceInfo.pApplicationInfo = &VKAppInfo;
		VKInstanceInfo.enabledExtensionCount = static_cast<uint32_t>(vExtCStr.size());
		VKInstanceInfo.ppEnabledExtensionNames = vExtCStr.data();
		VKInstanceInfo.enabledLayerCount = static_cast<uint32_t>(vLayersCStr.size());
		VKInstanceInfo.ppEnabledLayerNames = vLayersCStr.data();

		bool TryAgain = false;

		VkResult Res = vkCreateInstance(&VKInstanceInfo, NULL, &m_VKInstance);
		const char *pCritErrorMsg = CheckVulkanCriticalError(Res);
		if(pCritErrorMsg != nullptr)
		{
			SetError("Creating instance failed.", pCritErrorMsg);
			return false;
		}
		else if(Res == VK_ERROR_LAYER_NOT_PRESENT || Res == VK_ERROR_EXTENSION_NOT_PRESENT)
			TryAgain = true;

		if(TryAgain && TryDebugExtensions)
			return CreateVulkanInstance(vVKLayers, vVKExtensions, false);

		return true;
	}

	STWGraphicGPU::ETWGraphicsGPUType VKGPUTypeToGraphicsGPUType(VkPhysicalDeviceType VKGPUType)
	{
		if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			return STWGraphicGPU::ETWGraphicsGPUType::GRAPHICS_GPU_TYPE_DISCRETE;
		else if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
			return STWGraphicGPU::ETWGraphicsGPUType::GRAPHICS_GPU_TYPE_INTEGRATED;
		else if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
			return STWGraphicGPU::ETWGraphicsGPUType::GRAPHICS_GPU_TYPE_VIRTUAL;
		else if(VKGPUType == VkPhysicalDeviceType::VK_PHYSICAL_DEVICE_TYPE_CPU)
			return STWGraphicGPU::ETWGraphicsGPUType::GRAPHICS_GPU_TYPE_CPU;

		return STWGraphicGPU::ETWGraphicsGPUType::GRAPHICS_GPU_TYPE_CPU;
	}

	// from: https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5c3986798afc39d736b825bf8a5fbf92b8d9ed49/includes/functions.php#L364
	const char *GetDriverVerson(char (&aBuff)[256], uint32_t DriverVersion, uint32_t VendorID)
	{
		// NVIDIA
		if(VendorID == 4318)
		{
			str_format(aBuff, std::size(aBuff), "%d.%d.%d.%d",
				(DriverVersion >> 22) & 0x3ff,
				(DriverVersion >> 14) & 0x0ff,
				(DriverVersion >> 6) & 0x0ff,
				(DriverVersion)&0x003f);
		}
#ifdef CONF_FAMILY_WINDOWS
		// windows only
		else if(VendorID == 0x8086)
		{
			str_format(aBuff, std::size(aBuff),
				"%d.%d",
				(DriverVersion >> 14),
				(DriverVersion)&0x3fff);
		}
#endif
		else
		{
			// Use Vulkan version conventions if vendor mapping is not available
			str_format(aBuff, std::size(aBuff),
				"%d.%d.%d",
				(DriverVersion >> 22),
				(DriverVersion >> 12) & 0x3ff,
				DriverVersion & 0xfff);
		}

		return aBuff;
	}

	bool SelectGPU(char *pRendererName, char *pVendorName, char *pVersionName)
	{
		uint32_t DevicesCount = 0;
		vkEnumeratePhysicalDevices(m_VKInstance, &DevicesCount, nullptr);
		if(DevicesCount == 0)
		{
			SetError("No vulkan compatible devices found.");
			return false;
		}

		std::vector<VkPhysicalDevice> vDeviceList(DevicesCount);
		vkEnumeratePhysicalDevices(m_VKInstance, &DevicesCount, vDeviceList.data());

		size_t Index = 0;
		std::vector<VkPhysicalDeviceProperties> vDevicePropList(vDeviceList.size());
		m_pGPUList->m_vGPUs.reserve(vDeviceList.size());

		size_t FoundDeviceIndex = 0;
		size_t FoundGPUType = STWGraphicGPU::ETWGraphicsGPUType::GRAPHICS_GPU_TYPE_INVALID;

		STWGraphicGPU::ETWGraphicsGPUType AutoGPUType = STWGraphicGPU::ETWGraphicsGPUType::GRAPHICS_GPU_TYPE_INVALID;

		bool IsAutoGPU = str_comp(g_Config.m_GfxGPUName, "auto") == 0;

		for(auto &CurDevice : vDeviceList)
		{
			vkGetPhysicalDeviceProperties(CurDevice, &(vDevicePropList[Index]));

			auto &DeviceProp = vDevicePropList[Index];

			STWGraphicGPU::ETWGraphicsGPUType GPUType = VKGPUTypeToGraphicsGPUType(DeviceProp.deviceType);

			STWGraphicGPU::STWGraphicGPUItem NewGPU;
			str_copy(NewGPU.m_aName, DeviceProp.deviceName);
			NewGPU.m_GPUType = GPUType;
			m_pGPUList->m_vGPUs.push_back(NewGPU);

			Index++;

			int DevAPIMajor = (int)VK_API_VERSION_MAJOR(DeviceProp.apiVersion);
			int DevAPIMinor = (int)VK_API_VERSION_MINOR(DeviceProp.apiVersion);

			if(GPUType < AutoGPUType && (DevAPIMajor > gs_BackendVulkanMajor || (DevAPIMajor == gs_BackendVulkanMajor && DevAPIMinor >= gs_BackendVulkanMinor)))
			{
				str_copy(m_pGPUList->m_AutoGPU.m_aName, DeviceProp.deviceName);
				m_pGPUList->m_AutoGPU.m_GPUType = GPUType;

				AutoGPUType = GPUType;
			}

			if(((IsAutoGPU && GPUType < FoundGPUType) || str_comp(DeviceProp.deviceName, g_Config.m_GfxGPUName) == 0) && (DevAPIMajor > gs_BackendVulkanMajor || (DevAPIMajor == gs_BackendVulkanMajor && DevAPIMinor >= gs_BackendVulkanMinor)))
			{
				FoundDeviceIndex = Index;
				FoundGPUType = GPUType;
			}
		}

		if(FoundDeviceIndex == 0)
			FoundDeviceIndex = 1;

		{
			auto &DeviceProp = vDevicePropList[FoundDeviceIndex - 1];

			int DevAPIMajor = (int)VK_API_VERSION_MAJOR(DeviceProp.apiVersion);
			int DevAPIMinor = (int)VK_API_VERSION_MINOR(DeviceProp.apiVersion);
			int DevAPIPatch = (int)VK_API_VERSION_PATCH(DeviceProp.apiVersion);

			str_copy(pRendererName, DeviceProp.deviceName, gs_GPUInfoStringSize);
			const char *pVendorNameStr = NULL;
			switch(DeviceProp.vendorID)
			{
			case 0x1002:
				pVendorNameStr = "AMD";
				break;
			case 0x1010:
				pVendorNameStr = "ImgTec";
				break;
			case 0x106B:
				pVendorNameStr = "Apple";
				break;
			case 0x10DE:
				pVendorNameStr = "NVIDIA";
				break;
			case 0x13B5:
				pVendorNameStr = "ARM";
				break;
			case 0x5143:
				pVendorNameStr = "Qualcomm";
				break;
			case 0x8086:
				pVendorNameStr = "INTEL";
				break;
			case 0x10005:
				pVendorNameStr = "Mesa";
				break;
			default:
				dbg_msg("vulkan", "unknown gpu vendor %u", DeviceProp.vendorID);
				pVendorNameStr = "unknown";
				break;
			}

			char aBuff[256];
			str_copy(pVendorName, pVendorNameStr, gs_GPUInfoStringSize);
			str_format(pVersionName, gs_GPUInfoStringSize, "Vulkan %d.%d.%d (driver: %s)", DevAPIMajor, DevAPIMinor, DevAPIPatch, GetDriverVerson(aBuff, DeviceProp.driverVersion, DeviceProp.vendorID));

			// get important device limits
			m_NonCoherentMemAlignment = DeviceProp.limits.nonCoherentAtomSize;
			m_OptimalImageCopyMemAlignment = DeviceProp.limits.optimalBufferCopyOffsetAlignment;
			m_MaxTextureSize = DeviceProp.limits.maxImageDimension2D;
			m_MaxSamplerAnisotropy = DeviceProp.limits.maxSamplerAnisotropy;

			m_MinUniformAlign = DeviceProp.limits.minUniformBufferOffsetAlignment;
			m_MaxMultiSample = DeviceProp.limits.framebufferColorSampleCounts;

			if(IsVerbose())
			{
				dbg_msg("vulkan", "device prop: non-coherent align: %" PRIu64 ", optimal image copy align: %" PRIu64 ", max texture size: %u, max sampler anisotropy: %u", (size_t)m_NonCoherentMemAlignment, (size_t)m_OptimalImageCopyMemAlignment, m_MaxTextureSize, m_MaxSamplerAnisotropy);
				dbg_msg("vulkan", "device prop: min uniform align: %u, multi sample: %u", m_MinUniformAlign, (uint32_t)m_MaxMultiSample);
			}
		}

		VkPhysicalDevice CurDevice = vDeviceList[FoundDeviceIndex - 1];

		uint32_t FamQueueCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(CurDevice, &FamQueueCount, nullptr);
		if(FamQueueCount == 0)
		{
			SetError("No vulkan queue family properties found.");
			return false;
		}

		std::vector<VkQueueFamilyProperties> vQueuePropList(FamQueueCount);
		vkGetPhysicalDeviceQueueFamilyProperties(CurDevice, &FamQueueCount, vQueuePropList.data());

		uint32_t QueueNodeIndex = std::numeric_limits<uint32_t>::max();
		for(uint32_t i = 0; i < FamQueueCount; i++)
		{
			if(vQueuePropList[i].queueCount > 0 && (vQueuePropList[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			{
				QueueNodeIndex = i;
			}
			/*if(vQueuePropList[i].queueCount > 0 && (vQueuePropList[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
			{
				QueueNodeIndex = i;
			}*/
		}

		if(QueueNodeIndex == std::numeric_limits<uint32_t>::max())
		{
			SetError("No vulkan queue found that matches the requirements: graphics queue");
			return false;
		}

		m_VKGPU = CurDevice;
		m_VKGraphicsQueueIndex = QueueNodeIndex;
		return true;
	}

	bool CreateLogicalDevice(const std::vector<std::string> &vVKLayers)
	{
		std::vector<const char *> vLayerCNames;
		vLayerCNames.reserve(vVKLayers.size());
		for(const auto &Layer : vVKLayers)
			vLayerCNames.emplace_back(Layer.c_str());

		uint32_t DevPropCount = 0;
		if(vkEnumerateDeviceExtensionProperties(m_VKGPU, NULL, &DevPropCount, NULL) != VK_SUCCESS)
		{
			SetError("Querying logical device extension propterties failed.");
			return false;
		}

		std::vector<VkExtensionProperties> vDevPropList(DevPropCount);
		if(vkEnumerateDeviceExtensionProperties(m_VKGPU, NULL, &DevPropCount, vDevPropList.data()) != VK_SUCCESS)
		{
			SetError("Querying logical device extension propterties failed.");
			return false;
		}

		std::vector<const char *> vDevPropCNames;
		std::set<std::string> OurDevExt = OurDeviceExtensions();

		for(const auto &CurExtProp : vDevPropList)
		{
			auto it = OurDevExt.find(std::string(CurExtProp.extensionName));
			if(it != OurDevExt.end())
			{
				vDevPropCNames.emplace_back(CurExtProp.extensionName);
			}
		}

		VkDeviceQueueCreateInfo VKQueueCreateInfo;
		VKQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		VKQueueCreateInfo.queueFamilyIndex = m_VKGraphicsQueueIndex;
		VKQueueCreateInfo.queueCount = 1;
		float QueuePrio = 1.0f;
		VKQueueCreateInfo.pQueuePriorities = &QueuePrio;
		VKQueueCreateInfo.pNext = NULL;
		VKQueueCreateInfo.flags = 0;

		VkDeviceCreateInfo VKCreateInfo;
		VKCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		VKCreateInfo.queueCreateInfoCount = 1;
		VKCreateInfo.pQueueCreateInfos = &VKQueueCreateInfo;
		VKCreateInfo.ppEnabledLayerNames = vLayerCNames.data();
		VKCreateInfo.enabledLayerCount = static_cast<uint32_t>(vLayerCNames.size());
		VKCreateInfo.ppEnabledExtensionNames = vDevPropCNames.data();
		VKCreateInfo.enabledExtensionCount = static_cast<uint32_t>(vDevPropCNames.size());
		VKCreateInfo.pNext = NULL;
		VKCreateInfo.pEnabledFeatures = NULL;
		VKCreateInfo.flags = 0;

		VkResult res = vkCreateDevice(m_VKGPU, &VKCreateInfo, nullptr, &m_VKDevice);
		if(res != VK_SUCCESS)
		{
			SetError("Logical device could not be created.");
			return false;
		}

		return true;
	}

	bool CreateSurface(SDL_Window *pWindow)
	{
		if(!SDL_Vulkan_CreateSurface(pWindow, m_VKInstance, &m_VKPresentSurface))
		{
			dbg_msg("vulkan", "error from sdl: %s", SDL_GetError());
			SetError("Creating a vulkan surface for the SDL window failed.");
			return false;
		}

		VkBool32 IsSupported = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(m_VKGPU, m_VKGraphicsQueueIndex, m_VKPresentSurface, &IsSupported);
		if(!IsSupported)
		{
			SetError("The device surface does not support presenting the framebuffer to a screen. (maybe the wrong GPU was selected?)");
			return false;
		}

		return true;
	}

	void DestroySurface()
	{
		vkDestroySurfaceKHR(m_VKInstance, m_VKPresentSurface, nullptr);
	}

	bool GetPresentationMode(VkPresentModeKHR &VKIOMode)
	{
		uint32_t PresentModeCount = 0;
		if(vkGetPhysicalDeviceSurfacePresentModesKHR(m_VKGPU, m_VKPresentSurface, &PresentModeCount, NULL) != VK_SUCCESS)
		{
			SetError("The device surface presentation modes could not be fetched.");
			return false;
		}

		std::vector<VkPresentModeKHR> vPresentModeList(PresentModeCount);
		if(vkGetPhysicalDeviceSurfacePresentModesKHR(m_VKGPU, m_VKPresentSurface, &PresentModeCount, vPresentModeList.data()) != VK_SUCCESS)
		{
			SetError("The device surface presentation modes could not be fetched.");
			return false;
		}

		VKIOMode = g_Config.m_GfxVsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
		for(auto &Mode : vPresentModeList)
		{
			if(Mode == VKIOMode)
				return true;
		}

		dbg_msg("vulkan", "warning: requested presentation mode was not available. falling back to mailbox / fifo relaxed.");
		VKIOMode = g_Config.m_GfxVsync ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
		for(auto &Mode : vPresentModeList)
		{
			if(Mode == VKIOMode)
				return true;
		}

		dbg_msg("vulkan", "warning: requested presentation mode was not available. using first available.");
		if(PresentModeCount > 0)
			VKIOMode = vPresentModeList[0];

		return true;
	}

	bool GetSurfaceProperties(VkSurfaceCapabilitiesKHR &VKSurfCapabilities)
	{
		if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_VKGPU, m_VKPresentSurface, &VKSurfCapabilities) != VK_SUCCESS)
		{
			SetError("The device surface capabilities could not be fetched.");
			return false;
		}
		return true;
	}

	uint32_t GetNumberOfSwapImages(const VkSurfaceCapabilitiesKHR &VKCapabilities)
	{
		uint32_t ImgNumber = VKCapabilities.minImageCount + 1;
		if(IsVerbose())
		{
			dbg_msg("vulkan", "minimal swap image count %u", VKCapabilities.minImageCount);
		}
		return (VKCapabilities.maxImageCount > 0 && ImgNumber > VKCapabilities.maxImageCount) ? VKCapabilities.maxImageCount : ImgNumber;
	}

	SSwapImgViewportExtent GetSwapImageSize(const VkSurfaceCapabilitiesKHR &VKCapabilities)
	{
		VkExtent2D RetSize = {m_CanvasWidth, m_CanvasHeight};

		if(VKCapabilities.currentExtent.width == std::numeric_limits<uint32_t>::max())
		{
			RetSize.width = clamp<uint32_t>(RetSize.width, VKCapabilities.minImageExtent.width, VKCapabilities.maxImageExtent.width);
			RetSize.height = clamp<uint32_t>(RetSize.height, VKCapabilities.minImageExtent.height, VKCapabilities.maxImageExtent.height);
		}
		else
		{
			RetSize = VKCapabilities.currentExtent;
		}

		VkExtent2D AutoViewportExtent = RetSize;
		bool UsesForcedViewport = false;
		// keep this in sync with graphics_threaded AdjustViewport's check
		if(AutoViewportExtent.height > 4 * AutoViewportExtent.width / 5)
		{
			AutoViewportExtent.height = 4 * AutoViewportExtent.width / 5;
			UsesForcedViewport = true;
		}

		SSwapImgViewportExtent Ext;
		Ext.m_SwapImageViewport = RetSize;
		Ext.m_ForcedViewport = AutoViewportExtent;
		Ext.m_HasForcedViewport = UsesForcedViewport;

		return Ext;
	}

	bool GetImageUsage(const VkSurfaceCapabilitiesKHR &VKCapabilities, VkImageUsageFlags &VKOutUsage)
	{
		std::vector<VkImageUsageFlags> vOurImgUsages = OurImageUsages();
		if(vOurImgUsages.empty())
		{
			SetError("Framebuffer image attachment types not supported.");
			return false;
		}

		VKOutUsage = vOurImgUsages[0];

		for(const auto &ImgUsage : vOurImgUsages)
		{
			VkImageUsageFlags ImgUsageFlags = ImgUsage & VKCapabilities.supportedUsageFlags;
			if(ImgUsageFlags != ImgUsage)
			{
				SetError("Framebuffer image attachment types not supported.");
				return false;
			}

			VKOutUsage = (VKOutUsage | ImgUsage);
		}

		return true;
	}

	VkSurfaceTransformFlagBitsKHR GetTransform(const VkSurfaceCapabilitiesKHR &VKCapabilities)
	{
		if(VKCapabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
			return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		return VKCapabilities.currentTransform;
	}

	bool GetFormat()
	{
		uint32_t SurfFormats = 0;
		VkResult Res = vkGetPhysicalDeviceSurfaceFormatsKHR(m_VKGPU, m_VKPresentSurface, &SurfFormats, nullptr);
		if(Res != VK_SUCCESS && Res != VK_INCOMPLETE)
		{
			SetError("The device surface format fetching failed.");
			return false;
		}

		std::vector<VkSurfaceFormatKHR> vSurfFormatList(SurfFormats);
		Res = vkGetPhysicalDeviceSurfaceFormatsKHR(m_VKGPU, m_VKPresentSurface, &SurfFormats, vSurfFormatList.data());
		if(Res != VK_SUCCESS && Res != VK_INCOMPLETE)
		{
			SetError("The device surface format fetching failed.");
			return false;
		}

		if(Res == VK_INCOMPLETE)
		{
			dbg_msg("vulkan", "warning: not all surface formats are requestable with your current settings.");
		}

		if(vSurfFormatList.size() == 1 && vSurfFormatList[0].format == VK_FORMAT_UNDEFINED)
		{
			m_VKSurfFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
			m_VKSurfFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			dbg_msg("vulkan", "warning: surface format was undefined. This can potentially cause bugs.");
			return true;
		}

		for(const auto &FindFormat : vSurfFormatList)
		{
			if(FindFormat.format == VK_FORMAT_B8G8R8A8_UNORM && FindFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				m_VKSurfFormat = FindFormat;
				return true;
			}
			else if(FindFormat.format == VK_FORMAT_R8G8B8A8_UNORM && FindFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				m_VKSurfFormat = FindFormat;
				return true;
			}
		}

		dbg_msg("vulkan", "warning: surface format was not RGBA(or variants of it). This can potentially cause weird looking images(too bright etc.).");
		m_VKSurfFormat = vSurfFormatList[0];
		return true;
	}

	bool CreateSwapChain(VkSwapchainKHR &OldSwapChain)
	{
		VkSurfaceCapabilitiesKHR VKSurfCap;
		if(!GetSurfaceProperties(VKSurfCap))
			return false;

		VkPresentModeKHR PresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
		if(!GetPresentationMode(PresentMode))
			return false;

		uint32_t SwapImgCount = GetNumberOfSwapImages(VKSurfCap);

		m_VKSwapImgAndViewportExtent = GetSwapImageSize(VKSurfCap);

		VkImageUsageFlags UsageFlags;
		if(!GetImageUsage(VKSurfCap, UsageFlags))
			return false;

		VkSurfaceTransformFlagBitsKHR TransformFlagBits = GetTransform(VKSurfCap);

		if(!GetFormat())
			return false;

		OldSwapChain = m_VKSwapChain;

		VkSwapchainCreateInfoKHR SwapInfo;
		SwapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		SwapInfo.pNext = nullptr;
		SwapInfo.flags = 0;
		SwapInfo.surface = m_VKPresentSurface;
		SwapInfo.minImageCount = SwapImgCount;
		SwapInfo.imageFormat = m_VKSurfFormat.format;
		SwapInfo.imageColorSpace = m_VKSurfFormat.colorSpace;
		SwapInfo.imageExtent = m_VKSwapImgAndViewportExtent.m_SwapImageViewport;
		SwapInfo.imageArrayLayers = 1;
		SwapInfo.imageUsage = UsageFlags;
		SwapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		SwapInfo.queueFamilyIndexCount = 0;
		SwapInfo.pQueueFamilyIndices = nullptr;
		SwapInfo.preTransform = TransformFlagBits;
		SwapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		SwapInfo.presentMode = PresentMode;
		SwapInfo.clipped = true;
		SwapInfo.oldSwapchain = OldSwapChain;

		m_VKSwapChain = VK_NULL_HANDLE;
		VkResult SwapchainCreateRes = vkCreateSwapchainKHR(m_VKDevice, &SwapInfo, nullptr, &m_VKSwapChain);
		const char *pCritErrorMsg = CheckVulkanCriticalError(SwapchainCreateRes);
		if(pCritErrorMsg != nullptr)
		{
			SetError("Creating the swap chain failed.", pCritErrorMsg);
			return false;
		}
		else if(SwapchainCreateRes == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
			return false;

		return true;
	}

	void DestroySwapChain(bool ForceDestroy)
	{
		if(ForceDestroy)
		{
			vkDestroySwapchainKHR(m_VKDevice, m_VKSwapChain, nullptr);
			m_VKSwapChain = VK_NULL_HANDLE;
		}
	}

	bool GetSwapChainImageHandles()
	{
		uint32_t ImgCount = 0;
		VkResult res = vkGetSwapchainImagesKHR(m_VKDevice, m_VKSwapChain, &ImgCount, nullptr);
		if(res != VK_SUCCESS)
		{
			SetError("Could not get swap chain images.");
			return false;
		}

		m_SwapChainImageCount = ImgCount;

		m_vSwapChainImages.resize(ImgCount);
		if(vkGetSwapchainImagesKHR(m_VKDevice, m_VKSwapChain, &ImgCount, m_vSwapChainImages.data()) != VK_SUCCESS)
		{
			SetError("Could not get swap chain images.");
			return false;
		}

		return true;
	}

	void ClearSwapChainImageHandles()
	{
		m_vSwapChainImages.clear();
	}

	void GetDeviceQueue()
	{
		vkGetDeviceQueue(m_VKDevice, m_VKGraphicsQueueIndex, 0, &m_VKGraphicsQueue);
		vkGetDeviceQueue(m_VKDevice, m_VKGraphicsQueueIndex, 0, &m_VKPresentQueue);
	}

#ifdef VK_EXT_debug_utils
	static VKAPI_ATTR VkBool32 VKAPI_CALL VKDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT MessageSeverity, VkDebugUtilsMessageTypeFlagsEXT MessageType, const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
	{
		if((MessageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
		{
			dbg_msg("vulkan_debug", "validation error: %s", pCallbackData->pMessage);
		}
		else
		{
			dbg_msg("vulkan_debug", "%s", pCallbackData->pMessage);
		}

		return VK_FALSE;
	}

	VkResult CreateDebugUtilsMessengerEXT(const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDebugUtilsMessengerEXT *pDebugMessenger)
	{
		auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_VKInstance, "vkCreateDebugUtilsMessengerEXT");
		if(func != nullptr)
		{
			return func(m_VKInstance, pCreateInfo, pAllocator, pDebugMessenger);
		}
		else
		{
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}
	}

	void DestroyDebugUtilsMessengerEXT(VkDebugUtilsMessengerEXT &DebugMessenger)
	{
		auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_VKInstance, "vkDestroyDebugUtilsMessengerEXT");
		if(func != nullptr)
		{
			func(m_VKInstance, DebugMessenger, nullptr);
		}
	}
#endif

	void SetupDebugCallback()
	{
#ifdef VK_EXT_debug_utils
		VkDebugUtilsMessengerCreateInfoEXT CreateInfo = {};
		CreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		CreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		CreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT; // | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT <- too annoying
		CreateInfo.pfnUserCallback = VKDebugCallback;

		if(CreateDebugUtilsMessengerEXT(&CreateInfo, nullptr, &m_DebugMessenger) != VK_SUCCESS)
		{
			m_DebugMessenger = VK_NULL_HANDLE;
			dbg_msg("vulkan", "didn't find vulkan debug layer.");
		}
		else
		{
			dbg_msg("vulkan", "enabled vulkan debug context.");
		}
#endif
	}

	void UnregisterDebugCallback()
	{
#ifdef VK_EXT_debug_utils
		if(m_DebugMessenger != VK_NULL_HANDLE)
			DestroyDebugUtilsMessengerEXT(m_DebugMessenger);
#endif
	}

	bool CreateImageViews()
	{
		m_vSwapChainImageViewList.resize(m_SwapChainImageCount);

		for(size_t i = 0; i < m_SwapChainImageCount; i++)
		{
			VkImageViewCreateInfo CreateInfo{};
			CreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			CreateInfo.image = m_vSwapChainImages[i];
			CreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			CreateInfo.format = m_VKSurfFormat.format;
			CreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			CreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			CreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			CreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			CreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			CreateInfo.subresourceRange.baseMipLevel = 0;
			CreateInfo.subresourceRange.levelCount = 1;
			CreateInfo.subresourceRange.baseArrayLayer = 0;
			CreateInfo.subresourceRange.layerCount = 1;

			if(vkCreateImageView(m_VKDevice, &CreateInfo, nullptr, &m_vSwapChainImageViewList[i]) != VK_SUCCESS)
			{
				SetError("Could not create image views for the swap chain framebuffers.");
				return false;
			}
		}

		return true;
	}

	void DestroyImageViews()
	{
		for(auto &ImageView : m_vSwapChainImageViewList)
		{
			vkDestroyImageView(m_VKDevice, ImageView, nullptr);
		}

		m_vSwapChainImageViewList.clear();
	}

	bool CreateMultiSamplerImageAttachments()
	{
		m_vSwapChainMultiSamplingImages.resize(m_SwapChainImageCount);
		if(HasMultiSampling())
		{
			for(size_t i = 0; i < m_SwapChainImageCount; ++i)
			{
				CreateImage(m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width, m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height, 1, 1, m_VKSurfFormat.format, VK_IMAGE_TILING_OPTIMAL, m_vSwapChainMultiSamplingImages[i].m_Image, m_vSwapChainMultiSamplingImages[i].m_ImgMem, VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
				m_vSwapChainMultiSamplingImages[i].m_ImgView = CreateImageView(m_vSwapChainMultiSamplingImages[i].m_Image, m_VKSurfFormat.format, VK_IMAGE_VIEW_TYPE_2D, 1, 1);
			}
		}

		return true;
	}

	void DestroyMultiSamplerImageAttachments()
	{
		if(HasMultiSampling())
		{
			m_vSwapChainMultiSamplingImages.resize(m_SwapChainImageCount);
			for(size_t i = 0; i < m_SwapChainImageCount; ++i)
			{
				vkDestroyImage(m_VKDevice, m_vSwapChainMultiSamplingImages[i].m_Image, nullptr);
				vkDestroyImageView(m_VKDevice, m_vSwapChainMultiSamplingImages[i].m_ImgView, nullptr);
				FreeImageMemBlock(m_vSwapChainMultiSamplingImages[i].m_ImgMem);
			}
		}
		m_vSwapChainMultiSamplingImages.clear();
	}

	bool CreateRenderPass(bool ClearAttachs)
	{
		bool HasMultiSamplingTargets = HasMultiSampling();
		VkAttachmentDescription MultiSamplingColorAttachment{};
		MultiSamplingColorAttachment.format = m_VKSurfFormat.format;
		MultiSamplingColorAttachment.samples = GetSampleCount();
		MultiSamplingColorAttachment.loadOp = ClearAttachs ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		MultiSamplingColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		MultiSamplingColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		MultiSamplingColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		MultiSamplingColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		MultiSamplingColorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentDescription ColorAttachment{};
		ColorAttachment.format = m_VKSurfFormat.format;
		ColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		ColorAttachment.loadOp = ClearAttachs && !HasMultiSamplingTargets ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		ColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		ColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		ColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		ColorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference MultiSamplingColorAttachmentRef{};
		MultiSamplingColorAttachmentRef.attachment = 0;
		MultiSamplingColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference ColorAttachmentRef{};
		ColorAttachmentRef.attachment = HasMultiSamplingTargets ? 1 : 0;
		ColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription Subpass{};
		Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		Subpass.colorAttachmentCount = 1;
		Subpass.pColorAttachments = HasMultiSamplingTargets ? &MultiSamplingColorAttachmentRef : &ColorAttachmentRef;
		Subpass.pResolveAttachments = HasMultiSamplingTargets ? &ColorAttachmentRef : nullptr;

		std::array<VkAttachmentDescription, 2> aAttachments;
		aAttachments[0] = MultiSamplingColorAttachment;
		aAttachments[1] = ColorAttachment;

		VkSubpassDependency Dependency{};
		Dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		Dependency.dstSubpass = 0;
		Dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		Dependency.srcAccessMask = 0;
		Dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		Dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo CreateRenderPassInfo{};
		CreateRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		CreateRenderPassInfo.attachmentCount = HasMultiSamplingTargets ? 2 : 1;
		CreateRenderPassInfo.pAttachments = HasMultiSamplingTargets ? aAttachments.data() : aAttachments.data() + 1;
		CreateRenderPassInfo.subpassCount = 1;
		CreateRenderPassInfo.pSubpasses = &Subpass;
		CreateRenderPassInfo.dependencyCount = 1;
		CreateRenderPassInfo.pDependencies = &Dependency;

		if(vkCreateRenderPass(m_VKDevice, &CreateRenderPassInfo, nullptr, &m_VKRenderPass) != VK_SUCCESS)
		{
			SetError("Creating the render pass failed.");
			return false;
		}

		return true;
	}

	void DestroyRenderPass()
	{
		vkDestroyRenderPass(m_VKDevice, m_VKRenderPass, nullptr);
	}

	bool CreateFramebuffers()
	{
		m_vFramebufferList.resize(m_SwapChainImageCount);

		for(size_t i = 0; i < m_SwapChainImageCount; i++)
		{
			std::array<VkImageView, 2> aAttachments = {
				m_vSwapChainMultiSamplingImages[i].m_ImgView,
				m_vSwapChainImageViewList[i]};

			bool HasMultiSamplingTargets = HasMultiSampling();

			VkFramebufferCreateInfo FramebufferInfo{};
			FramebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			FramebufferInfo.renderPass = m_VKRenderPass;
			FramebufferInfo.attachmentCount = HasMultiSamplingTargets ? aAttachments.size() : aAttachments.size() - 1;
			FramebufferInfo.pAttachments = HasMultiSamplingTargets ? aAttachments.data() : aAttachments.data() + 1;
			FramebufferInfo.width = m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width;
			FramebufferInfo.height = m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height;
			FramebufferInfo.layers = 1;

			if(vkCreateFramebuffer(m_VKDevice, &FramebufferInfo, nullptr, &m_vFramebufferList[i]) != VK_SUCCESS)
			{
				SetError("Creating the framebuffers failed.");
				return false;
			}
		}

		return true;
	}

	void DestroyFramebuffers()
	{
		for(auto &FrameBuffer : m_vFramebufferList)
		{
			vkDestroyFramebuffer(m_VKDevice, FrameBuffer, nullptr);
		}

		m_vFramebufferList.clear();
	}

	bool CreateShaderModule(const std::vector<uint8_t> &vCode, VkShaderModule &ShaderModule)
	{
		VkShaderModuleCreateInfo CreateInfo{};
		CreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		CreateInfo.codeSize = vCode.size();
		CreateInfo.pCode = (const uint32_t *)(vCode.data());

		if(vkCreateShaderModule(m_VKDevice, &CreateInfo, nullptr, &ShaderModule) != VK_SUCCESS)
		{
			SetError("Shader module was not created.");
			return false;
		}

		return true;
	}

	bool CreateDescriptorSetLayouts()
	{
		VkDescriptorSetLayoutBinding SamplerLayoutBinding{};
		SamplerLayoutBinding.binding = 0;
		SamplerLayoutBinding.descriptorCount = 1;
		SamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		SamplerLayoutBinding.pImmutableSamplers = nullptr;
		SamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		std::array<VkDescriptorSetLayoutBinding, 1> aBindings = {SamplerLayoutBinding};
		VkDescriptorSetLayoutCreateInfo LayoutInfo{};
		LayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		LayoutInfo.bindingCount = aBindings.size();
		LayoutInfo.pBindings = aBindings.data();

		if(vkCreateDescriptorSetLayout(m_VKDevice, &LayoutInfo, nullptr, &m_StandardTexturedDescriptorSetLayout) != VK_SUCCESS)
		{
			SetError("Creating descriptor layout failed.");
			return false;
		}

		if(vkCreateDescriptorSetLayout(m_VKDevice, &LayoutInfo, nullptr, &m_Standard3DTexturedDescriptorSetLayout) != VK_SUCCESS)
		{
			SetError("Creating descriptor layout failed.");
			return false;
		}
		return true;
	}

	void DestroyDescriptorSetLayouts()
	{
		vkDestroyDescriptorSetLayout(m_VKDevice, m_StandardTexturedDescriptorSetLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_VKDevice, m_Standard3DTexturedDescriptorSetLayout, nullptr);
	}

	bool LoadShader(const char *pFileName, std::vector<uint8_t> *&pvShaderData)
	{
		auto it = m_ShaderFiles.find(pFileName);
		if(it == m_ShaderFiles.end())
		{
			void *pShaderBuff;
			unsigned FileSize;
			if(!m_pStorage->ReadFile(pFileName, IStorage::TYPE_ALL, &pShaderBuff, &FileSize))
				return false;

			std::vector<uint8_t> vShaderBuff;
			vShaderBuff.resize(FileSize);
			mem_copy(vShaderBuff.data(), pShaderBuff, FileSize);
			free(pShaderBuff);

			it = m_ShaderFiles.insert({pFileName, {std::move(vShaderBuff)}}).first;
		}

		pvShaderData = &it->second.m_vBinary;

		return true;
	}

	bool CreateShaders(const char *pVertName, const char *pFragName, VkPipelineShaderStageCreateInfo (&aShaderStages)[2], SShaderModule &ShaderModule)
	{
		bool ShaderLoaded = true;

		std::vector<uint8_t> *pvVertBuff;
		std::vector<uint8_t> *pvFragBuff;
		ShaderLoaded &= LoadShader(pVertName, pvVertBuff);
		ShaderLoaded &= LoadShader(pFragName, pvFragBuff);

		ShaderModule.m_VKDevice = m_VKDevice;

		if(!ShaderLoaded)
		{
			SetError("A shader file could not load correctly");
			return false;
		}

		if(!CreateShaderModule(*pvVertBuff, ShaderModule.m_VertShaderModule))
			return false;

		if(!CreateShaderModule(*pvFragBuff, ShaderModule.m_FragShaderModule))
			return false;

		VkPipelineShaderStageCreateInfo &VertShaderStageInfo = aShaderStages[0];
		VertShaderStageInfo = {};
		VertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		VertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
		VertShaderStageInfo.module = ShaderModule.m_VertShaderModule;
		VertShaderStageInfo.pName = "main";

		VkPipelineShaderStageCreateInfo &FragShaderStageInfo = aShaderStages[1];
		FragShaderStageInfo = {};
		FragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		FragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		FragShaderStageInfo.module = ShaderModule.m_FragShaderModule;
		FragShaderStageInfo.pName = "main";
		return true;
	}

	bool GetStandardPipelineInfo(VkPipelineInputAssemblyStateCreateInfo &InputAssembly,
		VkViewport &Viewport,
		VkRect2D &Scissor,
		VkPipelineViewportStateCreateInfo &ViewportState,
		VkPipelineRasterizationStateCreateInfo &Rasterizer,
		VkPipelineMultisampleStateCreateInfo &Multisampling,
		VkPipelineColorBlendAttachmentState &ColorBlendAttachment,
		VkPipelineColorBlendStateCreateInfo &ColorBlending)
	{
		InputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		InputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		InputAssembly.primitiveRestartEnable = VK_FALSE;

		Viewport.x = 0.0f;
		Viewport.y = 0.0f;
		Viewport.width = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.width;
		Viewport.height = (float)m_VKSwapImgAndViewportExtent.m_SwapImageViewport.height;
		Viewport.minDepth = 0.0f;
		Viewport.maxDepth = 1.0f;

		Scissor.offset = {0, 0};
		Scissor.extent = m_VKSwapImgAndViewportExtent.m_SwapImageViewport;

		ViewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		ViewportState.viewportCount = 1;
		ViewportState.pViewports = &Viewport;
		ViewportState.scissorCount = 1;
		ViewportState.pScissors = &Scissor;

		Rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		Rasterizer.depthClampEnable = VK_FALSE;
		Rasterizer.rasterizerDiscardEnable = VK_FALSE;
		Rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		Rasterizer.lineWidth = 1.0f;
		Rasterizer.cullMode = VK_CULL_MODE_NONE;
		Rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
		Rasterizer.depthBiasEnable = VK_FALSE;

		Multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		Multisampling.sampleShadingEnable = VK_FALSE;
		Multisampling.rasterizationSamples = GetSampleCount();

		ColorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		ColorBlendAttachment.blendEnable = VK_TRUE;

		ColorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ColorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ColorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		ColorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		ColorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ColorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

		ColorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		ColorBlending.logicOpEnable = VK_FALSE;
		ColorBlending.logicOp = VK_LOGIC_OP_COPY;
		ColorBlending.attachmentCount = 1;
		ColorBlending.pAttachments = &ColorBlendAttachment;
		ColorBlending.blendConstants[0] = 0.0f;
		ColorBlending.blendConstants[1] = 0.0f;
		ColorBlending.blendConstants[2] = 0.0f;
		ColorBlending.blendConstants[3] = 0.0f;

		return true;
	}

	template<bool ForceRequireDescriptors, size_t ArraySize, size_t DescrArraySize, size_t PushArraySize>
	bool CreateGraphicsPipeline(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, uint32_t Stride, std::array<VkVertexInputAttributeDescription, ArraySize> &aInputAttr,
		std::array<VkDescriptorSetLayout, DescrArraySize> &aSetLayouts, std::array<VkPushConstantRange, PushArraySize> &aPushConstants, EVulkanBackendTextureModes TexMode,
		EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode, bool IsLinePrim = false)
	{
		VkPipelineShaderStageCreateInfo aShaderStages[2];
		SShaderModule Module;
		if(!CreateShaders(pVertName, pFragName, aShaderStages, Module))
			return false;

		bool HasSampler = TexMode == VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

		VkPipelineVertexInputStateCreateInfo VertexInputInfo{};
		VertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		VkVertexInputBindingDescription BindingDescription{};
		BindingDescription.binding = 0;
		BindingDescription.stride = Stride;
		BindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		VertexInputInfo.vertexBindingDescriptionCount = 1;
		VertexInputInfo.vertexAttributeDescriptionCount = aInputAttr.size();
		VertexInputInfo.pVertexBindingDescriptions = &BindingDescription;
		VertexInputInfo.pVertexAttributeDescriptions = aInputAttr.data();

		VkPipelineInputAssemblyStateCreateInfo InputAssembly{};
		VkViewport Viewport{};
		VkRect2D Scissor{};
		VkPipelineViewportStateCreateInfo ViewportState{};
		VkPipelineRasterizationStateCreateInfo Rasterizer{};
		VkPipelineMultisampleStateCreateInfo Multisampling{};
		VkPipelineColorBlendAttachmentState ColorBlendAttachment{};
		VkPipelineColorBlendStateCreateInfo ColorBlending{};

		GetStandardPipelineInfo(InputAssembly, Viewport, Scissor, ViewportState, Rasterizer, Multisampling, ColorBlendAttachment, ColorBlending);
		InputAssembly.topology = IsLinePrim ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineLayoutCreateInfo PipelineLayoutInfo{};
		PipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		PipelineLayoutInfo.setLayoutCount = (HasSampler || ForceRequireDescriptors) ? aSetLayouts.size() : 0;
		PipelineLayoutInfo.pSetLayouts = (HasSampler || ForceRequireDescriptors) && !aSetLayouts.empty() ? aSetLayouts.data() : nullptr;

		PipelineLayoutInfo.pushConstantRangeCount = aPushConstants.size();
		PipelineLayoutInfo.pPushConstantRanges = !aPushConstants.empty() ? aPushConstants.data() : nullptr;

		VkPipelineLayout &PipeLayout = GetPipeLayout(PipeContainer, HasSampler, size_t(BlendMode), size_t(DynamicMode));
		VkPipeline &Pipeline = GetPipeline(PipeContainer, HasSampler, size_t(BlendMode), size_t(DynamicMode));

		if(vkCreatePipelineLayout(m_VKDevice, &PipelineLayoutInfo, nullptr, &PipeLayout) != VK_SUCCESS)
		{
			SetError("Creating pipeline layout failed.");
			return false;
		}

		VkGraphicsPipelineCreateInfo PipelineInfo{};
		PipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		PipelineInfo.stageCount = 2;
		PipelineInfo.pStages = aShaderStages;
		PipelineInfo.pVertexInputState = &VertexInputInfo;
		PipelineInfo.pInputAssemblyState = &InputAssembly;
		PipelineInfo.pViewportState = &ViewportState;
		PipelineInfo.pRasterizationState = &Rasterizer;
		PipelineInfo.pMultisampleState = &Multisampling;
		PipelineInfo.pColorBlendState = &ColorBlending;
		PipelineInfo.layout = PipeLayout;
		PipelineInfo.renderPass = m_VKRenderPass;
		PipelineInfo.subpass = 0;
		PipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

		std::array<VkDynamicState, 2> aDynamicStates = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR,
		};

		VkPipelineDynamicStateCreateInfo DynamicStateCreate{};
		DynamicStateCreate.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		DynamicStateCreate.dynamicStateCount = aDynamicStates.size();
		DynamicStateCreate.pDynamicStates = aDynamicStates.data();

		if(DynamicMode == VULKAN_BACKEND_CLIP_MODE_DYNAMIC_SCISSOR_AND_VIEWPORT)
		{
			PipelineInfo.pDynamicState = &DynamicStateCreate;
		}

		if(vkCreateGraphicsPipelines(m_VKDevice, VK_NULL_HANDLE, 1, &PipelineInfo, nullptr, &Pipeline) != VK_SUCCESS)
		{
			SetError("Creating the graphic pipeline failed.");
			return false;
		}

		return true;
	}

	bool CreateStandardGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode, bool IsLinePrim)
	{
		std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};

		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2};
		aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * (2 + 2)};

		std::array<VkDescriptorSetLayout, 1> aSetLayouts = {m_StandardTexturedDescriptorSetLayout};

		std::array<VkPushConstantRange, 1> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos)};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * (2 + 2) + sizeof(uint8_t) * 4, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode, IsLinePrim);
	}

	bool CreateStandardGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler, bool IsLinePipe)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateStandardGraphicsPipelineImpl(pVertName, pFragName, IsLinePipe ? m_StandardLinePipeline : m_StandardPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j), IsLinePipe);
			}
		}

		return Ret;
	}

	bool CreateStandard3DGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};

		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * 2};
		aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 2 + sizeof(uint8_t) * 4};

		std::array<VkDescriptorSetLayout, 1> aSetLayouts = {m_Standard3DTexturedDescriptorSetLayout};

		std::array<VkPushConstantRange, 1> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos)};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * 2 + sizeof(uint8_t) * 4 + sizeof(float) * 3, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	bool CreateStandard3DGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateStandard3DGraphicsPipelineImpl(pVertName, pFragName, m_Standard3DPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	bool CreateTextDescriptorSetLayout()
	{
		VkDescriptorSetLayoutBinding SamplerLayoutBinding{};
		SamplerLayoutBinding.binding = 0;
		SamplerLayoutBinding.descriptorCount = 1;
		SamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		SamplerLayoutBinding.pImmutableSamplers = nullptr;
		SamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		auto SamplerLayoutBinding2 = SamplerLayoutBinding;
		SamplerLayoutBinding2.binding = 1;

		std::array<VkDescriptorSetLayoutBinding, 2> aBindings = {SamplerLayoutBinding, SamplerLayoutBinding2};
		VkDescriptorSetLayoutCreateInfo LayoutInfo{};
		LayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		LayoutInfo.bindingCount = aBindings.size();
		LayoutInfo.pBindings = aBindings.data();

		if(vkCreateDescriptorSetLayout(m_VKDevice, &LayoutInfo, nullptr, &m_TextDescriptorSetLayout) != VK_SUCCESS)
		{
			SetError("Creating descriptor layout failed.");
			return false;
		}

		return true;
	}

	void DestroyTextDescriptorSetLayout()
	{
		vkDestroyDescriptorSetLayout(m_VKDevice, m_TextDescriptorSetLayout, nullptr);
	}

	bool CreateTextGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2};
		aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * (2 + 2)};

		std::array<VkDescriptorSetLayout, 1> aSetLayouts = {m_TextDescriptorSetLayout};

		std::array<VkPushConstantRange, 2> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGTextPos)};
		aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformGTextPos) + sizeof(SUniformTextGFragmentOffset), sizeof(SUniformTextGFragmentConstants)};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * (2 + 2) + sizeof(uint8_t) * 4, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	bool CreateTextGraphicsPipeline(const char *pVertName, const char *pFragName)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateTextGraphicsPipelineImpl(pVertName, pFragName, m_TextPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	template<bool HasSampler>
	bool CreateTileGraphicsPipelineImpl(const char *pVertName, const char *pFragName, int Type, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, HasSampler ? 2 : 1> aAttributeDescriptions = {};
		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
		if(HasSampler)
			aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 2};

		std::array<VkDescriptorSetLayout, 1> aSetLayouts;
		aSetLayouts[0] = m_Standard3DTexturedDescriptorSetLayout;

		uint32_t VertPushConstantSize = sizeof(SUniformTileGPos);
		if(Type == 1)
			VertPushConstantSize = sizeof(SUniformTileGPosBorder);
		else if(Type == 2)
			VertPushConstantSize = sizeof(SUniformTileGPosBorderLine);

		uint32_t FragPushConstantSize = sizeof(SUniformTileGVertColor);

		std::array<VkPushConstantRange, 2> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, VertPushConstantSize};
		aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformTileGPosBorder) + sizeof(SUniformTileGVertColorAlign), FragPushConstantSize};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, HasSampler ? (sizeof(float) * (2 + 3)) : (sizeof(float) * 2), aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	template<bool HasSampler>
	bool CreateTileGraphicsPipeline(const char *pVertName, const char *pFragName, int Type)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateTileGraphicsPipelineImpl<HasSampler>(pVertName, pFragName, Type, Type == 0 ? m_TilePipeline : (Type == 1 ? m_TileBorderPipeline : m_TileBorderLinePipeline), TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	bool CreatePrimExGraphicsPipelineImpl(const char *pVertName, const char *pFragName, bool Rotationless, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2};
		aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * (2 + 2)};

		std::array<VkDescriptorSetLayout, 1> aSetLayouts;
		aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;
		uint32_t VertPushConstantSize = sizeof(SUniformPrimExGPos);
		if(Rotationless)
			VertPushConstantSize = sizeof(SUniformPrimExGPosRotationless);

		uint32_t FragPushConstantSize = sizeof(SUniformPrimExGVertColor);

		std::array<VkPushConstantRange, 2> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, VertPushConstantSize};
		aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformPrimExGPos) + sizeof(SUniformPrimExGVertColorAlign), FragPushConstantSize};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * (2 + 2) + sizeof(uint8_t) * 4, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	bool CreatePrimExGraphicsPipeline(const char *pVertName, const char *pFragName, bool HasSampler, bool Rotationless)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreatePrimExGraphicsPipelineImpl(pVertName, pFragName, Rotationless, Rotationless ? m_PrimExRotationlessPipeline : m_PrimExPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	bool CreateUniformDescriptorSetLayout(VkDescriptorSetLayout &SetLayout, VkShaderStageFlags StageFlags)
	{
		VkDescriptorSetLayoutBinding SamplerLayoutBinding{};
		SamplerLayoutBinding.binding = 1;
		SamplerLayoutBinding.descriptorCount = 1;
		SamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		SamplerLayoutBinding.pImmutableSamplers = nullptr;
		SamplerLayoutBinding.stageFlags = StageFlags;

		std::array<VkDescriptorSetLayoutBinding, 1> aBindings = {SamplerLayoutBinding};
		VkDescriptorSetLayoutCreateInfo LayoutInfo{};
		LayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		LayoutInfo.bindingCount = aBindings.size();
		LayoutInfo.pBindings = aBindings.data();

		if(vkCreateDescriptorSetLayout(m_VKDevice, &LayoutInfo, nullptr, &SetLayout) != VK_SUCCESS)
		{
			SetError("Creating descriptor layout failed.");
			return false;
		}
		return true;
	}

	bool CreateSpriteMultiUniformDescriptorSetLayout()
	{
		return CreateUniformDescriptorSetLayout(m_SpriteMultiUniformDescriptorSetLayout, VK_SHADER_STAGE_VERTEX_BIT);
	}

	bool CreateQuadUniformDescriptorSetLayout()
	{
		return CreateUniformDescriptorSetLayout(m_QuadUniformDescriptorSetLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	}

	void DestroyUniformDescriptorSetLayouts()
	{
		vkDestroyDescriptorSetLayout(m_VKDevice, m_QuadUniformDescriptorSetLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_VKDevice, m_SpriteMultiUniformDescriptorSetLayout, nullptr);
	}

	bool CreateUniformDescriptorSets(size_t RenderThreadIndex, VkDescriptorSetLayout &SetLayout, SDeviceDescriptorSet *pSets, size_t SetCount, VkBuffer BindBuffer, size_t SingleBufferInstanceSize, VkDeviceSize MemoryOffset)
	{
		GetDescriptorPoolForAlloc(m_vUniformBufferDescrPools[RenderThreadIndex], pSets, SetCount);
		VkDescriptorSetAllocateInfo DesAllocInfo{};
		DesAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		DesAllocInfo.descriptorSetCount = 1;
		DesAllocInfo.pSetLayouts = &SetLayout;
		for(size_t i = 0; i < SetCount; ++i)
		{
			DesAllocInfo.descriptorPool = pSets[i].m_pPools->m_vPools[pSets[i].m_PoolIndex].m_Pool;
			if(vkAllocateDescriptorSets(m_VKDevice, &DesAllocInfo, &pSets[i].m_Descriptor) != VK_SUCCESS)
			{
				return false;
			}

			VkDescriptorBufferInfo BufferInfo{};
			BufferInfo.buffer = BindBuffer;
			BufferInfo.offset = MemoryOffset + SingleBufferInstanceSize * i;
			BufferInfo.range = SingleBufferInstanceSize;

			std::array<VkWriteDescriptorSet, 1> aDescriptorWrites{};

			aDescriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			aDescriptorWrites[0].dstSet = pSets[i].m_Descriptor;
			aDescriptorWrites[0].dstBinding = 1;
			aDescriptorWrites[0].dstArrayElement = 0;
			aDescriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			aDescriptorWrites[0].descriptorCount = 1;
			aDescriptorWrites[0].pBufferInfo = &BufferInfo;

			vkUpdateDescriptorSets(m_VKDevice, static_cast<uint32_t>(aDescriptorWrites.size()), aDescriptorWrites.data(), 0, nullptr);
		}

		return true;
	}

	void DestroyUniformDescriptorSets(SDeviceDescriptorSet *pSets, size_t SetCount)
	{
		for(size_t i = 0; i < SetCount; ++i)
		{
			vkFreeDescriptorSets(m_VKDevice, pSets[i].m_pPools->m_vPools[pSets[i].m_PoolIndex].m_Pool, 1, &pSets[i].m_Descriptor);
			pSets[i].m_Descriptor = VK_NULL_HANDLE;
		}
	}

	bool CreateSpriteMultiGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2};
		aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * (2 + 2)};

		std::array<VkDescriptorSetLayout, 2> aSetLayouts;
		aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;
		aSetLayouts[1] = m_SpriteMultiUniformDescriptorSetLayout;

		uint32_t VertPushConstantSize = sizeof(SUniformSpriteMultiGPos);
		uint32_t FragPushConstantSize = sizeof(SUniformSpriteMultiGVertColor);

		std::array<VkPushConstantRange, 2> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, VertPushConstantSize};
		aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiGPos) + sizeof(SUniformSpriteMultiGVertColorAlign), FragPushConstantSize};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * (2 + 2) + sizeof(uint8_t) * 4, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	bool CreateSpriteMultiGraphicsPipeline(const char *pVertName, const char *pFragName)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateSpriteMultiGraphicsPipelineImpl(pVertName, pFragName, m_SpriteMultiPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	bool CreateSpriteMultiPushGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, 3> aAttributeDescriptions = {};
		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2};
		aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * (2 + 2)};

		std::array<VkDescriptorSetLayout, 1> aSetLayouts;
		aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;

		uint32_t VertPushConstantSize = sizeof(SUniformSpriteMultiPushGPos);
		uint32_t FragPushConstantSize = sizeof(SUniformSpriteMultiPushGVertColor);

		std::array<VkPushConstantRange, 2> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, VertPushConstantSize};
		aPushConstants[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiPushGPos), FragPushConstantSize};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * (2 + 2) + sizeof(uint8_t) * 4, aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	bool CreateSpriteMultiPushGraphicsPipeline(const char *pVertName, const char *pFragName)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = VULKAN_BACKEND_TEXTURE_MODE_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateSpriteMultiPushGraphicsPipelineImpl(pVertName, pFragName, m_SpriteMultiPushPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	template<bool IsTextured>
	bool CreateQuadGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, IsTextured ? 3 : 2> aAttributeDescriptions = {};
		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * 4};
		if(IsTextured)
			aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 4 + sizeof(uint8_t) * 4};

		std::array<VkDescriptorSetLayout, IsTextured ? 2 : 1> aSetLayouts;
		if(IsTextured)
		{
			aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;
			aSetLayouts[1] = m_QuadUniformDescriptorSetLayout;
		}
		else
		{
			aSetLayouts[0] = m_QuadUniformDescriptorSetLayout;
		}

		uint32_t PushConstantSize = sizeof(SUniformQuadGPos);

		std::array<VkPushConstantRange, 1> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT, 0, PushConstantSize};

		return CreateGraphicsPipeline<true>(pVertName, pFragName, PipeContainer, sizeof(float) * 4 + sizeof(uint8_t) * 4 + (IsTextured ? (sizeof(float) * 2) : 0), aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	template<bool HasSampler>
	bool CreateQuadGraphicsPipeline(const char *pVertName, const char *pFragName)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateQuadGraphicsPipelineImpl<HasSampler>(pVertName, pFragName, m_QuadPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	template<bool IsTextured>
	bool CreateQuadPushGraphicsPipelineImpl(const char *pVertName, const char *pFragName, SPipelineContainer &PipeContainer, EVulkanBackendTextureModes TexMode, EVulkanBackendBlendModes BlendMode, EVulkanBackendClipModes DynamicMode)
	{
		std::array<VkVertexInputAttributeDescription, IsTextured ? 3 : 2> aAttributeDescriptions = {};
		aAttributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
		aAttributeDescriptions[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, sizeof(float) * 4};
		if(IsTextured)
			aAttributeDescriptions[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 4 + sizeof(uint8_t) * 4};

		std::array<VkDescriptorSetLayout, 1> aSetLayouts;
		aSetLayouts[0] = m_StandardTexturedDescriptorSetLayout;

		uint32_t PushConstantSize = sizeof(SUniformQuadPushGPos);

		std::array<VkPushConstantRange, 1> aPushConstants{};
		aPushConstants[0] = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PushConstantSize};

		return CreateGraphicsPipeline<false>(pVertName, pFragName, PipeContainer, sizeof(float) * 4 + sizeof(uint8_t) * 4 + (IsTextured ? (sizeof(float) * 2) : 0), aAttributeDescriptions, aSetLayouts, aPushConstants, TexMode, BlendMode, DynamicMode);
	}

	template<bool HasSampler>
	bool CreateQuadPushGraphicsPipeline(const char *pVertName, const char *pFragName)
	{
		bool Ret = true;

		EVulkanBackendTextureModes TexMode = HasSampler ? VULKAN_BACKEND_TEXTURE_MODE_TEXTURED : VULKAN_BACKEND_TEXTURE_MODE_NOT_TEXTURED;

		for(size_t i = 0; i < VULKAN_BACKEND_BLEND_MODE_COUNT; ++i)
		{
			for(size_t j = 0; j < VULKAN_BACKEND_CLIP_MODE_COUNT; ++j)
			{
				Ret &= CreateQuadPushGraphicsPipelineImpl<HasSampler>(pVertName, pFragName, m_QuadPushPipeline, TexMode, EVulkanBackendBlendModes(i), EVulkanBackendClipModes(j));
			}
		}

		return Ret;
	}

	bool CreateCommandPool()
	{
		VkCommandPoolCreateInfo CreatePoolInfo{};
		CreatePoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		CreatePoolInfo.queueFamilyIndex = m_VKGraphicsQueueIndex;
		CreatePoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		m_vCommandPools.resize(m_ThreadCount);
		for(size_t i = 0; i < m_ThreadCount; ++i)
		{
			if(vkCreateCommandPool(m_VKDevice, &CreatePoolInfo, nullptr, &m_vCommandPools[i]) != VK_SUCCESS)
			{
				SetError("Creating the command pool failed.");
				return false;
			}
		}
		return true;
	}

	void DestroyCommandPool()
	{
		for(size_t i = 0; i < m_ThreadCount; ++i)
		{
			vkDestroyCommandPool(m_VKDevice, m_vCommandPools[i], nullptr);
		}
	}

	bool CreateCommandBuffers()
	{
		m_vMainDrawCommandBuffers.resize(m_SwapChainImageCount);
		if(m_ThreadCount > 1)
		{
			m_vvThreadDrawCommandBuffers.resize(m_ThreadCount);
			m_vvUsedThreadDrawCommandBuffer.resize(m_ThreadCount);
			m_vHelperThreadDrawCommandBuffers.resize(m_ThreadCount);
			for(auto &ThreadDrawCommandBuffers : m_vvThreadDrawCommandBuffers)
			{
				ThreadDrawCommandBuffers.resize(m_SwapChainImageCount);
			}
			for(auto &UsedThreadDrawCommandBuffer : m_vvUsedThreadDrawCommandBuffer)
			{
				UsedThreadDrawCommandBuffer.resize(m_SwapChainImageCount, false);
			}
		}
		m_vMemoryCommandBuffers.resize(m_SwapChainImageCount);
		m_vUsedMemoryCommandBuffer.resize(m_SwapChainImageCount, false);

		VkCommandBufferAllocateInfo AllocInfo{};
		AllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		AllocInfo.commandPool = m_vCommandPools[0];
		AllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		AllocInfo.commandBufferCount = (uint32_t)m_vMainDrawCommandBuffers.size();

		if(vkAllocateCommandBuffers(m_VKDevice, &AllocInfo, m_vMainDrawCommandBuffers.data()) != VK_SUCCESS)
		{
			SetError("Allocating command buffers failed.");
			return false;
		}

		AllocInfo.commandBufferCount = (uint32_t)m_vMemoryCommandBuffers.size();

		if(vkAllocateCommandBuffers(m_VKDevice, &AllocInfo, m_vMemoryCommandBuffers.data()) != VK_SUCCESS)
		{
			SetError("Allocating memory command buffers failed.");
			return false;
		}

		if(m_ThreadCount > 1)
		{
			size_t Count = 0;
			for(auto &ThreadDrawCommandBuffers : m_vvThreadDrawCommandBuffers)
			{
				AllocInfo.commandPool = m_vCommandPools[Count];
				++Count;
				AllocInfo.commandBufferCount = (uint32_t)ThreadDrawCommandBuffers.size();
				AllocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
				if(vkAllocateCommandBuffers(m_VKDevice, &AllocInfo, ThreadDrawCommandBuffers.data()) != VK_SUCCESS)
				{
					SetError("Allocating thread command buffers failed.");
					return false;
				}
			}
		}

		return true;
	}

	void DestroyCommandBuffer()
	{
		if(m_ThreadCount > 1)
		{
			size_t Count = 0;
			for(auto &ThreadDrawCommandBuffers : m_vvThreadDrawCommandBuffers)
			{
				vkFreeCommandBuffers(m_VKDevice, m_vCommandPools[Count], static_cast<uint32_t>(ThreadDrawCommandBuffers.size()), ThreadDrawCommandBuffers.data());
				++Count;
			}
		}

		vkFreeCommandBuffers(m_VKDevice, m_vCommandPools[0], static_cast<uint32_t>(m_vMemoryCommandBuffers.size()), m_vMemoryCommandBuffers.data());
		vkFreeCommandBuffers(m_VKDevice, m_vCommandPools[0], static_cast<uint32_t>(m_vMainDrawCommandBuffers.size()), m_vMainDrawCommandBuffers.data());

		m_vvThreadDrawCommandBuffers.clear();
		m_vvUsedThreadDrawCommandBuffer.clear();
		m_vHelperThreadDrawCommandBuffers.clear();

		m_vMainDrawCommandBuffers.clear();
		m_vMemoryCommandBuffers.clear();
		m_vUsedMemoryCommandBuffer.clear();
	}

	bool CreateSyncObjects()
	{
		m_vWaitSemaphores.resize(m_SwapChainImageCount);
		m_vSigSemaphores.resize(m_SwapChainImageCount);

		m_vMemorySemaphores.resize(m_SwapChainImageCount);

		m_vFrameFences.resize(m_SwapChainImageCount);
		m_vImagesFences.resize(m_SwapChainImageCount, VK_NULL_HANDLE);

		VkSemaphoreCreateInfo CreateSemaphoreInfo{};
		CreateSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo FenceInfo{};
		FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for(size_t i = 0; i < m_SwapChainImageCount; i++)
		{
			if(vkCreateSemaphore(m_VKDevice, &CreateSemaphoreInfo, nullptr, &m_vWaitSemaphores[i]) != VK_SUCCESS ||
				vkCreateSemaphore(m_VKDevice, &CreateSemaphoreInfo, nullptr, &m_vSigSemaphores[i]) != VK_SUCCESS ||
				vkCreateSemaphore(m_VKDevice, &CreateSemaphoreInfo, nullptr, &m_vMemorySemaphores[i]) != VK_SUCCESS ||
				vkCreateFence(m_VKDevice, &FenceInfo, nullptr, &m_vFrameFences[i]) != VK_SUCCESS)
			{
				SetError("Creating swap chain sync objects(fences, semaphores) failed.");
				return false;
			}
		}

		return true;
	}

	void DestroySyncObjects()
	{
		for(size_t i = 0; i < m_SwapChainImageCount; i++)
		{
			vkDestroySemaphore(m_VKDevice, m_vWaitSemaphores[i], nullptr);
			vkDestroySemaphore(m_VKDevice, m_vSigSemaphores[i], nullptr);
			vkDestroySemaphore(m_VKDevice, m_vMemorySemaphores[i], nullptr);
			vkDestroyFence(m_VKDevice, m_vFrameFences[i], nullptr);
		}

		m_vWaitSemaphores.clear();
		m_vSigSemaphores.clear();

		m_vMemorySemaphores.clear();

		m_vFrameFences.clear();
		m_vImagesFences.clear();
	}

	void DestroyBufferOfFrame(size_t ImageIndex, SFrameBuffers &Buffer)
	{
		CleanBufferPair(ImageIndex, Buffer.m_Buffer, Buffer.m_BufferMem);
	}

	void DestroyUniBufferOfFrame(size_t ImageIndex, SFrameUniformBuffers &Buffer)
	{
		CleanBufferPair(ImageIndex, Buffer.m_Buffer, Buffer.m_BufferMem);
		for(auto &DescrSet : Buffer.m_aUniformSets)
		{
			if(DescrSet.m_Descriptor != VK_NULL_HANDLE)
			{
				DestroyUniformDescriptorSets(&DescrSet, 1);
			}
		}
	}

	/*************
	* SWAP CHAIN
	**************/

	void CleanupVulkanSwapChain(bool ForceSwapChainDestruct)
	{
		m_StandardPipeline.Destroy(m_VKDevice);
		m_StandardLinePipeline.Destroy(m_VKDevice);
		m_Standard3DPipeline.Destroy(m_VKDevice);
		m_TextPipeline.Destroy(m_VKDevice);
		m_TilePipeline.Destroy(m_VKDevice);
		m_TileBorderPipeline.Destroy(m_VKDevice);
		m_TileBorderLinePipeline.Destroy(m_VKDevice);
		m_PrimExPipeline.Destroy(m_VKDevice);
		m_PrimExRotationlessPipeline.Destroy(m_VKDevice);
		m_SpriteMultiPipeline.Destroy(m_VKDevice);
		m_SpriteMultiPushPipeline.Destroy(m_VKDevice);
		m_QuadPipeline.Destroy(m_VKDevice);
		m_QuadPushPipeline.Destroy(m_VKDevice);

		DestroyFramebuffers();

		DestroyRenderPass();

		DestroyMultiSamplerImageAttachments();

		DestroyImageViews();
		ClearSwapChainImageHandles();

		DestroySwapChain(ForceSwapChainDestruct);

		m_SwapchainCreated = false;
	}

	template<bool IsLastCleanup>
	void CleanupVulkan()
	{
		if(IsLastCleanup)
		{
			if(m_SwapchainCreated)
				CleanupVulkanSwapChain(true);

			// clean all images, buffers, buffer containers
			for(auto &Texture : m_vTextures)
			{
				if(Texture.m_VKTextDescrSet.m_Descriptor != VK_NULL_HANDLE && IsVerbose())
				{
					dbg_msg("vulkan", "text textures not cleared over cmd.");
				}
				DestroyTexture(Texture);
			}

			for(auto &BufferObject : m_vBufferObjects)
			{
				if(!BufferObject.m_IsStreamedBuffer)
					FreeVertexMemBlock(BufferObject.m_BufferObject.m_Mem);
			}

			m_vBufferContainers.clear();
		}

		m_vImageLastFrameCheck.clear();

		m_vLastPipeline.clear();

		for(size_t i = 0; i < m_ThreadCount; ++i)
		{
			m_vStreamedVertexBuffers[i].Destroy([&](size_t ImageIndex, SFrameBuffers &Buffer) { DestroyBufferOfFrame(ImageIndex, Buffer); });
			m_vStreamedUniformBuffers[i].Destroy([&](size_t ImageIndex, SFrameUniformBuffers &Buffer) { DestroyUniBufferOfFrame(ImageIndex, Buffer); });
		}
		m_vStreamedVertexBuffers.clear();
		m_vStreamedUniformBuffers.clear();

		for(size_t i = 0; i < m_SwapChainImageCount; ++i)
		{
			ClearFrameData(i);
		}

		m_vvFrameDelayedBufferCleanup.clear();
		m_vvFrameDelayedTextureCleanup.clear();
		m_vvFrameDelayedTextTexturesCleanup.clear();

		m_StagingBufferCache.DestroyFrameData(m_SwapChainImageCount);
		m_StagingBufferCacheImage.DestroyFrameData(m_SwapChainImageCount);
		m_VertexBufferCache.DestroyFrameData(m_SwapChainImageCount);
		for(auto &ImageBufferCache : m_ImageBufferCaches)
			ImageBufferCache.second.DestroyFrameData(m_SwapChainImageCount);

		if(IsLastCleanup)
		{
			m_StagingBufferCache.Destroy(m_VKDevice);
			m_StagingBufferCacheImage.Destroy(m_VKDevice);
			m_VertexBufferCache.Destroy(m_VKDevice);
			for(auto &ImageBufferCache : m_ImageBufferCaches)
				ImageBufferCache.second.Destroy(m_VKDevice);

			m_ImageBufferCaches.clear();

			DestroyTextureSamplers();
			DestroyDescriptorPools();

			DeletePresentedImageDataImage();
		}

		DestroySyncObjects();
		DestroyCommandBuffer();

		if(IsLastCleanup)
		{
			DestroyCommandPool();
		}

		if(IsLastCleanup)
		{
			DestroyUniformDescriptorSetLayouts();
			DestroyTextDescriptorSetLayout();
			DestroyDescriptorSetLayouts();
		}
	}

	void CleanupVulkanSDL()
	{
		if(m_VKInstance != VK_NULL_HANDLE)
		{
			DestroySurface();
			vkDestroyDevice(m_VKDevice, nullptr);

			if(g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL)
			{
				UnregisterDebugCallback();
			}
			vkDestroyInstance(m_VKInstance, nullptr);
		}
	}

	int RecreateSwapChain()
	{
		int Ret = 0;
		vkDeviceWaitIdle(m_VKDevice);

		if(IsVerbose())
		{
			dbg_msg("vulkan", "recreating swap chain.");
		}

		VkSwapchainKHR OldSwapChain = VK_NULL_HANDLE;
		uint32_t OldSwapChainImageCount = m_SwapChainImageCount;

		if(m_SwapchainCreated)
			CleanupVulkanSwapChain(false);

		// set new multi sampling if it was requested
		if(m_NextMultiSamplingCount != std::numeric_limits<uint32_t>::max())
		{
			m_MultiSamplingCount = m_NextMultiSamplingCount;
			m_NextMultiSamplingCount = std::numeric_limits<uint32_t>::max();
		}

		if(!m_SwapchainCreated)
			Ret = InitVulkanSwapChain(OldSwapChain);

		if(OldSwapChainImageCount != m_SwapChainImageCount)
		{
			CleanupVulkan<false>();
			InitVulkan<false>();
		}

		if(OldSwapChain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(m_VKDevice, OldSwapChain, nullptr);
		}

		if(Ret != 0 && IsVerbose())
		{
			dbg_msg("vulkan", "recreating swap chain failed.");
		}

		return Ret;
	}

	int InitVulkanSDL(SDL_Window *pWindow, uint32_t CanvasWidth, uint32_t CanvasHeight, char *pRendererString, char *pVendorString, char *pVersionString)
	{
		std::vector<std::string> vVKExtensions;
		std::vector<std::string> vVKLayers;

		m_CanvasWidth = CanvasWidth;
		m_CanvasHeight = CanvasHeight;

		if(!GetVulkanExtensions(pWindow, vVKExtensions))
			return -1;

		if(!GetVulkanLayers(vVKLayers))
			return -1;

		if(!CreateVulkanInstance(vVKLayers, vVKExtensions, true))
			return -1;

		if(g_Config.m_DbgGfx == DEBUG_GFX_MODE_MINIMUM || g_Config.m_DbgGfx == DEBUG_GFX_MODE_ALL)
		{
			SetupDebugCallback();

			for(auto &VKLayer : vVKLayers)
			{
				dbg_msg("vulkan", "Validation layer: %s", VKLayer.c_str());
			}
		}

		if(!SelectGPU(pRendererString, pVendorString, pVersionString))
			return -1;

		if(!CreateLogicalDevice(vVKLayers))
			return -1;

		GetDeviceQueue();

		if(!CreateSurface(pWindow))
			return -1;

		return 0;
	}

	/************************
	* MEMORY MANAGMENT
	************************/

	uint32_t FindMemoryType(VkPhysicalDevice PhyDevice, uint32_t TypeFilter, VkMemoryPropertyFlags Properties)
	{
		VkPhysicalDeviceMemoryProperties MemProperties;
		vkGetPhysicalDeviceMemoryProperties(PhyDevice, &MemProperties);

		for(uint32_t i = 0; i < MemProperties.memoryTypeCount; i++)
		{
			if((TypeFilter & (1 << i)) && (MemProperties.memoryTypes[i].propertyFlags & Properties) == Properties)
			{
				return i;
			}
		}

		return 0;
	}

	bool CreateBuffer(VkDeviceSize BufferSize, EMemoryBlockUsage MemUsage, VkBufferUsageFlags BufferUsage, VkMemoryPropertyFlags MemoryProperties, VkBuffer &VKBuffer, SDeviceMemoryBlock &VKBufferMemory)
	{
		VkBufferCreateInfo BufferInfo{};
		BufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		BufferInfo.size = BufferSize;
		BufferInfo.usage = BufferUsage;
		BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if(vkCreateBuffer(m_VKDevice, &BufferInfo, nullptr, &VKBuffer) != VK_SUCCESS)
		{
			SetError("Buffer creation failed.");
			return false;
		}

		VkMemoryRequirements MemRequirements;
		vkGetBufferMemoryRequirements(m_VKDevice, VKBuffer, &MemRequirements);

		VkMemoryAllocateInfo MemAllocInfo{};
		MemAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		MemAllocInfo.allocationSize = MemRequirements.size;
		MemAllocInfo.memoryTypeIndex = FindMemoryType(m_VKGPU, MemRequirements.memoryTypeBits, MemoryProperties);

		VKBufferMemory.m_Size = MemRequirements.size;

		if(MemUsage == MEMORY_BLOCK_USAGE_BUFFER)
			m_pBufferMemoryUsage->store(m_pBufferMemoryUsage->load(std::memory_order_relaxed) + MemRequirements.size, std::memory_order_relaxed);
		else if(MemUsage == MEMORY_BLOCK_USAGE_STAGING)
			m_pStagingMemoryUsage->store(m_pStagingMemoryUsage->load(std::memory_order_relaxed) + MemRequirements.size, std::memory_order_relaxed);
		else if(MemUsage == MEMORY_BLOCK_USAGE_STREAM)
			m_pStreamMemoryUsage->store(m_pStreamMemoryUsage->load(std::memory_order_relaxed) + MemRequirements.size, std::memory_order_relaxed);

		if(IsVerbose())
		{
			VerboseAllocatedMemory(MemRequirements.size, m_CurImageIndex, MemUsage);
		}

		if(!AllocateVulkanMemory(&MemAllocInfo, &VKBufferMemory.m_Mem))
		{
			SetError("Allocation for buffer object failed.");
			return false;
		}

		VKBufferMemory.m_UsageType = MemUsage;

		if(vkBindBufferMemory(m_VKDevice, VKBuffer, VKBufferMemory.m_Mem, 0) != VK_SUCCESS)
		{
			SetError("Binding memory to buffer failed.");
			return false;
		}

		return true;
	}

	bool AllocateDescriptorPool(SDeviceDescriptorPools &DescriptorPools, size_t AllocPoolSize)
	{
		SDeviceDescriptorPool NewPool;
		NewPool.m_Size = AllocPoolSize;

		VkDescriptorPoolSize PoolSize{};
		if(DescriptorPools.m_IsUniformPool)
			PoolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		else
			PoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		PoolSize.descriptorCount = AllocPoolSize;

		VkDescriptorPoolCreateInfo PoolInfo{};
		PoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		PoolInfo.poolSizeCount = 1;
		PoolInfo.pPoolSizes = &PoolSize;
		PoolInfo.maxSets = AllocPoolSize;
		PoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

		if(vkCreateDescriptorPool(m_VKDevice, &PoolInfo, nullptr, &NewPool.m_Pool) != VK_SUCCESS)
		{
			SetError("Creating the descriptor pool failed.");
			return false;
		}

		DescriptorPools.m_vPools.push_back(NewPool);

		return true;
	}

	bool CreateDescriptorPools()
	{
		m_StandardTextureDescrPool.m_IsUniformPool = false;
		m_StandardTextureDescrPool.m_DefaultAllocSize = 1024;
		m_TextTextureDescrPool.m_IsUniformPool = false;
		m_TextTextureDescrPool.m_DefaultAllocSize = 8;

		m_vUniformBufferDescrPools.resize(m_ThreadCount);
		for(auto &UniformBufferDescrPool : m_vUniformBufferDescrPools)
		{
			UniformBufferDescrPool.m_IsUniformPool = true;
			UniformBufferDescrPool.m_DefaultAllocSize = 512;
		}

		bool Ret = AllocateDescriptorPool(m_StandardTextureDescrPool, CCommandBuffer::MAX_TEXTURES);
		Ret |= AllocateDescriptorPool(m_TextTextureDescrPool, 8);

		for(auto &UniformBufferDescrPool : m_vUniformBufferDescrPools)
		{
			Ret |= AllocateDescriptorPool(UniformBufferDescrPool, 64);
		}

		return Ret;
	}

	void DestroyDescriptorPools()
	{
		for(auto &DescrPool : m_StandardTextureDescrPool.m_vPools)
			vkDestroyDescriptorPool(m_VKDevice, DescrPool.m_Pool, nullptr);
		for(auto &DescrPool : m_TextTextureDescrPool.m_vPools)
			vkDestroyDescriptorPool(m_VKDevice, DescrPool.m_Pool, nullptr);

		for(auto &UniformBufferDescrPool : m_vUniformBufferDescrPools)
		{
			for(auto &DescrPool : UniformBufferDescrPool.m_vPools)
				vkDestroyDescriptorPool(m_VKDevice, DescrPool.m_Pool, nullptr);
		}
		m_vUniformBufferDescrPools.clear();
	}

	VkDescriptorPool GetDescriptorPoolForAlloc(SDeviceDescriptorPools &DescriptorPools, SDeviceDescriptorSet *pSets, size_t AllocNum)
	{
		size_t CurAllocNum = AllocNum;
		size_t CurAllocOffset = 0;
		VkDescriptorPool RetDescr = VK_NULL_HANDLE;

		while(CurAllocNum > 0)
		{
			size_t AllocatedInThisRun = 0;

			bool Found = false;
			size_t DescriptorPoolIndex = std::numeric_limits<size_t>::max();
			for(size_t i = 0; i < DescriptorPools.m_vPools.size(); ++i)
			{
				auto &Pool = DescriptorPools.m_vPools[i];
				if(Pool.m_CurSize + CurAllocNum < Pool.m_Size)
				{
					AllocatedInThisRun = CurAllocNum;
					Pool.m_CurSize += CurAllocNum;
					Found = true;
					if(RetDescr == VK_NULL_HANDLE)
						RetDescr = Pool.m_Pool;
					DescriptorPoolIndex = i;
					break;
				}
				else
				{
					size_t RemainingPoolCount = Pool.m_Size - Pool.m_CurSize;
					if(RemainingPoolCount > 0)
					{
						AllocatedInThisRun = RemainingPoolCount;
						Pool.m_CurSize += RemainingPoolCount;
						Found = true;
						if(RetDescr == VK_NULL_HANDLE)
							RetDescr = Pool.m_Pool;
						DescriptorPoolIndex = i;
						break;
					}
				}
			}

			if(!Found)
			{
				DescriptorPoolIndex = DescriptorPools.m_vPools.size();

				AllocateDescriptorPool(DescriptorPools, DescriptorPools.m_DefaultAllocSize);

				AllocatedInThisRun = minimum((size_t)DescriptorPools.m_DefaultAllocSize, CurAllocNum);

				auto &Pool = DescriptorPools.m_vPools.back();
				Pool.m_CurSize += AllocatedInThisRun;
				if(RetDescr == VK_NULL_HANDLE)
					RetDescr = Pool.m_Pool;
			}

			for(size_t i = CurAllocOffset; i < CurAllocOffset + AllocatedInThisRun; ++i)
			{
				pSets[i].m_pPools = &DescriptorPools;
				pSets[i].m_PoolIndex = DescriptorPoolIndex;
			}
			CurAllocOffset += AllocatedInThisRun;
			CurAllocNum -= AllocatedInThisRun;
		}

		return RetDescr;
	}

	bool CreateNewTexturedStandardDescriptorSets(size_t TextureSlot, size_t DescrIndex)
	{
		auto &Texture = m_vTextures[TextureSlot];

		auto &DescrSet = Texture.m_aVKStandardTexturedDescrSets[DescrIndex];

		VkDescriptorSetAllocateInfo DesAllocInfo{};
		DesAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		DesAllocInfo.descriptorPool = GetDescriptorPoolForAlloc(m_StandardTextureDescrPool, &DescrSet, 1);
		DesAllocInfo.descriptorSetCount = 1;
		DesAllocInfo.pSetLayouts = &m_StandardTexturedDescriptorSetLayout;

		if(vkAllocateDescriptorSets(m_VKDevice, &DesAllocInfo, &DescrSet.m_Descriptor) != VK_SUCCESS)
		{
			return false;
		}

		VkDescriptorImageInfo ImageInfo{};
		ImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		ImageInfo.imageView = Texture.m_ImgView;
		ImageInfo.sampler = Texture.m_aSamplers[DescrIndex];

		std::array<VkWriteDescriptorSet, 1> aDescriptorWrites{};

		aDescriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		aDescriptorWrites[0].dstSet = DescrSet.m_Descriptor;
		aDescriptorWrites[0].dstBinding = 0;
		aDescriptorWrites[0].dstArrayElement = 0;
		aDescriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		aDescriptorWrites[0].descriptorCount = 1;
		aDescriptorWrites[0].pImageInfo = &ImageInfo;

		vkUpdateDescriptorSets(m_VKDevice, static_cast<uint32_t>(aDescriptorWrites.size()), aDescriptorWrites.data(), 0, nullptr);

		return true;
	}

	void DestroyTexturedStandardDescriptorSets(CTexture &Texture, size_t DescrIndex)
	{
		auto &DescrSet = Texture.m_aVKStandardTexturedDescrSets[DescrIndex];
		if(DescrSet.m_PoolIndex != std::numeric_limits<size_t>::max())
			vkFreeDescriptorSets(m_VKDevice, DescrSet.m_pPools->m_vPools[DescrSet.m_PoolIndex].m_Pool, 1, &DescrSet.m_Descriptor);
		DescrSet = {};
	}

	bool CreateNew3DTexturedStandardDescriptorSets(size_t TextureSlot)
	{
		auto &Texture = m_vTextures[TextureSlot];

		auto &DescrSet = Texture.m_VKStandard3DTexturedDescrSet;

		VkDescriptorSetAllocateInfo DesAllocInfo{};
		DesAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		DesAllocInfo.descriptorPool = GetDescriptorPoolForAlloc(m_StandardTextureDescrPool, &DescrSet, 1);
		DesAllocInfo.descriptorSetCount = 1;
		DesAllocInfo.pSetLayouts = &m_Standard3DTexturedDescriptorSetLayout;

		if(vkAllocateDescriptorSets(m_VKDevice, &DesAllocInfo, &DescrSet.m_Descriptor) != VK_SUCCESS)
		{
			return false;
		}

		VkDescriptorImageInfo ImageInfo{};
		ImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		ImageInfo.imageView = Texture.m_Img3DView;
		ImageInfo.sampler = Texture.m_Sampler3D;

		std::array<VkWriteDescriptorSet, 1> aDescriptorWrites{};

		aDescriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		aDescriptorWrites[0].dstSet = DescrSet.m_Descriptor;
		aDescriptorWrites[0].dstBinding = 0;
		aDescriptorWrites[0].dstArrayElement = 0;
		aDescriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		aDescriptorWrites[0].descriptorCount = 1;
		aDescriptorWrites[0].pImageInfo = &ImageInfo;

		vkUpdateDescriptorSets(m_VKDevice, static_cast<uint32_t>(aDescriptorWrites.size()), aDescriptorWrites.data(), 0, nullptr);

		return true;
	}

	void DestroyTextured3DStandardDescriptorSets(CTexture &Texture)
	{
		auto &DescrSet = Texture.m_VKStandard3DTexturedDescrSet;
		if(DescrSet.m_PoolIndex != std::numeric_limits<size_t>::max())
			vkFreeDescriptorSets(m_VKDevice, DescrSet.m_pPools->m_vPools[DescrSet.m_PoolIndex].m_Pool, 1, &DescrSet.m_Descriptor);
	}

	bool CreateNewTextDescriptorSets(size_t Texture, size_t TextureOutline)
	{
		auto &TextureText = m_vTextures[Texture];
		auto &TextureTextOutline = m_vTextures[TextureOutline];
		auto &DescrSetText = TextureText.m_VKTextDescrSet;
		auto &DescrSetTextOutline = TextureText.m_VKTextDescrSet;

		VkDescriptorSetAllocateInfo DesAllocInfo{};
		DesAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		DesAllocInfo.descriptorPool = GetDescriptorPoolForAlloc(m_TextTextureDescrPool, &DescrSetText, 1);
		DesAllocInfo.descriptorSetCount = 1;
		DesAllocInfo.pSetLayouts = &m_TextDescriptorSetLayout;

		if(vkAllocateDescriptorSets(m_VKDevice, &DesAllocInfo, &DescrSetText.m_Descriptor) != VK_SUCCESS)
		{
			return false;
		}

		std::array<VkDescriptorImageInfo, 2> aImageInfo{};
		aImageInfo[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		aImageInfo[0].imageView = TextureText.m_ImgView;
		aImageInfo[0].sampler = TextureText.m_aSamplers[0];
		aImageInfo[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		aImageInfo[1].imageView = TextureTextOutline.m_ImgView;
		aImageInfo[1].sampler = TextureTextOutline.m_aSamplers[0];

		std::array<VkWriteDescriptorSet, 2> aDescriptorWrites{};

		aDescriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		aDescriptorWrites[0].dstSet = DescrSetText.m_Descriptor;
		aDescriptorWrites[0].dstBinding = 0;
		aDescriptorWrites[0].dstArrayElement = 0;
		aDescriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		aDescriptorWrites[0].descriptorCount = 1;
		aDescriptorWrites[0].pImageInfo = aImageInfo.data();
		aDescriptorWrites[1] = aDescriptorWrites[0];
		aDescriptorWrites[1].dstBinding = 1;
		aDescriptorWrites[1].pImageInfo = &aImageInfo[1];

		vkUpdateDescriptorSets(m_VKDevice, static_cast<uint32_t>(aDescriptorWrites.size()), aDescriptorWrites.data(), 0, nullptr);

		DescrSetTextOutline = DescrSetText;

		return true;
	}

	void DestroyTextDescriptorSets(CTexture &Texture, CTexture &TextureOutline)
	{
		auto &DescrSet = Texture.m_VKTextDescrSet;
		if(DescrSet.m_PoolIndex != std::numeric_limits<size_t>::max())
			vkFreeDescriptorSets(m_VKDevice, DescrSet.m_pPools->m_vPools[DescrSet.m_PoolIndex].m_Pool, 1, &DescrSet.m_Descriptor);
	}

	bool HasMultiSampling()
	{
		return GetSampleCount() != VK_SAMPLE_COUNT_1_BIT;
	}

	VkSampleCountFlagBits GetMaxSampleCount()
	{
		if(m_MaxMultiSample & VK_SAMPLE_COUNT_64_BIT)
			return VK_SAMPLE_COUNT_64_BIT;
		else if(m_MaxMultiSample & VK_SAMPLE_COUNT_32_BIT)
			return VK_SAMPLE_COUNT_32_BIT;
		else if(m_MaxMultiSample & VK_SAMPLE_COUNT_16_BIT)
			return VK_SAMPLE_COUNT_16_BIT;
		else if(m_MaxMultiSample & VK_SAMPLE_COUNT_8_BIT)
			return VK_SAMPLE_COUNT_8_BIT;
		else if(m_MaxMultiSample & VK_SAMPLE_COUNT_4_BIT)
			return VK_SAMPLE_COUNT_4_BIT;
		else if(m_MaxMultiSample & VK_SAMPLE_COUNT_2_BIT)
			return VK_SAMPLE_COUNT_2_BIT;

		return VK_SAMPLE_COUNT_1_BIT;
	}

	VkSampleCountFlagBits GetSampleCount()
	{
		auto MaxSampleCount = GetMaxSampleCount();
		if(m_MultiSamplingCount >= 64 && MaxSampleCount >= VK_SAMPLE_COUNT_64_BIT)
			return VK_SAMPLE_COUNT_64_BIT;
		else if(m_MultiSamplingCount >= 32 && MaxSampleCount >= VK_SAMPLE_COUNT_32_BIT)
			return VK_SAMPLE_COUNT_32_BIT;
		else if(m_MultiSamplingCount >= 16 && MaxSampleCount >= VK_SAMPLE_COUNT_16_BIT)
			return VK_SAMPLE_COUNT_16_BIT;
		else if(m_MultiSamplingCount >= 8 && MaxSampleCount >= VK_SAMPLE_COUNT_8_BIT)
			return VK_SAMPLE_COUNT_8_BIT;
		else if(m_MultiSamplingCount >= 4 && MaxSampleCount >= VK_SAMPLE_COUNT_4_BIT)
			return VK_SAMPLE_COUNT_4_BIT;
		else if(m_MultiSamplingCount >= 2 && MaxSampleCount >= VK_SAMPLE_COUNT_2_BIT)
			return VK_SAMPLE_COUNT_2_BIT;

		return VK_SAMPLE_COUNT_1_BIT;
	}

	int InitVulkanSwapChain(VkSwapchainKHR &OldSwapChain)
	{
		OldSwapChain = VK_NULL_HANDLE;
		if(!CreateSwapChain(OldSwapChain))
			return -1;

		if(!GetSwapChainImageHandles())
			return -1;

		if(!CreateImageViews())
			return -1;

		if(!CreateMultiSamplerImageAttachments())
		{
			return -1;
		}

		m_LastPresentedSwapChainImageIndex = std::numeric_limits<decltype(m_LastPresentedSwapChainImageIndex)>::max();

		if(!CreateRenderPass(true))
			return -1;

		if(!CreateFramebuffers())
			return -1;

		if(!CreateStandardGraphicsPipeline("shader/vulkan/prim.vert.spv", "shader/vulkan/prim.frag.spv", false, false))
			return -1;

		if(!CreateStandardGraphicsPipeline("shader/vulkan/prim_textured.vert.spv", "shader/vulkan/prim_textured.frag.spv", true, false))
			return -1;

		if(!CreateStandardGraphicsPipeline("shader/vulkan/prim.vert.spv", "shader/vulkan/prim.frag.spv", false, true))
			return -1;

		if(!CreateStandard3DGraphicsPipeline("shader/vulkan/prim3d.vert.spv", "shader/vulkan/prim3d.frag.spv", false))
			return -1;

		if(!CreateStandard3DGraphicsPipeline("shader/vulkan/prim3d_textured.vert.spv", "shader/vulkan/prim3d_textured.frag.spv", true))
			return -1;

		if(!CreateTextGraphicsPipeline("shader/vulkan/text.vert.spv", "shader/vulkan/text.frag.spv"))
			return -1;

		if(!CreateTileGraphicsPipeline<false>("shader/vulkan/tile.vert.spv", "shader/vulkan/tile.frag.spv", 0))
			return -1;

		if(!CreateTileGraphicsPipeline<true>("shader/vulkan/tile_textured.vert.spv", "shader/vulkan/tile_textured.frag.spv", 0))
			return -1;

		if(!CreateTileGraphicsPipeline<false>("shader/vulkan/tile_border.vert.spv", "shader/vulkan/tile_border.frag.spv", 1))
			return -1;

		if(!CreateTileGraphicsPipeline<true>("shader/vulkan/tile_border_textured.vert.spv", "shader/vulkan/tile_border_textured.frag.spv", 1))
			return -1;

		if(!CreateTileGraphicsPipeline<false>("shader/vulkan/tile_border_line.vert.spv", "shader/vulkan/tile_border_line.frag.spv", 2))
			return -1;

		if(!CreateTileGraphicsPipeline<true>("shader/vulkan/tile_border_line_textured.vert.spv", "shader/vulkan/tile_border_line_textured.frag.spv", 2))
			return -1;

		if(!CreatePrimExGraphicsPipeline("shader/vulkan/primex_rotationless.vert.spv", "shader/vulkan/primex_rotationless.frag.spv", false, true))
			return -1;

		if(!CreatePrimExGraphicsPipeline("shader/vulkan/primex_tex_rotationless.vert.spv", "shader/vulkan/primex_tex_rotationless.frag.spv", true, true))
			return -1;

		if(!CreatePrimExGraphicsPipeline("shader/vulkan/primex.vert.spv", "shader/vulkan/primex.frag.spv", false, false))
			return -1;

		if(!CreatePrimExGraphicsPipeline("shader/vulkan/primex_tex.vert.spv", "shader/vulkan/primex_tex.frag.spv", true, false))
			return -1;

		if(!CreateSpriteMultiGraphicsPipeline("shader/vulkan/spritemulti.vert.spv", "shader/vulkan/spritemulti.frag.spv"))
			return -1;

		if(!CreateSpriteMultiPushGraphicsPipeline("shader/vulkan/spritemulti_push.vert.spv", "shader/vulkan/spritemulti_push.frag.spv"))
			return -1;

		if(!CreateQuadGraphicsPipeline<false>("shader/vulkan/quad.vert.spv", "shader/vulkan/quad.frag.spv"))
			return -1;

		if(!CreateQuadGraphicsPipeline<true>("shader/vulkan/quad_textured.vert.spv", "shader/vulkan/quad_textured.frag.spv"))
			return -1;

		if(!CreateQuadPushGraphicsPipeline<false>("shader/vulkan/quad_push.vert.spv", "shader/vulkan/quad_push.frag.spv"))
			return -1;

		if(!CreateQuadPushGraphicsPipeline<true>("shader/vulkan/quad_push_textured.vert.spv", "shader/vulkan/quad_push_textured.frag.spv"))
			return -1;

		m_SwapchainCreated = true;
		return 0;
	}

	template<bool IsFirstInitialization>
	int InitVulkan()
	{
		if(!CreateDescriptorSetLayouts())
			return -1;

		if(!CreateTextDescriptorSetLayout())
			return -1;

		if(!CreateSpriteMultiUniformDescriptorSetLayout())
			return -1;

		if(!CreateQuadUniformDescriptorSetLayout())
			return -1;

		if(IsFirstInitialization)
		{
			VkSwapchainKHR OldSwapChain = VK_NULL_HANDLE;
			if(InitVulkanSwapChain(OldSwapChain) != 0)
				return -1;
		}

		if(IsFirstInitialization)
		{
			if(!CreateCommandPool())
				return -1;
		}

		if(!CreateCommandBuffers())
			return -1;

		if(!CreateSyncObjects())
			return -1;

		if(IsFirstInitialization)
		{
			if(!CreateDescriptorPools())
				return -1;

			if(!CreateTextureSamplers())
				return -1;
		}

		m_vStreamedVertexBuffers.resize(m_ThreadCount);
		m_vStreamedUniformBuffers.resize(m_ThreadCount);
		for(size_t i = 0; i < m_ThreadCount; ++i)
		{
			m_vStreamedVertexBuffers[i].Init(m_SwapChainImageCount);
			m_vStreamedUniformBuffers[i].Init(m_SwapChainImageCount);
		}

		m_vLastPipeline.resize(m_ThreadCount, VK_NULL_HANDLE);

		m_vvFrameDelayedBufferCleanup.resize(m_SwapChainImageCount);
		m_vvFrameDelayedTextureCleanup.resize(m_SwapChainImageCount);
		m_vvFrameDelayedTextTexturesCleanup.resize(m_SwapChainImageCount);
		m_StagingBufferCache.Init(m_SwapChainImageCount);
		m_StagingBufferCacheImage.Init(m_SwapChainImageCount);
		m_VertexBufferCache.Init(m_SwapChainImageCount);
		for(auto &ImageBufferCache : m_ImageBufferCaches)
			ImageBufferCache.second.Init(m_SwapChainImageCount);

		m_vImageLastFrameCheck.resize(m_SwapChainImageCount, 0);

		if(IsFirstInitialization)
		{
			// check if image format supports linear blitting
			VkFormatProperties FormatProperties;
			vkGetPhysicalDeviceFormatProperties(m_VKGPU, VK_FORMAT_R8G8B8A8_UNORM, &FormatProperties);
			if((FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0)
			{
				m_AllowsLinearBlitting = true;
			}
			if((FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 && (FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0)
			{
				m_OptimalRGBAImageBlitting = true;
			}
			// check if image format supports blitting to linear tiled images
			if((FormatProperties.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0)
			{
				m_LinearRGBAImageBlitting = true;
			}

			vkGetPhysicalDeviceFormatProperties(m_VKGPU, m_VKSurfFormat.format, &FormatProperties);
			if((FormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0)
			{
				m_OptimalSwapChainImageBlitting = true;
			}
		}

		return 0;
	}

	VkCommandBuffer &GetMemoryCommandBuffer()
	{
		VkCommandBuffer &MemCommandBuffer = m_vMemoryCommandBuffers[m_CurImageIndex];
		if(!m_vUsedMemoryCommandBuffer[m_CurImageIndex])
		{
			m_vUsedMemoryCommandBuffer[m_CurImageIndex] = true;

			vkResetCommandBuffer(MemCommandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

			VkCommandBufferBeginInfo BeginInfo{};
			BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if(vkBeginCommandBuffer(MemCommandBuffer, &BeginInfo) != VK_SUCCESS)
			{
				SetError("Command buffer cannot be filled anymore.");
			}
		}
		return MemCommandBuffer;
	}

	VkCommandBuffer &GetGraphicCommandBuffer(size_t RenderThreadIndex)
	{
		if(m_ThreadCount < 2)
		{
			return m_vMainDrawCommandBuffers[m_CurImageIndex];
		}
		else
		{
			VkCommandBuffer &DrawCommandBuffer = m_vvThreadDrawCommandBuffers[RenderThreadIndex][m_CurImageIndex];
			if(!m_vvUsedThreadDrawCommandBuffer[RenderThreadIndex][m_CurImageIndex])
			{
				m_vvUsedThreadDrawCommandBuffer[RenderThreadIndex][m_CurImageIndex] = true;

				vkResetCommandBuffer(DrawCommandBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

				VkCommandBufferBeginInfo BeginInfo{};
				BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;

				VkCommandBufferInheritanceInfo InheretInfo{};
				InheretInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
				InheretInfo.framebuffer = m_vFramebufferList[m_CurImageIndex];
				InheretInfo.occlusionQueryEnable = VK_FALSE;
				InheretInfo.renderPass = m_VKRenderPass;
				InheretInfo.subpass = 0;

				BeginInfo.pInheritanceInfo = &InheretInfo;

				if(vkBeginCommandBuffer(DrawCommandBuffer, &BeginInfo) != VK_SUCCESS)
				{
					SetError("Thread draw command buffer cannot be filled anymore.");
				}
			}
			return DrawCommandBuffer;
		}
	}

	VkCommandBuffer &GetMainGraphicCommandBuffer()
	{
		return m_vMainDrawCommandBuffers[m_CurImageIndex];
	}

	/************************
	* STREAM BUFFERS SETUP
	************************/

	typedef std::function<void(SFrameBuffers &, VkBuffer, VkDeviceSize)> TNewMemFunc;

	// returns true, if the stream memory was just allocated
	template<typename TStreamMemName, typename TInstanceTypeName, size_t InstanceTypeCount, size_t BufferCreateCount, bool UsesCurrentCountOffset>
	void CreateStreamBuffer(TStreamMemName *&pBufferMem, TNewMemFunc &&NewMemFunc, SStreamMemory<TStreamMemName> &StreamUniformBuffer, VkBufferUsageFlagBits Usage, VkBuffer &NewBuffer, SDeviceMemoryBlock &NewBufferMem, size_t &BufferOffset, const void *pData, size_t DataSize)
	{
		VkBuffer Buffer = VK_NULL_HANDLE;
		SDeviceMemoryBlock BufferMem;
		size_t Offset = 0;

		uint8_t *pMem = nullptr;

		size_t it = 0;
		if(UsesCurrentCountOffset)
			it = StreamUniformBuffer.GetUsedCount(m_CurImageIndex);
		for(; it < StreamUniformBuffer.GetBuffers(m_CurImageIndex).size(); ++it)
		{
			auto &BufferOfFrame = StreamUniformBuffer.GetBuffers(m_CurImageIndex)[it];
			if(BufferOfFrame.m_Size >= DataSize + BufferOfFrame.m_UsedSize)
			{
				if(BufferOfFrame.m_UsedSize == 0)
					StreamUniformBuffer.IncreaseUsedCount(m_CurImageIndex);
				Buffer = BufferOfFrame.m_Buffer;
				BufferMem = BufferOfFrame.m_BufferMem;
				Offset = BufferOfFrame.m_UsedSize;
				BufferOfFrame.m_UsedSize += DataSize;
				pMem = (uint8_t *)BufferOfFrame.m_pMappedBufferData;
				pBufferMem = &BufferOfFrame;
				break;
			}
		}

		if(BufferMem.m_Mem == VK_NULL_HANDLE)
		{
			// create memory
			VkBuffer StreamBuffer;
			SDeviceMemoryBlock StreamBufferMemory;
			const VkDeviceSize NewBufferSingleSize = sizeof(TInstanceTypeName) * InstanceTypeCount;
			const VkDeviceSize NewBufferSize = NewBufferSingleSize * BufferCreateCount;
			CreateBuffer(NewBufferSize, MEMORY_BLOCK_USAGE_STREAM, Usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, StreamBuffer, StreamBufferMemory);

			void *pMappedData = nullptr;
			vkMapMemory(m_VKDevice, StreamBufferMemory.m_Mem, 0, VK_WHOLE_SIZE, 0, &pMappedData);

			size_t NewBufferIndex = StreamUniformBuffer.GetBuffers(m_CurImageIndex).size();
			for(size_t i = 0; i < BufferCreateCount; ++i)
			{
				StreamUniformBuffer.GetBuffers(m_CurImageIndex).push_back(TStreamMemName(StreamBuffer, StreamBufferMemory, NewBufferSingleSize * i, NewBufferSingleSize, 0, ((uint8_t *)pMappedData) + (NewBufferSingleSize * i)));
				StreamUniformBuffer.GetRanges(m_CurImageIndex).push_back({});
				NewMemFunc(StreamUniformBuffer.GetBuffers(m_CurImageIndex).back(), StreamBuffer, NewBufferSingleSize * i);
			}
			auto &NewStreamBuffer = StreamUniformBuffer.GetBuffers(m_CurImageIndex)[NewBufferIndex];

			Buffer = StreamBuffer;
			BufferMem = StreamBufferMemory;

			pBufferMem = &NewStreamBuffer;
			pMem = (uint8_t *)NewStreamBuffer.m_pMappedBufferData;
			Offset = NewStreamBuffer.m_OffsetInBuffer;
			NewStreamBuffer.m_UsedSize += DataSize;

			StreamUniformBuffer.IncreaseUsedCount(m_CurImageIndex);
		}

		{
			mem_copy(pMem + Offset, pData, (size_t)DataSize);
		}

		NewBuffer = Buffer;
		NewBufferMem = BufferMem;
		BufferOffset = Offset;
	}

	void CreateStreamVertexBuffer(size_t RenderThreadIndex, VkBuffer &NewBuffer, SDeviceMemoryBlock &NewBufferMem, size_t &BufferOffset, const void *pData, size_t DataSize)
	{
		SFrameBuffers *pStreamBuffer;
		CreateStreamBuffer<SFrameBuffers, GL_SVertexTex3DStream, CCommandBuffer::MAX_VERTICES * 2, 1, false>(
			pStreamBuffer, [](SFrameBuffers &, VkBuffer, VkDeviceSize) {}, m_vStreamedVertexBuffers[RenderThreadIndex], VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, NewBuffer, NewBufferMem, BufferOffset, pData, DataSize);
	}

	template<typename TName, size_t InstanceMaxParticleCount, size_t MaxInstances>
	void GetUniformBufferObjectImpl(size_t RenderThreadIndex, bool RequiresSharedStagesDescriptor, SStreamMemory<SFrameUniformBuffers> &StreamUniformBuffer, SDeviceDescriptorSet &DescrSet, const void *pData, size_t DataSize)
	{
		VkBuffer NewBuffer;
		SDeviceMemoryBlock NewBufferMem;
		size_t BufferOffset;
		SFrameUniformBuffers *pMem;
		CreateStreamBuffer<SFrameUniformBuffers, TName, InstanceMaxParticleCount, MaxInstances, true>(
			pMem,
			[this, RenderThreadIndex](SFrameBuffers &Mem, VkBuffer Buffer, VkDeviceSize MemOffset) {
				CreateUniformDescriptorSets(RenderThreadIndex, m_SpriteMultiUniformDescriptorSetLayout, ((SFrameUniformBuffers *)(&Mem))->m_aUniformSets.data(), 1, Buffer, InstanceMaxParticleCount * sizeof(TName), MemOffset);
				CreateUniformDescriptorSets(RenderThreadIndex, m_QuadUniformDescriptorSetLayout, &((SFrameUniformBuffers *)(&Mem))->m_aUniformSets[1], 1, Buffer, InstanceMaxParticleCount * sizeof(TName), MemOffset);
			},
			StreamUniformBuffer, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, NewBuffer, NewBufferMem, BufferOffset, pData, DataSize);

		DescrSet = pMem->m_aUniformSets[RequiresSharedStagesDescriptor ? 1 : 0];
	}

	void GetUniformBufferObject(size_t RenderThreadIndex, bool RequiresSharedStagesDescriptor, SDeviceDescriptorSet &DescrSet, size_t ParticleCount, const void *pData, size_t DataSize)
	{
		GetUniformBufferObjectImpl<IGraphics::SRenderSpriteInfo, 512, 128>(RenderThreadIndex, RequiresSharedStagesDescriptor, m_vStreamedUniformBuffers[RenderThreadIndex], DescrSet, pData, DataSize);
	}

	bool CreateIndexBuffer(void *pData, size_t DataSize, VkBuffer &Buffer, SDeviceMemoryBlock &Memory)
	{
		VkDeviceSize BufferDataSize = DataSize;

		auto StagingBuffer = GetStagingBuffer(pData, DataSize);

		SDeviceMemoryBlock VertexBufferMemory;
		VkBuffer VertexBuffer;
		CreateBuffer(BufferDataSize, MEMORY_BLOCK_USAGE_BUFFER, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VertexBuffer, VertexBufferMemory);

		MemoryBarrier(VertexBuffer, 0, BufferDataSize, VK_ACCESS_INDEX_READ_BIT, true);
		CopyBuffer(StagingBuffer.m_Buffer, VertexBuffer, StagingBuffer.m_HeapData.m_OffsetToAlign, 0, BufferDataSize);
		MemoryBarrier(VertexBuffer, 0, BufferDataSize, VK_ACCESS_INDEX_READ_BIT, false);

		UploadAndFreeStagingMemBlock(StagingBuffer);

		Buffer = VertexBuffer;
		Memory = VertexBufferMemory;
		return true;
	}

	void DestroyIndexBuffer(VkBuffer &Buffer, SDeviceMemoryBlock &Memory)
	{
		CleanBufferPair(0, Buffer, Memory);
	}

	/************************
	* COMMAND IMPLEMENTATION
	************************/
	template<typename TName>
	static bool IsInCommandRange(TName CMD, TName Min, TName Max)
	{
		return CMD >= Min && CMD < Max;
	}

	bool RunCommand(const CCommandBuffer::SCommand *pBaseCommand) override
	{
		if(IsInCommandRange<decltype(pBaseCommand->m_Cmd)>(pBaseCommand->m_Cmd, CCommandBuffer::CMD_FIRST, CCommandBuffer::CMD_COUNT))
		{
			auto &CallbackObj = m_aCommandCallbacks[CommandBufferCMDOff(CCommandBuffer::ECommandBufferCMD(pBaseCommand->m_Cmd))];
			SRenderCommandExecuteBuffer Buffer;
			Buffer.m_Command = (CCommandBuffer::ECommandBufferCMD)pBaseCommand->m_Cmd;
			Buffer.m_pRawCommand = pBaseCommand;
			Buffer.m_ThreadIndex = 0;

			if(m_CurCommandInPipe + 1 == m_CommandsInPipe && Buffer.m_Command != CCommandBuffer::CMD_FINISH)
			{
				m_LastCommandsInPipeThreadIndex = std::numeric_limits<decltype(m_LastCommandsInPipeThreadIndex)>::max();
			}

			bool CanStartThread = false;
			if(CallbackObj.m_IsRenderCommand)
			{
				bool ForceSingleThread = m_LastCommandsInPipeThreadIndex == std::numeric_limits<decltype(m_LastCommandsInPipeThreadIndex)>::max();

				size_t PotentiallyNextThread = (((m_CurCommandInPipe * (m_ThreadCount - 1)) / m_CommandsInPipe) + 1);
				if(PotentiallyNextThread - 1 > m_LastCommandsInPipeThreadIndex)
				{
					CanStartThread = true;
					m_LastCommandsInPipeThreadIndex = PotentiallyNextThread - 1;
				}
				Buffer.m_ThreadIndex = m_ThreadCount > 1 && !ForceSingleThread ? (m_LastCommandsInPipeThreadIndex + 1) : 0;
				CallbackObj.m_FillExecuteBuffer(Buffer, pBaseCommand);
				m_CurRenderCallCountInPipe += Buffer.m_EstimatedRenderCallCount;
			}
			bool Ret = true;
			if(!CallbackObj.m_IsRenderCommand || (Buffer.m_ThreadIndex == 0 && !m_RenderingPaused))
			{
				Ret = CallbackObj.m_CommandCB(pBaseCommand, Buffer);
			}
			else if(!m_RenderingPaused)
			{
				if(CanStartThread)
				{
					StartRenderThread(m_LastCommandsInPipeThreadIndex - 1);
				}
				m_vvThreadCommandLists[Buffer.m_ThreadIndex - 1].push_back(Buffer);
			}

			++m_CurCommandInPipe;
			return Ret;
		}

		if(m_CurCommandInPipe + 1 == m_CommandsInPipe)
		{
			m_LastCommandsInPipeThreadIndex = std::numeric_limits<decltype(m_LastCommandsInPipeThreadIndex)>::max();
		}
		++m_CurCommandInPipe;

		switch(pBaseCommand->m_Cmd)
		{
		case CCommandProcessorFragment_GLBase::CMD_INIT:
			Cmd_Init(static_cast<const SCommand_Init *>(pBaseCommand));
			break;
		case CCommandProcessorFragment_GLBase::CMD_SHUTDOWN:
			Cmd_Shutdown(static_cast<const SCommand_Shutdown *>(pBaseCommand));
			break;

		case CCommandProcessorFragment_GLBase::CMD_PRE_INIT:
			Cmd_PreInit(static_cast<const CCommandProcessorFragment_GLBase::SCommand_PreInit *>(pBaseCommand));
			break;
		case CCommandProcessorFragment_GLBase::CMD_POST_SHUTDOWN:
			Cmd_PostShutdown(static_cast<const CCommandProcessorFragment_GLBase::SCommand_PostShutdown *>(pBaseCommand));
			break;
		default:
			return false;
		}

		return true;
	}

	void Cmd_Init(const SCommand_Init *pCommand)
	{
		pCommand->m_pCapabilities->m_TileBuffering = true;
		pCommand->m_pCapabilities->m_QuadBuffering = true;
		pCommand->m_pCapabilities->m_TextBuffering = true;
		pCommand->m_pCapabilities->m_QuadContainerBuffering = true;
		pCommand->m_pCapabilities->m_ShaderSupport = true;

		pCommand->m_pCapabilities->m_MipMapping = true;
		pCommand->m_pCapabilities->m_3DTextures = false;
		pCommand->m_pCapabilities->m_2DArrayTextures = true;
		pCommand->m_pCapabilities->m_NPOTTextures = true;

		pCommand->m_pCapabilities->m_ContextMajor = 1;
		pCommand->m_pCapabilities->m_ContextMinor = 1;
		pCommand->m_pCapabilities->m_ContextPatch = 0;

		pCommand->m_pCapabilities->m_TrianglesAsQuads = true;

		m_GlobalTextureLodBIAS = g_Config.m_GfxGLTextureLODBIAS;
		m_pTextureMemoryUsage = pCommand->m_pTextureMemoryUsage;
		m_pBufferMemoryUsage = pCommand->m_pBufferMemoryUsage;
		m_pStreamMemoryUsage = pCommand->m_pStreamMemoryUsage;
		m_pStagingMemoryUsage = pCommand->m_pStagingMemoryUsage;

		m_MultiSamplingCount = (g_Config.m_GfxFsaaSamples & 0xFFFFFFFE); // ignore the uneven bit, only even multi sampling works

		TGLBackendReadPresentedImageData &ReadPresentedImgDataFunc = *pCommand->m_pReadPresentedImageDataFunc;
		ReadPresentedImgDataFunc = [this](uint32_t &Width, uint32_t &Height, uint32_t &Format, std::vector<uint8_t> &vDstData) { return GetPresentedImageData(Width, Height, Format, vDstData); };

		m_pWindow = pCommand->m_pWindow;

		*pCommand->m_pInitError = m_VKInstance != VK_NULL_HANDLE ? 0 : -1;

		if(m_VKInstance == VK_NULL_HANDLE)
		{
			*pCommand->m_pInitError = -2;
			return;
		}

		m_pStorage = pCommand->m_pStorage;
		if(InitVulkan<true>() != 0)
		{
			*pCommand->m_pInitError = -2;
			return;
		}

		std::array<uint32_t, (size_t)CCommandBuffer::MAX_VERTICES / 4 * 6> aIndices;
		int Primq = 0;
		for(int i = 0; i < CCommandBuffer::MAX_VERTICES / 4 * 6; i += 6)
		{
			aIndices[i] = Primq;
			aIndices[i + 1] = Primq + 1;
			aIndices[i + 2] = Primq + 2;
			aIndices[i + 3] = Primq;
			aIndices[i + 4] = Primq + 2;
			aIndices[i + 5] = Primq + 3;
			Primq += 4;
		}

		PrepareFrame();
		if(m_HasError)
		{
			*pCommand->m_pInitError = -2;
			return;
		}

		if(!CreateIndexBuffer(aIndices.data(), sizeof(uint32_t) * aIndices.size(), m_IndexBuffer, m_IndexBufferMemory))
		{
			*pCommand->m_pInitError = -2;
			return;
		}
		if(!CreateIndexBuffer(aIndices.data(), sizeof(uint32_t) * aIndices.size(), m_RenderIndexBuffer, m_RenderIndexBufferMemory))
		{
			*pCommand->m_pInitError = -2;
			return;
		}
		m_CurRenderIndexPrimitiveCount = CCommandBuffer::MAX_VERTICES / 4;

		m_CanAssert = true;
	}

	void Cmd_Shutdown(const SCommand_Shutdown *pCommand)
	{
		vkDeviceWaitIdle(m_VKDevice);

		DestroyIndexBuffer(m_IndexBuffer, m_IndexBufferMemory);
		DestroyIndexBuffer(m_RenderIndexBuffer, m_RenderIndexBufferMemory);

		CleanupVulkan<true>();
	}

	void Cmd_Texture_Update(const CCommandBuffer::SCommand_Texture_Update *pCommand)
	{
		size_t IndexTex = pCommand->m_Slot;

		void *pData = pCommand->m_pData;

		UpdateTexture(IndexTex, VK_FORMAT_B8G8R8A8_UNORM, pData, pCommand->m_X, pCommand->m_Y, pCommand->m_Width, pCommand->m_Height, TexFormatToImageColorChannelCount(pCommand->m_Format));

		free(pData);
	}

	void Cmd_Texture_Destroy(const CCommandBuffer::SCommand_Texture_Destroy *pCommand)
	{
		size_t ImageIndex = (size_t)pCommand->m_Slot;
		auto &Texture = m_vTextures[ImageIndex];

		m_vvFrameDelayedTextureCleanup[m_CurImageIndex].push_back(Texture);

		Texture = CTexture{};
	}

	void Cmd_Texture_Create(const CCommandBuffer::SCommand_Texture_Create *pCommand)
	{
		int Slot = pCommand->m_Slot;
		int Width = pCommand->m_Width;
		int Height = pCommand->m_Height;
		int PixelSize = pCommand->m_PixelSize;
		int Format = pCommand->m_Format;
		int StoreFormat = pCommand->m_StoreFormat;
		int Flags = pCommand->m_Flags;
		void *pData = pCommand->m_pData;

		CreateTextureCMD(Slot, Width, Height, PixelSize, TextureFormatToVulkanFormat(Format), TextureFormatToVulkanFormat(StoreFormat), Flags, pData);

		free(pData);
	}

	void Cmd_TextTextures_Create(const CCommandBuffer::SCommand_TextTextures_Create *pCommand)
	{
		int Slot = pCommand->m_Slot;
		int SlotOutline = pCommand->m_SlotOutline;
		int Width = pCommand->m_Width;
		int Height = pCommand->m_Height;

		void *pTmpData = pCommand->m_pTextData;
		void *pTmpData2 = pCommand->m_pTextOutlineData;

		CreateTextureCMD(Slot, Width, Height, 1, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, CCommandBuffer::TEXFLAG_NOMIPMAPS, pTmpData);
		CreateTextureCMD(SlotOutline, Width, Height, 1, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, CCommandBuffer::TEXFLAG_NOMIPMAPS, pTmpData2);

		CreateNewTextDescriptorSets(Slot, SlotOutline);

		free(pTmpData);
		free(pTmpData2);
	}

	void Cmd_TextTextures_Destroy(const CCommandBuffer::SCommand_TextTextures_Destroy *pCommand)
	{
		size_t ImageIndex = (size_t)pCommand->m_Slot;
		size_t ImageIndexOutline = (size_t)pCommand->m_SlotOutline;
		auto &Texture = m_vTextures[ImageIndex];
		auto &TextureOutline = m_vTextures[ImageIndexOutline];

		m_vvFrameDelayedTextTexturesCleanup[m_CurImageIndex].push_back({Texture, TextureOutline});

		Texture = {};
		TextureOutline = {};
	}

	void Cmd_TextTexture_Update(const CCommandBuffer::SCommand_TextTexture_Update *pCommand)
	{
		size_t IndexTex = pCommand->m_Slot;

		void *pData = pCommand->m_pData;

		UpdateTexture(IndexTex, VK_FORMAT_R8_UNORM, pData, pCommand->m_X, pCommand->m_Y, pCommand->m_Width, pCommand->m_Height, 1);

		free(pData);
	}

	void Cmd_Clear_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_Clear *pCommand)
	{
		if(!pCommand->m_ForceClear)
		{
			bool ColorChanged = m_aClearColor[0] != pCommand->m_Color.r || m_aClearColor[1] != pCommand->m_Color.g ||
					    m_aClearColor[2] != pCommand->m_Color.b || m_aClearColor[3] != pCommand->m_Color.a;
			m_aClearColor[0] = pCommand->m_Color.r;
			m_aClearColor[1] = pCommand->m_Color.g;
			m_aClearColor[2] = pCommand->m_Color.b;
			m_aClearColor[3] = pCommand->m_Color.a;
			if(ColorChanged)
				ExecBuffer.m_ClearColorInRenderThread = true;
		}
		else
		{
			ExecBuffer.m_ClearColorInRenderThread = true;
		}
		ExecBuffer.m_EstimatedRenderCallCount = 0;
	}

	void Cmd_Clear(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_Clear *pCommand)
	{
		if(ExecBuffer.m_ClearColorInRenderThread)
		{
			std::array<VkClearAttachment, 1> aAttachments = {VkClearAttachment{VK_IMAGE_ASPECT_COLOR_BIT, 0, VkClearValue{VkClearColorValue{{pCommand->m_Color.r, pCommand->m_Color.g, pCommand->m_Color.b, pCommand->m_Color.a}}}}};
			std::array<VkClearRect, 1> aClearRects = {VkClearRect{{{0, 0}, m_VKSwapImgAndViewportExtent.m_SwapImageViewport}, 0, 1}};
			vkCmdClearAttachments(GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex), aAttachments.size(), aAttachments.data(), aClearRects.size(), aClearRects.data());
		}
	}

	void Cmd_Render_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_Render *pCommand)
	{
		bool IsTextured = GetIsTextured(pCommand->m_State);
		if(IsTextured)
		{
			size_t AddressModeIndex = GetAddressModeIndex(pCommand->m_State);
			auto &DescrSet = m_vTextures[pCommand->m_State.m_Texture].m_aVKStandardTexturedDescrSets[AddressModeIndex];
			ExecBuffer.m_aDescriptors[0] = DescrSet;
		}

		ExecBuffer.m_IndexBuffer = m_IndexBuffer;

		ExecBuffer.m_EstimatedRenderCallCount = 1;

		ExecBufferFillDynamicStates(pCommand->m_State, ExecBuffer);
	}

	void Cmd_Render(const CCommandBuffer::SCommand_Render *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		RenderStandard<CCommandBuffer::SVertex, false>(ExecBuffer, pCommand->m_State, pCommand->m_PrimType, pCommand->m_pVertices, pCommand->m_PrimCount);
	}

	void Cmd_Screenshot(const CCommandBuffer::SCommand_TrySwapAndScreenshot *pCommand)
	{
		NextFrame();
		*pCommand->m_pSwapped = true;

		uint32_t Width;
		uint32_t Height;
		uint32_t Format;
		if(GetPresentedImageDataImpl(Width, Height, Format, m_vScreenshotHelper, false, true))
		{
			size_t ImgSize = (size_t)Width * (size_t)Height * (size_t)4;
			pCommand->m_pImage->m_pData = malloc(ImgSize);
			mem_copy(pCommand->m_pImage->m_pData, m_vScreenshotHelper.data(), ImgSize);
		}
		else
		{
			pCommand->m_pImage->m_pData = nullptr;
		}
		pCommand->m_pImage->m_Width = (int)Width;
		pCommand->m_pImage->m_Height = (int)Height;
		pCommand->m_pImage->m_Format = (int)Format;
	}

	void Cmd_RenderTex3D_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderTex3D *pCommand)
	{
		bool IsTextured = GetIsTextured(pCommand->m_State);
		if(IsTextured)
		{
			auto &DescrSet = m_vTextures[pCommand->m_State.m_Texture].m_VKStandard3DTexturedDescrSet;
			ExecBuffer.m_aDescriptors[0] = DescrSet;
		}

		ExecBuffer.m_IndexBuffer = m_IndexBuffer;

		ExecBuffer.m_EstimatedRenderCallCount = 1;

		ExecBufferFillDynamicStates(pCommand->m_State, ExecBuffer);
	}

	void Cmd_RenderTex3D(const CCommandBuffer::SCommand_RenderTex3D *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		RenderStandard<CCommandBuffer::SVertexTex3DStream, true>(ExecBuffer, pCommand->m_State, pCommand->m_PrimType, pCommand->m_pVertices, pCommand->m_PrimCount);
	}

	void Cmd_Update_Viewport_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_Update_Viewport *pCommand)
	{
		ExecBuffer.m_EstimatedRenderCallCount = 0;
	}

	void Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand)
	{
		if(pCommand->m_ByResize)
		{
			if(IsVerbose())
			{
				dbg_msg("vulkan", "queueing swap chain recreation because the viewport changed");
			}
			m_CanvasWidth = (uint32_t)pCommand->m_Width;
			m_CanvasHeight = (uint32_t)pCommand->m_Height;
			m_RecreateSwapChain = true;
		}
		else if(!pCommand->m_ByResize)
		{
			auto Viewport = m_VKSwapImgAndViewportExtent.GetPresentedImageViewport();
			if(pCommand->m_X != 0 || pCommand->m_Y != 0 || (uint32_t)pCommand->m_Width != Viewport.width || (uint32_t)pCommand->m_Height != Viewport.height)
			{
				m_HasDynamicViewport = true;

				// convert viewport from OGL to vulkan
				int32_t ViewportY = (int32_t)Viewport.height - ((int32_t)pCommand->m_Y + (int32_t)pCommand->m_Height);
				uint32_t ViewportH = (int32_t)pCommand->m_Height;
				m_DynamicViewportOffset = {(int32_t)pCommand->m_X, ViewportY};
				m_DynamicViewportSize = {(uint32_t)pCommand->m_Width, ViewportH};
			}
			else
			{
				m_HasDynamicViewport = false;
			}
		}
	}

	void Cmd_VSync(const CCommandBuffer::SCommand_VSync *pCommand)
	{
		if(IsVerbose())
		{
			dbg_msg("vulkan", "queueing swap chain recreation because vsync was changed");
		}
		m_RecreateSwapChain = true;
		*pCommand->m_pRetOk = true;
	}

	void Cmd_MultiSampling(const CCommandBuffer::SCommand_MultiSampling *pCommand)
	{
		if(IsVerbose())
		{
			dbg_msg("vulkan", "queueing swap chain recreation because multi sampling was changed");
		}
		m_RecreateSwapChain = true;

		uint32_t MSCount = (std::min(pCommand->m_RequestedMultiSamplingCount, (uint32_t)GetMaxSampleCount()) & 0xFFFFFFFE); // ignore the uneven bits
		m_NextMultiSamplingCount = MSCount;

		*pCommand->m_pRetMultiSamplingCount = MSCount;
		*pCommand->m_pRetOk = true;
	}

	void Cmd_Finish(const CCommandBuffer::SCommand_Finish *pCommand)
	{ // just ignore it with vulkan
	}

	void Cmd_Swap(const CCommandBuffer::SCommand_Swap *pCommand)
	{
		NextFrame();
	}

	void Cmd_CreateBufferObject(const CCommandBuffer::SCommand_CreateBufferObject *pCommand)
	{
		bool IsOneFrameBuffer = (pCommand->m_Flags & IGraphics::EBufferObjectCreateFlags::BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT) != 0;
		CreateBufferObject((size_t)pCommand->m_BufferIndex, pCommand->m_pUploadData, (VkDeviceSize)pCommand->m_DataSize, IsOneFrameBuffer);
		if(pCommand->m_DeletePointer)
			free(pCommand->m_pUploadData);
	}

	void Cmd_UpdateBufferObject(const CCommandBuffer::SCommand_UpdateBufferObject *pCommand)
	{
		size_t BufferIndex = (size_t)pCommand->m_BufferIndex;
		bool DeletePointer = pCommand->m_DeletePointer;
		VkDeviceSize Offset = (VkDeviceSize)((intptr_t)pCommand->m_pOffset);
		void *pUploadData = pCommand->m_pUploadData;
		VkDeviceSize DataSize = (VkDeviceSize)pCommand->m_DataSize;

		auto StagingBuffer = GetStagingBuffer(pUploadData, DataSize);

		auto &MemBlock = m_vBufferObjects[BufferIndex].m_BufferObject.m_Mem;
		VkBuffer VertexBuffer = MemBlock.m_Buffer;
		MemoryBarrier(VertexBuffer, Offset + MemBlock.m_HeapData.m_OffsetToAlign, DataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, true);
		CopyBuffer(StagingBuffer.m_Buffer, VertexBuffer, StagingBuffer.m_HeapData.m_OffsetToAlign, Offset + MemBlock.m_HeapData.m_OffsetToAlign, DataSize);
		MemoryBarrier(VertexBuffer, Offset + MemBlock.m_HeapData.m_OffsetToAlign, DataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, false);

		UploadAndFreeStagingMemBlock(StagingBuffer);

		if(DeletePointer)
			free(pUploadData);
	}

	void Cmd_RecreateBufferObject(const CCommandBuffer::SCommand_RecreateBufferObject *pCommand)
	{
		DeleteBufferObject((size_t)pCommand->m_BufferIndex);
		bool IsOneFrameBuffer = (pCommand->m_Flags & IGraphics::EBufferObjectCreateFlags::BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT) != 0;
		CreateBufferObject((size_t)pCommand->m_BufferIndex, pCommand->m_pUploadData, (VkDeviceSize)pCommand->m_DataSize, IsOneFrameBuffer);
	}

	void Cmd_CopyBufferObject(const CCommandBuffer::SCommand_CopyBufferObject *pCommand)
	{
		size_t ReadBufferIndex = (size_t)pCommand->m_ReadBufferIndex;
		size_t WriteBufferIndex = (size_t)pCommand->m_WriteBufferIndex;
		auto &ReadMemBlock = m_vBufferObjects[ReadBufferIndex].m_BufferObject.m_Mem;
		auto &WriteMemBlock = m_vBufferObjects[WriteBufferIndex].m_BufferObject.m_Mem;
		VkBuffer ReadBuffer = ReadMemBlock.m_Buffer;
		VkBuffer WriteBuffer = WriteMemBlock.m_Buffer;

		VkDeviceSize DataSize = (VkDeviceSize)pCommand->m_CopySize;
		VkDeviceSize ReadOffset = (VkDeviceSize)pCommand->m_ReadOffset + ReadMemBlock.m_HeapData.m_OffsetToAlign;
		VkDeviceSize WriteOffset = (VkDeviceSize)pCommand->m_WriteOffset + WriteMemBlock.m_HeapData.m_OffsetToAlign;

		MemoryBarrier(ReadBuffer, ReadOffset, DataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, true);
		MemoryBarrier(WriteBuffer, WriteOffset, DataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, true);
		CopyBuffer(ReadBuffer, WriteBuffer, ReadOffset, WriteOffset, DataSize);
		MemoryBarrier(WriteBuffer, WriteOffset, DataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, false);
		MemoryBarrier(ReadBuffer, ReadOffset, DataSize, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, false);
	}

	void Cmd_DeleteBufferObject(const CCommandBuffer::SCommand_DeleteBufferObject *pCommand)
	{
		size_t BufferIndex = (size_t)pCommand->m_BufferIndex;
		DeleteBufferObject(BufferIndex);
	}

	void Cmd_CreateBufferContainer(const CCommandBuffer::SCommand_CreateBufferContainer *pCommand)
	{
		size_t ContainerIndex = (size_t)pCommand->m_BufferContainerIndex;
		while(ContainerIndex >= m_vBufferContainers.size())
			m_vBufferContainers.resize((m_vBufferContainers.size() * 2) + 1);

		m_vBufferContainers[ContainerIndex].m_BufferObjectIndex = pCommand->m_VertBufferBindingIndex;
	}

	void Cmd_UpdateBufferContainer(const CCommandBuffer::SCommand_UpdateBufferContainer *pCommand)
	{
		size_t ContainerIndex = (size_t)pCommand->m_BufferContainerIndex;
		m_vBufferContainers[ContainerIndex].m_BufferObjectIndex = pCommand->m_VertBufferBindingIndex;
	}

	void Cmd_DeleteBufferContainer(const CCommandBuffer::SCommand_DeleteBufferContainer *pCommand)
	{
		size_t ContainerIndex = (size_t)pCommand->m_BufferContainerIndex;
		bool DeleteAllBO = pCommand->m_DestroyAllBO;
		if(DeleteAllBO)
		{
			size_t BufferIndex = (size_t)m_vBufferContainers[ContainerIndex].m_BufferObjectIndex;
			DeleteBufferObject(BufferIndex);
		}
	}

	void Cmd_IndicesRequiredNumNotify(const CCommandBuffer::SCommand_IndicesRequiredNumNotify *pCommand)
	{
		size_t IndicesCount = pCommand->m_RequiredIndicesNum;
		if(m_CurRenderIndexPrimitiveCount < IndicesCount / 6)
		{
			m_vvFrameDelayedBufferCleanup[m_CurImageIndex].push_back({m_RenderIndexBuffer, m_RenderIndexBufferMemory});
			std::vector<uint32_t> vIndices(IndicesCount);
			uint32_t Primq = 0;
			for(size_t i = 0; i < IndicesCount; i += 6)
			{
				vIndices[i] = Primq;
				vIndices[i + 1] = Primq + 1;
				vIndices[i + 2] = Primq + 2;
				vIndices[i + 3] = Primq;
				vIndices[i + 4] = Primq + 2;
				vIndices[i + 5] = Primq + 3;
				Primq += 4;
			}
			CreateIndexBuffer(vIndices.data(), vIndices.size() * sizeof(uint32_t), m_RenderIndexBuffer, m_RenderIndexBufferMemory);
			m_CurRenderIndexPrimitiveCount = IndicesCount / 6;
		}
	}

	void Cmd_RenderTileLayer_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderTileLayer *pCommand)
	{
		RenderTileLayer_FillExecuteBuffer(ExecBuffer, pCommand->m_IndicesDrawNum, pCommand->m_State, pCommand->m_BufferContainerIndex);
	}

	void Cmd_RenderTileLayer(const CCommandBuffer::SCommand_RenderTileLayer *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		int Type = 0;
		vec2 Dir{};
		vec2 Off{};
		int32_t JumpIndex = 0;
		RenderTileLayer(ExecBuffer, pCommand->m_State, Type, pCommand->m_Color, Dir, Off, JumpIndex, (size_t)pCommand->m_IndicesDrawNum, pCommand->m_pIndicesOffsets, pCommand->m_pDrawCount, 1);
	}

	void Cmd_RenderBorderTile_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderBorderTile *pCommand)
	{
		RenderTileLayer_FillExecuteBuffer(ExecBuffer, 1, pCommand->m_State, pCommand->m_BufferContainerIndex);
	}

	void Cmd_RenderBorderTile(const CCommandBuffer::SCommand_RenderBorderTile *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		int Type = 1;
		vec2 Dir = pCommand->m_Dir;
		vec2 Off = pCommand->m_Offset;
		unsigned int DrawNum = 6;
		RenderTileLayer(ExecBuffer, pCommand->m_State, Type, pCommand->m_Color, Dir, Off, pCommand->m_JumpIndex, (size_t)1, &pCommand->m_pIndicesOffset, &DrawNum, pCommand->m_DrawNum);
	}

	void Cmd_RenderBorderTileLine_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderBorderTileLine *pCommand)
	{
		RenderTileLayer_FillExecuteBuffer(ExecBuffer, 1, pCommand->m_State, pCommand->m_BufferContainerIndex);
	}

	void Cmd_RenderBorderTileLine(const CCommandBuffer::SCommand_RenderBorderTileLine *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		int Type = 2;
		vec2 Dir = pCommand->m_Dir;
		vec2 Off = pCommand->m_Offset;
		RenderTileLayer(ExecBuffer, pCommand->m_State, Type, pCommand->m_Color, Dir, Off, 0, (size_t)1, &pCommand->m_pIndicesOffset, &pCommand->m_IndexDrawNum, pCommand->m_DrawNum);
	}

	void Cmd_RenderQuadLayer_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderQuadLayer *pCommand)
	{
		size_t BufferContainerIndex = (size_t)pCommand->m_BufferContainerIndex;
		size_t BufferObjectIndex = (size_t)m_vBufferContainers[BufferContainerIndex].m_BufferObjectIndex;
		auto &BufferObject = m_vBufferObjects[BufferObjectIndex];

		ExecBuffer.m_Buffer = BufferObject.m_CurBuffer;
		ExecBuffer.m_BufferOff = BufferObject.m_CurBufferOffset;

		bool IsTextured = GetIsTextured(pCommand->m_State);
		if(IsTextured)
		{
			size_t AddressModeIndex = GetAddressModeIndex(pCommand->m_State);
			auto &DescrSet = m_vTextures[pCommand->m_State.m_Texture].m_aVKStandardTexturedDescrSets[AddressModeIndex];
			ExecBuffer.m_aDescriptors[0] = DescrSet;
		}

		ExecBuffer.m_IndexBuffer = m_RenderIndexBuffer;

		ExecBuffer.m_EstimatedRenderCallCount = ((pCommand->m_QuadNum - 1) / gs_GraphicsMaxQuadsRenderCount) + 1;

		ExecBufferFillDynamicStates(pCommand->m_State, ExecBuffer);
	}

	void Cmd_RenderQuadLayer(const CCommandBuffer::SCommand_RenderQuadLayer *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(pCommand->m_State, m);

		bool CanBePushed = pCommand->m_QuadNum == 1;

		bool IsTextured;
		size_t BlendModeIndex;
		size_t DynamicIndex;
		size_t AddressModeIndex;
		GetStateIndices(ExecBuffer, pCommand->m_State, IsTextured, BlendModeIndex, DynamicIndex, AddressModeIndex);
		auto &PipeLayout = GetPipeLayout(CanBePushed ? m_QuadPushPipeline : m_QuadPipeline, IsTextured, BlendModeIndex, DynamicIndex);
		auto &PipeLine = GetPipeline(CanBePushed ? m_QuadPushPipeline : m_QuadPipeline, IsTextured, BlendModeIndex, DynamicIndex);

		auto &CommandBuffer = GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex);

		BindPipeline(ExecBuffer.m_ThreadIndex, CommandBuffer, ExecBuffer, PipeLine, pCommand->m_State);

		std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, 0, VK_INDEX_TYPE_UINT32);

		if(IsTextured)
		{
			vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 0, 1, &ExecBuffer.m_aDescriptors[0].m_Descriptor, 0, nullptr);
		}

		if(CanBePushed)
		{
			SUniformQuadPushGPos PushConstantVertex;

			PushConstantVertex.m_BOPush = pCommand->m_pQuadInfo[0];

			mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
			PushConstantVertex.m_QuadOffset = pCommand->m_QuadOffset;

			vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SUniformQuadPushGPos), &PushConstantVertex);
		}
		else
		{
			SUniformQuadGPos PushConstantVertex;
			mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
			PushConstantVertex.m_QuadOffset = pCommand->m_QuadOffset;

			vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstantVertex), &PushConstantVertex);
		}

		uint32_t DrawCount = (uint32_t)pCommand->m_QuadNum;
		size_t RenderOffset = 0;

		while(DrawCount > 0)
		{
			uint32_t RealDrawCount = (DrawCount > gs_GraphicsMaxQuadsRenderCount ? gs_GraphicsMaxQuadsRenderCount : DrawCount);

			VkDeviceSize IndexOffset = (VkDeviceSize)((ptrdiff_t)(pCommand->m_QuadOffset + RenderOffset) * 6);
			if(!CanBePushed)
			{
				// create uniform buffer
				SDeviceDescriptorSet UniDescrSet;
				GetUniformBufferObject(ExecBuffer.m_ThreadIndex, true, UniDescrSet, RealDrawCount, (const float *)(pCommand->m_pQuadInfo + RenderOffset), RealDrawCount * sizeof(SQuadRenderInfo));

				vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, IsTextured ? 1 : 0, 1, &UniDescrSet.m_Descriptor, 0, nullptr);
				if(RenderOffset > 0)
				{
					int32_t QuadOffset = pCommand->m_QuadOffset + RenderOffset;
					vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(SUniformQuadGPos) - sizeof(int32_t), sizeof(int32_t), &QuadOffset);
				}
			}

			vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(RealDrawCount * 6), 1, IndexOffset, 0, 0);

			RenderOffset += RealDrawCount;
			DrawCount -= RealDrawCount;
		}
	}

	void Cmd_RenderText_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderText *pCommand)
	{
		size_t BufferContainerIndex = (size_t)pCommand->m_BufferContainerIndex;
		size_t BufferObjectIndex = (size_t)m_vBufferContainers[BufferContainerIndex].m_BufferObjectIndex;
		auto &BufferObject = m_vBufferObjects[BufferObjectIndex];

		ExecBuffer.m_Buffer = BufferObject.m_CurBuffer;
		ExecBuffer.m_BufferOff = BufferObject.m_CurBufferOffset;

		auto &TextTextureDescr = m_vTextures[pCommand->m_TextTextureIndex].m_VKTextDescrSet;
		ExecBuffer.m_aDescriptors[0] = TextTextureDescr;

		ExecBuffer.m_IndexBuffer = m_RenderIndexBuffer;

		ExecBuffer.m_EstimatedRenderCallCount = 1;

		ExecBufferFillDynamicStates(pCommand->m_State, ExecBuffer);
	}

	void Cmd_RenderText(const CCommandBuffer::SCommand_RenderText *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(pCommand->m_State, m);

		bool IsTextured;
		size_t BlendModeIndex;
		size_t DynamicIndex;
		size_t AddressModeIndex;
		GetStateIndices(ExecBuffer, pCommand->m_State, IsTextured, BlendModeIndex, DynamicIndex, AddressModeIndex);
		IsTextured = true; // text is always textured
		auto &PipeLayout = GetPipeLayout(m_TextPipeline, IsTextured, BlendModeIndex, DynamicIndex);
		auto &PipeLine = GetPipeline(m_TextPipeline, IsTextured, BlendModeIndex, DynamicIndex);

		auto &CommandBuffer = GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex);

		BindPipeline(ExecBuffer.m_ThreadIndex, CommandBuffer, ExecBuffer, PipeLine, pCommand->m_State);

		std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, 0, VK_INDEX_TYPE_UINT32);

		vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 0, 1, &ExecBuffer.m_aDescriptors[0].m_Descriptor, 0, nullptr);

		SUniformGTextPos PosTexSizeConstant;
		mem_copy(PosTexSizeConstant.m_aPos, m.data(), m.size() * sizeof(float));
		PosTexSizeConstant.m_TextureSize = pCommand->m_TextureSize;

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGTextPos), &PosTexSizeConstant);

		SUniformTextFragment FragmentConstants;

		FragmentConstants.m_Constants.m_TextColor = pCommand->m_TextColor;
		FragmentConstants.m_Constants.m_TextOutlineColor = pCommand->m_TextOutlineColor;
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformGTextPos) + sizeof(SUniformTextGFragmentOffset), sizeof(SUniformTextFragment), &FragmentConstants);

		vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(pCommand->m_DrawNum), 1, 0, 0, 0);
	}

	void BufferContainer_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SState &State, size_t BufferContainerIndex, size_t DrawCalls)
	{
		size_t BufferObjectIndex = (size_t)m_vBufferContainers[BufferContainerIndex].m_BufferObjectIndex;
		auto &BufferObject = m_vBufferObjects[BufferObjectIndex];

		ExecBuffer.m_Buffer = BufferObject.m_CurBuffer;
		ExecBuffer.m_BufferOff = BufferObject.m_CurBufferOffset;

		bool IsTextured = GetIsTextured(State);
		if(IsTextured)
		{
			size_t AddressModeIndex = GetAddressModeIndex(State);
			auto &DescrSet = m_vTextures[State.m_Texture].m_aVKStandardTexturedDescrSets[AddressModeIndex];
			ExecBuffer.m_aDescriptors[0] = DescrSet;
		}

		ExecBuffer.m_IndexBuffer = m_RenderIndexBuffer;

		ExecBuffer.m_EstimatedRenderCallCount = DrawCalls;

		ExecBufferFillDynamicStates(State, ExecBuffer);
	}

	void Cmd_RenderQuadContainer_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderQuadContainer *pCommand)
	{
		BufferContainer_FillExecuteBuffer(ExecBuffer, pCommand->m_State, (size_t)pCommand->m_BufferContainerIndex, 1);
	}

	void Cmd_RenderQuadContainer(const CCommandBuffer::SCommand_RenderQuadContainer *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(pCommand->m_State, m);

		bool IsTextured;
		size_t BlendModeIndex;
		size_t DynamicIndex;
		size_t AddressModeIndex;
		GetStateIndices(ExecBuffer, pCommand->m_State, IsTextured, BlendModeIndex, DynamicIndex, AddressModeIndex);
		auto &PipeLayout = GetStandardPipeLayout(false, IsTextured, BlendModeIndex, DynamicIndex);
		auto &PipeLine = GetStandardPipe(false, IsTextured, BlendModeIndex, DynamicIndex);

		auto &CommandBuffer = GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex);

		BindPipeline(ExecBuffer.m_ThreadIndex, CommandBuffer, ExecBuffer, PipeLine, pCommand->m_State);

		std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		VkDeviceSize IndexOffset = (VkDeviceSize)((ptrdiff_t)pCommand->m_pOffset);

		vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, IndexOffset, VK_INDEX_TYPE_UINT32);

		if(IsTextured)
		{
			vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 0, 1, &ExecBuffer.m_aDescriptors[0].m_Descriptor, 0, nullptr);
		}

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformGPos), m.data());

		vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(pCommand->m_DrawNum), 1, 0, 0, 0);
	}

	void Cmd_RenderQuadContainerEx_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderQuadContainerEx *pCommand)
	{
		BufferContainer_FillExecuteBuffer(ExecBuffer, pCommand->m_State, (size_t)pCommand->m_BufferContainerIndex, 1);
	}

	void Cmd_RenderQuadContainerEx(const CCommandBuffer::SCommand_RenderQuadContainerEx *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(pCommand->m_State, m);

		bool IsRotationless = !(pCommand->m_Rotation != 0);
		bool IsTextured;
		size_t BlendModeIndex;
		size_t DynamicIndex;
		size_t AddressModeIndex;
		GetStateIndices(ExecBuffer, pCommand->m_State, IsTextured, BlendModeIndex, DynamicIndex, AddressModeIndex);
		auto &PipeLayout = GetPipeLayout(IsRotationless ? m_PrimExRotationlessPipeline : m_PrimExPipeline, IsTextured, BlendModeIndex, DynamicIndex);
		auto &PipeLine = GetPipeline(IsRotationless ? m_PrimExRotationlessPipeline : m_PrimExPipeline, IsTextured, BlendModeIndex, DynamicIndex);

		auto &CommandBuffer = GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex);

		BindPipeline(ExecBuffer.m_ThreadIndex, CommandBuffer, ExecBuffer, PipeLine, pCommand->m_State);

		std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		VkDeviceSize IndexOffset = (VkDeviceSize)((ptrdiff_t)pCommand->m_pOffset);

		vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, IndexOffset, VK_INDEX_TYPE_UINT32);

		if(IsTextured)
		{
			vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 0, 1, &ExecBuffer.m_aDescriptors[0].m_Descriptor, 0, nullptr);
		}

		SUniformPrimExGVertColor PushConstantColor;
		SUniformPrimExGPos PushConstantVertex;
		size_t VertexPushConstantSize = sizeof(PushConstantVertex);

		PushConstantColor = pCommand->m_VertexColor;
		mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));

		if(!IsRotationless)
		{
			PushConstantVertex.m_Rotation = pCommand->m_Rotation;
			PushConstantVertex.m_Center = {pCommand->m_Center.x, pCommand->m_Center.y};
		}
		else
		{
			VertexPushConstantSize = sizeof(SUniformPrimExGPosRotationless);
		}

		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, VertexPushConstantSize, &PushConstantVertex);
		vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformPrimExGPos) + sizeof(SUniformPrimExGVertColorAlign), sizeof(PushConstantColor), &PushConstantColor);

		vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(pCommand->m_DrawNum), 1, 0, 0, 0);
	}

	void Cmd_RenderQuadContainerAsSpriteMultiple_FillExecuteBuffer(SRenderCommandExecuteBuffer &ExecBuffer, const CCommandBuffer::SCommand_RenderQuadContainerAsSpriteMultiple *pCommand)
	{
		BufferContainer_FillExecuteBuffer(ExecBuffer, pCommand->m_State, (size_t)pCommand->m_BufferContainerIndex, ((pCommand->m_DrawCount - 1) / gs_GraphicsMaxParticlesRenderCount) + 1);
	}

	void Cmd_RenderQuadContainerAsSpriteMultiple(const CCommandBuffer::SCommand_RenderQuadContainerAsSpriteMultiple *pCommand, SRenderCommandExecuteBuffer &ExecBuffer)
	{
		std::array<float, (size_t)4 * 2> m;
		GetStateMatrix(pCommand->m_State, m);

		bool CanBePushed = pCommand->m_DrawCount <= 1;

		bool IsTextured;
		size_t BlendModeIndex;
		size_t DynamicIndex;
		size_t AddressModeIndex;
		GetStateIndices(ExecBuffer, pCommand->m_State, IsTextured, BlendModeIndex, DynamicIndex, AddressModeIndex);
		auto &PipeLayout = GetPipeLayout(CanBePushed ? m_SpriteMultiPushPipeline : m_SpriteMultiPipeline, IsTextured, BlendModeIndex, DynamicIndex);
		auto &PipeLine = GetPipeline(CanBePushed ? m_SpriteMultiPushPipeline : m_SpriteMultiPipeline, IsTextured, BlendModeIndex, DynamicIndex);

		auto &CommandBuffer = GetGraphicCommandBuffer(ExecBuffer.m_ThreadIndex);

		BindPipeline(ExecBuffer.m_ThreadIndex, CommandBuffer, ExecBuffer, PipeLine, pCommand->m_State);

		std::array<VkBuffer, 1> aVertexBuffers = {ExecBuffer.m_Buffer};
		std::array<VkDeviceSize, 1> aOffsets = {(VkDeviceSize)ExecBuffer.m_BufferOff};
		vkCmdBindVertexBuffers(CommandBuffer, 0, 1, aVertexBuffers.data(), aOffsets.data());

		VkDeviceSize IndexOffset = (VkDeviceSize)((ptrdiff_t)pCommand->m_pOffset);
		vkCmdBindIndexBuffer(CommandBuffer, ExecBuffer.m_IndexBuffer, IndexOffset, VK_INDEX_TYPE_UINT32);

		vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 0, 1, &ExecBuffer.m_aDescriptors[0].m_Descriptor, 0, nullptr);

		if(CanBePushed)
		{
			SUniformSpriteMultiPushGVertColor PushConstantColor;
			SUniformSpriteMultiPushGPos PushConstantVertex;

			PushConstantColor = pCommand->m_VertexColor;

			mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
			PushConstantVertex.m_Center = pCommand->m_Center;

			for(size_t i = 0; i < pCommand->m_DrawCount; ++i)
				PushConstantVertex.m_aPSR[i] = vec4(pCommand->m_pRenderInfo[i].m_Pos.x, pCommand->m_pRenderInfo[i].m_Pos.y, pCommand->m_pRenderInfo[i].m_Scale, pCommand->m_pRenderInfo[i].m_Rotation);

			vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SUniformSpriteMultiPushGPosBase) + sizeof(vec4) * pCommand->m_DrawCount, &PushConstantVertex);
			vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiPushGPos), sizeof(PushConstantColor), &PushConstantColor);
		}
		else
		{
			SUniformSpriteMultiGVertColor PushConstantColor;
			SUniformSpriteMultiGPos PushConstantVertex;

			PushConstantColor = pCommand->m_VertexColor;

			mem_copy(PushConstantVertex.m_aPos, m.data(), sizeof(PushConstantVertex.m_aPos));
			PushConstantVertex.m_Center = pCommand->m_Center;

			vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstantVertex), &PushConstantVertex);
			vkCmdPushConstants(CommandBuffer, PipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(SUniformSpriteMultiGPos) + sizeof(SUniformSpriteMultiGVertColorAlign), sizeof(PushConstantColor), &PushConstantColor);
		}

		const int RSPCount = 512;
		int DrawCount = pCommand->m_DrawCount;
		size_t RenderOffset = 0;

		while(DrawCount > 0)
		{
			int UniformCount = (DrawCount > RSPCount ? RSPCount : DrawCount);

			if(!CanBePushed)
			{
				// create uniform buffer
				SDeviceDescriptorSet UniDescrSet;
				GetUniformBufferObject(ExecBuffer.m_ThreadIndex, false, UniDescrSet, UniformCount, (const float *)(pCommand->m_pRenderInfo + RenderOffset), UniformCount * sizeof(IGraphics::SRenderSpriteInfo));

				vkCmdBindDescriptorSets(CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, PipeLayout, 1, 1, &UniDescrSet.m_Descriptor, 0, nullptr);
			}

			vkCmdDrawIndexed(CommandBuffer, static_cast<uint32_t>(pCommand->m_DrawNum), UniformCount, 0, 0, 0);

			RenderOffset += RSPCount;
			DrawCount -= RSPCount;
		}
	}

	void Cmd_WindowCreateNtf(const CCommandBuffer::SCommand_WindowCreateNtf *pCommand)
	{
		log_debug("vulkan", "creating new surface.");
		m_pWindow = SDL_GetWindowFromID(pCommand->m_WindowID);
		if(m_RenderingPaused)
		{
#ifdef CONF_PLATFORM_ANDROID
			CreateSurface(m_pWindow);
			m_RecreateSwapChain = true;
#endif
			m_RenderingPaused = false;
			PureMemoryFrame();
			PrepareFrame();
		}
	}

	void Cmd_WindowDestroyNtf(const CCommandBuffer::SCommand_WindowDestroyNtf *pCommand)
	{
		log_debug("vulkan", "surface got destroyed.");
		if(!m_RenderingPaused)
		{
			WaitFrame();
			m_RenderingPaused = true;
			vkDeviceWaitIdle(m_VKDevice);
#ifdef CONF_PLATFORM_ANDROID
			CleanupVulkanSwapChain(true);
#endif
		}
	}

	void Cmd_PreInit(const CCommandProcessorFragment_GLBase::SCommand_PreInit *pCommand)
	{
		m_pGPUList = pCommand->m_pGPUList;
		if(InitVulkanSDL(pCommand->m_pWindow, pCommand->m_Width, pCommand->m_Height, pCommand->m_pRendererString, pCommand->m_pVendorString, pCommand->m_pVersionString) != 0)
		{
			m_VKInstance = VK_NULL_HANDLE;
		}

		RegisterCommands();

		m_ThreadCount = g_Config.m_GfxRenderThreadCount;
		if(m_ThreadCount <= 1)
			m_ThreadCount = 1;
		else
		{
			m_ThreadCount = clamp<decltype(m_ThreadCount)>(m_ThreadCount, 3, std::max<decltype(m_ThreadCount)>(3, std::thread::hardware_concurrency()));
		}

		// start threads
		dbg_assert(m_ThreadCount != 2, "Either use 1 main thread or at least 2 extra rendering threads.");
		if(m_ThreadCount > 1)
		{
			m_vvThreadCommandLists.resize(m_ThreadCount - 1);
			m_vThreadHelperHadCommands.resize(m_ThreadCount - 1, false);
			for(auto &ThreadCommandList : m_vvThreadCommandLists)
			{
				ThreadCommandList.reserve(256);
			}

			for(size_t i = 0; i < m_ThreadCount - 1; ++i)
			{
				auto *pRenderThread = new SRenderThread();
				std::unique_lock<std::mutex> Lock(pRenderThread->m_Mutex);
				m_vpRenderThreads.emplace_back(pRenderThread);
				pRenderThread->m_Thread = std::thread([this, i]() { RunThread(i); });
				// wait until thread started
				pRenderThread->m_Cond.wait(Lock, [pRenderThread]() -> bool { return pRenderThread->m_Started; });
			}
		}
	}

	void Cmd_PostShutdown(const CCommandProcessorFragment_GLBase::SCommand_PostShutdown *pCommand)
	{
		for(size_t i = 0; i < m_ThreadCount - 1; ++i)
		{
			auto *pThread = m_vpRenderThreads[i].get();
			{
				std::unique_lock<std::mutex> Lock(pThread->m_Mutex);
				pThread->m_Finished = true;
				pThread->m_Cond.notify_one();
			}
			pThread->m_Thread.join();
		}
		m_vpRenderThreads.clear();
		m_vvThreadCommandLists.clear();
		m_vThreadHelperHadCommands.clear();

		m_ThreadCount = 1;

		CleanupVulkanSDL();
	}

	void StartCommands(size_t CommandCount, size_t EstimatedRenderCallCount) override
	{
		m_CommandsInPipe = CommandCount;
		m_RenderCallsInPipe = EstimatedRenderCallCount;
		m_CurCommandInPipe = 0;
		m_CurRenderCallCountInPipe = 0;
	}

	void EndCommands() override
	{
		FinishRenderThreads();
		m_CommandsInPipe = 0;
		m_RenderCallsInPipe = 0;
	}

	/****************
	* RENDER THREADS
	*****************/

	void RunThread(size_t ThreadIndex)
	{
		auto *pThread = m_vpRenderThreads[ThreadIndex].get();
		std::unique_lock<std::mutex> Lock(pThread->m_Mutex);
		pThread->m_Started = true;
		pThread->m_Cond.notify_one();

		while(!pThread->m_Finished)
		{
			pThread->m_Cond.wait(Lock, [pThread]() -> bool { return pThread->m_IsRendering || pThread->m_Finished; });
			pThread->m_Cond.notify_one();

			// set this to true, if you want to benchmark the render thread times
			static constexpr bool s_BenchmarkRenderThreads = false;
			std::chrono::nanoseconds ThreadRenderTime = 0ns;
			if(IsVerbose() && s_BenchmarkRenderThreads)
			{
				ThreadRenderTime = time_get_nanoseconds();
			}

			if(!pThread->m_Finished)
			{
				for(auto &NextCmd : m_vvThreadCommandLists[ThreadIndex])
				{
					m_aCommandCallbacks[CommandBufferCMDOff(NextCmd.m_Command)].m_CommandCB(NextCmd.m_pRawCommand, NextCmd);
				}
				m_vvThreadCommandLists[ThreadIndex].clear();

				if(m_vvUsedThreadDrawCommandBuffer[ThreadIndex + 1][m_CurImageIndex])
				{
					auto &GraphicThreadCommandBuffer = m_vvThreadDrawCommandBuffers[ThreadIndex + 1][m_CurImageIndex];
					vkEndCommandBuffer(GraphicThreadCommandBuffer);
				}
			}

			if(IsVerbose() && s_BenchmarkRenderThreads)
			{
				dbg_msg("vulkan", "render thread %" PRIu64 " took %d ns to finish", ThreadIndex, (int)(time_get_nanoseconds() - ThreadRenderTime).count());
			}

			pThread->m_IsRendering = false;
		}
	}
};

CCommandProcessorFragment_GLBase *CreateVulkanCommandProcessorFragment()
{
	return new CCommandProcessorFragment_Vulkan();
}

#endif
