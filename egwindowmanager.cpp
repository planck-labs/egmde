/*
 * Copyright © 2016-20 Octopull Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alan Griffiths <alan@octopull.co.uk>
 */

#include "egwindowmanager.h"
#include "egshellcommands.h"
#include "egwallpaper.h"

#include <miral/application_info.h>
#include <miral/window_info.h>
#include <miral/window_manager_tools.h>

#include <linux/input.h>

using namespace mir::geometry;
using namespace miral;

namespace
{
struct WorkspaceInfo
{
    bool in_hidden_workspace{false};

    MirWindowState old_state;
};

inline WorkspaceInfo& workspace_info_for(WindowInfo const& info)
{
    return *std::static_pointer_cast<WorkspaceInfo>(info.userdata());
}
}

egmde::WindowManagerPolicy::WindowManagerPolicy(
    WindowManagerTools const& tools,
    Wallpaper const& wallpaper,
    ShellCommands& commands,
    int const& no_of_workspaces) :
    MinimalWindowManager{tools},
    wallpaper{&wallpaper},
    commands{&commands}
{
    commands.init_window_manager(this);

    for (auto i = 0; i != no_of_workspaces; ++i)
        workspaces.push_back(this->tools.create_workspace());

    active_workspace = workspaces.begin();
}

miral::WindowSpecification egmde::WindowManagerPolicy::place_new_window(
    miral::ApplicationInfo const& app_info, miral::WindowSpecification const& request_parameters)
{
    auto result = MinimalWindowManager::place_new_window(app_info, request_parameters);

    if (app_info.application() == wallpaper->session())
    {
        result.type() = mir_window_type_decoration;
    }

    result.userdata() = std::make_shared<WorkspaceInfo>();
    return result;
}

void egmde::WindowManagerPolicy::advise_new_window(const miral::WindowInfo &window_info)
{
    WindowManagementPolicy::advise_new_window(window_info);
    if (window_info.window().application() == wallpaper->session())
    {
        commands->add_shell_app(wallpaper->session());
    }

    commands->advise_new_window_for(window_info.window().application());

    if (auto const& parent = window_info.parent())
    {
        if (workspace_info_for(tools.info_for(parent)).in_hidden_workspace)
            apply_workspace_hidden_to(window_info.window());
    }
    else
    {
        tools.add_tree_to_workspace(window_info.window(), *active_workspace);
    }
}

void egmde::WindowManagerPolicy::advise_delete_window(const miral::WindowInfo &window_info)
{
    WindowManagementPolicy::advise_delete_window(window_info);
    commands->advise_delete_window_for(window_info.window().application());
}

void egmde::WindowManagerPolicy::advise_delete_app(miral::ApplicationInfo const& application)
{
    WindowManagementPolicy::advise_delete_app(application);

    commands->del_shell_app(application.application());
}

void egmde::WindowManagerPolicy::workspace_up(bool take_active)
{
    tools.invoke_under_lock(
        [this, take_active]
        {
            auto const& window = take_active ? tools.active_window() : Window{};
            auto const& old_active = *active_workspace;
            auto const& new_active = *((active_workspace != workspaces.begin()) ? --active_workspace
                                                                                : (active_workspace = --workspaces.end()));
            change_active_workspace(new_active, old_active, window);
        });
}

void egmde::WindowManagerPolicy::workspace_down(bool take_active)
{
    tools.invoke_under_lock(
        [this, take_active]
        {
            auto const& window = take_active ? tools.active_window() : Window{};
            auto const& old_active = *active_workspace;
            auto const& new_active = *((++active_workspace != workspaces.end()) ? active_workspace : (active_workspace = workspaces.begin()));
            change_active_workspace(new_active, old_active, window);
        });
}

void egmde::WindowManagerPolicy::dock_active_window_left()
{
    tools.invoke_under_lock(
        [this]
        {
            if (auto active_window = tools.active_window())
            {
                auto active_output = tools.active_output();
                auto& window_info = tools.info_for(active_window);
                WindowSpecification modifications;

                modifications.state() = mir_window_state_vertmaximized;
                tools.place_and_size_for_state(modifications, window_info);
                modifications.top_left() = active_output.top_left;
                tools.modify_window(window_info, modifications);
            }
        });
}

