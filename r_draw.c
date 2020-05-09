#include "r_draw.h"
#include "spr.h"
#include "lib/dstuff/containers/stack_list.h"
#include "lib/dstuff/containers/ringbuffer.h"
#include "lib/dstuff/file/file.h"
#include <stdlib.h>
#include <stdio.h>

struct r_draw_cmd_buffer_t *r_draw_cmd_buffer;
struct r_draw_uniform_data_t *r_draw_uniform_data;
struct stack_list_t r_draw_cmd_lists;
struct ringbuffer_t r_pending_draw_cmd_lists;
struct list_t r_draw_calls;
uint32_t r_current_draw_cmd_list = 0xffffffff;

struct r_render_pass_handle_t r_render_pass;
struct r_render_pass_handle_t r_debug_render_pass;
struct r_framebuffer_handle_t r_framebuffer;
struct r_buffer_h r_vertex_buffer;
struct list_t r_uniform_buffers;
struct list_t r_free_uniform_buffers;
struct list_t r_used_uniform_buffers;
VkQueue r_draw_queue;
VkFence r_draw_fence;

#define R_VERTEX_BUFFER_MEMORY 8388608
#define R_UNIFORM_BUFFER_MEMORY 8388608

//VkCommandPool r_command_pool;
//VkCommandBuffer r_command_buffer;

