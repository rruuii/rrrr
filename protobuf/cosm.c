#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/stat.h>
#include "/home/abyrd/git/rrrr/protobuf/pbf.h"

// Test this out on a small dataset.
// To test, can we just run this in memory and let swapping happen?
// Map long indexes to files of a fixed size (1 gigastruct), then index within file
// 14 bits -> 1.7km at 45 degrees
// 13 bits -> 3.4km at 45 degrees
// at 45 degrees cos(pi/4)~=0.7
// TODO maybe shift one more bit off of y to make more square
#define GRID_BITS 14
#define GRID_DIM (1 << GRID_BITS) /* The size of the grid root is 2^bits. */
#define MAX_NODE_ID 4000000000
#define MAX_WAY_ID 400000000
// ^ There are over 10 times as many nodes as ways in OSM
#define WAY_BLOCK_SIZE 32 /* Think of typical number of ways per grid cell... */
#define TAG_HIGHWAY_PRIMARY   1
#define TAG_HIGHWAY_SECONDARY 2
#define TAG_HIGHWAY_TERTIARY  3

static const char *database_path = "/mnt/ssd2/cosm_db/";

typedef struct {
    int32_t refs[WAY_BLOCK_SIZE]; 
    int32_t next; // index of next way block, or number of free slots if negative
} WayBlock; // actually way /references/

/* 
  A node's int32 coordinates, from which we can easily determine its grid bin by bitshifting. 
  An array of 2^64 these serves as a map from node ids to nodes. Node IDs are assigned
  sequentially, so you really need less than 2^32 entries as of 2014.
  Note that when nodes are deleted their IDs are not reused, so there are holes in
  this range, but the filesystem should take care of that.
*/
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t tags; // bit flags for common tags we care about. highway = ?
} Node;

typedef struct {
    uint32_t node_ref_offset;
    uint32_t tags; // bit flags for common tags we care about. highway = ?
} Way;

/* 
  The leaves of the grid tree. 
  These contain linked lists of nodes and ways. 
  Assuming each cell contains at least one node and one way, we could avoid a level 
  of indirection by storing the head nodes of the lists directly here rather then indexes 
  to dynamically allocated ones, with additional blocks being dynamically allocated.
  However this makes for a really big file, and some grid cells will in fact be empty,
  so we can use UINT32_MAX to mean "none". It is already 11GB with only the int indexes here.
*/

/* 
  The intermediate level of the grid tree. 
  These nodes consume the most significant remaining 8 bits on each axis.
  There can be at most (2^16 * 2^16) == 2^32 leaf nodes, so we could use 32 bit integers to 
  index them. However, unlike the root node, there is probably some object in every subgrid. 
  If that is the case then there is no need for any indirection, we can just store the bin 
  structs directly at this level.
*/

/* 
  The spatial index grid.
  It is likely to be only 1/3 full due to ocean and wilderness. 
  TODO eliminate coastlines etc.
*/
typedef struct {
    int32_t blocks[GRID_DIM][GRID_DIM]; // contains indexes to way_blocks
} Grid;

// cannot mmap to the same location, so don't use pointers. 
// instead use object indexes (which could be 32-bit!) and one mmapped file per object type.
// sort nodes so we can do a binary search?
// " Deleted node ids must not be reused, unless a former node is now undeleted. "

/* Print human readable size. TODO write this into an internal buffer. */
void human (double s) {
    if (s < 1024) {
        printf ("%.1lfbytes", s);
        return;
    }
    s /= 1024;
    if (s < 1024) {
        printf ("%.1lfkB", s);
        return;
    }
    s /= 1024;
    if (s < 1024) {
        printf ("%.1lfMB", s);
        return;
    }
    s /= 1024;
    if (s < 1024) {
        printf ("%.1lfGB", s);
        return;
    }
    s /= 1024;
    printf ("%.1lfTB", s);
}

void die (char *s) {
    printf("%s\n", s);
    exit(EXIT_FAILURE);
}

/*
  mmap will map a zero-length file, in which case it will cause a bus error when it tries to write. 
  you could "reserve" the address space for the maximum size of the file by 
  providing that size when the file is first mapped in with mmap().
  Linux provides the mremap() system call for expanding or shrinking the size of a given mapping. 
  msync() flushes.
  Interestingly the ext4 filesystem seems to understand "holes". Creating 100GB of empty file by
  calling truncate() does not increase the disk usage, though the files appear to have their full size.
  So you can make the file as big as you'll ever need, and the filesystem will take care of it! 
*/
void *map_file(const char *name, size_t size) {
    char buf[256];
    if (strlen(name) >= sizeof(buf) - strlen(database_path) - 2)
        die ("name too long");
    sprintf (buf, "%s/%s", database_path, name); 
    printf("mapping file '%s' of size ", buf);
    human(size);
    printf("\n");
    // including O_TRUNC causes much slower write (swaps pages in?)
    int fd = open(buf, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) 
        die("could not memory map file");
    if (ftruncate (fd, size - 1)) // resize file
        die ("error resizing file");
    return base;
}

