#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/render.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>
#include <wayfire/plugins/ipc/ipc-helpers.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/opengl.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// GLSL shader for display adjustments
static const char *display_vertex_shader =
    R"(
#version 100
attribute highp vec2 position;
attribute highp vec2 uvPosition;
varying highp vec2 uvpos;
void main() {
    gl_Position = vec4(position.xy, 0.0, 1.0);
    uvpos = uvPosition;
})";

static const char *display_fragment_shader =
    R"(
#version 100
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp float brightness;
uniform highp float gamma;
uniform highp float r_mult;
uniform highp float g_mult;
uniform highp float b_mult;

void main() {
    highp vec4 color = texture2D(smp, uvpos);
    
    // Apply brightness
    color.rgb *= brightness;
    
    // Apply gamma correction
    color.r = pow(color.r, 1.0 / gamma);
    color.g = pow(color.g, 1.0 / gamma);
    color.b = pow(color.b, 1.0 / gamma);
    
    // Apply temperature (color multiplier)
    color.r *= r_mult;
    color.g *= g_mult;
    color.b *= b_mult;
    
    gl_FragColor = color;
})";

class wayfire_display_output_t : public wf::per_output_plugin_instance_t
{
    // Configuration options
    wf::option_wrapper_t<double> default_brightness{"display/brightness"};
    wf::option_wrapper_t<double> default_gamma{"display/gamma"};
    wf::option_wrapper_t<int> default_temperature{"display/temperature"};
    
    // Keyboard shortcuts
    wf::option_wrapper_t<wf::keybinding_t> brightness_up{"display/brightness_up"};
    wf::option_wrapper_t<wf::keybinding_t> brightness_down{"display/brightness_down"};
    wf::option_wrapper_t<wf::keybinding_t> gamma_up{"display/gamma_up"};
    wf::option_wrapper_t<wf::keybinding_t> gamma_down{"display/gamma_down"};
    wf::option_wrapper_t<wf::keybinding_t> temperature_up{"display/temperature_up"};
    wf::option_wrapper_t<wf::keybinding_t> temperature_down{"display/temperature_down"};
    wf::option_wrapper_t<wf::keybinding_t> reset_all{"display/reset_all"};

    // IPC
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc_repo;

    // Current values
    double current_brightness = 1.0;
    double current_gamma = 1.0;
    int current_temperature = 6500;

    // Animation
    wf::option_wrapper_t<wf::animation_description_t> animation_duration{"display/animation_duration"};
    wf::animation::simple_animation_t brightness_animation{animation_duration};
    wf::animation::simple_animation_t gamma_animation{animation_duration};
    wf::animation::simple_animation_t temperature_animation{animation_duration};

    bool hook_set = false;
    
    // OpenGL resources
    OpenGL::program_t program;

  public:
    void init() override
    {
        if (!wf::get_core().is_gles2())
        {
            const char *render_type =
                wf::get_core().is_vulkan() ? "vulkan" : (wf::get_core().is_pixman() ? "pixman" : "unknown");
            LOGE("display: requires GLES2 support, but current renderer is ", render_type);
            return;
        }

        // Initialize current values from config
        current_brightness = default_brightness;
        current_gamma = default_gamma;
        current_temperature = default_temperature;

        brightness_animation.set(current_brightness, current_brightness);
        gamma_animation.set(current_gamma, current_gamma);
        temperature_animation.set(current_temperature, current_temperature);

        // Setup keybindings
        output->add_key(brightness_up, &brightness_up_binding);
        output->add_key(brightness_down, &brightness_down_binding);
        output->add_key(gamma_up, &gamma_up_binding);
        output->add_key(gamma_down, &gamma_down_binding);
        output->add_key(temperature_up, &temperature_up_binding);
        output->add_key(temperature_down, &temperature_down_binding);
        output->add_key(reset_all, &reset_all_binding);

        // Setup option callbacks
        default_brightness.set_callback([=] () {
            set_brightness(default_brightness, true);
        });
        default_gamma.set_callback([=] () {
            set_gamma(default_gamma, true);
        });
        default_temperature.set_callback([=] () {
            set_temperature(default_temperature, true);
        });

        // Register IPC methods
        ipc_repo->register_method("display/set-brightness", ipc_set_brightness);
        ipc_repo->register_method("display/set-gamma", ipc_set_gamma);
        ipc_repo->register_method("display/set-temperature", ipc_set_temperature);
        ipc_repo->register_method("display/get-state", ipc_get_state);
        ipc_repo->register_method("display/reset", ipc_reset);
        ipc_repo->register_method("display/increase-brightness", ipc_increase_brightness);
        ipc_repo->register_method("display/decrease-brightness", ipc_decrease_brightness);
        ipc_repo->register_method("display/increase-gamma", ipc_increase_gamma);
        ipc_repo->register_method("display/decrease-gamma", ipc_decrease_gamma);
        ipc_repo->register_method("display/increase-temperature", ipc_increase_temperature);
        ipc_repo->register_method("display/decrease-temperature", ipc_decrease_temperature);

        // Compile shader
        wf::gles::run_in_context([&]
        {
            program.set_simple(OpenGL::compile_program(display_vertex_shader, display_fragment_shader));
        });
        
        set_hook();
    }

