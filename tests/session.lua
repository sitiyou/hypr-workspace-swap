for _, output in ipairs({ "WSS-1", "WSS-2" }) do
    hl.monitor({ output = output, mode = "1280x720@60", position = "auto-right", scale = 1 })
end
hl.monitor({ output = "", disabled = true })
hl.config({
    general = { layout = "master", gaps_in = 4, gaps_out = 8, border_size = 2 },
    master = { mfact = 0.60, new_status = "slave", orientation = "left" },
    dwindle = { preserve_split = true },
    animations = { enabled = false },
    input = { follow_mouse = 0 },
    misc = {
        disable_hyprland_logo = true,
        force_default_wallpaper = 0,
        enable_anr_dialog = false,
    },
    ecosystem = { no_update_news = true, no_donation_nag = true },
    debug = { disable_logs = false },
})
