/* 
 * This file is part of the Minecraft Overviewer.
 *
 * Minecraft Overviewer is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * Minecraft Overviewer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with the Overviewer.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "block_class.h"
#include "mc_id.h"
#include "overviewer.h"

static PyObject* textures = NULL;

uint32_t max_blockid = 0;
uint32_t max_data = 0;
uint8_t* block_properties = NULL;

static PyObject* known_blocks = NULL;
static PyObject* transparent_blocks = NULL;
static PyObject* solid_blocks = NULL;
static PyObject* fluid_blocks = NULL;
static PyObject* nospawn_blocks = NULL;
static PyObject* nodata_blocks = NULL;

/* interned chunk and section keys, looked up for every section loaded */
static PyObject* key_sections = NULL;
static PyObject* key_y = NULL;
static PyObject* key_blocks = NULL;
static PyObject* key_data = NULL;
static PyObject* key_skylight = NULL;
static PyObject* key_blocklight = NULL;
static PyObject* key_biomes = NULL;

PyObject* init_chunk_render(void) {

    PyObject* tmp = NULL;
    uint32_t i;

    /* this function only needs to be called once, anything more should be
     * ignored */
    if (textures) {
        Py_RETURN_NONE;
    }
    textures = PyImport_ImportModule("overviewer_core.textures");
    /* ensure none of these pointers are NULL */
    if ((!textures)) {
        return NULL;
    }

    block_class_init();

    key_sections = PyUnicode_InternFromString("Sections");
    key_y = PyUnicode_InternFromString("Y");
    key_blocks = PyUnicode_InternFromString("Blocks");
    key_data = PyUnicode_InternFromString("Data");
    key_skylight = PyUnicode_InternFromString("SkyLight");
    key_blocklight = PyUnicode_InternFromString("BlockLight");
    key_biomes = PyUnicode_InternFromString("Biomes");
    if (!key_sections || !key_y || !key_blocks || !key_data ||
        !key_skylight || !key_blocklight || !key_biomes)
        return NULL;

    tmp = PyObject_GetAttrString(textures, "max_blockid");
    if (!tmp)
        return NULL;
    max_blockid = PyLong_AsLong(tmp);
    Py_DECREF(tmp);

    tmp = PyObject_GetAttrString(textures, "max_data");
    if (!tmp)
        return NULL;
    max_data = PyLong_AsLong(tmp);
    Py_DECREF(tmp);

    /* assemble the property table */
    known_blocks = PyObject_GetAttrString(textures, "known_blocks");
    if (!known_blocks)
        return NULL;
    transparent_blocks = PyObject_GetAttrString(textures, "transparent_blocks");
    if (!transparent_blocks)
        return NULL;
    solid_blocks = PyObject_GetAttrString(textures, "solid_blocks");
    if (!solid_blocks)
        return NULL;
    fluid_blocks = PyObject_GetAttrString(textures, "fluid_blocks");
    if (!fluid_blocks)
        return NULL;
    nospawn_blocks = PyObject_GetAttrString(textures, "nospawn_blocks");
    if (!nospawn_blocks)
        return NULL;
    nodata_blocks = PyObject_GetAttrString(textures, "nodata_blocks");
    if (!nodata_blocks)
        return NULL;

    block_properties = calloc(max_blockid, sizeof(uint8_t));
    for (i = 0; i < max_blockid; i++) {
        PyObject* block = PyLong_FromLong(i);

        if (PySequence_Contains(known_blocks, block))
            block_properties[i] |= 1 << KNOWN;
        if (PySequence_Contains(transparent_blocks, block))
            block_properties[i] |= 1 << TRANSPARENT;
        if (PySequence_Contains(solid_blocks, block))
            block_properties[i] |= 1 << SOLID;
        if (PySequence_Contains(fluid_blocks, block))
            block_properties[i] |= 1 << FLUID;
        if (PySequence_Contains(nospawn_blocks, block))
            block_properties[i] |= 1 << NOSPAWN;
        if (PySequence_Contains(nodata_blocks, block))
            block_properties[i] |= 1 << NODATA;

        Py_DECREF(block);
    }

    Py_RETURN_NONE;
}

