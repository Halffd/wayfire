#include "wayfire/object.hpp"
#include "wayfire/plugins/common/input-grab.hpp"
#include "wayfire/scene-input.hpp"
#include "wayfire/scene-operations.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/view-helpers.hpp"
#include <wayfire/window-manager.hpp>
#include <memory>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/core.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/plugins/common/util.hpp>
#include <wayfire/seat.hpp>

#include <wayfire/view.hpp>
#include <wayfire/output.hpp>
#include <wayfire/signal-definitions.hpp>

#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-set.hpp>

#include <wayfire/util/duration.hpp>
#include <wayfire/nonstd/reverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <set>
#include <cmath>

constexpr const char *switcher_transformer = "switcher-3d";
constexpr const char *switcher_transformer_background = "switcher-3d";
constexpr float background_dim_factor = 0.6;

using namespace wf::animation;
class SwitcherPaintAttribs
{
  public:
    SwitcherPaintAttribs(const duration_t& duration) :
        scale_x(duration, 1, 1), scale_y(duration, 1, 1),
        off_x(duration, 0, 0), off_y(duration, 0, 0), off_z(duration, 0, 0),
        rotation(duration, 0, 0), alpha(duration, 1, 1)
    {}

    timed_transition_t scale_x, scale_y;
    timed_transition_t off_x, off_y, off_z;
    timed_transition_t rotation, alpha;
};

struct SwitcherView
{
    wayfire_toplevel_view view;
    SwitcherPaintAttribs attribs;
    int index; // position in grid
    wf::geometry_t target_geometry; // target position in grid

    SwitcherView(duration_t& duration) : attribs(duration), index(-1)
    {}

    /* Make animation start values the current progress of duration */
    void refresh_start()
    {
        for_each([] (timed_transition_t& t) { t.restart_same_end(); });
    }

    void to_end()
    {
        for_each([] (timed_transition_t& t) { t.set(t.end, t.end); });
    }

  private:
    void for_each(std::function<void(timed_transition_t& t)> call)
    {
        call(attribs.off_x);
        call(attribs.off_y);
        call(attribs.off_z);

        call(attribs.scale_x);
        call(attribs.scale_y);

        call(attribs.alpha);
        call(attribs.rotation);
    }
};

class WayfireSwitcher : public wf::per_output_plugin_instance_t, public wf::keyboard_interaction_t
{
    wf::option_wrapper_t<int> view_thumbnail_width{"switcher/view_thumbnail_width"};
    wf::option_wrapper_t<int> grid_width_percent{"switcher/grid_width_percent"};
    wf::option_wrapper_t<wf::animation_description_t> speed{"switcher/speed"};

    duration_t duration{speed};
    duration_t background_dim_duration{speed};
    timed_transition_t background_dim{background_dim_duration};

    std::unique_ptr<wf::input_grab_t> input_grab;

    std::vector<SwitcherView> views;
    int selected_index = 0; // currently selected thumbnail in grid

    // Grid layout calculations
    int grid_cols = 0;
    int grid_rows = 0;
    int thumbnail_width = 600;
    float grid_width_percentage = 0.9;

    // the modifiers which were used to activate switcher
    uint32_t activating_modifiers = 0;
    bool active = false;

    class switcher_render_node_t : public wf::scene::node_t
    {
        class switcher_render_instance_t : public wf::scene::render_instance_t
        {
            std::shared_ptr<switcher_render_node_t> self;
            wf::scene::damage_callback push_damage;
            wf::signal::connection_t<wf::scene::node_damage_signal> on_switcher_damage =
                [=] (wf::scene::node_damage_signal *ev)
            {
                push_damage(ev->region);
            };

          public:
            switcher_render_instance_t(switcher_render_node_t *self, wf::scene::damage_callback push_damage)
            {
                this->self = std::dynamic_pointer_cast<switcher_render_node_t>(self->shared_from_this());
                this->push_damage = push_damage;
                self->connect(&on_switcher_damage);
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

                // Don't render anything below
                auto bbox = self->get_bounding_box();
                damage ^= bbox;
            }

