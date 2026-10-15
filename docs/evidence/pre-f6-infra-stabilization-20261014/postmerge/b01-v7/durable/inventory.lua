function main(output)
    import("core.project.config")
    config.load()
    import("core.project.project")
    import("core.base.json")
    local out = {}
    for _, name in ipairs({"sluice_core", "sluice_async"}) do
        local t = project.target(name)
        out[name] = {sources = t:sourcefiles(), objects = t:objectfiles(), target = t:targetfile()}
    end
    io.writefile(output, json.encode(out))
end
