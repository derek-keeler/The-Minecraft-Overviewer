# An example multi-render config for contrib/profile_render.py --config: five
# overworld renders (two lighting modes and three overlays), the nether and
# the end, all of one world. profile_render.py points the world, the output
# directory and the textures at its own inputs, so the paths here are
# placeholders.
# flake8: noqa: F821
# pylint: disable=undefined-variable

worlds["world"] = "/path/to/world"
outputdir = "/path/to/output"

renders["day"] = {
    "title": "Day",
    "world": "world",
    "dimension": "overworld",
    "rendermode": "smooth_lighting",
}

renders["night"] = {
    "title": "Night",
    "world": "world",
    "dimension": "overworld",
    "rendermode": "smooth_night",
}

renders["nether"] = {
    "title": "Nether",
    "world": "world",
    "dimension": "nether",
    "rendermode": "nether_smooth_lighting",
}

renders["end"] = {
    "title": "End",
    "world": "world",
    "dimension": "end",
    "rendermode": [Base(), EdgeLines(), SmoothLighting(strength=0.5)],
}

renders["overlay_biome"] = {
    "title": "Biome Coloring Overlay",
    "world": "world",
    "dimension": "overworld",
    "overlay": ["day"],
    "rendermode": [ClearBase(), BiomeOverlay()],
}

renders["overlay_mobs"] = {
    "title": "Mob Spawnable Areas Overlay",
    "world": "world",
    "dimension": "overworld",
    "overlay": ["day"],
    "rendermode": [ClearBase(), SpawnOverlay()],
}

renders["overlay_slime"] = {
    "title": "Slime Chunk Overlay",
    "world": "world",
    "dimension": "overworld",
    "overlay": ["day"],
    "rendermode": [ClearBase(), SlimeOverlay()],
}