            void render(const wf::scene::render_instruction_t& data) override
            {
                self->switcher->render(data);
            }
        };

      public:
        switcher_render_node_t(WayfireSwitcher *switcher) : node_t(false)
        {
            this->switcher = switcher;
        }

        virtual void gen_render_instances(
            std::vector<wf::scene::render_instance_uptr>& instances,
            wf::scene::damage_callback push_damage, wf::output_t *shown_on)
        {
            if (shown_on != this->switcher->output)
            {
                return;
            }

            instances.push_back(std::make_unique<switcher_render_instance_t>(this, push_damage));
        }

        wf::geometry_t get_bounding_box()
        {
            return switcher->output->get_layout_geometry();
        }

      private:
        WayfireSwitcher *switcher;
    };

    std::shared_ptr<switcher_render_node_t> render_node;
    wf::plugin_activation_data_t grab_interface = {
        .name = "switcher",
        .capabilities = wf::CAPABILITY_MANAGE_COMPOSITOR,
    };

  public:
    void init() override
    {
        if (!wf::get_core().is_gles2())
        {
            const char *render_type =
                wf::get_core().is_vulkan() ? "vulkan" : (wf::get_core().is_pixman() ? "pixman" : "unknown");
            LOGE("switcher: requires GLES2 support, but current renderer is ", render_type);
            return;
        }

        output->add_key(
            wf::option_wrapper_t<wf::keybinding_t>{"switcher/next_view"},
            &next_view_binding);
        output->add_key(
            wf::option_wrapper_t<wf::keybinding_t>{"switcher/prev_view"},
            &prev_view_binding);
        output->connect(&view_disappeared);

        input_grab = std::make_unique<wf::input_grab_t>("switcher", output, this, nullptr, nullptr);
        grab_interface.cancel = [=] () {deinit_switcher();};
    }

    void handle_keyboard_key(wf::seat_t*, wlr_keyboard_key_event event) override
    {
        auto mod = wf::get_core().seat->modifier_from_keycode(event.keycode);
        if ((event.state == WLR_KEY_RELEASED) && (mod & activating_modifiers))
        {
            handle_done();
        }
    }

    wf::key_callback next_view_binding = [=] (auto)
    {
        handle_switch_request(-1);
        return false;
    };

    wf::key_callback prev_view_binding = [=] (auto)
    {
        handle_switch_request(1);
        return false;
    };

    wf::effect_hook_t pre_hook = [=] ()
    {
        dim_background(background_dim);
        wf::scene::damage_node(render_node, render_node->get_bounding_box());

        if (!duration.running())
        {
            cleanup_expired();
            if (!active)
            {
                deinit_switcher();
            }
        }
    };

    wf::signal::connection_t<wf::view_disappeared_signal> view_disappeared =
        [=] (wf::view_disappeared_signal *ev)
    {
        if (auto toplevel = toplevel_cast(ev->view))
        {
            handle_view_removed(toplevel);
        }
    };

    void handle_view_removed(wayfire_toplevel_view view)
    {
        // not running at all, don't care
        if (!output->is_plugin_active(grab_interface.name))
        {
            return;
        }

        bool need_action = false;
        for (auto& sv : views)
        {
            need_action |= (sv.view == view);
        }

        // don't do anything if we're not using this view
        if (!need_action)
        {
            return;
        }

        if (active)
        {
            arrange();
        } else
        {
            cleanup_views([=] (SwitcherView& sv)
            { return sv.view == view; });
        }
    }

    bool handle_switch_request(int dir)
    {
        if (get_workspace_views().empty())
        {
            return false;
        }

        /* If we haven't grabbed, then we haven't setup anything */
        if (!output->is_plugin_active(grab_interface.name))
        {
            if (!init_switcher())
            {
                return false;
            }
        }

        /* Maybe we're still animating the exit animation from a previous
         * switcher activation? */
        if (!active)
        {
            active = true;
            input_grab->grab_input(wf::scene::layer::OVERLAY);

            focus_next(dir);
            arrange();
            activating_modifiers = wf::get_core().seat->get_keyboard_modifiers();
        } else
        {
            next_view(dir);
        }

        return true;
    }