void r_DrawInit()
{
    FILE *file;
    struct r_shader_description_t vertex_description = {};
    struct r_shader_description_t fragment_description = {};
    struct r_render_pass_description_t render_pass_description = {};
    long code_size;
//    struct r_buffer_t *buffer;

    file = fopen("shaders/shader.vert.spv", "rb");
    read_file(file, &vertex_description.code, &code_size);
    vertex_description.code_size = (uint32_t)code_size;
    fclose(file);
    vertex_description.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertex_description.vertex_binding_count = 1;
    vertex_description.vertex_bindings = (struct r_vertex_binding_t []){
        [0] = {
            .size = sizeof(struct vertex_t),
            .attribs = (struct r_vertex_attrib_t []){
                [0] = {
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                    .offset = offsetof(struct vertex_t, position),
                },
                [1] = {
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                    .offset = offsetof(struct vertex_t, normal),
                },
                [2] = {
                    .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                    .offset = offsetof(struct vertex_t, tex_coords),
                }
            },
            .attrib_count = 3
        }
    };
    vertex_description.resources = (struct r_resource_binding_t []){
        [0] = {
            .descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .count = 1
        }
    };
    vertex_description.resource_count = 1;
    vertex_description.push_constant_count = 1;
    vertex_description.push_constants = (struct r_push_constant_t []){
        [0] = {
            .offset = 0,
            .size = sizeof(mat4_t)
        },
    };

    struct r_shader_t *vertex_shader = r_GetShaderPointer(r_CreateShader(&vertex_description));
    free(vertex_description.code);

    fopen("shaders/shader.frag.spv", "rb");
    read_file(file, &fragment_description.code, &code_size);
    fragment_description.code_size = (uint32_t)code_size;
    fclose(file);
    fragment_description.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragment_description.resource_count = 1;
    fragment_description.resources = (struct r_resource_binding_t []){
        [0] = {
            .descriptor_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .count = 1,
        }
    };
    struct r_shader_t *fragment_shader = r_GetShaderPointer(r_CreateShader(&fragment_description));
    free(fragment_description.code);

    render_pass_description = (struct r_render_pass_description_t){
        .attachments = (VkAttachmentDescription []){
            {.format = VK_FORMAT_R32G32B32A32_SFLOAT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR},
            {.format = VK_FORMAT_D32_SFLOAT}
        },
        .attachment_count = 2,
        .subpass_count = 1,
        .subpasses = (struct r_subpass_description_t[]){
            {
                .color_attachment_count = 1,
                .color_attachments = (VkAttachmentReference []){
                    {.attachment = 0}
                },
                .depth_stencil_attachment = &(VkAttachmentReference){
                    .attachment = 1
                },
                .pipeline_description = &(struct r_pipeline_description_t){
                    .shader_count = 2,
                    .shaders = (struct r_shader_t *[]){
                        vertex_shader, fragment_shader
                    },
                    .vertex_input_state = &(VkPipelineVertexInputStateCreateInfo){
                        .vertexBindingDescriptionCount = 1,
                        .pVertexBindingDescriptions = (VkVertexInputBindingDescription []){
                            {.stride = sizeof(struct vertex_t)}
                        },
                        .vertexAttributeDescriptionCount = 3,
                        .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription []){
                            {
                                .location = 0,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                .offset = offsetof(struct vertex_t, position)
                            },
                            {
                                .location = 1,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                .offset = offsetof(struct vertex_t, normal)
                            },
                            {
                                .location = 2,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                .offset = offsetof(struct vertex_t, tex_coords)
                            }
                        }
                    }
                },
            }
        },
    };

    r_render_pass = r_CreateRenderPass(&render_pass_description);

    struct r_framebuffer_description_t framebuffer_description = (struct r_framebuffer_description_t){
        .attachment_count = render_pass_description.attachment_count,
        .attachments = render_pass_description.attachments,
        .frame_count = 2,
        .width = R_DEFAULT_WIDTH,
        .height = R_DEFAULT_HEIGHT,
        .render_pass = r_GetRenderPassPointer(r_render_pass)
    };

    r_framebuffer = r_CreateFramebuffer(&framebuffer_description);
    r_draw_queue = r_GetDrawQueue();
    r_draw_fence = r_CreateFence();

    VkBufferCreateInfo buffer_create_info = {};
    buffer_create_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_create_info.size = R_VERTEX_BUFFER_MEMORY;
    buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r_vertex_buffer = r_CreateBuffer(&buffer_create_info);

    struct vertex_t *data = (struct vertex_t []){
        {.position = vec4_t_c(-1.0, -1.0, -1.2, 1.0),.tex_coords = vec4_t_c(0.0, 1.0, 0.0, 0.0)},
        {.position = vec4_t_c(1.0, -1.0, -1.2, 1.0),.tex_coords = vec4_t_c(1.0, 1.0, 0.0, 0.0)},
        {.position = vec4_t_c(1.0, 1.0, -1.2, 1.0),.tex_coords = vec4_t_c(1.0, 0.0, 0.0, 0.0)},


        {.position = vec4_t_c(1.0, 1.0, -1.2, 1.0),.tex_coords = vec4_t_c(1.0, 0.0, 0.0, 0.0)},
        {.position = vec4_t_c(-1.0, 1.0, -1.2, 1.0),.tex_coords = vec4_t_c(0.0, 0.0, 0.0, 0.0)},
        {.position = vec4_t_c(-1.0, -1.0, -1.2, 1.0),.tex_coords = vec4_t_c(0.0, 1.0, 0.0, 0.0)},
    };

    r_FillBufferChunk(r_vertex_buffer, data, sizeof(struct vertex_t) * 6, 0);

    r_draw_cmd_lists = create_stack_list(sizeof(struct r_draw_cmd_list_t), 32);
    r_pending_draw_cmd_lists = create_ringbuffer(sizeof(uint32_t), 32);
    r_draw_calls = create_list(sizeof(struct r_draw_call_data_t), R_DRAW_CMD_BUFFER_DRAW_CMDS);

    r_uniform_buffers = create_list(sizeof(struct r_uniform_buffer_t), 64);
    r_free_uniform_buffers = create_list(sizeof(uint32_t), 64);
    r_used_uniform_buffers = create_list(sizeof(uint32_t), 64);
}

void r_DrawShutdown()
{

}

void r_BeginFrame()
{

}

void r_EndFrame()
{
    r_DispatchPending();
    r_PresentFramebuffer(r_framebuffer);
}

/*
=================================================================
=================================================================
=================================================================
*/

void r_BeginSubmission(mat4_t *view_matrix, mat4_t *projection_matrix)
{
    struct r_draw_cmd_list_t *cmd_list;

    if(r_current_draw_cmd_list == 0xffffffff)
    {
        r_current_draw_cmd_list = add_stack_list_element(&r_draw_cmd_lists, NULL);
        cmd_list = get_stack_list_element(&r_draw_cmd_lists, r_current_draw_cmd_list);
    }

    if(view_matrix && projection_matrix)
    {
        memcpy(&cmd_list->view_matrix, view_matrix, sizeof(mat4_t));
        mat4_t_mul(&cmd_list->view_projection_matrix, view_matrix, projection_matrix);
    }

    cmd_list->draw_cmd_count = 0;
}