/* helper for load_chunk, loads a section into a chunk */
static inline void load_chunk_section(ChunkData* dest, int32_t i, PyObject* section) {
    dest->sections[i].blocks = (PyArrayObject*)PyDict_GetItem(section, key_blocks);
    dest->sections[i].data = (PyArrayObject*)PyDict_GetItem(section, key_data);
    dest->sections[i].skylight = (PyArrayObject*)PyDict_GetItem(section, key_skylight);
    dest->sections[i].blocklight = (PyArrayObject*)PyDict_GetItem(section, key_blocklight);
    dest->sections[i].biomes = (PyArrayObject*)PyDict_GetItem(section, key_biomes);
    Py_INCREF(dest->sections[i].biomes);
    Py_INCREF(dest->sections[i].blocks);
    Py_INCREF(dest->sections[i].data);
    Py_INCREF(dest->sections[i].skylight);
    Py_INCREF(dest->sections[i].blocklight);
}

/* fetches chunk column (x, z) from the regionset into dest's sections, which
 * then hold references to the section arrays. Returns true on error, with a
 * Python exception set (or, oddly, not, if the chunk has no Sections tag).
 */
static bool fetch_chunk(PyObject* regionset, int32_t x, int32_t z, ChunkData* dest) {
    int32_t i;
    PyObject* chunk = NULL;
    PyObject* sections = NULL;

    /* set reasonable defaults */
    for (i = 0; i < SECTIONS_PER_CHUNK; i++) {
        dest->sections[i].blocks = NULL;
        dest->sections[i].data = NULL;
        dest->sections[i].skylight = NULL;
        dest->sections[i].blocklight = NULL;
        dest->sections[i].biomes = NULL;
    }

    chunk = PyObject_CallMethod(regionset, "get_chunk", "ii", x, z);
    if (chunk == NULL) {
        // An exception is already set. RegionSet.get_chunk sets
        // ChunkDoesntExist
        return true;
    }

    sections = PyDict_GetItem(chunk, key_sections);
    if (sections) {
        sections = PySequence_Fast(sections, "Sections tag was not a list!");
    }
    if (sections == NULL) {
        // exception set, again
        Py_DECREF(chunk);
        return true;
    }

    for (i = 0; i < PySequence_Fast_GET_SIZE(sections); i++) {
        PyObject* ycoord = NULL;
        int32_t sectiony = 0;
        PyObject* section = PySequence_Fast_GET_ITEM(sections, i);
        ycoord = PyDict_GetItem(section, key_y);
        if (!ycoord)
            continue;

        sectiony = PyLong_AsLong(ycoord) + 4;
        if (sectiony >= 0 && sectiony < SECTIONS_PER_CHUNK)
            load_chunk_section(dest, sectiony, section);
    }
    Py_DECREF(sections);
    Py_DECREF(chunk);

    return false;
}

/* The chunk columns fetched while rendering one tile. A tile draws a few
 * hundred chunk sections from about a hundred columns, and every section looks
 * at its own column and its neighbours'. Each column is fetched from Python
 * (and its sections unpacked) once per tile instead of once per section. The
 * entries own the references to the section arrays; columns that could not be
 * fetched are remembered as missing.
 */
typedef struct {
    int32_t x, z;
    bool used, missing;
    ChunkData data;
} ChunkCacheEntry;

struct ChunkCache {
    ChunkCacheEntry* entries;
    /* size is a power of two */
    uint32_t size, count;
};

