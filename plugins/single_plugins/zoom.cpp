#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/render.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>
#include <wayfire/plugins/ipc/ipc-helpers.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>

class wayfire_zoom_screen : public wf::per_output_plugin_instance_t
{
    enum class interpolation_method_t
    {
        LINEAR  = 0,
        NEAREST = 1,
    };

    wf::option_wrapper_t<wf::keybinding_t> modifier{"zoom/modifier"};
    wf::option_wrapper_t<wf::keybinding_t> zoom_in_key{"zoom/zoom_in"};
    wf::option_wrapper_t<wf::keybinding_t> zoom_out_key{"zoom/zoom_out"};
    wf::option_wrapper_t<wf::keybinding_t> zoom_reset_key{"zoom/zoom_reset"};
    wf::option_wrapper_t<double> speed{"zoom/speed"};
    wf::option_wrapper_t<wf::animation_description_t> smoothing_duration{"zoom/smoothing_duration"};
    wf::option_wrapper_t<int> interpolation_method{"zoom/interpolation_method"};
    wf::animation::simple_animation_t progression{smoothing_duration};
    bool hook_set = false;

    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc_repo;

    wf::plugin_activation_data_t grab_interface = {
        .name = "zoom",
        .capabilities = 0,
    };

  public:
    void init() override
    {
        progression.set(1, 1);
        output->add_axis(modifier, &axis);
        output->add_key(zoom_in_key, &zoom_in_binding);
        output->add_key(zoom_out_key, &zoom_out_binding);
        output->add_key(zoom_reset_key, &zoom_reset_binding);

        // Register IPC methods
        ipc_repo->register_method("zoom/setZoom", set_zoom);
        ipc_repo->register_method("zoom/zoomIn", zoom_in_ipc);
        ipc_repo->register_method("zoom/zoomOut", zoom_out_ipc);
        ipc_repo->register_method("zoom/getZoom", get_zoom);
    }

    void update_zoom_target(float delta)
    {
        float target = progression.end;
        target -= target * delta * speed;
        target  = wf::clamp(target, 1.0f, 50.0f);

        if (target != progression.end)
        {
            progression.animate(target);

            if (!hook_set)
            {
                hook_set = true;
                output->render->add_post(&render_hook);
                output->render->set_redraw_always();
            }
        }
    }

    wf::axis_callback axis = [=] (wlr_pointer_axis_event *ev)
    {
        if (!output->can_activate_plugin(&grab_interface))
        {
            return false;
        }

        if (ev->orientation != WL_POINTER_AXIS_VERTICAL_SCROLL)
        {
            return false;
        }

        update_zoom_target(ev->delta);

        return true;
    };

    wf::key_callback zoom_in_binding = [=] (auto)
    {
        update_zoom_target(-1.0);
        return false;
    };

    wf::key_callback zoom_out_binding = [=] (auto)
    {
        update_zoom_target(1.0);
        return false;
    };

    wf::key_callback zoom_reset_binding = [=] (auto)
    {
        if (progression.end != 1.0f)
        {
            progression.animate(1.0f);

            if (!hook_set)
            {
                hook_set = true;
                output->render->add_post(&render_hook);
                output->render->set_redraw_always();
            }
        }
        return false;
    };

    // IPC methods
    wf::ipc::method_callback set_zoom = [=] (const wf::json_t& data)
    {
        double factor = wf::ipc::json_get_double(data, "factor");
        bool with_animation = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);

        if (factor < 1.0 || factor > 50.0)
        {
            return wf::ipc::json_error("Zoom factor must be between 1.0 and 50.0");
        }

        if (with_animation)
        {
            progression.animate((float)factor);
        } else
        {
            progression.set((float)factor, (float)factor);
        }

        if (!hook_set)
        {
            hook_set = true;
            output->render->add_post(&render_hook);
            output->render->set_redraw_always();
        }

        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback zoom_in_ipc = [=] (const wf::json_t& data)
    {
        double delta = wf::ipc::json_get_optional_double(data, "delta").value_or(1.0);
        update_zoom_target(-(float)delta);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback zoom_out_ipc = [=] (const wf::json_t& data)
    {
        double delta = wf::ipc::json_get_optional_double(data, "delta").value_or(1.0);
        update_zoom_target((float)delta);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback get_zoom = [=] (const wf::json_t&)
    {
        wf::json_t response;
        response["factor"] = (double)progression;
        response["target"] = (double)progression.end;
        response["is_animating"] = progression.running();
        response["animation_duration"] = ((wf::animation_description_t)smoothing_duration).length_ms;
        response["interpolation_method"] = (int)interpolation_method;
        return response;
    };

    wf::post_hook_t render_hook = [=] (wf::auxilliary_buffer_t& source,
                                       const wf::render_buffer_t& destination)
    {
        auto w = destination.get_size().width;
        auto h = destination.get_size().height;
        if ((w <= 0) || (h <= 0))
        {
            LOGE("Invalid output size in zoom plugin!");
            return;
        }

        auto oc = output->get_cursor_position();
        double x, y;
        wlr_box b = output->get_relative_geometry();
        wlr_box_closest_point(&b, oc.x, oc.y, &x, &y);

        /* get rotation & scale */
        wlr_box box = {int(x), int(y), 1, 1};
        box = output->render->get_target_framebuffer().framebuffer_box_from_geometry_box(box);
        x   = box.x;
        y   = box.y;

        // Store progression once to avoid its value changing in subsequent calls, could be very tricky due to
        // timing. And if we use slightly different progressions, we can get an invalid rect.
        const float factor = (float)progression;
        const float scale  = (factor - 1) / factor;
        const float x1     = std::clamp(float(x * scale), 0.0f, w - 1.0f);
        const float y1     = std::clamp(float(y * scale), 0.0f, h - 1.0f);
        const float tw     = std::clamp(w / factor, 0.0f, w - x1);
        const float th     = std::clamp(h / factor, 0.0f, h - y1);
        auto filter_mode   = (interpolation_method == (int)interpolation_method_t::NEAREST) ?
            WLR_SCALE_FILTER_NEAREST : WLR_SCALE_FILTER_BILINEAR;
        destination.blit(source, {x1, y1, tw, th}, {0, 0, w, h}, filter_mode);
        if (!progression.running() && (progression - 1 <= 0.01))
        {
            unset_hook();
        }
    };

    void unset_hook()
    {
        output->render->set_redraw_always(false);
        output->render->rem_post(&render_hook);
        hook_set = false;
    }

    void fini() override
    {
        if (hook_set)
        {
            output->render->rem_post(&render_hook);
        }

        output->rem_binding(&axis);
        output->rem_binding(&zoom_in_binding);
        output->rem_binding(&zoom_out_binding);
        output->rem_binding(&zoom_reset_binding);

        // Unregister IPC methods
        ipc_repo->unregister_method("zoom/setZoom");
        ipc_repo->unregister_method("zoom/zoomIn");
        ipc_repo->unregister_method("zoom/zoomOut");
        ipc_repo->unregister_method("zoom/getZoom");
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_zoom_screen>);