void r_DrawAnimationFrame(struct spr_animation_h animation, vec2_t *position, float scale, float rotation, uint32_t frame, uint32_t flipped)
{
    struct spr_animation_t *animation_ptr;
    struct spr_anim_frame_t *frame_ptr;
    vec2_t final_position;
    animation_ptr = spr_GetAnimationPointer(animation);
    frame_ptr = spr_GetAnimationFrame(animation, frame);

    final_position = *position;
    if(flipped)
    {
        final_position.x -= frame_ptr->offset.x;
    }
    else
    {
        final_position.x += frame_ptr->offset.x;
    }

    final_position.y += frame_ptr->offset.y;

    r_DrawSprite(animation_ptr->sprite, &final_position, scale, rotation, frame_ptr->sprite_frame, flipped);
}

void r_DrawSprite(struct spr_sprite_h sprite, vec2_t *position, float scale, float rotation, uint32_t frame, uint32_t flipped)
{
    struct r_draw_cmd_list_t *cmd_list;
    struct r_draw_cmd_t *draw_cmd;

    if(sprite.index != SPR_INVALID_SPRITE_INDEX)
    {
        cmd_list = get_stack_list_element(&r_draw_cmd_lists, r_current_draw_cmd_list);
        draw_cmd = cmd_list->draw_cmds + cmd_list->draw_cmd_count;
        cmd_list->draw_cmd_count++;

        draw_cmd->position = *position;
        draw_cmd->scale = scale;
        draw_cmd->rotation = rotation;
        draw_cmd->sprite = sprite;
        draw_cmd->frame = frame;
        draw_cmd->flipped = flipped;
    }
}

int r_CmpDrawCmds(const void *a, const void *b)
{
    struct r_draw_cmd_t *cmd_a = (struct r_draw_cmd_t *)a;
    struct r_draw_cmd_t *cmd_b = (struct r_draw_cmd_t *)b;
    return cmd_a->sprite.sprite_sheet.index - cmd_b->sprite.sprite_sheet.index;
}

void r_EndSubmission()
{
    struct r_draw_cmd_list_t *cmd_list;

    if(r_current_draw_cmd_list != 0xffffffff)
    {
        cmd_list = get_stack_list_element(&r_draw_cmd_lists, r_current_draw_cmd_list);
        qsort(cmd_list->draw_cmds, cmd_list->draw_cmd_count, sizeof(struct r_draw_cmd_t), r_CmpDrawCmds);
        if(add_ringbuffer_element(&r_pending_draw_cmd_lists, &r_current_draw_cmd_list) == 0xffffffff)
        {
            r_DispatchPending();
            add_ringbuffer_element(&r_pending_draw_cmd_lists, &r_current_draw_cmd_list);
        }
        r_current_draw_cmd_list = 0xffffffff;
    }
}

