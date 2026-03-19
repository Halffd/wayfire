#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/plugins/common/input-grab.hpp>
#include <wayfire/plugins/common/util.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <vector>
#include <cmath>
#include <linux/input-event-codes.h>

constexpr const char *SWITCHER_GRID_TRANSFORMER = "switcher-grid";

class switcher_grid_output_t : public wf::per_output_plugin_instance_t,
    public wf::keyboard_interaction_t, public wf::pointer_interaction_t
{
    // Configuration
    wf::option_wrapper_t<wf::keybinding_t> activate_key{"switcher-grid/activate"};
    wf::option_wrapper_t<wf::keybinding_t> activate_backward{"switcher-grid/activate_backward"};
    wf::option_wrapper_t<int> thumbnail_width{"switcher-grid/thumbnail_width"};
    wf::option_wrapper_t<int> grid_width_percent{"switcher-grid/grid_width_percent"};
    wf::option_wrapper_t<wf::animation_description_t> animation_duration{"switcher-grid/animation_duration"};
    wf::option_wrapper_t<double> selected_alpha{"switcher-grid/selected_alpha"};
    wf::option_wrapper_t<double> inactive_alpha{"switcher-grid/inactive_alpha"};
    wf::option_wrapper_t<bool> show_workspace{"switcher-grid/show_workspace"};

    // State
    bool active = false;
    int selected_index = 0;
    std::vector<wayfire_toplevel_view> views;
    std::vector<wayfire_toplevel_view> filtered_views; // For search/filter
    std::string search_query;
    std::unique_ptr<wf::input_grab_t> input_grab;

    // Grid layout
    int grid_cols = 0;
    int grid_rows = 0;
    int thumb_width = 500;
    float grid_width_pct = 0.9f;

    wf::plugin_activation_data_t grab_interface = {
        .name = SWITCHER_GRID_TRANSFORMER,
        .capabilities = wf::CAPABILITY_MANAGE_COMPOSITOR,
        .cancel = [=] () { switcher_done(); },
    };

    struct view_transform_data
    {
        wf::geometry_t target_geometry;
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        float offset_x = 0.0f;
        float offset_y = 0.0f;
        float alpha = 1.0f;
    };

    std::map<wayfire_toplevel_view, view_transform_data> view_data;

    class switcher_render_node_t : public wf::scene::node_t
    {
        class switcher_render_instance_t : public wf::scene::render_instance_t
        {
            std::shared_ptr<switcher_render_node_t> self;
            wf::scene::damage_callback push_damage;

          public:
            switcher_render_instance_t(switcher_render_node_t *self, wf::scene::damage_callback push_damage)
            {
                this->self = std::dynamic_pointer_cast<switcher_render_node_t>(self->shared_from_this());
                this->push_damage = push_damage;
            }

            void schedule_instructions(
                std::vector<wf::scene::render_instruction_t>& instructions,
                const wf::render_target_t& target, wf::region_t& damage) override
            {
                instructions.push_back(wf::scene::render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = damage & self->get_bounding_box(),
                });
            }

            void render(const wf::scene::render_instruction_t& data) override
            {
                self->grid->render(data);
            }
        };

      public:
        switcher_render_node_t(switcher_grid_output_t *grid) : node_t(false)
        {
            this->grid = grid;
        }

        virtual void gen_render_instances(
            std::vector<wf::scene::render_instance_uptr>& instances,
            wf::scene::damage_callback push_damage, wf::output_t *shown_on)
        {
            if (shown_on != this->grid->output)
            {
                return;
            }

            instances.push_back(std::make_unique<switcher_render_instance_t>(this, push_damage));
        }

        wf::geometry_t get_bounding_box()
        {
            return grid->output->get_layout_geometry();
        }

      private:
        switcher_grid_output_t *grid;
    };

    std::shared_ptr<switcher_render_node_t> render_node;

  public:
    void init() override
    {
        if (!wf::get_core().is_gles2())
        {
            const char *render_type =
                wf::get_core().is_vulkan() ? "vulkan" : (wf::get_core().is_pixman() ? "pixman" : "unknown");
            LOGE("switcher-grid: requires GLES2 support, but current renderer is ", render_type);
            return;
        }

        output->add_key(activate_key, &key_next, wf::binding_state_t::RELEASE);
        output->add_key(activate_backward, &key_prev, wf::binding_state_t::RELEASE);
        input_grab = std::make_unique<wf::input_grab_t>("switcher-grid", output, this, this, nullptr);
        grab_interface.cancel = [=] () { switcher_done(); };
    }

    void handle_pointer_button(const wlr_pointer_button_event& ev) override
    {
        if (!active || ev.state != (wlr_button_state)WLR_BUTTON_RELEASED) return;
        
        auto gc = wf::get_core().get_cursor_position();
        auto og = output->get_layout_geometry();
        gc.x -= og.x;
        gc.y -= og.y;
        
        // Find which thumbnail was clicked
        for (size_t i = 0; i < filtered_views.size(); i++)
        {
            if (!view_data.count(filtered_views[i])) continue;
            
            auto& geom = view_data[filtered_views[i]].target_geometry;
            if (gc.x >= geom.x && gc.x <= geom.x + geom.width &&
                gc.y >= geom.y && gc.y <= geom.y + geom.height)
            {
                // Middle click closes window
                if (ev.button == BTN_MIDDLE)
                {
                    filtered_views[i]->close();
                    // Remove from filtered views and recalculate
                    filtered_views.erase(filtered_views.begin() + i);
                    if (filtered_views.empty())
                    {
                        switcher_done();
                    } else
                    {
                        selected_index = std::min(selected_index, (int)filtered_views.size() - 1);
                        arrange_grid();
                    }
                    return;
                }
                
                // Left click selects and activates
                selected_index = i;
                switcher_done();
                return;
            }
        }
    }

    void calculate_grid_layout(int view_count)
    {
        auto og = output->get_relative_geometry();
        
        thumb_width = thumbnail_width;
        grid_width_pct = grid_width_percent / 100.0f;
        
        float grid_width = og.width * grid_width_pct;
        
        // Calculate number of columns
        grid_cols = std::max(1, (int)(grid_width / thumb_width));
        
        // Calculate number of rows
        grid_rows = (view_count + grid_cols - 1) / grid_cols;
        if (grid_rows < 1) grid_rows = 1;
    }

    wf::geometry_t calculate_thumbnail_geometry(int row, int col, const wf::geometry_t& view_bbox)
    {
        auto og = output->get_relative_geometry();

        float grid_width = og.width * grid_width_pct;

        // Calculate thumbnail height maintaining aspect ratio
        float aspect_ratio = (float)view_bbox.width / view_bbox.height;
        int thumb_height = thumb_width / aspect_ratio;

        // Calculate spacing
        int h_spacing = (grid_width - (grid_cols * thumb_width)) / (grid_cols + 1);
        int v_spacing = 30;

        // Calculate total grid height
        int total_grid_height = grid_rows * thumb_height + (grid_rows + 1) * v_spacing;

        // Starting position (centered)
        int start_x = (og.width - grid_width) / 2;
        int start_y = (og.height - total_grid_height) / 2 + 50;

        wf::geometry_t geom;
        geom.x = start_x + h_spacing + col * (thumb_width + h_spacing);
        geom.y = start_y + v_spacing + row * (thumb_height + v_spacing);
        geom.width = thumb_width;
        geom.height = thumb_height;

        return geom;
    }

    void add_transformer(wayfire_toplevel_view view)
    {
        if (view->get_transformed_node()->get_transformer(SWITCHER_GRID_TRANSFORMER))
        {
            return;
        }

        auto tr = std::make_shared<wf::scene::view_2d_transformer_t>(view);
        view->get_transformed_node()->add_transformer(tr, wf::TRANSFORMER_2D + 1,
            SWITCHER_GRID_TRANSFORMER);

        view_data[view] = view_transform_data{};
    }

    void remove_transformer(wayfire_toplevel_view view)
    {
        if (!view) return;

        view->get_transformed_node()->rem_transformer(SWITCHER_GRID_TRANSFORMER);
        view_data.erase(view);
    }

    void update_views()
    {
        views = output->wset()->get_views(
            wf::WSET_CURRENT_WORKSPACE | wf::WSET_MAPPED_ONLY | wf::WSET_EXCLUDE_MINIMIZED);
        
        // Sort by focus timestamp (most recent first)
        std::sort(views.begin(), views.end(), [] (wayfire_toplevel_view& a, wayfire_toplevel_view& b)
        {
            return get_focus_timestamp(a) > get_focus_timestamp(b);
        });
        
        // Apply search filter
        apply_filter();
    }

    void apply_filter()
    {
        filtered_views.clear();
        
        if (search_query.empty())
        {
            filtered_views = views;
            return;
        }
        
        // Filter by title match
        std::string query_lower = search_query;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
        
        for (auto& view : views)
        {
            std::string title = view->get_title();
            std::transform(title.begin(), title.end(), title.begin(), ::tolower);
            
            if (title.find(query_lower) != std::string::npos)
            {
                filtered_views.push_back(view);
            }
        }
        
        // Adjust selection if needed
        if (selected_index >= (int)filtered_views.size())
        {
            selected_index = std::max(0, (int)filtered_views.size() - 1);
        }
    }

    void handle_keyboard_key(wf::seat_t*, wlr_keyboard_key_event event) override
    {
        if (!active) return;
        
        auto mod = wf::get_core().seat->modifier_from_keycode(event.keycode);
        
        // Check if Alt is released
        if ((event.state == WLR_KEY_RELEASED) && !(mod & WLR_MODIFIER_ALT))
        {
            switcher_done();
            return;
        }
        
        // Handle key presses
        if (event.state != WLR_KEY_PRESSED) return;
        
        uint32_t keycode = event.keycode;
        
        // Handle text input for search (letters and numbers)
        if (keycode >= 2 && keycode <= 11) // Numbers 0-9
        {
            // Get the character from keycode (simplified)
            search_query += (char)('0' + (keycode - 2));
            apply_filter();
            return;
        }
        
        // Backspace removes from search query
        if (keycode == 22) // KEY_BACKSPACE
        {
            if (!search_query.empty())
            {
                search_query.pop_back();
                apply_filter();
            } else if (selected_index >= 0 && selected_index < (int)filtered_views.size())
            {
                // Close selected window if no search query
                filtered_views[selected_index]->close();
                filtered_views.erase(filtered_views.begin() + selected_index);
                if (filtered_views.empty())
                {
                    switcher_done();
                } else
                {
                    selected_index = std::min(selected_index, (int)filtered_views.size() - 1);
                    arrange_grid();
                }
            }
            return;
        }
        
        // Escape clears search
        if (keycode == 1) // KEY_ESC
        {
            search_query.clear();
            apply_filter();
            return;
        }
        
        // Delete closes selected window
        if (keycode == 119) // KEY_DELETE
        {
            if (selected_index >= 0 && selected_index < (int)filtered_views.size())
            {
                filtered_views[selected_index]->close();
                filtered_views.erase(filtered_views.begin() + selected_index);
                if (filtered_views.empty())
                {
                    switcher_done();
                } else
                {
                    selected_index = std::min(selected_index, (int)filtered_views.size() - 1);
                    arrange_grid();
                }
            }
            return;
        }
        
        // Arrow keys for navigation
        if (keycode == 105 || keycode == 106) // Left/Right
        {
            if (keycode == 106) // Right
            {
                select_next();
            } else // Left
            {
                select_prev();
            }
        } else if (keycode == 103 || keycode == 108) // Up/Down
        {
            if (keycode == 108) // Down
            {
                selected_index = (selected_index + grid_cols) % filtered_views.size();
            } else // Up
            {
                selected_index = (selected_index - grid_cols + filtered_views.size()) % filtered_views.size();
            }
            update_selection();
        }
    }

    void arrange_grid()
    {
        update_views();
        
        if (filtered_views.empty())
        {
            return;
        }

        calculate_grid_layout(filtered_views.size());

        // Add transformers and calculate positions
        for (size_t i = 0; i < filtered_views.size(); i++)
        {
            int row = i / grid_cols;
            int col = i % grid_cols;
            
            add_transformer(filtered_views[i]);
            
            auto bbox = filtered_views[i]->get_geometry();
            auto target_geom = calculate_thumbnail_geometry(row, col, bbox);
            
            view_data[filtered_views[i]].target_geometry = target_geom;
            
            // Calculate scale
            float scale = (float)thumb_width / bbox.width;
            view_data[filtered_views[i]].scale_x = scale;
            view_data[filtered_views[i]].scale_y = scale;
            
            // Calculate offset to center
            float center_x = (output->get_relative_geometry().width / 2.0) - (bbox.width / 2.0);
            float center_y = (output->get_relative_geometry().height / 2.0) - (bbox.height / 2.0);
            
            view_data[filtered_views[i]].offset_x = target_geom.x - center_x;
            view_data[filtered_views[i]].offset_y = target_geom.y - center_y;
            
            // Set alpha (highlight selected)
            float alpha = (i == (size_t)selected_index) ? selected_alpha : inactive_alpha;
            view_data[filtered_views[i]].alpha = alpha;
            
            // Apply transform
            apply_transform(filtered_views[i]);
        }
    }

    void apply_transform(wayfire_toplevel_view view)
    {
        if (!view_data.count(view)) return;
        
        auto tr = view->get_transformed_node()->get_transformer<wf::scene::view_2d_transformer_t>(
            SWITCHER_GRID_TRANSFORMER);
        if (!tr) return;
        
        auto& data = view_data[view];
        
        view->get_transformed_node()->begin_transform_update();
        tr->translation_x = data.offset_x;
        tr->translation_y = data.offset_y;
        tr->scale_x = data.scale_x;
        tr->scale_y = data.scale_y;
        tr->alpha = data.alpha;
        view->get_transformed_node()->end_transform_update();
    }

    void select_next()
    {
        if (selected_index >= 0 && selected_index < (int)filtered_views.size())
        {
            view_data[filtered_views[selected_index]].alpha = inactive_alpha;
            apply_transform(filtered_views[selected_index]);
        }
        
        selected_index = (selected_index + 1) % filtered_views.size();
        update_selection();
    }

    void select_prev()
    {
        if (selected_index >= 0 && selected_index < (int)filtered_views.size())
        {
            view_data[filtered_views[selected_index]].alpha = inactive_alpha;
            apply_transform(filtered_views[selected_index]);
        }
        
        selected_index = (selected_index - 1 + filtered_views.size()) % filtered_views.size();
        update_selection();
    }

    void update_selection()
    {
        if (selected_index >= 0 && selected_index < (int)filtered_views.size())
        {
            view_data[filtered_views[selected_index]].alpha = selected_alpha;
            apply_transform(filtered_views[selected_index]);
            wf::get_core().seat->focus_view(filtered_views[selected_index]);
        }
    }

    void switcher_done()
    {
        if (!active) return;
        
        active = false;
        input_grab->ungrab_input();
        output->deactivate_plugin(&grab_interface);
        
        // Remove transformers and restore views
        for (auto& view : filtered_views)
        {
            remove_transformer(view);
        }
        
        views.clear();
        filtered_views.clear();
        view_data.clear();
        selected_index = 0;
        search_query.clear();
        
        // Focus selected view
        if (!filtered_views.empty() && selected_index >= 0 && selected_index < (int)filtered_views.size())
        {
            wf::get_core().default_wm->focus_raise_view(filtered_views[selected_index]);
        }
        
        if (render_node)
        {
            wf::scene::remove_child(render_node);
            render_node = nullptr;
        }
    }

    void switch_next()
    {
        if (!active)
        {
            if (!output->activate_plugin(&grab_interface))
            {
                return;
            }
            
            active = true;
            input_grab->grab_input(wf::scene::layer::OVERLAY);
            selected_index = 0;
            
            render_node = std::make_shared<switcher_render_node_t>(this);
            wf::scene::add_front(wf::get_core().scene(), render_node);
            
            arrange_grid();
        } else
        {
            select_next();
        }
    }

    void switch_prev()
    {
        if (!active)
        {
            switch_next();
            return;
        }
        
        select_prev();
    }

    void render_view(wayfire_view view, const wf::render_target_t& buffer)
    {
        std::vector<wf::scene::render_instance_uptr> instances;
        view->get_transformed_node()->gen_render_instances(instances, [] (auto) {});
        
        wf::render_pass_params_t params;
        params.instances = &instances;
        params.damage = view->get_transformed_node()->get_bounding_box();
        params.reference_output = this->output;
        params.target = buffer;
        wf::render_pass_t::run(params);
    }

    void render(const wf::scene::render_instruction_t& data)
    {
        // Render background (slightly dimmed)
        auto background_views = wf::collect_views_from_output(output,
            {wf::scene::layer::BACKGROUND, wf::scene::layer::BOTTOM});
        
        for (auto view : background_views)
        {
            render_view(view, data.target);
        }

        // Render grid views
        for (auto& view : filtered_views)
        {
            render_view(view, data.target);
        }

        // Render overlay views
        auto overlay_views = wf::collect_views_from_output(output,
            {wf::scene::layer::TOP, wf::scene::layer::OVERLAY, wf::scene::layer::DWIDGET});

        for (auto view : overlay_views)
        {
            render_view(view, data.target);
        }
    }

    void fini() override
    {
        if (active)
        {
            switcher_done();
        }
        
        output->rem_binding(&key_next);
        output->rem_binding(&key_prev);
    }

    wf::key_callback key_next = [=] (auto)
    {
        switch_next();
        return false;
    };

    wf::key_callback key_prev = [=] (auto)
    {
        switch_prev();
        return false;
    };
};

class switcher_grid_global_t : public wf::plugin_interface_t,
    public wf::per_output_tracker_mixin_t<switcher_grid_output_t>
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

DECLARE_WAYFIRE_PLUGIN(switcher_grid_global_t);