    /* When switcher is done and starts animating towards end */
    void handle_done()
    {
        cleanup_expired();
        dearrange();
        input_grab->ungrab_input();
    }

    /* Sets up basic hooks needed while switcher works and/or displays animations.
     * Also lower any fullscreen views that are active */
    bool init_switcher()
    {
        if (!output->activate_plugin(&grab_interface))
        {
            return false;
        }

        output->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);

        render_node = std::make_shared<switcher_render_node_t>(this);
        wf::scene::add_front(wf::get_core().scene(), render_node);
        return true;
    }

    /* The reverse of init_switcher */
    void deinit_switcher()
    {
        output->deactivate_plugin(&grab_interface);

        output->render->rem_effect(&pre_hook);
        wf::scene::remove_child(render_node);
        render_node = nullptr;

        for (auto& view : output->wset()->get_views())
        {
            if (view->has_data("switcher-minimized-showed"))
            {
                view->erase_data("switcher-minimized-showed");
                wf::scene::set_node_enabled(view->get_root_node(), false);
            }

            view->get_transformed_node()->rem_transformer(switcher_transformer);
            view->get_transformed_node()->rem_transformer(
                switcher_transformer_background);
        }

        views.clear();

        wf::scene::update(wf::get_core().scene(),
            wf::scene::update_flag::INPUT_STATE);
    }

    /* Calculate grid layout: number of rows and columns */
    void calculate_grid_layout(int view_count)
    {
        auto og = output->get_relative_geometry();
        
        thumbnail_width = view_thumbnail_width;
        grid_width_percentage = grid_width_percent / 100.0;
        
        float grid_width = og.width * grid_width_percentage;
        
        // Calculate number of columns that fit in grid width
        grid_cols = std::max(1, (int)(grid_width / thumbnail_width));
        
        // Ensure at least 1 column
        if (grid_cols < 1)
        {
            grid_cols = 1;
        }
        
        // Calculate number of rows needed
        grid_rows = (view_count + grid_cols - 1) / grid_cols;
        
        // Ensure at least 1 row
        if (grid_rows < 1)
        {
            grid_rows = 1;
        }
    }

    /* Calculate the target geometry for a thumbnail at given grid position */
    wf::geometry_t calculate_thumbnail_geometry(int row, int col, const wf::geometry_t& view_bbox)
    {
        auto og = output->get_relative_geometry();
        
        float grid_width = og.width * grid_width_percentage;
        
        // Calculate thumbnail height maintaining aspect ratio
        float aspect_ratio = (float)view_bbox.width / view_bbox.height;
        int thumb_height = thumbnail_width / aspect_ratio;
        
        // Calculate spacing
        int h_spacing = (grid_width - (grid_cols * thumbnail_width)) / (grid_cols + 1);
        int v_spacing = 20; // vertical spacing between rows
        
        // Calculate total grid height
        int total_grid_height = grid_rows * thumb_height + (grid_rows + 1) * v_spacing;
        
        // Starting position (centered)
        int start_x = (og.width - grid_width) / 2;
        int start_y = (og.height - total_grid_height) / 2;
        
        wf::geometry_t geom;
        geom.x = start_x + h_spacing + col * (thumbnail_width + h_spacing);
        geom.y = start_y + v_spacing + row * (thumb_height + v_spacing);
        geom.width = thumbnail_width;
        geom.height = thumb_height;
        
        return geom;
    }

    /* Calculate alpha for the view when switcher is inactive. */
    float get_view_normal_alpha(wayfire_toplevel_view view)
    {
        /* Usually views are visible, but if they were minimized,
         * and we aren't restoring the view, it has target alpha 0.0 */
        if (view->minimized && (views.empty() || (view != views[0].view)))
        {
            return 0.0;
        }

        return 1.0;
    }

    // returns a list of mapped views
    std::vector<wayfire_toplevel_view> get_workspace_views() const
    {
        return output->wset()->get_views(wf::WSET_MAPPED_ONLY | wf::WSET_CURRENT_WORKSPACE);
    }

    /* Change the current focus to the next or the previous view */
    void focus_next(int dir)
    {
        auto ws_views = get_workspace_views();
        /* Change the focused view and rearrange views so that focused is on top */
        int size = ws_views.size();

        // calculate focus index & focus it
        int focused_view_index = (size + dir) % size;
        auto focused_view = ws_views[focused_view_index];
        wf::view_bring_to_front(focused_view);
    }

    /* Create the initial arrangement on the screen
     * Also changes the focus to the next or the last view, depending on dir */
    void arrange()
    {
        // clear views in case that deinit() hasn't been run
        views.clear();

        duration.start();
        background_dim.set(1, background_dim_factor);
        background_dim_duration.start();

        auto ws_views = get_workspace_views();
        for (auto v : ws_views)
        {
            views.push_back(create_switcher_view(v));
        }

        std::sort(views.begin(), views.end(), [] (SwitcherView& a, SwitcherView& b)
        {
            return wf::get_focus_timestamp(a.view) > wf::get_focus_timestamp(b.view);
        });

        if (ws_views.empty())
        {
            return;
        }

        // Calculate grid layout
        calculate_grid_layout(views.size());

        // Assign grid positions to each view
        for (size_t i = 0; i < views.size(); i++)
        {
            int row = i / grid_cols;
            int col = i % grid_cols;
            views[i].index = i;
            
            auto bbox = wf::view_bounding_box_up_to(views[i].view, switcher_transformer);
            views[i].target_geometry = calculate_thumbnail_geometry(row, col, bbox);
        }

        // Set initial animation targets
        for (auto& sv : views)
        {
            animate_to_grid_position(sv);
        }

        // We want the next view to be focused right off the bat
        // But we want it to be animated.
        handle_switch_request(-1);
    }

    void animate_to_grid_position(SwitcherView& sv)
    {
        auto bbox = wf::view_bounding_box_up_to(sv.view, switcher_transformer);
        
        sv.attribs.off_x.set(0, sv.target_geometry.x - bbox.x);
        sv.attribs.off_y.set(0, sv.target_geometry.y - bbox.y);
        sv.attribs.off_z.set(0, 0);

        // Calculate scale to fit thumbnail width
        float scale = (float)thumbnail_width / bbox.width;
        sv.attribs.scale_x.set(1, scale);
        sv.attribs.scale_y.set(1, scale);
        sv.attribs.rotation.set(0, 0);
        sv.attribs.alpha.set(get_view_normal_alpha(sv.view), 1.0);
    }

    void dearrange()
    {
        for (auto& sv : views)
        {
            sv.attribs.off_x.restart_with_end(0);
            sv.attribs.off_y.restart_with_end(0);
            sv.attribs.off_z.restart_with_end(0);

            sv.attribs.scale_x.restart_with_end(1.0);
            sv.attribs.scale_y.restart_with_end(1.0);

            sv.attribs.rotation.restart_with_end(0);
            sv.attribs.alpha.restart_with_end(get_view_normal_alpha(sv.view));
        }

        background_dim.restart_with_end(1);
        background_dim_duration.start();
        duration.start();
        active = false;

        /* Potentially restore view[0] if it was maximized */
        if (views.size())
        {
            wf::get_core().default_wm->focus_raise_view(views[0].view);
        }
    }

    std::vector<wayfire_view> get_background_views() const
    {
        return wf::collect_views_from_output(output,
            {wf::scene::layer::BACKGROUND, wf::scene::layer::BOTTOM});
    }

    std::vector<wayfire_view> get_overlay_views() const
    {
        return wf::collect_views_from_output(output,
            {wf::scene::layer::TOP, wf::scene::layer::OVERLAY, wf::scene::layer::DWIDGET});
    }

    void dim_background(float dim)
    {
        for (auto view : get_background_views())
        {
            if (dim == 1.0)
            {
                view->get_transformed_node()->rem_transformer(
                    switcher_transformer_background);
            } else
            {
                auto tr =
                    wf::ensure_named_transformer<wf::scene::view_3d_transformer_t>(
                        view, wf::TRANSFORMER_3D, switcher_transformer_background,
                        view);
                tr->color[0] = tr->color[1] = tr->color[2] = dim;
            }
        }
    }

    SwitcherView create_switcher_view(wayfire_toplevel_view view)
    {
        /* we add a view transform if there isn't any.
         *
         * Note that a view might be visible on more than 1 place, so damage
         * tracking doesn't work reliably. To circumvent this, we simply damage
         * the whole output */
        if (!view->get_transformed_node()->get_transformer(switcher_transformer))
        {
            if (view->minimized)
            {
                wf::scene::set_node_enabled(view->get_root_node(), true);
                view->store_data(std::make_unique<wf::custom_data_t>(),
                    "switcher-minimized-showed");
            }

            view->get_transformed_node()->add_transformer(
                std::make_shared<wf::scene::view_3d_transformer_t>(view),
                wf::TRANSFORMER_3D, switcher_transformer);
        }

        SwitcherView sw{duration};
        sw.view     = view;
        sw.index    = -1;

        return sw;
    }

    void render_view_scene(wayfire_view view, const wf::render_target_t& buffer)
    {
        std::vector<wf::scene::render_instance_uptr> instances;
        view->get_transformed_node()->gen_render_instances(instances, [] (auto) {});

        wf::render_pass_params_t params;
        params.instances = &instances;
        params.damage    = view->get_transformed_node()->get_bounding_box();
        params.reference_output = this->output;
        params.target = buffer;
        wf::render_pass_t::run(params);
    }

    void render_view(const SwitcherView& sv, const wf::render_target_t& buffer)
    {
        auto transform = sv.view->get_transformed_node()
            ->get_transformer<wf::scene::view_3d_transformer_t>(switcher_transformer);
        assert(transform);

        transform->translation = glm::translate(glm::mat4(1.0),
        {(double)sv.attribs.off_x, (double)sv.attribs.off_y,
            (double)sv.attribs.off_z});

        transform->scaling = glm::scale(glm::mat4(1.0),
            {(double)sv.attribs.scale_x, (double)sv.attribs.scale_y, 1.0});

        transform->rotation = glm::rotate(glm::mat4(1.0),
            (float)sv.attribs.rotation, {0.0, 1.0, 0.0});

        transform->color[3] = sv.attribs.alpha;
        render_view_scene(sv.view, buffer);
    }

    void render(const wf::scene::render_instruction_t& data)
    {
        data.pass->clear(data.target.geometry, {0, 0, 0, 1});

        auto local_target = data.target.translated(-wf::origin(render_node->get_bounding_box()));
        for (auto view : get_background_views())
        {
            render_view_scene(view, local_target);
        }

        /* Render in the reverse order because we don't use depth testing */
        for (auto& view : wf::reverse(views))
        {
            render_view(view, local_target);
        }

        for (auto view : get_overlay_views())
        {
            render_view_scene(view, local_target);
        }
    }

    /* delete all views matching the given criteria, skipping the first "start" views
     * */
    void cleanup_views(std::function<bool(SwitcherView&)> criteria)
    {
        auto it = views.begin();
        while (it != views.end())
        {
            if (criteria(*it))
            {
                it = views.erase(it);
            } else
            {
                ++it;
            }
        }
    }

    /* Removes all expired views from the list */
    void cleanup_expired()
    {
        // Grid layout doesn't have expired views
    }

    void next_view(int dir)
    {
        if (views.empty())
        {
            return;
        }

        // Update selected index
        int new_index = (selected_index + dir + views.size()) % views.size();
        
        // Bring selected view to front
        wf::view_bring_to_front(views[new_index].view);
        selected_index = new_index;
        
        duration.start();
    }

    void fini() override
    {
        if (output->is_plugin_active(grab_interface.name))
        {
            input_grab->ungrab_input();
            deinit_switcher();
        }

        output->rem_binding(&next_view_binding);
        output->rem_binding(&prev_view_binding);
    }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<WayfireSwitcher>);