static uint32_t chunk_cache_slot(struct ChunkCache* cache, int32_t x, int32_t z) {
    uint32_t slot = ((uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u) & (cache->size - 1);
    while (cache->entries[slot].used && (cache->entries[slot].x != x || cache->entries[slot].z != z))
        slot = (slot + 1) & (cache->size - 1);
    return slot;
}

static bool chunk_cache_init(struct ChunkCache* cache) {
    cache->size = 256;
    cache->count = 0;
    cache->entries = calloc(cache->size, sizeof(ChunkCacheEntry));
    return cache->entries != NULL;
}

static void chunk_cache_free(struct ChunkCache* cache) {
    uint32_t i, k;
    for (i = 0; i < cache->size; i++) {
        ChunkCacheEntry* entry = &cache->entries[i];
        if (!entry->used || entry->missing)
            continue;
        for (k = 0; k < SECTIONS_PER_CHUNK; k++) {
            Py_XDECREF(entry->data.sections[k].blocks);
            Py_XDECREF(entry->data.sections[k].data);
            Py_XDECREF(entry->data.sections[k].skylight);
            Py_XDECREF(entry->data.sections[k].blocklight);
            Py_XDECREF(entry->data.sections[k].biomes);
        }
    }
    free(cache->entries);
    cache->entries = NULL;
}

/* the entry for column (x, z), adding an empty one (*created) if there isn't
   one yet; NULL if out of memory */
static ChunkCacheEntry* chunk_cache_find(struct ChunkCache* cache, int32_t x, int32_t z, bool* created) {
    uint32_t slot = chunk_cache_slot(cache, x, z);
    ChunkCacheEntry* entry = &cache->entries[slot];

    *created = false;
    if (entry->used)
        return entry;

    if ((cache->count + 1) * 4 > cache->size * 3) {
        /* grow, keeping it at most 3/4 full */
        struct ChunkCache bigger = {calloc(cache->size * 2, sizeof(ChunkCacheEntry)), cache->size * 2, cache->count};
        uint32_t i;
        if (!bigger.entries)
            return NULL;
        for (i = 0; i < cache->size; i++) {
            if (cache->entries[i].used)
                bigger.entries[chunk_cache_slot(&bigger, cache->entries[i].x, cache->entries[i].z)] = cache->entries[i];
        }
        free(cache->entries);
        *cache = bigger;
        entry = &cache->entries[chunk_cache_slot(cache, x, z)];
    }

    entry->used = true;
    entry->missing = false;
    entry->x = x;
    entry->z = z;
    cache->count++;
    *created = true;
    return entry;
}

/* load_chunk() through the tile's chunk cache: dest borrows the cached arrays */
static bool load_cached_chunk(RenderState* state, ChunkData* dest, int32_t x, int32_t z, uint8_t required) {
    bool created;
    ChunkCacheEntry* entry = chunk_cache_find(state->chunk_cache, x, z, &created);

    if (entry == NULL) {
        int32_t i;
        for (i = 0; i < SECTIONS_PER_CHUNK; i++) {
            dest->sections[i].blocks = NULL;
            dest->sections[i].data = NULL;
            dest->sections[i].skylight = NULL;
            dest->sections[i].blocklight = NULL;
            dest->sections[i].biomes = NULL;
        }
        dest->loaded = 1;
        dest->borrowed = 0;
        PyErr_NoMemory();
        if (!required)
            PyErr_Clear();
        return true;
    }

    if (created || (entry->missing && required)) {
        /* fetch it, or for a required column that is missing, fetch it
           again to raise its error as an uncached load would */
        entry->missing = fetch_chunk(state->regionset, x, z, &entry->data);
        if (entry->missing && !required)
            PyErr_Clear();
    }

    *dest = entry->data;
    dest->loaded = 1;
    dest->borrowed = 1;
    return entry->missing;
}

/* loads the given chunk into the chunks[] array in the state
 * returns true on error
 *
 * if required is true, failure to load the chunk will raise a python
 * exception and return true.
 */
bool load_chunk(RenderState* state, int32_t x, int32_t z, uint8_t required) {
    ChunkData* dest = &(state->chunks[1 + x][1 + z]);

    if (dest->loaded)
        return false;

    x += state->chunkx;
    z += state->chunkz;

    if (state->chunk_cache)
        return load_cached_chunk(state, dest, x, z, required);

    dest->loaded = 1;
    dest->borrowed = 0;
    if (fetch_chunk(state->regionset, x, z, dest)) {
        if (!required) {
            PyErr_Clear();
        }
        return true;
    }

    return false;
}

/* helper to unload all loaded chunks */
static void
unload_all_chunks(RenderState* state) {
    uint32_t i, j, k;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            if (state->chunks[i][j].loaded && !state->chunks[i][j].borrowed) {
                for (k = 0; k < SECTIONS_PER_CHUNK; k++) {
                    Py_XDECREF(state->chunks[i][j].sections[k].blocks);
                    Py_XDECREF(state->chunks[i][j].sections[k].data);
                    Py_XDECREF(state->chunks[i][j].sections[k].skylight);
                    Py_XDECREF(state->chunks[i][j].sections[k].blocklight);
                    Py_XDECREF(state->chunks[i][j].sections[k].biomes);
                }
            }
            state->chunks[i][j].loaded = 0;
        }
    }
}

