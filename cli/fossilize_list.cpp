/* Copyright (c) 2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "fossilize_inttypes.h"
#include "fossilize_db.hpp"
#include "cli_parser.hpp"
#include "layer/utils.hpp"
#include <memory>
#include <vector>
#include <unordered_set>

using namespace Fossilize;
using namespace std;

#define PRINT_SAVED_BUFFER_INNER(SavedBufferName, VkObjectName, Tag)\
for (auto It = list_replayer.SavedBufferName.begin(); It != list_replayer.SavedBufferName.end(); ++It)\
{\
	printf(VkObjectName "(%d):%016" PRIx64 ", ", Tag, *It);\
}\

#define PRINT_SAVED_BUFFER(SavedBufferName, VkObjectName, Tag) PRINT_SAVED_BUFFER_INNER(SavedBufferName, VkObjectName, Tag)

static const ResourceTag playback_order[] = {
	RESOURCE_SAMPLER,
	RESOURCE_DESCRIPTOR_SET_LAYOUT,
	RESOURCE_PIPELINE_LAYOUT,
	RESOURCE_SHADER_MODULE,
	RESOURCE_RENDER_PASS,
	RESOURCE_GRAPHICS_PIPELINE,
	RESOURCE_COMPUTE_PIPELINE,
	RESOURCE_RAYTRACING_PIPELINE
};

static void print_help()
{
	LOGI("Usage: fossilize-list\n"
		"\t<database path>\n"
		"\t[--tag index]\n"
		"\t[--size]\n"
		"\t[--connectivity]\n");
}

template <typename T>
static inline T fake_handle(uint64_t v)
{
	return (T)v;
}

template <typename T>
static inline const T* find_pnext(VkStructureType type, const void* pNext)
{
	while (pNext != nullptr)
	{
		auto* sin = static_cast<const VkBaseInStructure*>(pNext);
		if (sin->sType == type)
			return static_cast<const T*>(pNext);

		pNext = sin->pNext;
	}

	return nullptr;
}

struct ListReplayer : StateCreatorInterface
{
	unordered_set<Hash> saved_samplers;
	unordered_set<Hash> saved_descriptor_sets;
	unordered_set<Hash> saved_pipeline_layouts;
	unordered_set<Hash> saved_shader_modules;
	unordered_set<Hash> saved_render_passes;
	unordered_set<Hash> saved_graphics_pipelines;
	unordered_set<Hash> saved_raytracing_pipelines;

	ResourceTag selected_tag;

	void clear_saved_buffers()
	{
		saved_samplers.clear();
		saved_descriptor_sets.clear();
		saved_pipeline_layouts.clear();
		saved_shader_modules.clear();
		saved_render_passes.clear();
		saved_graphics_pipelines.clear();
		saved_raytracing_pipelines.clear();
	}

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo* create_info, VkSampler* sampler) override
	{
		*sampler = fake_handle<VkSampler>(hash);
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo* create_info, VkDescriptorSetLayout* layout) override
	{
		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		if (selected_tag != RESOURCE_DESCRIPTOR_SET_LAYOUT)
			return true;

		for (uint32_t binding = 0; binding < create_info->bindingCount; binding++)
		{
			auto& bind = create_info->pBindings[binding];
			if (bind.pImmutableSamplers && bind.descriptorCount != 0)
			{
				for (uint32_t i = 0; i < bind.descriptorCount; i++)
					if (bind.pImmutableSamplers[i] != VK_NULL_HANDLE)
						saved_samplers.insert((Hash)bind.pImmutableSamplers[i]);
			}
		}

		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo* create_info, VkPipelineLayout* layout) override
	{
		*layout = fake_handle<VkPipelineLayout>(hash);
		if (selected_tag != RESOURCE_PIPELINE_LAYOUT)
			return true;

		for (uint32_t layout = 0; layout < create_info->setLayoutCount; layout++)
			saved_descriptor_sets.insert((Hash)create_info->pSetLayouts[layout]);

		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo* create_info, VkShaderModule* module) override
	{
		*module = fake_handle<VkShaderModule>(hash);
		return true;
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo* create_info, VkRenderPass* render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return true;
	}

	bool enqueue_create_render_pass2(Hash hash, const VkRenderPassCreateInfo2* create_info, VkRenderPass* render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo* create_info, VkPipeline* pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		if (selected_tag != RESOURCE_COMPUTE_PIPELINE)
			return true;

		saved_pipeline_layouts.insert((Hash)create_info->layout);
		saved_shader_modules.insert((Hash)create_info->stage.module);

		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo* create_info, VkPipeline* pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		if (selected_tag != RESOURCE_GRAPHICS_PIPELINE)
			return true;

		for (uint32_t i = 0; i < create_info->stageCount; i++)
		{
			saved_shader_modules.insert((Hash)create_info->pStages[i].module);
		}

		saved_pipeline_layouts.insert((Hash)create_info->layout);
		saved_render_passes.insert((Hash)create_info->renderPass);

		auto* library_info =
			find_pnext<VkPipelineLibraryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
				create_info->pNext);

		if (library_info)
		{
			for (uint32_t i = 0; i < library_info->libraryCount; i++)
			{
				saved_graphics_pipelines.insert((Hash)library_info->pLibraries[i]);
			}
		}

		return true;
	}

	bool enqueue_create_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR* create_info, VkPipeline* pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		if (selected_tag != RESOURCE_RAYTRACING_PIPELINE)
			return true;

		saved_pipeline_layouts.insert((Hash)create_info->layout);
		for (uint32_t stage = 0; stage < create_info->stageCount; stage++)
		{
			saved_shader_modules.insert((Hash)create_info->pStages[stage].module);
		}

		if (create_info->pLibraryInfo)
		{
			for (uint32_t i = 0; i < create_info->pLibraryInfo->libraryCount; i++)
			{
				saved_raytracing_pipelines.insert((Hash)create_info->pLibraryInfo->pLibraries[i]);
			}
		}

		return true;
	}
};

bool replayer_create_info_fill(ResourceTag selected_tag, StateReplayer& replayer, ListReplayer& list_replayer, const std::unique_ptr<DatabaseInterface>& input_db)
{
	vector<uint8_t> state_db;
	for (auto tag : playback_order)
	{
		if (tag == selected_tag)
			break;

		size_t hash_count = 0;
		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
		{
			LOGE("Failed to get hashes.\n");
			return EXIT_FAILURE;
		}

		vector<Hash> hashes(hash_count);
		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
		{
			LOGE("Failed to get shader module hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto hash : hashes)
		{
			size_t state_db_size;
			if (!input_db->read_entry(tag, hash, &state_db_size, nullptr, 0))
			{
				LOGE("Failed to query blob size.\n");
				return EXIT_FAILURE;
			}

			state_db.resize(state_db_size);
			if (!input_db->read_entry(tag, hash, &state_db_size, state_db.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			if (!replayer.parse(list_replayer, input_db.get(), state_db.data(), state_db.size()))
				LOGE("Failed to parse blob (tag: %d, hash: 0x%" PRIx64 ").\n", tag, hash);
		}
	}

	return true;
}

void print_connectivity(const ListReplayer& list_replayer)
{
	PRINT_SAVED_BUFFER(saved_samplers, "sampler", RESOURCE_SAMPLER);
	PRINT_SAVED_BUFFER(saved_descriptor_sets, "descriptorSet", RESOURCE_DESCRIPTOR_SET_LAYOUT);
	PRINT_SAVED_BUFFER(saved_pipeline_layouts, "pipelineLayout", RESOURCE_PIPELINE_LAYOUT);
	PRINT_SAVED_BUFFER(saved_shader_modules, "shaderModule", RESOURCE_SHADER_MODULE);
	PRINT_SAVED_BUFFER(saved_render_passes, "renderPass", RESOURCE_RENDER_PASS);
	PRINT_SAVED_BUFFER(saved_graphics_pipelines, "graphicsPipeline", RESOURCE_GRAPHICS_PIPELINE);
	PRINT_SAVED_BUFFER(saved_raytracing_pipelines, "raytracingPipeline", RESOURCE_RAYTRACING_PIPELINE);
}

int main(int argc, char** argv)
{
	CLICallbacks cbs;
	string db_path;
	unsigned tag_uint = 0;
	bool log_size = false;
	bool log_connectivity = false;
	cbs.default_handler = [&](const char* path) { db_path = path; };
	cbs.add("--help", [&](CLIParser& parser) { print_help(); parser.end(); });
	cbs.add("--tag", [&](CLIParser& parser) { tag_uint = parser.next_uint(); });
	cbs.add("--size", [&](CLIParser&) { log_size = true; });
	cbs.add("--connectivity", [&](CLIParser&) { log_connectivity = true; });
	cbs.error_handler = [] { print_help(); };
	CLIParser parser(std::move(cbs), argc - 1, argv + 1);

	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (db_path.empty())
	{
		print_help();
		return EXIT_FAILURE;
	}

	auto input_db = std::unique_ptr<DatabaseInterface>(create_database(db_path.c_str(), DatabaseMode::ReadOnly));
	if (!input_db || !input_db->prepare())
	{
		LOGE("Failed to load database: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (tag_uint >= RESOURCE_COUNT)
	{
		LOGE("--tag (%u) is out of range.\n", tag_uint);
		return EXIT_FAILURE;
	}

	auto tag = static_cast<ResourceTag>(tag_uint);

	size_t hash_count = 0;
	if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
	{
		LOGE("Failed to get hashes.\n");
		return EXIT_FAILURE;
	}

	vector<Hash> hashes(hash_count);
	if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
	{
		LOGE("Failed to get shader module hashes.\n");
		return EXIT_FAILURE;
	}

	StateReplayer replayer;
	ListReplayer list_replayer;

	uint64_t compressed_total_size = 0;
	uint64_t uncompressed_total_size = 0;
	vector<uint8_t> state_db;
	for (auto hash : hashes)
	{
		if (log_connectivity)
		{
			size_t state_db_size;
			if (!input_db->read_entry(tag, hash, &state_db_size, nullptr, 0))
			{
				LOGE("Failed to query blob size.\n");
				return EXIT_FAILURE;
			}

			state_db.resize(state_db_size);
			if (!input_db->read_entry(tag, hash, &state_db_size, state_db.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			list_replayer.clear_saved_buffers();
			list_replayer.selected_tag = tag;
			replayer_create_info_fill(tag, replayer, list_replayer, input_db);
			if (!replayer.parse(list_replayer, input_db.get(), state_db.data(), state_db.size()))
			{
				LOGE("Failed to parse blob (tag: %d, hash: 0x%" PRIx64 ").\n", tag, hash);
				return EXIT_FAILURE;
			}

			printf("%016" PRIx64 " : ", hash);
			print_connectivity(list_replayer);
			printf(";\n");
		}

		if (log_size)
		{
			size_t compressed_blob_size = 0;
			size_t uncompressed_blob_size = 0;
			if (!input_db->read_entry(tag, hash, &compressed_blob_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT) ||
				!input_db->read_entry(tag, hash, &uncompressed_blob_size, nullptr, 0))
			{
				LOGE("Failed to query blob size.\n");
				return EXIT_FAILURE;
			}
			compressed_total_size += compressed_blob_size;
			uncompressed_total_size += uncompressed_blob_size;
			printf("%016" PRIx64 " %u compressed bytes, %u uncompressed bytes\n", hash,
				unsigned(compressed_blob_size), unsigned(uncompressed_blob_size));
		}
		else if (!log_connectivity)
			printf("%016" PRIx64 "\n", hash);
	}

	if (log_size)
	{
		printf("Total size (compressed): %" PRIu64 " bytes.\n", compressed_total_size);
		printf("Total size (uncompressed): %" PRIu64 " bytes.\n", uncompressed_total_size);
	}
}
