--[[
  Ibis Archive: the workspace.

  darktable shows every module to every photographer. A birder's session is
  import, cull, identify, review, place, export, so the panels are set up
  to read that way: the modules of that flow visible and in reach, the rest
  hidden (one click away in the module visibility menu, nothing removed).

  Applied once per configuration (a preference remembers the version), so a
  user who turns a module back on keeps it. Bump WORKSPACE_VERSION when the
  layout below changes and it applies again on the next start.

  Module visibility is stored per view by darktable, so each view is set
  when it is first entered.
]]

local dt = require "darktable"

local WORKSPACE_VERSION = 1
local PREF = "workspace_version"

local LAYOUT = {
  lighttable = {
    -- right panel, top to bottom by darktable's positions: identify birds (900),
    -- selection (800), metadata editor (510), tagging (500), geotagging (450), export (0)
    visible = {
      ibis_identify = true, select = true, metadata = true, tagging = true,
      geotagging = true, export = true,
      -- left: import, collections, filters, image information stay
      import = true, collect = true, filtering = true, metadata_view = true,
      -- hidden: not part of an outing
      styles = false, copy_history = false, image = false,
      neural_restore = false, recentcollect = false,
    },
    expanded = { ibis_identify = true, tagging = false, geotagging = false, export = false, metadata = false },
  },
  darkroom = {
    visible = { neural_restore = false },
    expanded = {},
  },
  map = {
    visible = { map_locations = false },
    expanded = {},
  },
}

local applied = {}

local function apply(view_id)
  local layout = LAYOUT[view_id]
  if not layout or applied[view_id] then return end
  applied[view_id] = true
  for name, vis in pairs(layout.visible) do
    local lib = dt.gui.libs[name]
    if lib then
      local ok, err = pcall(function() lib.visible = vis end)
      if not ok then dt.print_log("[ibis] workspace: " .. name .. ".visible: " .. tostring(err)) end
    end
  end
  for name, exp in pairs(layout.expanded) do
    local lib = dt.gui.libs[name]
    if lib and lib.expandable then
      local ok, err = pcall(function() lib.expanded = exp end)
      if not ok then dt.print_log("[ibis] workspace: " .. name .. ".expanded: " .. tostring(err)) end
    end
  end
  dt.print_log("[ibis] workspace v" .. WORKSPACE_VERSION .. " applied to " .. view_id)
end

local current = dt.preferences.read("ibis_workspace", PREF, "integer")
if current == nil or current < WORKSPACE_VERSION then
  dt.register_event("ibis_workspace", "view-changed", function(event, old_view, new_view)
    if not new_view then return end
    apply(new_view.id)
    -- all three views seen: remember the version so user changes persist from here on
    if applied.lighttable and applied.darkroom and applied.map then
      dt.preferences.write("ibis_workspace", PREF, "integer", WORKSPACE_VERSION)
    end
  end)
  -- the lighttable is already up when luarc runs on a normal start
  if dt.gui.current_view() and dt.gui.current_view().id == "lighttable" then
    apply("lighttable")
  end
end