uint16_t
check_adjacent_blocks(RenderState* state, int32_t x, int32_t y, int32_t z, mc_block_t blockid) {
    /*
     * Generates a pseudo ancillary data for blocks that depend of 
     * what are surrounded and don't have ancillary data. This 
     * function is used through generate_pseudo_data.
     *
     * This uses a binary number of 4 digits to encode the info:
     *
     * 0b1234:
     * Bit:   1   2   3   4
     * Side: +x  +z  -x  -z
     * Values: bit = 0 -> The corresponding side block has different blockid
     *         bit = 1 -> The corresponding side block has same blockid
     * Example: if the bit1 is 1 that means that there is a block with 
     * blockid in the side of the +x direction.
     */

    uint8_t pdata = 0;

    if (get_data(state, BLOCKS, x + 1, y, z) == blockid) {
        pdata = pdata | (1 << 3);
    }
    if (get_data(state, BLOCKS, x, y, z + 1) == blockid) {
        pdata = pdata | (1 << 2);
    }
    if (get_data(state, BLOCKS, x - 1, y, z) == blockid) {
        pdata = pdata | (1 << 1);
    }
    if (get_data(state, BLOCKS, x, y, z - 1) == blockid) {
        pdata = pdata | (1 << 0);
    }

    return pdata;
}

