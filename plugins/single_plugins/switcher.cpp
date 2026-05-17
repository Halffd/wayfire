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
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

constexpr const char *switcher_transformer = "switcher-3d";
constexpr const char *switcher_transformer_background = "switcher-3d";

using namespace wf::animation;
class SwitcherPaintAttribs
{
  public:
    SwitcherPaintAttribs(wf::option_sptr_t<wf::animation_description_t> speed) :
        timer{speed},
        scale_x(timer, 1, 1), scale_y(timer, 1, 1),
        off_x(timer, 0, 0), off_y(timer, 0, 0), off_z(timer, 0, 0),
        rotation(timer, 0, 0), alpha(timer, 1, 1)
    {}

    duration_t timer;
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

    SwitcherView(const wf::option_sptr_t<wf::animation_description_t>& duration) :
        attribs(duration), index(-1)
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

    bool animation_finished()
    {
        return attribs.timer.running() == false;
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

class WayfireSwitcher : public wf::per_output_plugin_instance_t, public wf::keyboard_interaction_t, public wf::pointer_interaction_t
{
  wf::option_wrapper_t<int> view_thumbnail_width{"switcher/view_thumbnail_width"};
  wf::option_wrapper_t<int> grid_width_percent{"switcher/grid_width_percent"};
  wf::option_wrapper_t<wf::animation_description_t> speed{"switcher/speed"};
  wf::option_wrapper_t<double> selected_scale{"switcher/selected_scale"};
  wf::option_wrapper_t<double> inactive_alpha_opt{"switcher/inactive_alpha"};
  wf::option_wrapper_t<int> v_spacing{"switcher/vertical_spacing"};
  wf::option_wrapper_t<double> background_dim_opt{"switcher/background_dim"};
  wf::option_wrapper_t<bool> click_to_select{"switcher/click_to_select"};

  duration_t background_dim_duration{speed};
  timed_transition_t background_dim{background_dim_duration};

  std::unique_ptr<wf::input_grab_t> input_grab;

  std::vector<SwitcherView> views;
  std::vector<SwitcherView*> filtered_views;
  std::string search_query;
  int selected_index = 0;

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

        input_grab = std::make_unique<wf::input_grab_t>("switcher", output, this, this, nullptr);
        grab_interface.cancel = [=] () {deinit_switcher();};
    }

  void handle_pointer_button(const wlr_pointer_button_event& ev) override
  {
    if (!active || (int)ev.state != (int)WLR_BUTTON_RELEASED)
    {
      return;
    }

    if (!click_to_select)
    {
      return;
    }

    auto gc = wf::get_core().get_cursor_position();
    auto og = output->get_layout_geometry();
    gc.x -= og.x;
    gc.y -= og.y;

    for (size_t i = 0; i < filtered_views.size(); i++)
    {
      if (gc.x >= filtered_views[i]->target_geometry.x &&
          gc.x <= filtered_views[i]->target_geometry.x + filtered_views[i]->target_geometry.width &&
          gc.y >= filtered_views[i]->target_geometry.y &&
          gc.y <= filtered_views[i]->target_geometry.y + filtered_views[i]->target_geometry.height)
      {
        if (ev.button == BTN_MIDDLE)
        {
          close_selected_view(i);
          return;
        }

        selected_index = i;
        handle_done();
        return;
      }
    }
  }

  void close_selected_view(int idx)
  {
    if (idx < 0 || idx >= (int)filtered_views.size())
    {
      return;
    }

    auto view_to_close = filtered_views[idx]->view;
    view_to_close->close();

    cleanup_views([=] (SwitcherView& sv)
    {
      return sv.view == view_to_close;
    });

    apply_filter();

    if (filtered_views.empty())
    {
      handle_done();
      return;
    }

    if (selected_index >= (int)filtered_views.size())
    {
      selected_index = (int)filtered_views.size() - 1;
    }

    recalc_grid_animation();
  }

  void handle_keyboard_key(wf::seat_t*, wlr_keyboard_key_event event) override
  {
    if (!active)
    {
      auto mod = wf::get_core().seat->modifier_from_keycode(event.keycode);
      if ((event.state == WLR_KEY_RELEASED) && (mod & activating_modifiers))
      {
        handle_done();
      }
      return;
    }

    if (event.state != WLR_KEY_PRESSED)
    {
      auto mod = wf::get_core().seat->modifier_from_keycode(event.keycode);
      if ((event.state == WLR_KEY_RELEASED) && (mod & activating_modifiers))
      {
        handle_done();
      }
      return;
    }

    auto mod = wf::get_core().seat->get_keyboard_modifiers();
    if (active && !mod)
    {
      switch (event.keycode)
      {
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_UP:
        case KEY_DOWN:
        case KEY_HOME:
        case KEY_END:
        case KEY_ENTER:
        case KEY_ESC:
        case KEY_DELETE:
        case KEY_BACKSPACE:
        case KEY_TAB:
        case KEY_PAGEUP:
        case KEY_PAGEDOWN:
          break;

        default:
        {
          xkb_state *xkb = wf::get_core().seat->get_xkb_state();
          if (xkb)
          {
            xkb_keycode_t xkb_keycode = event.keycode + 8;
            uint32_t ch32 = xkb_state_key_get_utf32(xkb, xkb_keycode);
            if (ch32 >= ' ' && ch32 < 0x7f)
            {
              char ch = (ch32 >= 'A' && ch32 <= 'Z') ?
                (char)(ch32 - 'A' + 'a') : (char)ch32;
              search_query += ch;
              apply_filter();
              recalc_grid_animation();
              return;
            }
          }
          break;
        }
      }
    }

    switch (event.keycode)
    {
    case KEY_LEFT:
      next_view(-1);
      break;

    case KEY_RIGHT:
      next_view(1);
      break;

    case KEY_TAB:
      next_view(1);
      break;

      case KEY_UP:
        if (filtered_views.empty()) break;
        {
          int cur_row = selected_index / grid_cols;
          int cur_col = selected_index % grid_cols;
          int new_row = (cur_row - 1 + grid_rows) % grid_rows;
          int new_index = new_row * grid_cols + cur_col;
          if (new_index >= (int)filtered_views.size())
          {
            new_index = (int)filtered_views.size() - 1;
          }
          selected_index = new_index;
          recalc_grid_animation();
        }
        break;

      case KEY_DOWN:
        if (filtered_views.empty()) break;
        {
          int cur_row = selected_index / grid_cols;
          int cur_col = selected_index % grid_cols;
          int new_row = (cur_row + 1) % grid_rows;
          int new_index = new_row * grid_cols + cur_col;
          if (new_index >= (int)filtered_views.size())
          {
            new_index = (int)filtered_views.size() - 1;
          }
          selected_index = new_index;
          recalc_grid_animation();
        }
        break;

      case KEY_HOME:
        if (!filtered_views.empty())
        {
          selected_index = 0;
          recalc_grid_animation();
        }
        break;

      case KEY_END:
        if (!filtered_views.empty())
        {
          selected_index = (int)filtered_views.size() - 1;
          recalc_grid_animation();
        }
        break;

      case KEY_ENTER:
        handle_done();
        break;

      case KEY_ESC:
        if (!search_query.empty())
        {
          search_query.clear();
          apply_filter();
          recalc_grid_animation();
          break;
        }

        selected_index = 0;
        handle_done();
        break;

      case KEY_DELETE:
        close_selected_view(selected_index);
        break;

      case KEY_BACKSPACE:
        if (!search_query.empty())
        {
          search_query.pop_back();
          apply_filter();
          recalc_grid_animation();
        } else
        {
          close_selected_view(selected_index);
        }
        break;

      default:
        {
          auto mod = wf::get_core().seat->modifier_from_keycode(event.keycode);
          if (mod & activating_modifiers)
          {
            handle_done();
          }
        }
        break;
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

    bool animations_running()
    {
        return std::any_of(views.begin(), views.end(), [] (SwitcherView& sv)
        {
            return !sv.animation_finished();
        });
    }

    wf::effect_hook_t pre_hook = [=] ()
    {
        dim_background(background_dim);
        wf::scene::damage_node(render_node, render_node->get_bounding_box());

        if (!animations_running())
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
    if (!output->is_plugin_active(grab_interface.name))
    {
      return;
    }

    bool need_action = false;
    for (auto& sv : views)
    {
      need_action |= (sv.view == view);
    }

    if (!need_action)
    {
      return;
    }

    if (active)
    {
      cleanup_views([=] (SwitcherView& sv)
      { return sv.view == view; });
      apply_filter();
      if (filtered_views.empty())
      {
        handle_done();
      } else
      {
        if (selected_index >= (int)filtered_views.size())
        {
          selected_index = (int)filtered_views.size() - 1;
        }
        recalc_grid_animation();
      }
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

    if (!output->is_plugin_active(grab_interface.name))
    {
      if (!init_switcher())
      {
        return false;
      }
    }

    if (!active)
    {
      active = true;
      input_grab->grab_input(wf::scene::layer::OVERLAY);
      arrange(dir);
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
    if (!filtered_views.empty())
    {
      wf::get_core().default_wm->focus_raise_view(filtered_views[selected_index]->view);
    }
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
    filtered_views.clear();
    search_query.clear();

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
    int vert_spacing = v_spacing;

        // Calculate total grid height
    int total_grid_height = grid_rows * thumb_height + (grid_rows + 1) * vert_spacing;

    int start_x = (og.width - grid_width) / 2;
    int start_y = (og.height - total_grid_height) / 2;

    wf::geometry_t geom;
    geom.x = start_x + h_spacing + col * (thumbnail_width + h_spacing);
    geom.y = start_y + vert_spacing + row * (thumb_height + vert_spacing);
        geom.width  = thumbnail_width;
        geom.height = thumb_height;

        return geom;
    }

    /* Calculate alpha for the view when switcher is inactive. */
  float get_view_normal_alpha(wayfire_toplevel_view view)
  {
    if (view->minimized && (filtered_views.empty() || (view != filtered_views[0]->view)))
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

    /**
     * Create initial arrangement on screen.
     * The center view is directly the new focus, depending on the direction
     * (-1 for right aka second in the focus order, 1 for left aka last in the
     * focus order, 0 for no change in focus).
     */
    void arrange(int focus_direction)
    {
        // clear views in case that deinit() hasn't been run
    views.clear();
    background_dim.set(1, (double)background_dim_opt);
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

    // Start with the first view selected
    selected_index = 0;
    search_query.clear();
    apply_filter();
  }

  void animate_to_grid_position(SwitcherView& sv)
  {
    auto bbox = wf::view_bounding_box_up_to(sv.view, switcher_transformer);

    sv.attribs.off_x.set(0, sv.target_geometry.x - bbox.x);
    sv.attribs.off_y.set(0, sv.target_geometry.y - bbox.y);
    sv.attribs.off_z.set(0, 0);

    float scale = (float)thumbnail_width / bbox.width;
    float scale_target = (sv.index == selected_index) ? scale * (float)(double)selected_scale : scale;
    sv.attribs.scale_x.set(1, scale_target);
    sv.attribs.scale_y.set(1, scale_target);
    sv.attribs.rotation.set(0, 0);

    float alpha_target = (sv.index == selected_index) ? 1.0 : (float)(double)inactive_alpha_opt;
    sv.attribs.alpha.set(get_view_normal_alpha(sv.view), alpha_target);
    sv.attribs.timer.start();
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
      sv.attribs.timer.start();
    }

    background_dim.restart_with_end(1);
    background_dim_duration.start();
    active = false;

    if (!filtered_views.empty())
    {
      wf::get_core().default_wm->focus_raise_view(filtered_views[0]->view);
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

        SwitcherView sw{speed};
        sw.view = view;
        sw.index = -1;

        return sw;
    }

    void render_view_scene(wayfire_view view, const wf::render_target_t& buffer)
    {
        std::vector<wf::scene::render_instance_uptr> instances;
        view->get_transformed_node()->gen_render_instances(instances, [] (auto) {});

        wf::render_pass_params_t params;
        params.instances  = &instances;
        params.damage     = view->get_transformed_node()->get_bounding_box();
        params.reference_output = this->output;
        params.target     = buffer;
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
    for (auto it = filtered_views.rbegin(); it != filtered_views.rend(); ++it)
    {
      render_view(**it, local_target);
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

  void apply_filter()
  {
    filtered_views.clear();

    if (search_query.empty())
    {
      for (auto& sv : views)
      {
        filtered_views.push_back(&sv);
      }
    } else
    {
      std::string query_lower = search_query;
      std::transform(query_lower.begin(), query_lower.end(),
        query_lower.begin(), ::tolower);

      for (auto& sv : views)
      {
        std::string title = sv.view->get_title();
        std::transform(title.begin(), title.end(), title.begin(), ::tolower);
        if (title.find(query_lower) != std::string::npos)
        {
          filtered_views.push_back(&sv);
        }
      }
    }

    if (selected_index >= (int)filtered_views.size())
    {
      selected_index = std::max(0, (int)filtered_views.size() - 1);
    }
  }

  void cleanup_expired()
  {
    auto it = views.begin();
    bool changed = false;
    while (it != views.end())
    {
      if (!it->view->is_mapped())
      {
        it = views.erase(it);
        changed = true;
      } else
      {
        ++it;
      }
    }

    if (changed)
    {
      apply_filter();
      if (!filtered_views.empty())
      {
        recalc_grid_animation();
      }
    }
  }

  void recalc_grid_animation()
  {
    if (filtered_views.empty())
    {
      return;
    }

    if (selected_index >= 0 && selected_index < (int)filtered_views.size())
    {
      wf::view_bring_to_front(filtered_views[selected_index]->view);
    }

    calculate_grid_layout(filtered_views.size());
    for (size_t i = 0; i < filtered_views.size(); i++)
    {
      int row = i / grid_cols;
      int col = i % grid_cols;
      filtered_views[i]->index = i;

      auto bbox = wf::view_bounding_box_up_to(filtered_views[i]->view, switcher_transformer);
      filtered_views[i]->target_geometry = calculate_thumbnail_geometry(row, col, bbox);

      filtered_views[i]->refresh_start();
      animate_to_grid_position(*filtered_views[i]);
    }
  }

  void next_view(int dir)
  {
    if (filtered_views.empty())
    {
      return;
    }

    selected_index = (selected_index + dir + (int)filtered_views.size()) % (int)filtered_views.size();
    recalc_grid_animation();
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