/* Arrays of memory-mapped structs */
Grid     *grid;
Node     *nodes;
Way      *ways;
WayBlock *way_blocks;
int64_t  *node_refs;  // negative means last in a list of refs.
uint32_t n_node_refs; // the number of used node refs.
/* 
  The number way blocks currently allocated.
  Start at 1 so we can use the "empty file" 0 value to mean "unassigned". 
*/
uint32_t way_block_count  = 1;
static uint32_t new_way_block() {
    if (way_block_count % 10000 == 0)
        printf("%dk way blocks in use.\n", way_block_count / 1000);
    // neg value in last ref -> number of free slots
    way_blocks[way_block_count].refs[WAY_BLOCK_SIZE-1] = -WAY_BLOCK_SIZE; 
    // printf("created way block %d\n", way_block_count);
    return way_block_count++;
}

// note that this is essentially a node stem plus offsets. 
// we could either explicitly use the node stem type, or save the whole mess in the node table
// with only the node references in the grid leaves. This also avoids having to scan through the bin
// to remove a node.
// then the node and way lists could just be copy-on-write compacted reference lists.
// or to begin with, uncompacted reference lists.
// this could in fact be a union type of two 32-bit x and y or four 16-bit bins or 
// two 8-bit bin levels and one 16-bit offset per axis.
// 8block-8leaf-16offset

// replace internal_coord_t with a union

static uint32_t get_grid_way_block (Node *node) {
    uint32_t xb = ((uint32_t)node->x) >> (32 - GRID_BITS); // unsigned: logical shift
    uint32_t yb = ((uint32_t)node->y) >> (32 - GRID_BITS); // unsigned: logical shift
    uint32_t index = grid->blocks[xb][yb];
    if (index == 0) {
        index = new_way_block();
        grid->blocks[xb][yb] = index;
    }
    // printf("xbin=%d ybin=%d\n", xb, yb);
    // printf("index=%d\n", index);
    return index;
}

// TODO pass node by value since it is small?
static void set_grid_way_block (Node *node, uint32_t wbi) {
    // TODO remove duplicate code (see above)
    uint32_t xb = ((uint32_t)node->x) >> (32 - GRID_BITS); // unsigned: logical shift
    uint32_t yb = ((uint32_t)node->y) >> (32 - GRID_BITS); // unsigned: logical shift
    grid->blocks[xb][yb] = wbi;
}

static int32_t lon_to_x (double lon) {
    return (lon * INT32_MAX) / 180;
}

static int32_t lat_to_y (double lat) {
    return (lat * INT32_MAX) / 90;
}

static long nodes_loaded = 0;
static long ways_loaded = 0;

// Considering ocean covers 70 percent of the earth and the total number of theoretical 
// grid bins is 2^16, we would estimate (UINT16_MAX / 3) ~= 22000 blocks actually used.
// Experiment gives a similar but somewhat higher number of ~24000 grid blocks actually used 
// when loading the entire planet.pbf.
// #define MAX_GRID_BLOCKS (30000)
// Max number of node/way blocks. This is probably an underestimate due to underfilled node blocks.
// The ratio of node to way block sizes should make this value also usable for ways.
#define MAX_WAY_BLOCKS (0.35 * GRID_DIM * GRID_DIM) /* about 100 Mstructs */
#define MAX_NODE_BLOCKS MAX_WAY_ID /* One additional block of node refs per way. */

static void handle_node (OSMPBF__Node *node) {
    if (node->id > MAX_NODE_ID) 
        die("OSM data contains nodes with larger IDs than expected.");
    if (ways_loaded > 0)
        die("All nodes must appear before any ways in input file.");
    double lat = node->lat * 0.0000001;
    double lon = node->lon * 0.0000001;
    nodes[node->id].x = lon_to_x(lon);
    nodes[node->id].y = lat_to_y(lat);
    nodes_loaded++;
    if (nodes_loaded % 1000000 == 0)
        printf("loaded %ldM nodes\n", nodes_loaded / 1000 / 1000);
    //printf ("---\nlon=%.5f lat=%.5f\nx=%d y=%d\n", lon, lat, nodes[node->id].x, nodes[node->id].y); 
}