uint16_t
generate_pseudo_data(RenderState* state, uint16_t ancilData) {
    /*
     * Generates a fake ancillary data for blocks that are drawn 
     * depending on what are surrounded.
     */
    int32_t x = state->x, y = state->y, z = state->z;
    uint16_t data = 0;

    if (state->block == block_grass) { /* grass */
        /* return 0x10 if grass is covered in snow */
        if (get_data(state, BLOCKS, x, y + 1, z) == 78)
            return 0x10;
        return ancilData;
    } else if (block_class_is_subset(state->block, (mc_block_t[]){block_flowing_water, block_water}, 2)) { /* water */
        data = check_adjacent_blocks(state, x, y, z, state->block) ^ 0x0f;
        /* an aditional bit for top is added to the 4 bits of check_adjacent_blocks */
        if (get_data(state, BLOCKS, x, y + 1, z) != state->block)
            data |= 0x10;
        return data;
    } else if (block_class_is_subset(state->block, (mc_block_t[]){block_glass, block_ice, block_stained_glass}, 3)) { /* glass and ice and stained glass*/
        /* an aditional bit for top is added to the 4 bits of check_adjacent_blocks
         * Note that stained glass encodes 16 colors using 4 bits.  this pushes us over the 8-bits of an uint8_t, 
         * forcing us to use an uint16_t to hold 16 bits of pseudo ancil data
         * */
        if ((get_data(state, BLOCKS, x, y + 1, z) == 20) || (get_data(state, BLOCKS, x, y + 1, z) == 95)) {
            data = 0;
        } else {
            data = 16;
        }
        data = (check_adjacent_blocks(state, x, y, z, state->block) ^ 0x0f) | data;
        return (data << 4) | (ancilData & 0x0f);

    } else if (state->block == block_redstone_wire) { /* redstone */
        /* three addiotional bit are added, one for on/off state, and
         * another two for going-up redstone wire in the same block
         * (connection with the level y+1) */
        uint8_t above_level_data = 0, same_level_data = 0, below_level_data = 0, possibly_connected = 0, final_data = 0;

        /* check for air in y+1, no air = no connection with upper level */
        if (get_data(state, BLOCKS, x, y + 1, z) == 0) {
            above_level_data = check_adjacent_blocks(state, x, y + 1, z, state->block);
        } /* else above_level_data = 0 */

        /* check connection with same level (other redstone and trapped chests */
        same_level_data = check_adjacent_blocks(state, x, y, z, 55) | check_adjacent_blocks(state, x, y, z, 146);

        /* check the posibility of connection with y-1 level, check for air */
        possibly_connected = check_adjacent_blocks(state, x, y, z, 0);

        /* check connection with y-1 level */
        below_level_data = check_adjacent_blocks(state, x, y - 1, z, state->block);

        final_data = above_level_data | same_level_data | (below_level_data & possibly_connected);

        /* add the three bits */
        if (ancilData > 0) { /* powered redstone wire */
            final_data = final_data | 0x40;
        }
        if ((above_level_data & 0x01)) { /* draw top left going up redstonewire */
            final_data = final_data | 0x20;
        }
        if ((above_level_data & 0x08)) { /* draw top right going up redstonewire */
            final_data = final_data | 0x10;
        }
        return final_data;

    } else if (state->block == block_portal) {
        /* portal */
        return check_adjacent_blocks(state, x, y, z, state->block);

    } else if (block_class_has(state->block, BLOCK_CLASS_DOOR)) {
        /* use bottom block data format plus one bit for top/down
         * block (0x8) and one bit for hinge position (0x10)
         */
        uint8_t data = 0;
        if ((ancilData & 0x8) == 0x8) {
            /* top door block */
            uint8_t b_data = get_data(state, DATA, x, y - 1, z);
            if ((ancilData & 0x1) == 0x1) {
                /* hinge on the left */
                data = b_data | 0x8 | 0x10;
            } else {
                data = b_data | 0x8;
            }
        } else {
            /* bottom door block */
            uint8_t t_data = get_data(state, DATA, x, y + 1, z);
            if ((t_data & 0x1) == 0x1) {
                /* hinge on the left */
                data = ancilData | 0x10;
            } else {
                data = ancilData;
            }
        }
        return data;
    } else if (block_class_is_wall(state->block)) {
        /* check for walls and add one bit with the type of wall (mossy or cobblestone)*/
        if (ancilData == 0x1) {
            return check_adjacent_blocks(state, x, y, z, state->block) | 0x10;
        } else {
            return check_adjacent_blocks(state, x, y, z, state->block);
        }
    } else if (state->block == block_waterlily) {
        int32_t wx, wz, wy, rotation;
        int64_t pr;
        /* calculate the global block coordinates of this position */
        wx = (state->chunkx * 16) + x;
        wz = (state->chunkz * 16) + z;
        wy = ((state->chunky - 4) * 16) + y;
        /* lilypads orientation is obtained with these magic numbers */
        /* magic numbers obtained from: */
        /* http://llbit.se/?p=1537 */
        pr = (wx * 3129871) ^ (wz * 116129781) ^ (wy);
        pr = pr * pr * 42317861 + pr * 11;
        rotation = 3 & (pr >> 16);
        return rotation;
    } else if (block_class_has(state->block, BLOCK_CLASS_STAIR)) { /* stairs */
        /* 4 ancillary bits will be added to indicate which quarters of the block contain the 
         * upper step. Regular stairs will have 2 bits set & corner stairs will have 1 or 3.
         *     Southwest quarter is part of the upper step - 0x40
         *    / Southeast " - 0x20
         *    |/ Northeast " - 0x10
         *    ||/ Northwest " - 0x8
         *    |||/ flip upside down (Minecraft)
         *    ||||/ has North/South alignment (Minecraft)
         *    |||||/ ascends North or West, not South or East (Minecraft)
         *    ||||||/
         *  0b0011011 = Stair ascending north, upside up, with both north quarters filled
         */

        /* keep track of whether neighbors are stairs, and their data */
        uint8_t stairs_base[8];
        uint8_t neigh_base[8];
        uint8_t* stairs = stairs_base;
        uint8_t* neigh = neigh_base;

        /* amount to rotate/roll to get to east, west, south, north */
        size_t rotations[] = {0, 2, 3, 1};

        /* masks for the filled (ridge) stair quarters: */
        /* Example: the ridge for an east-ascending stair are the two east quarters */
        /*                  ascending: east  west south north */
        uint8_t ridge_mask[] = {0x30, 0x48, 0x60, 0x18};

        /* masks for the open (trench) stair quarters: */
        uint8_t trench_mask[] = {0x48, 0x30, 0x18, 0x60};

        /* boat analogy! up the stairs is toward the bow of the boat */
        /* masks for port and starboard, i.e. left and right sides while ascending: */
        uint8_t port_mask[] = {0x18, 0x60, 0x30, 0x48};
        uint8_t starboard_mask[] = {0x60, 0x18, 0x48, 0x30};

        /* we may need to lock some quarters into place depending on neighbors */
        uint8_t lock_mask = 0;

        uint8_t repair_rot[] = {0, 1, 2, 3, 2, 3, 1, 0, 1, 0, 3, 2, 3, 2, 0, 1};

        /* need to get northdirection of the render */
        /* TODO: get this just once? store in state? */
        PyObject* texrot;
        int32_t northdir;
        texrot = PyObject_GetAttrString(state->textures, "rotation");
        northdir = PyLong_AsLong(texrot);

/* fix the rotation value for different northdirections */
#define FIX_ROT(x) (((x) & ~0x3) | repair_rot[((x)&0x3) | (northdir << 2)])
        ancilData = FIX_ROT(ancilData);

        /* fill the ancillary bits assuming normal stairs with no corner yet */
        ancilData |= ridge_mask[ancilData & 0x3];

        /* get block & data for neighbors in this order: east, north, west, south */
        /* so we can rotate things easily */
        stairs[0] = stairs[4] = block_class_has(get_data(state, BLOCKS, x + 1, y, z), BLOCK_CLASS_STAIR);
        stairs[1] = stairs[5] = block_class_has(get_data(state, BLOCKS, x, y, z - 1), BLOCK_CLASS_STAIR);
        stairs[2] = stairs[6] = block_class_has(get_data(state, BLOCKS, x - 1, y, z), BLOCK_CLASS_STAIR);
        stairs[3] = stairs[7] = block_class_has(get_data(state, BLOCKS, x, y, z + 1), BLOCK_CLASS_STAIR);
        neigh[0] = neigh[4] = FIX_ROT(get_data(state, DATA, x + 1, y, z));
        neigh[1] = neigh[5] = FIX_ROT(get_data(state, DATA, x, y, z - 1));
        neigh[2] = neigh[6] = FIX_ROT(get_data(state, DATA, x - 1, y, z));
        neigh[3] = neigh[7] = FIX_ROT(get_data(state, DATA, x, y, z + 1));

#undef FIX_ROT

        /* Rotate the neighbors so we only have to worry about one orientation
         * No matter which way the boat is facing, the the neighbors will be:
         *   0: bow
         *   1: port
         *   2: stern
         *   3: starboard */
        stairs += rotations[ancilData & 0x3];
        neigh += rotations[ancilData & 0x3];

        /* Matching neighbor stairs to the sides should prevent cornering on that side */
        /* If found, set bits in lock_mask to lock the current quarters as they are */
        if (stairs[1] && (neigh[1] & 0x7) == (ancilData & 0x7)) {
            /* Neighbor on port side is stairs of the same orientation as me */
            /* Do NOT allow changing quarters on the port side */
            lock_mask |= port_mask[ancilData & 0x3];
        }
        if (stairs[3] && (neigh[3] & 0x7) == (ancilData & 0x7)) {
            /* Neighbor on starboard side is stairs of the same orientation as me */
            /* Do NOT allow changing quarters on the starboard side */
            lock_mask |= starboard_mask[ancilData & 0x3];
        }

        /* Make corner stairs -- prefer outside corners like Minecraft */
        if (stairs[0] && (neigh[0] & 0x4) == (ancilData & 0x4)) {
            /* neighbor at bow is stairs with same flip */
            if ((neigh[0] & 0x2) != (ancilData & 0x2)) {
                /* neighbor is perpendicular, cut a trench, but not where locked */
                ancilData &= ~trench_mask[neigh[0] & 0x3] | lock_mask;
            }
        } else if (stairs[2] && (neigh[2] & 0x4) == (ancilData & 0x4)) {
            /* neighbor at stern is stairs with same flip */
            if ((neigh[2] & 0x2) != (ancilData & 0x2)) {
                /* neighbor is perpendicular, add a ridge, but not where locked */
                ancilData |= ridge_mask[neigh[2] & 0x3] & ~lock_mask;
            }
        }

        return ancilData;
    } else if (state->block == block_double_plant) { /* doublePlants */
        /* use bottom block data format plus one bit for top
         * block (0x8)
         */
        if (get_data(state, BLOCKS, x, y - 1, z) == block_double_plant) {
            data = get_data(state, DATA, x, y - 1, z) | 0x8;
        } else {
            data = ancilData;
        }

        return data;
    }

    return 0;
}

