worlds['test'] = "test/data/settings/test_world"

# a default for every render
pngcompression = 3

renders["fast"] = {
    "title": "fast",
    "world": "test",
    "rendermode": normal,
    "dimension": "overworld",
}

renders["small"] = {
    "title": "small",
    "world": "test",
    "rendermode": normal,
    "dimension": "overworld",
    "pngcompression": 9,
}

outputdir = "/tmp/fictional/outputdir"