#define REF_HISTOGRAM_BINS 20
static int node_ref_histogram[REF_HISTOGRAM_BINS]; // initialized and displayed in main

/* Nodes must come before ways for this to work. */
static void handle_way (OSMPBF__Way *way) {
    if (way->id > MAX_WAY_ID)
        die("OSM data contains ways greater IDs than expected.");
    int nr = (way->n_refs > REF_HISTOGRAM_BINS - 1) ? REF_HISTOGRAM_BINS - 1 : way->n_refs;
    node_ref_histogram[nr]++;
    // Copy node references into. 
    // NOTE that all refs in a list are always known at once, so we can use exact-length lists. 
    // Each way stores the position of its first node ref, and a negative ref is used
    // to signal the end of the list.
    ways[way->id].node_ref_offset = n_node_refs;
    for (int r = 0; r < way->n_refs; r++) {
        node_refs[n_node_refs++] = way->refs[r];
    }
    node_refs[n_node_refs - 1] *= -1; // Negate last node ref to signal end of list.
    // Index this way, as being in the grid cell of its first node
    uint32_t wbi = get_grid_way_block(&(nodes[way->refs[0]]));
    WayBlock *wb = &(way_blocks[wbi]);
    // last ref >= 0 means no free slots remaining. chain an empty block.
    // insert at head to avoid too much scanning though large swaths of memory.
    if (wb->refs[WAY_BLOCK_SIZE - 1] >= 0) {
        int32_t n_wbi = new_way_block();
        wb = &(way_blocks[n_wbi]);
        wb->next = wbi;
        set_grid_way_block(&(nodes[way->refs[0]]), n_wbi);
    }
    // We are now certain to have a free slot in the current block.
    int nfree = wb->refs[WAY_BLOCK_SIZE - 1];
    if (nfree >= 0) die ("failed assertion: final ref should be negative.");
    // a final ref < 0 gives the number of remaining slots.
    int free_idx = WAY_BLOCK_SIZE + nfree; 
    wb->refs[free_idx] = way->id;
    // If this was not the last available slot, reduce number of free slots in this block by one
    if (nfree != -1) (wb->refs[WAY_BLOCK_SIZE - 1])++; 
    ways_loaded++;
    if (ways_loaded % 100000 == 0)
        printf("loaded %ldk ways\n", ways_loaded / 1000);
}

/* 
  With 8 bit (255x255) grid, planet.pbf gives 36.87% full
  With 14 bit grid: 248351486 empty 20083970 used, 7.48% full (!)
  With 13 bit grid: 248351486 empty 20083970 used, 10.76% full
*/
void fillFactor () {
    int used = 0;
    for (int i = 0; i < GRID_DIM; ++i) {
        for (int j = 0; j < GRID_DIM; ++j) {
            if (grid->blocks[i][j] != 0) used++;
        }
    }
    printf("index grid: %d used, %.2f%% full\n", 
        used, ((double)used) / (GRID_DIM * GRID_DIM) * 100); 
}

int main (int argc, const char * argv[]) {

    if (argc < 2) die("usage: cosm input.osm.pbf [database_dir]");
    const char *filename = argv[1];
    if (argc > 2) database_path = argv[2];
    grid       = map_file("grid",       sizeof(Grid));
    nodes      = map_file("nodes",      sizeof(Node)     * MAX_NODE_ID);
    ways       = map_file("ways",       sizeof(Way)      * MAX_WAY_ID);
    node_refs  = map_file("node_refs",  sizeof(int64_t)  * MAX_NODE_ID);
    way_blocks = map_file("way_blocks", sizeof(WayBlock) * MAX_WAY_BLOCKS);
    // ^ Assume that there are as many node refs as there are node ids including holes. [MAX_NODE_ID]

    osm_callbacks_t callbacks;
    callbacks.way = &handle_way;
    callbacks.node = &handle_node;
    for (int i=0; i<REF_HISTOGRAM_BINS; i++) node_ref_histogram[i] = 0;
    scan_pbf(filename, &callbacks);
    fillFactor();
    for (int i=0; i<REF_HISTOGRAM_BINS; i++) printf("%2d refs: %d\n", i, node_ref_histogram[i]);
    printf("loaded %ld nodes and %ld ways total.\n", nodes_loaded, ways_loaded);
    return EXIT_SUCCESS;

// TODO check fill proportion of grid blocks

}