/* reads an image's size; returns -1 with an exception set on error */
static int get_image_size(PyObject* img, int32_t* width, int32_t* height) {
    PyObject* size = PyObject_GetAttrString(img, "size");
    if (size == NULL)
        return -1;
    if (!PyArg_ParseTuple(size, "ii", width, height)) {
        Py_DECREF(size);
        return -1;
    }
    Py_DECREF(size);
    return 0;
}

/* the textures' blockmap (a new reference), or NULL with an exception set */
static PyObject* get_blockmap(PyObject* textures) {
    PyObject* blockmap = PyObject_GetAttrString(textures, "blockmap");
    if (blockmap == Py_None) {
        Py_DECREF(blockmap);
        PyErr_SetString(PyExc_RuntimeError, "you must call Textures.generate()");
        return NULL;
    }
    return blockmap;
}

/* Renders the chunk section at state->chunkx, chunky, chunkz onto state->img,
 * offset by (xoff, yoff). Returns -1 with a Python exception set on error.
 * A missing chunk raises ChunkDoesntExist, a missing section draws nothing.
 */
static int
render_section(RenderState* state, PyObject* modeobj, PyObject* blockmap,
               int32_t imgsize0, int32_t imgsize1, int32_t xoff, int32_t yoff) {
    PyArrayObject* blocks_py;
    RenderMode* rendermode;
    int32_t i, j;
    PyObject* t = NULL;

    /* set all block data to unloaded */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            state->chunks[i][j].loaded = 0;
        }
    }

    /* set up the render mode */
    state->rendermode = rendermode = render_mode_create(modeobj, state);
    if (rendermode == NULL) {
        return -1; // note that render_mode_create will
                   // set PyErr.  No need to set it here
    }

    /* get the block data for the center column, erroring out if needed */
    if (load_chunk(state, 0, 0, 1)) {
        render_mode_destroy(rendermode);
        unload_all_chunks(state);
        return -1;
    }
    if (state->chunks[1][1].sections[state->chunky].blocks == NULL) {
        /* this section doesn't exist, let's skeddadle */
        render_mode_destroy(rendermode);
        unload_all_chunks(state);
        return 0;
    }

    /* set blocks_py, state->blocks, and state->blockdatas as convenience */
    blocks_py = state->blocks = state->chunks[1][1].sections[state->chunky].blocks;
    state->blockdatas = state->chunks[1][1].sections[state->chunky].data;

    /* set up the random number generator again for each chunk
       so tallgrass is in the same place, no matter what mode is used */
    srand(1);

    for (state->x = 15; state->x > -1; state->x--) {
        for (state->z = 0; state->z < 16; state->z++) {

            /* set up the render coordinates */
            state->imgx = xoff + state->x * 12 + state->z * 12;
            /* 16*12 -- offset for y direction, 15*6 -- offset for x */
            state->imgy = yoff - state->x * 6 + state->z * 6 + 16 * 12 + 15 * 6;

            for (state->y = 0; state->y < 16; state->y++) {
                uint16_t ancilData;

                state->imgy -= 12;
                /* get blockid */
                state->block = getArrayShort3D(blocks_py, state->x, state->y, state->z);
                if (state->block == block_air || render_mode_hidden(rendermode, state->x, state->y, state->z)) {
                    continue;
                }

                /* make sure we're rendering inside the image boundaries */
                if ((state->imgx >= imgsize0 + 24) || (state->imgx <= -24)) {
                    continue;
                }
                if ((state->imgy >= imgsize1 + 24) || (state->imgy <= -24)) {
                    continue;
                }

                /* check for occlusion */
                if (render_mode_occluded(rendermode, state->x, state->y, state->z)) {
                    continue;
                }

                /* everything stored here will be a borrowed ref */

                if (block_has_property(state->block, NODATA)) {
                    /* block shouldn't have data associated with it, set it to 0 */
                    ancilData = 0;
                    state->block_data = 0;
                    state->block_pdata = 0;
                } else {
                    /* block has associated data, use it */
                    ancilData = getArrayByte3D(state->blockdatas, state->x, state->y, state->z);
                    state->block_data = ancilData;
                    /* block that need pseudo ancildata:
                     * grass, water, glass, chest, restone wire,
                     * ice, portal, iron bars,
                     * trapped chests, stairs */
                    if (block_class_has(state->block, BLOCK_CLASS_ANCIL)) {
                        ancilData = generate_pseudo_data(state, ancilData);
                        state->block_pdata = ancilData;
                    } else {
                        state->block_pdata = 0;
                    }
                }

                /* make sure our block info is in-bounds */
                if (state->block >= max_blockid || ancilData >= max_data)
                    continue;

                /* get the texture */
                t = PyList_GET_ITEM(blockmap, max_data * state->block + ancilData);
                /* if we don't get a texture, try it again with 0 data */
                if ((t == NULL || t == Py_None) && ancilData != 0)
                    t = PyList_GET_ITEM(blockmap, max_data * state->block);

                /* if we found a proper texture, render it! */
                if (t != NULL && t != Py_None) {
                    PyObject *src, *mask, *mask_light;
                    int32_t do_rand = (state->block == block_tallgrass /*|| state->block == block_red_flower || state->block == block_double_plant*/);
                    int32_t randx = 0, randy = 0;
                    src = PyTuple_GetItem(t, 0);
                    mask = PyTuple_GetItem(t, 0);
                    mask_light = PyTuple_GetItem(t, 1);

                    if (mask == Py_None)
                        mask = src;

                    if (do_rand) {
                        /* add a random offset to the postion of the tall grass to make it more wild */
                        randx = rand() % 6 + 1 - 3;
                        randy = rand() % 6 + 1 - 3;
                        state->imgx += randx;
                        state->imgy += randy;
                    }

                    render_mode_draw(rendermode, src, mask, mask_light);

                    if (do_rand) {
                        /* undo the random offsets */
                        state->imgx -= randx;
                        state->imgy -= randy;
                    }
                }
            }
        }
    }

    /* free up the rendermode info */
    render_mode_destroy(rendermode);
    unload_all_chunks(state);
    return 0;
}

