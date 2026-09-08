--[[
  Ibis Archive — eBird checklist export for darktable.

  Adds an export storage, "eBird checklist (Ibis Archive)". Select the frames
  from one outing, export with this storage, and you get:

    <folder>/<date>_<place>/checklist.csv         19 columns, header row, for you to read
    <folder>/<date>_<place>/checklist_upload.csv  the same rows, no header, for eBird's importer
    <folder>/<date>_<place>/<species>/            the exported JPEGs, one folder per species

  Everything the checklist needs is darktable's own data:
    species   tags  Birds|Species|<name>  (Kestrel's sidecars) or species|<name>
    where     the image's latitude/longitude (map view, GPX correlation)
    when      exif_datetime_taken; duration = first frame to last
    count     tag  count|<species>|<n>   optional; "X" (present, uncounted) otherwise
    place     tag  place|<name>          optional; else the folder's name
    region    tags country|NO  region|NO-11   optional
    protocol  tag  protocol|<Stationary|Traveling|Incidental|Historical|Area>; default Incidental

  The CSV rules are eBird's Record Format, unchanged from ibis_checklist.py:
  no quoting (the importer cannot read it; commas, quotes and newlines are
  stripped), MM/DD/YYYY, HH:MM, "casual" on the wire for Incidental, and the
  upload copy has no header because eBird treats a header as an observation.
]]

local dt = require "darktable"

local HEADER = {
  "Common Name", "Genus", "Species", "Number", "Species Comments",
  "Location Name", "Latitude", "Longitude", "Date", "Start Time",
  "State/Province", "Country Code", "Protocol", "Number of Observers",
  "Duration", "All observations reported?", "Effort Distance Miles",
  "Effort area acres", "Submission Comments",
}

local PROTOCOL_WIRE = {
  Stationary = "stationary", Traveling = "traveling", Incidental = "casual",
  Historical = "historical", Area = "area",
}

-- eBird's importer has no quoting. A comma shifts every later column, so the
-- character is removed and the gap closed.
local function clean(v)
  if v == nil then return "" end
  local s = tostring(v):gsub('[,"\r\n]', " "):gsub("%s%s+", " ")
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- "2023:01:11 16:31:26" (exif) -> "01/11/2023", "16:31", os.time
local function parse_exif(dtstr)
  local y, m, d, H, M, S = tostring(dtstr or ""):match("(%d%d%d%d)[:%-](%d%d)[:%-](%d%d)[ T](%d%d):(%d%d):?(%d?%d?)")
  if not y then return nil end
  return {
    date = string.format("%s/%s/%s", m, d, y),
    time = string.format("%s:%s", H, M),
    epoch = os.time({ year = tonumber(y), month = tonumber(m), day = tonumber(d),
                      hour = tonumber(H), min = tonumber(M), sec = tonumber(S) or 0 }),
    day = y .. "-" .. m .. "-" .. d,
  }
end

local function tag_names(image)
  local out = {}
  for _, t in ipairs(image:get_tags()) do out[#out + 1] = t.name end
  return out
end

-- The first tag under any of the given roots: "Birds|Species|Common Merganser" -> "Common Merganser".
local function tag_value(names, roots)
  for _, n in ipairs(names) do
    for _, root in ipairs(roots) do
      local v = n:match("^" .. root:gsub("|", "%%|") .. "|(.+)$")
      if v then return v end
    end
  end
  return nil
end

local function species_of(image)
  local names = tag_names(image)
  for _, n in ipairs(names) do
    for _, root in ipairs({ "Birds|Species", "species", "Species" }) do
      local v = n:match("^" .. root:gsub("|", "%%|") .. "|(.+)$")
      if v then
        -- Nested deeper (Birds|Species|Anatidae|Common Merganser): the last segment is the name.
        local leaf = v:match("([^|]+)$")
        -- Unidentified is the identify module's "not sure" marker, not a species
        if leaf and leaf ~= "Unidentified" then return leaf end
      end
    end
  end
  return nil
end

local function safe_dirname(name)
  local s = name:gsub('[<>:"/\\|%?%*%c]', "-"):gsub("[%s%.]+$", "")
  return s ~= "" and s or "Unidentified"
end

local function mkdir(path)
  if dt.configuration.running_os == "windows" then
    dt.control.execute('mkdir "' .. path:gsub("/", "\\") .. '" 2>NUL')
  else
    dt.control.execute('mkdir -p "' .. path .. '"')
  end
end

local function copy(src, dst)
  if dt.configuration.running_os == "windows" then
    dt.control.execute('copy /Y "' .. src:gsub("/", "\\") .. '" "' .. dst:gsub("/", "\\") .. '" >NUL')
  else
    dt.control.execute('cp -f "' .. src .. '" "' .. dst .. '"')
  end
end

-- Where the checklist and JPEGs go. darktable hands a Lua storage temporary
-- files, so the storage needs its own folder setting; it is remembered.
local PREF_DIR = "output_dir"
local default_dir = dt.preferences.read("ibis_ebird", PREF_DIR, "string")
if default_dir == nil or default_dir == "" then
  default_dir = (os.getenv("USERPROFILE") or os.getenv("HOME") or "."):gsub("\\", "/") .. "/Pictures/Ibis Archive/eBird"
end
mkdir(default_dir)
local folder_chooser = dt.new_widget("file_chooser_button"){
  title = "eBird output folder",
  is_directory = true,
  value = default_dir,
  changed_callback = function(w) dt.preferences.write("ibis_ebird", PREF_DIR, "string", w.value) end,
}
local storage_widget = dt.new_widget("box"){
  orientation = "vertical",
  dt.new_widget("label"){ label = "checklist and JPEGs go to a dated folder under:", halign = "start" },
  folder_chooser,
}

-- One export run = one outing. Rows accumulate here; finalize writes them.
local run = { images = {}, files = {} }

local function store(storage, image, format, filename, number, total, high_quality, extra)
  run.images[#run.images + 1] = image
  run.files[image] = filename
end

local function finalize(storage, image_table, extra)
  if #run.images == 0 then return end
  local first_img = run.images[1]

  -- Where the output goes: <chosen folder>/<date>_<place>, made below once
  -- the date and place are known. darktable wrote the JPEGs to a temporary
  -- location; the per-species copy is what keeps them.
  local base_dir = ((folder_chooser.value ~= nil and folder_chooser.value ~= "") and folder_chooser.value or default_dir):gsub("[/\\]+$", "")

  -- Frames, sorted by time; the outing's when.
  local frames = {}
  for _, img in ipairs(run.images) do
    frames[#frames + 1] = { img = img, when = parse_exif(img.exif_datetime_taken) }
  end
  table.sort(frames, function(a, b)
    return (a.when and a.when.epoch or 0) < (b.when and b.when.epoch or 0)
  end)
  local first_when = frames[1].when
  local last_when = frames[#frames].when
  local date = first_when and first_when.date or ""
  local start = first_when and first_when.time or ""
  local duration = ""
  if first_when and last_when and last_when.epoch > first_when.epoch then
    duration = tostring(math.max(1, math.ceil((last_when.epoch - first_when.epoch) / 60)))
  end

  -- Where: the median of the frames that have a position.
  local lats, lons = {}, {}
  for _, f in ipairs(frames) do
    if f.img.latitude and f.img.longitude then
      lats[#lats + 1] = f.img.latitude; lons[#lons + 1] = f.img.longitude
    end
  end
  table.sort(lats); table.sort(lons)
  local lat = #lats > 0 and lats[math.ceil(#lats / 2)] or nil
  local lon = #lons > 0 and lons[math.ceil(#lons / 2)] or nil

  -- Outing-level tags, from any frame: place, region, country, protocol.
  local all_names = {}
  for _, f in ipairs(frames) do
    for _, n in ipairs(tag_names(f.img)) do all_names[#all_names + 1] = n end
  end
  local place = tag_value(all_names, { "place" })
  if not place then
    -- the folder's name, skipping generic ones (selects, crops, DCIM, 100MSDCF...)
    local path = first_img.path:gsub("[/\\]+$", "")
    while path ~= "" do
      local leaf = path:match("([^/\\]+)$") or ""
      local l = leaf:lower()
      local generic = l == "" or l == "selects" or l == "crops" or l == "rejects" or l == "jpg"
        or l == "raw" or l == "export" or l == "dcim" or l:match("^%d%d%d%w+$") or l:match("^%d%d%d%d%-%d%d%-%d%d$")
      if not generic then place = leaf; break end
      path = path:match("^(.*)[/\\][^/\\]+$") or ""
    end
    place = place or "outing"
  end
  local region = tag_value(all_names, { "region" }) or ""
  local country = tag_value(all_names, { "country" }) or ""
  local protocol = tag_value(all_names, { "protocol" }) or "Incidental"
  local wire = PROTOCOL_WIRE[protocol] or "casual"
  if protocol == "Incidental" then duration = "" end

  -- Species, one row each, counts from count|<species>|<n> when present.
  local species, order = {}, {}
  for _, f in ipairs(frames) do
    local sp = species_of(f.img)
    if sp and not species[sp] then species[sp] = { count = "X", files = {} }; order[#order + 1] = sp end
    if sp then
      local files = species[sp].files
      files[#files + 1] = run.files[f.img]
      for _, n in ipairs(tag_names(f.img)) do
        local s, c = n:match("^count|([^|]+)|(%d+)$")
        if s == sp then species[sp].count = c end
      end
    end
  end

  local problems = {}
  if not (lat and lon) then problems[#problems + 1] = "no position on any frame (set it in the map view or via GPX)" end
  if date == "" then problems[#problems + 1] = "no capture time on the frames" end
  if #order == 0 then problems[#problems + 1] = "no species tags (Birds|Species|<name>) on the frames" end

  local when_dir = first_when and first_when.day or "undated"
  local out_dir = base_dir .. "/" .. when_dir .. "_" .. safe_dirname(place)
  mkdir(base_dir)
  mkdir(out_dir)

  local rows = {}
  for _, sp in ipairs(order) do
    rows[#rows + 1] = {
      clean(sp), "", "", clean(species[sp].count), "",
      clean(place), lat and string.format("%.6f", lat) or "", lon and string.format("%.6f", lon) or "",
      date, start, clean(region), clean(country), wire, "1", duration, "N", "", "", "",
    }
  end

  local function write_csv(path, with_header)
    local fh = assert(io.open(path, "w"))
    if with_header then fh:write(table.concat(HEADER, ",") .. "\r\n") end
    for _, r in ipairs(rows) do fh:write(table.concat(r, ",") .. "\r\n") end
    fh:close()
  end
  write_csv(out_dir .. "/checklist.csv", true)
  write_csv(out_dir .. "/checklist_upload.csv", false)

  -- One folder per species, the JPEGs darktable just exported, for eBird's
  -- "Add media" screen. Every frame is kept; eBird takes ten per species,
  -- so the first ten are what to upload.
  for _, sp in ipairs(order) do
    local dir = out_dir .. "/" .. safe_dirname(sp)
    mkdir(dir)
    for _, f in ipairs(species[sp].files) do
      copy(f, dir .. "/" .. (f:match("([^/\\]+)$")))
    end
  end
  -- frames without a species tag still belong to the outing
  local loose = {}
  for _, f in ipairs(frames) do
    if not species_of(f.img) then loose[#loose + 1] = run.files[f.img] end
  end
  if #loose > 0 then
    mkdir(out_dir .. "/Unidentified")
    for _, f in ipairs(loose) do copy(f, out_dir .. "/Unidentified/" .. (f:match("([^/\\]+)$"))) end
  end

  local msg = string.format("eBird checklist: %d species, %d frames -> %s", #order, #frames, out_dir)
  if #problems > 0 then msg = msg .. "  NEEDS A LOOK: " .. table.concat(problems, "; ") end
  dt.print(msg)
  dt.print_log("[ibis] " .. msg)

  run = { images = {}, files = {} }
end

local function supported(storage, format)
  -- JPEG is what eBird takes.
  return format.extension == "jpg" or format.extension == "jpeg"
end

dt.register_storage("ibis_ebird", "eBird checklist (Ibis Archive)", store, finalize, supported, nil, storage_widget)

dt.print_log("[ibis] eBird checklist storage registered")