void r_DispatchPending()
{
    struct r_draw_cmd_list_t *cmd_list;
    struct r_draw_cmd_t *draw_cmd;
    uint32_t *cmd_list_index;
    struct r_buffer_t *vertex_buffer;
    struct r_render_pass_t *render_pass;
    struct r_framebuffer_t *framebuffer;
//    struct r_texture_t *texture;
    struct spr_sprite_sheet_t *sprite_sheet;
//    struct spr_sprite
    struct spr_sprite_entry_t *entry;
    struct spr_sprite_sheet_h current_sprite_sheet;
//    void *staging_memory;
//    VkBuffer staging_buffer;
    VkViewport viewport;
    VkRect2D scissor;
    VkDescriptorSet descriptor_set;
    struct r_draw_call_data_t *current_call_data;
    struct r_draw_call_data_t new_call_data;
    struct r_draw_uniform_data_t *draw_data;
    struct r_uniform_buffer_t *uniform_buffer;

//    mat4_t model_view_projection_matrix;
    mat4_t transform;

    struct r_submit_info_t submit_info = {};
    union r_command_buffer_h command_buffer;
    VkRenderPassBeginInfo render_pass_begin_info = {};

    submit_info.s_type = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.command_buffer_count = r_pending_draw_cmd_lists.buffer_size;
    submit_info.command_buffer_count -= r_pending_draw_cmd_lists.free_slots;
    submit_info.command_buffers = alloca(sizeof(union r_command_buffer_h) * submit_info.command_buffer_count);
    submit_info.command_buffer_count = 0;

    vertex_buffer = r_GetBufferPointer(r_vertex_buffer);
    render_pass = r_GetRenderPassPointer(r_render_pass);
    framebuffer = r_GetFramebufferPointer(r_framebuffer);

    VkClearValue clear_values[2];
    clear_values[0].color.float32[0] = 0.1;
    clear_values[0].color.float32[1] = 0.1;
    clear_values[0].color.float32[2] = 0.1;
    clear_values[0].color.float32[3] = 1.0;
    clear_values[1].depthStencil.depth = 1.0;

    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.renderPass = render_pass->render_pass;
    render_pass_begin_info.framebuffer = framebuffer->buffers[0];
    render_pass_begin_info.renderArea.extent.width = R_DEFAULT_WIDTH;
    render_pass_begin_info.renderArea.extent.height = R_DEFAULT_HEIGHT;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.clearValueCount = 2;
    render_pass_begin_info.pClearValues = clear_values;

    viewport.x = 0;
    viewport.y = 0;
    viewport.width = R_DEFAULT_WIDTH;
    viewport.height = R_DEFAULT_HEIGHT;
    viewport.minDepth = 0.0;
    viewport.maxDepth = 1.0;

    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = R_DEFAULT_WIDTH;
    scissor.extent.height = R_DEFAULT_HEIGHT;

//    staging_memory = r_LockStagingMemory();
//    r_UnlockStagingMemory();

    current_sprite_sheet = SPR_INVALID_SPRITE_SHEET_HANDLE;
    r_draw_calls.cursor = 0;
    current_call_data = &new_call_data;
    current_call_data->count = 0;
    current_call_data->first = 0;
    current_call_data->first_instance = 0;
    current_call_data->instance_count = 0;
    current_call_data->texture = R_INVALID_TEXTURE_HANDLE;

//    staging_buffer = r_GetStagingBuffer();

    while((cmd_list_index = get_ringbuffer_element(&r_pending_draw_cmd_lists)))
    {
        cmd_list = get_stack_list_element(&r_draw_cmd_lists, *cmd_list_index);
        command_buffer = r_AllocateCommandBuffer();
        r_vkBeginCommandBuffer(command_buffer);
        r_vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pass->pipeline.pipeline);
        r_vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer->buffer, &(VkDeviceSize){0});
        r_vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
        r_vkCmdSetViewport(command_buffer, 0, 1, &viewport);
        r_vkCmdSetScissor(command_buffer, 0, 1, &scissor);

        uniform_buffer = r_AllocateUniformBuffer(command_buffer);

        descriptor_set = r_AllocateDescriptorSet(command_buffer, &render_pass->pipeline, VK_SHADER_STAGE_VERTEX_BIT);
        r_UpdateUniformBufferDescriptorSet(descriptor_set, 0, uniform_buffer->vk_buffer, 0, 65536);
        r_vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pass->pipeline.layout, 0, 1, &descriptor_set, 0, NULL);

        for(uint32_t draw_cmd_index = 0; draw_cmd_index < cmd_list->draw_cmd_count; draw_cmd_index++)
        {
            draw_cmd = cmd_list->draw_cmds + draw_cmd_index;

            if(draw_cmd->sprite.sprite_sheet.index != current_sprite_sheet.index)
            {
                current_sprite_sheet = draw_cmd->sprite.sprite_sheet;
                sprite_sheet = spr_GetSpriteSheetPointer(draw_cmd->sprite.sprite_sheet);

                new_call_data.count = 6;
                new_call_data.first = 0;
                new_call_data.first_instance = current_call_data->instance_count;
                new_call_data.instance_count = 0;
                new_call_data.texture = sprite_sheet->texture;

                current_call_data = get_list_element(&r_draw_calls, add_list_element(&r_draw_calls, &new_call_data));
            }

            entry = spr_GetSpriteEntry(draw_cmd->sprite, draw_cmd->frame);
            uint32_t offset = sizeof(struct r_draw_uniform_data_t) * (current_call_data->first_instance + current_call_data->instance_count);
            draw_data = (struct r_draw_uniform_data_t *)((char *)uniform_buffer->memory + offset);

            mat4_t_identity(&transform);
            transform.rows[3].x = draw_cmd->position.x;
            transform.rows[3].y = draw_cmd->position.y;
            transform.rows[0].x *= entry->width * 0.5 * draw_cmd->scale;
            transform.rows[1].y *= entry->height * 0.5 * draw_cmd->scale;

            mat4_t_mul(&draw_data->model_view_projection_matrix, &transform, &cmd_list->view_projection_matrix);
            draw_data->offset_size.x = entry->normalized_x;
            draw_data->offset_size.y = entry->normalized_y;
            draw_data->offset_size.z = draw_cmd->flipped ? -entry->normalized_width : entry->normalized_width;
            draw_data->offset_size.w = entry->normalized_height;

            current_call_data->instance_count++;
        }

        for(uint32_t draw_call_index = 0; draw_call_index < r_draw_calls.cursor; draw_call_index++)
        {
            current_call_data = get_list_element(&r_draw_calls, draw_call_index);
            descriptor_set = r_AllocateDescriptorSet(command_buffer, &render_pass->pipeline, VK_SHADER_STAGE_FRAGMENT_BIT);
            r_UpdateCombinedImageSamplerDescriptorSet(descriptor_set, 0, current_call_data->texture);
            r_vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pass->pipeline.layout, 1, 1, &descriptor_set, 0, NULL);
            r_vkCmdDraw(command_buffer, 6, current_call_data->instance_count, 0, current_call_data->first_instance);
        }

        r_vkCmdEndRenderPass(command_buffer);
        r_vkEndCommandBuffer(command_buffer);
        submit_info.command_buffers[submit_info.command_buffer_count] = command_buffer;
        submit_info.command_buffer_count++;
        remove_stack_list_element(&r_draw_cmd_lists, *cmd_list_index);
    }
    r_vkResetFences(1, &r_draw_fence);
    r_vkQueueSubmit(r_draw_queue, 1, &submit_info, r_draw_fence);
    r_vkWaitForFences(1, &r_draw_fence, 1, 0xffffffffffffffff);
}