/* TODO triple check this to make sure reference counting is correct */
PyObject*
chunk_render(PyObject* self, PyObject* args) {
    RenderState state;
    PyObject* modeobj;
    PyObject* blockmap;
    int32_t xoff, yoff;
    int32_t imgsize0, imgsize1;
    int result;

    if (!PyArg_ParseTuple(args, "OOiiiOiiOO", &state.world, &state.regionset, &state.chunkx, &state.chunky, &state.chunkz, &state.img, &xoff, &yoff, &modeobj, &state.textures))
        return NULL;
    state.chunk_cache = NULL;

    /* get the blockmap from the textures object */
    blockmap = get_blockmap(state.textures);
    if (blockmap == NULL)
        return NULL;

    /* get the image size */
    if (get_image_size(state.img, &imgsize0, &imgsize1) < 0) {
        Py_DECREF(blockmap);
        return NULL;
    }

    result = render_section(&state, modeobj, blockmap, imgsize0, imgsize1, xoff, yoff);
    Py_DECREF(blockmap);
    if (result < 0)
        return NULL;
    Py_RETURN_NONE;
}

/* render_tile(world, regionset, sections, img, rendermode, textures,
 *             ChunkDoesntExist, CorruptionError)
 *
 * Renders each (chunkx, chunky, chunkz, xoff, yoff) chunk section in the
 * sections list, in order, as render_loop() would, but fetching each chunk
 * column once for the whole list. Sections of missing chunks are skipped, as
 * are those of corrupt chunks, whose (chunkx, chunkz) are returned in a list.
 */