    void set_brightness(double value, bool animate = true)
    {
        value = std::clamp(value, 0.0001, 2.0);
        if (animate)
        {
            brightness_animation.animate(value);
        } else
        {
            brightness_animation.set(value, value);
        }
        current_brightness = value;
        set_hook();
    }

    void set_gamma(double value, bool animate = true)
    {
        value = std::clamp(value, 0.1, 3.0);
        if (animate)
        {
            gamma_animation.animate(value);
        } else
        {
            gamma_animation.set(value, value);
        }
        current_gamma = value;
        set_hook();
    }

    void set_temperature(int value, bool animate = true)
    {
        value = std::clamp(value, 1000, 20000);
        if (animate)
        {
            temperature_animation.animate((double)value);
        } else
        {
            temperature_animation.set((double)value, (double)value);
        }
        current_temperature = value;
        set_hook();
    }

    void adjust_brightness(double delta)
    {
        set_brightness(current_brightness + delta, true);
    }

    void adjust_gamma(double delta)
    {
        set_gamma(current_gamma + delta, true);
    }

    void adjust_temperature(int delta)
    {
        set_temperature(current_temperature + delta, true);
    }

    void reset_all_adjustments()
    {
        set_brightness(1.0, true);
        set_gamma(1.0, true);
        set_temperature(6500, true);
    }

    // Calculate RGB multipliers based on temperature
    // Based on Tanner Helland's algorithm
    void calculate_temperature_rgb(double temp, float& r, float& g, float& b)
    {
        temp = temp / 100.0;

        // Red
        if (temp <= 66.0)
        {
            r = 1.0;
        } else
        {
            r = std::clamp(1.29420527207247316 * pow(temp - 60.0, -0.1332047592), 0.0, 1.0);
        }

        // Green
        if (temp <= 66.0)
        {
            g = std::clamp(0.99470802586396710 * log(temp) - 1.61118095476228786, 0.0, 1.0);
        } else
        {
            g = std::clamp(1.12989086089529416 * pow(temp - 60.0, -0.0755148492), 0.0, 1.0);
        }

        // Blue
        if (temp >= 66.0)
        {
            b = 1.0;
        } else if (temp <= 19.0)
        {
            b = 0.0;
        } else
        {
            b = std::clamp(1.38517731223101504 * log(temp - 10.0) - 3.05044858510917239, 0.0, 1.0);
        }
    }

    wf::key_callback brightness_up_binding = [=] (auto)
    {
        adjust_brightness(0.1);
        return false;
    };

    wf::key_callback brightness_down_binding = [=] (auto)
    {
        adjust_brightness(-0.1);
        return false;
    };

    wf::key_callback gamma_up_binding = [=] (auto)
    {
        adjust_gamma(0.1);
        return false;
    };

    wf::key_callback gamma_down_binding = [=] (auto)
    {
        adjust_gamma(-0.1);
        return false;
    };

    wf::key_callback temperature_up_binding = [=] (auto)
    {
        adjust_temperature(500);
        return false;
    };

    wf::key_callback temperature_down_binding = [=] (auto)
    {
        adjust_temperature(-500);
        return false;
    };

    wf::key_callback reset_all_binding = [=] (auto)
    {
        reset_all_adjustments();
        return false;
    };