struct r_uniform_buffer_t *r_AllocateUniformBuffer(union r_command_buffer_h command_buffer)
{
    struct r_uniform_buffer_t *uniform_buffer = NULL;
    struct r_buffer_t *buffer;
    VkBufferCreateInfo buffer_create_info = {};
    uint32_t *buffer_index;
    uint32_t new_buffer_index;
    VkResult result;
    uint32_t recycled_buffers = 0;

    for(uint32_t index = 0; index < r_used_uniform_buffers.cursor; index++)
    {
        buffer_index = get_list_element(&r_used_uniform_buffers, index);
        uniform_buffer = get_list_element(&r_uniform_buffers, *buffer_index);
        result = r_vkGetEventStatus(uniform_buffer->event);

        if(result == VK_EVENT_SET)
        {
            r_vkResetEvent(uniform_buffer->event);
            remove_list_element(&r_used_uniform_buffers, index);
            add_list_element(&r_free_uniform_buffers, buffer_index);
            recycled_buffers++;
        }
    }

    buffer_index = get_list_element(&r_free_uniform_buffers, 0);

    if(!buffer_index)
    {
        new_buffer_index = add_list_element(&r_uniform_buffers, NULL);
        uniform_buffer = get_list_element(&r_uniform_buffers, new_buffer_index);
        buffer_create_info.size = 65536;
        buffer_create_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uniform_buffer->buffer = r_CreateBuffer(&buffer_create_info);
        uniform_buffer->event = r_CreateEvent();
        buffer = r_GetBufferPointer(uniform_buffer->buffer);
        uniform_buffer->memory = r_GetBufferChunkMappedMemory(buffer->memory);
        uniform_buffer->vk_buffer = buffer->buffer;
        add_list_element(&r_used_uniform_buffers, &new_buffer_index);
    }
    else
    {
        buffer = get_list_element(&r_uniform_buffers, *buffer_index);
        add_list_element(&r_used_uniform_buffers, buffer_index);
        remove_list_element(&r_free_uniform_buffers, 0);
    }

    r_AppendEvent(command_buffer, uniform_buffer->event);

    return uniform_buffer;
}

void r_DrawPoint(vec3_t *position, vec3_t *color, float size)
{

}