void egmde::WindowManagerPolicy::dock_active_window_right()
{
    tools.invoke_under_lock(
        [this]
        {
            if (auto active_window = tools.active_window())
            {
                auto active_output = tools.active_output();
                auto& window_info = tools.info_for(active_window);
                WindowSpecification modifications;

                modifications.state() = mir_window_state_vertmaximized;
                tools.place_and_size_for_state(modifications, window_info);

                if (modifications.size().is_set())
                {
                    modifications.top_left() =
                        active_output.top_right() - as_delta(modifications.size().value().width);
                }
                else
                {
                    modifications.top_left() =
                        active_output.top_right() - as_delta(active_window.size().width);
                }

                tools.modify_window(window_info, modifications);
            }
        });
}

void egmde::WindowManagerPolicy::apply_workspace_hidden_to(Window const& window)
{
    auto const& window_info = tools.info_for(window);
    auto& workspace_info = workspace_info_for(window_info);
    if (!workspace_info.in_hidden_workspace)
    {
        workspace_info.in_hidden_workspace = true;
        workspace_info.old_state = window_info.state();

        WindowSpecification modifications;
        modifications.state() = mir_window_state_hidden;
        tools.place_and_size_for_state(modifications, window_info);
        tools.modify_window(window_info.window(), modifications);
    }
}

void egmde::WindowManagerPolicy::apply_workspace_visible_to(Window const& window)
{
    auto const& window_info = tools.info_for(window);
    auto& workspace_info = workspace_info_for(window_info);
    if (workspace_info.in_hidden_workspace)
    {
        workspace_info.in_hidden_workspace = false;
        WindowSpecification modifications;
        modifications.state() = workspace_info.old_state;
        tools.place_and_size_for_state(modifications, window_info);
        tools.modify_window(window_info.window(), modifications);
    }
}

void egmde::WindowManagerPolicy::handle_modify_window(WindowInfo& window_info, WindowSpecification const& modifications)
{
    auto mods = modifications;

    auto& workspace_info = workspace_info_for(window_info);

    if (workspace_info.in_hidden_workspace && mods.state().is_set())
        workspace_info.old_state = mods.state().consume();

    MinimalWindowManager::handle_modify_window(window_info, mods);
}

void egmde::WindowManagerPolicy::change_active_workspace(
    std::shared_ptr<Workspace> const& new_active,
    std::shared_ptr<Workspace> const& old_active,
    Window const& window)
{
    if (new_active == old_active) return;

    auto const old_active_window = tools.active_window();

    if (!old_active_window)
    {
        // If there's no active window, the first shown grabs focus: get the right one
        if (auto const ww = workspace_to_active[new_active])
        {
            tools.for_each_workspace_containing(ww, [&](std::shared_ptr<Workspace> const& ws)
            {
                if (ws == new_active)
                {
                    apply_workspace_visible_to(ww);
                }
            });
        }
    }

    tools.remove_tree_from_workspace(window, old_active);
    tools.add_tree_to_workspace(window, new_active);

    tools.for_each_window_in_workspace(new_active, [&](Window const& ww)
    {
        if (ww.application() == wallpaper->session())
            return; // wallpaper is taken care of automatically

        apply_workspace_visible_to(ww);
    });

    bool hide_old_active = false;
    tools.for_each_window_in_workspace(old_active, [&](Window const& ww)
    {
        if (ww.application() == wallpaper->session())
            return; // wallpaper is taken care of automatically

        if (ww == old_active_window)
        {
            // If we hide the active window focus will shift: do that last
            hide_old_active = true;
            return;
        }

        apply_workspace_hidden_to(ww);
    });

    if (hide_old_active)
    {
        apply_workspace_hidden_to(old_active_window);

        // Remember the old active_window when we switch away
        workspace_to_active[old_active] = old_active_window;
    }
}

void egmde::WindowManagerPolicy::advise_adding_to_workspace(std::shared_ptr<Workspace> const& workspace,
                                                            std::vector<Window> const& windows)
{
    if (windows.empty())
        return;

    for (auto const& window : windows)
    {
        if (workspace == *active_workspace)
        {
            apply_workspace_visible_to(window);
        }
        else
        {
            apply_workspace_hidden_to(window);
        }
    }
}