PyObject*
tile_render(PyObject* self, PyObject* args) {
    RenderState state;
    struct ChunkCache cache;
    PyObject *modeobj, *sections, *missing_error, *corrupt_error;
    PyObject* blockmap = NULL;
    PyObject* corrupt = NULL;
    int32_t imgsize0, imgsize1;
    Py_ssize_t i;

    if (!PyArg_ParseTuple(args, "OOO!OOOOO", &state.world, &state.regionset, &PyList_Type, &sections,
                          &state.img, &modeobj, &state.textures, &missing_error, &corrupt_error))
        return NULL;

    if (!chunk_cache_init(&cache))
        return PyErr_NoMemory();
    state.chunk_cache = &cache;

    blockmap = get_blockmap(state.textures);
    if (blockmap == NULL || get_image_size(state.img, &imgsize0, &imgsize1) < 0)
        goto error;
    corrupt = PyList_New(0);
    if (corrupt == NULL)
        goto error;

    for (i = 0; i < PyList_GET_SIZE(sections); i++) {
        int32_t xoff, yoff;

        if (!PyArg_ParseTuple(PyList_GET_ITEM(sections, i), "iiiii", &state.chunkx, &state.chunky,
                              &state.chunkz, &xoff, &yoff))
            goto error;

        if (render_section(&state, modeobj, blockmap, imgsize0, imgsize1, xoff, yoff) < 0) {
            if (PyErr_ExceptionMatches(corrupt_error)) {
                PyObject* where;
                PyErr_Clear();
                where = Py_BuildValue("(ii)", state.chunkx, state.chunkz);
                if (where == NULL || PyList_Append(corrupt, where) < 0) {
                    Py_XDECREF(where);
                    goto error;
                }
                Py_DECREF(where);
            } else if (PyErr_ExceptionMatches(missing_error)) {
                /* Some chunks are present on disk but not fully initialized */
                PyErr_Clear();
            } else {
                goto error;
            }
        }
    }

    chunk_cache_free(&cache);
    Py_DECREF(blockmap);
    return corrupt;

error:
    chunk_cache_free(&cache);
    Py_XDECREF(blockmap);
    Py_XDECREF(corrupt);
    return NULL;
}