    // IPC methods
    wf::ipc::method_callback ipc_set_brightness = [=] (const wf::json_t& data)
    {
        double brightness = wf::ipc::json_get_double(data, "brightness");
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_brightness(brightness, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_set_gamma = [=] (const wf::json_t& data)
    {
        double gamma = wf::ipc::json_get_double(data, "gamma");
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_gamma(gamma, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_set_temperature = [=] (const wf::json_t& data)
    {
        int temperature = wf::ipc::json_get_int64(data, "temperature");
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_temperature(temperature, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_get_state = [=] (const wf::json_t&)
    {
        wf::json_t response;
        response["brightness"] = (double)brightness_animation;
        response["gamma"] = (double)gamma_animation;
        response["temperature"] = (int)(double)temperature_animation;
        response["output_name"] = output->to_string();
        response["output_id"] = (uint64_t)(size_t)output;
        return response;
    };

    wf::ipc::method_callback ipc_reset = [=] (const wf::json_t&)
    {
        reset_all_adjustments();
        return wf::ipc::json_ok();
    };

    // IPC methods for increasing/decreasing values
    wf::ipc::method_callback ipc_increase_brightness = [=] (const wf::json_t& data)
    {
        double delta = wf::ipc::json_get_optional_double(data, "delta").value_or(0.1);
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_brightness(current_brightness + delta, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_decrease_brightness = [=] (const wf::json_t& data)
    {
        double delta = wf::ipc::json_get_optional_double(data, "delta").value_or(0.1);
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_brightness(current_brightness - delta, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_increase_gamma = [=] (const wf::json_t& data)
    {
        double delta = wf::ipc::json_get_optional_double(data, "delta").value_or(0.1);
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_gamma(current_gamma + delta, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_decrease_gamma = [=] (const wf::json_t& data)
    {
        double delta = wf::ipc::json_get_optional_double(data, "delta").value_or(0.1);
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_gamma(current_gamma - delta, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_increase_temperature = [=] (const wf::json_t& data)
    {
        int delta = wf::ipc::json_get_optional_int64(data, "delta").value_or(500);
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_temperature(current_temperature + delta, animate);
        return wf::ipc::json_ok();
    };

    wf::ipc::method_callback ipc_decrease_temperature = [=] (const wf::json_t& data)
    {
        int delta = wf::ipc::json_get_optional_int64(data, "delta").value_or(500);
        bool animate = wf::ipc::json_get_optional_bool(data, "animation").value_or(true);
        set_temperature(current_temperature - delta, animate);
        return wf::ipc::json_ok();
    };

    void set_hook()
    {
        if (hook_set)
        {
            return;
        }

        output->render->add_post(&hook);
        output->render->schedule_redraw();
        hook_set = true;
    }

    void unset_hook()
    {
        if (!hook_set)
        {
            return;
        }

        output->render->rem_post(&hook);
        hook_set = false;
    }

    wf::post_hook_t hook = [=] (wf::auxilliary_buffer_t& source,
                                const wf::render_buffer_t& destination)
    {
        // Update current values from animations
        current_brightness = brightness_animation;
        current_gamma = gamma_animation;
        current_temperature = (int)(double)temperature_animation;

        // Check if we need to render
        bool needs_render = (std::abs(current_brightness - 1.0) > 0.001) ||
                           (std::abs(current_gamma - 1.0) > 0.001) ||
                           (current_temperature != 6500);

        if (!needs_render && !brightness_animation.running() && 
            !gamma_animation.running() && !temperature_animation.running())
        {
            unset_hook();
            return;
        }

        // Render the adjustment
        render_adjustments(source, destination);

        if (!brightness_animation.running() && !gamma_animation.running() && 
            !temperature_animation.running())
        {
            unset_hook();
        }
    };

    void render_adjustments(wf::auxilliary_buffer_t& source, const wf::render_buffer_t& destination)
    {
        static const float vertexData[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
             1.0f,  1.0f,
            -1.0f,  1.0f
        };

        static const float coordData[] = {
            0.0f, 0.0f,
            1.0f, 0.0f,
            1.0f, 1.0f,
            0.0f, 1.0f
        };

        float r_mult, g_mult, b_mult;
        calculate_temperature_rgb(current_temperature, r_mult, g_mult, b_mult);

        wf::gles::run_in_context([&]
        {
            wf::gles::bind_render_buffer(destination);
            program.use(wf::TEXTURE_TYPE_RGBA);
            GL_CALL(glBindTexture(GL_TEXTURE_2D, wf::gles_texture_t::from_aux(source).tex_id));
            GL_CALL(glActiveTexture(GL_TEXTURE0));

            program.attrib_pointer("position", 2, 0, vertexData);
            program.attrib_pointer("uvPosition", 2, 0, coordData);
            
            program.uniform1f("brightness", (float)current_brightness);
            program.uniform1f("gamma", (float)current_gamma);
            program.uniform1f("r_mult", r_mult);
            program.uniform1f("g_mult", g_mult);
            program.uniform1f("b_mult", b_mult);

            GL_CALL(glDisable(GL_BLEND));
            GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
            GL_CALL(glEnable(GL_BLEND));
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

            program.deactivate();
        });
    }

    void fini() override
    {
        unset_hook();
        
        output->rem_binding(&brightness_up_binding);
        output->rem_binding(&brightness_down_binding);
        output->rem_binding(&gamma_up_binding);
        output->rem_binding(&gamma_down_binding);
        output->rem_binding(&temperature_up_binding);
        output->rem_binding(&temperature_down_binding);
        output->rem_binding(&reset_all_binding);

        wf::gles::run_in_context_if_gles([&]
        {
            program.free_resources();
        });

        // Unregister IPC methods
        ipc_repo->unregister_method("display/set-brightness");
        ipc_repo->unregister_method("display/set-gamma");
        ipc_repo->unregister_method("display/set-temperature");
        ipc_repo->unregister_method("display/get-state");
        ipc_repo->unregister_method("display/reset");
        ipc_repo->unregister_method("display/increase-brightness");
        ipc_repo->unregister_method("display/decrease-brightness");
        ipc_repo->unregister_method("display/increase-gamma");
        ipc_repo->unregister_method("display/decrease-gamma");
        ipc_repo->unregister_method("display/increase-temperature");
        ipc_repo->unregister_method("display/decrease-temperature");
    }
};

class wayfire_display_global_t : public wf::plugin_interface_t,
    public wf::per_output_tracker_mixin_t<wayfire_display_output_t>
{
  public:
    void init() override
    {
        init_output_tracking();
    }

    void fini() override
    {
        fini_output_tracking();
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_display_global_t);
