function create_setting(id, name, step, track_width, postfix)
    local info = core.get_setting_info(id)
    if postfix == nil then
        postfix = ""
    end
    document.settings_panel:add(gui.template("track_setting", {
        id=id,
        name=gui.str(name, "settings"),
        value=core.get_setting(id),
        min=info.min,
        max=info.max,
        step=step,
        track_width=track_width,
        postfix=postfix
    }))
end

function update_setting(x, id, name, postfix)
    core.set_setting(id, x)
    -- updating label
    document[id..".L"].text = string.format(
        "%s: %s%s", name, core.str_setting(id), postfix
    )
end

function on_open()
    create_setting("chunks.load-distance", "Load Distance", 1, 3)
    create_setting("chunks.load-speed", "Load Speed", 1, 1)
    create_setting("graphics.fog-curve", "Fog Curve", 0.1, 2)
    create_setting("camera.fov", "FOV", 1, 4, "°")
end
